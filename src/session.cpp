// =============================================================================
//  rtpmech - RtpSession 实现
// =============================================================================
//  文件    : session.cpp
//  编码    : UTF-8
//
//  设计要点（详见 docs/design/overview-design.md）
//  ---------------------------------------------------------------------------
//  * **两个端点、一条线程**：RTP 端点正常创建；RTCP 端点用
//    `create_udp_endpoint_on(rtp, ...)` 跟随它的 io_context，RTCP 定时器也挂在
//    RTCP 端点上。三者完全串行 → `source_tracker` / `rtcp_scheduler` 无锁。
//  * **收包零分配**：netmech 的 UdpEndpoint 是推送 + borrow 交付，
//    回调里的 Buf 指向库的复用缓冲，解析完即弃。
//  * **时间戳按采样时刻**：固定增量（默认 25 fps @ 90 kHz = 3600）。
//    用发送时刻会把编码延迟与调度抖动写进时间戳，播放端会抖。
//  * **端口成对绑定**：RTP 偶数端口，RTCP = RTP + 1；任一失败则整体回滚。
// =============================================================================
#include "rtpmech/session.hpp"

#include <rtpmech/rtcp_packet.hpp>
#include <rtpmech/rtp_packet.hpp>

#include <netmech/net.hpp>

#include "rtcp_scheduler.hpp"
#include "source_tracker.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace rtpmech {

namespace {

using steady_clock_t = std::chrono::steady_clock;

/// 生成 SSRC / 初始时间戳（RFC 3550 §5.1 要求不可预测）。
std::uint32_t gen_random32() noexcept
{
    const auto now = static_cast<std::uint64_t>(steady_clock_t::now().time_since_epoch().count());
    const auto mix = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&now));
    std::uint64_t x = now ^ (mix << 17) ^ (mix >> 29);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 29;
    return static_cast<std::uint32_t>(x);
}

double steady_now_sec() noexcept
{
    return std::chrono::duration<double>(steady_clock_t::now().time_since_epoch()).count();
}

RtcpSchedulerParams make_scheduler_params(const RtpSessionConfig& cfg)
{
    RtcpSchedulerParams p;
    if (cfg.rtcp_min_interval_sec > 0.0) {
        p.min_interval_sec = cfg.rtcp_min_interval_sec;
    }
    return p;
}

std::uint16_t round_up_even(std::uint16_t p) noexcept
{
    return static_cast<std::uint16_t>((p & 1u) ? (p + 1u) : p);
}

/// 系统分配的端口若是奇数，丢弃重来（RFC 3550 §11 要求 RTP 用偶数端口）。
constexpr int kMaxAutoPortAttempts = 32;

} // namespace

// =============================================================================
//  Impl
// =============================================================================

class RtpSession::Impl : public std::enable_shared_from_this<RtpSession::Impl> {
public:
    explicit Impl(const RtpSessionConfig& config)
        : cfg_(config)
        , ssrc_(config.local_ssrc != 0 ? config.local_ssrc : gen_random32())
        , ts_base_(gen_random32())
        , ts_current_(ts_base_)
        , ts_increment_(config.timestamp_increment)
        , tracker_(config.local_ssrc != 0 ? config.local_ssrc : 0, config.clock_rate)
        , scheduler_(make_scheduler_params(config))
    {
    }

    ~Impl() { close(); }

    void set_facade(RtpSession* f) noexcept { facade_ = f; }

    // -------------------------------------------------------------------------
    //  两个 netmech handler：把报文转给 Impl
    // -------------------------------------------------------------------------

    class RtpSink final : public netmech::UdpHandler {
    public:
        explicit RtpSink(std::weak_ptr<Impl> owner) : owner_(std::move(owner)) {}

        void on_read(netmech::UdpEndpoint&, std::error_code ec, netmech::Buf buf,
                     std::size_t n, const netmech::Address& from) override
        {
            if (ec) {
                return;   // 截断等：丢这一条，端点仍可用
            }
            if (auto self = owner_.lock()) {
                self->on_rtp_datagram(static_cast<const std::uint8_t*>(buf.data), n, from);
            }
        }

        void on_close(netmech::UdpEndpoint&, std::error_code) override
        {
            if (auto self = owner_.lock()) {
                self->close();
            }
        }

    private:
        std::weak_ptr<Impl> owner_;
    };

    class RtcpSink final : public netmech::UdpHandler {
    public:
        explicit RtcpSink(std::weak_ptr<Impl> owner) : owner_(std::move(owner)) {}

        void on_read(netmech::UdpEndpoint&, std::error_code ec, netmech::Buf buf,
                     std::size_t n, const netmech::Address& from) override
        {
            if (ec) {
                return;
            }
            if (auto self = owner_.lock()) {
                self->on_rtcp_datagram(static_cast<const std::uint8_t*>(buf.data), n, from);
            }
        }

    private:
        std::weak_ptr<Impl> owner_;
    };

    // -------------------------------------------------------------------------
    //  创建
    // -------------------------------------------------------------------------

    std::error_code open(std::shared_ptr<RtpHandler> h)
    {
        handler_ = std::move(h);

        // ---- RTP 端点 ----
        std::error_code ec;
        if (!open_rtp_endpoint(ec)) {
            return ec;
        }

        // ---- RTCP 端点：跟随 RTP 的 io 线程，端口 = RTP + 1 ----
        if (cfg_.enable_rtcp) {
            const std::uint16_t rtcp_port = static_cast<std::uint16_t>(rtp_port_ + 1);

            netmech::UdpConfig rtcp_cfg;
            rtcp_cfg.bind          = netmech::Address(cfg_.local.address_string(), rtcp_port);
            rtcp_cfg.recv_capacity = std::max<std::size_t>(cfg_.recv_capacity, 64);

            rtcp_ep_ = netmech::create_udp_endpoint_on(
                *rtp_ep_, rtcp_cfg, std::make_shared<RtcpSink>(weak_from_this()), nullptr, ec);
            if (!rtcp_ep_) {
                // ★ 整对失败：RTP 必须回滚。否则端口被一个"半成品会话"占着，
                //   调用方重试永远绑不上。
                rtp_ep_->close();
                rtp_ep_.reset();
                rtp_port_ = 0;
                return ec;
            }
            rtcp_port_ = rtcp_ep_->local_address().port();
        }

        scheduler_.set_session_bandwidth(
            cfg_.session_bandwidth > 0.0 ? cfg_.session_bandwidth / 8.0 : 0.0);
        scheduler_.update_members(1, 1);
        scheduler_.reset(steady_now_sec());

        first_send_time_ = steady_clock_t::now();
        open_.store(true, std::memory_order_release);

        if (cfg_.enable_rtcp) {
            schedule_rtcp();
        }
        return {};
    }

    // -------------------------------------------------------------------------
    //  接收
    // -------------------------------------------------------------------------

    void on_rtp_datagram(const std::uint8_t* data, std::size_t len,
                         const netmech::Address& from)
    {
        if (!open_.load(std::memory_order_acquire)) {
            return;
        }

        std::error_code pec;
        auto pkt = RtpPacket::parse(data, len, pec);
        if (!pkt) {
            parse_errors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // SSRC 冲突（RFC 3550 §8.2）：收到的包 SSRC 与自己相同但来源不同。
        if (pkt->ssrc() == ssrc_.load(std::memory_order_acquire)) {
            on_ssrc_collision(from);
            return;
        }

        rtp_received_.fetch_add(1, std::memory_order_relaxed);
        tracker_.on_rtp_received(*pkt, steady_now_us());

        // 回调里 pkt 是借用视图，返回即失效
        if (handler_ && facade_ != nullptr) {
            handler_->on_packet(*facade_, *pkt, from);
        }
    }

    void on_rtcp_datagram(const std::uint8_t* data, std::size_t len,
                          const netmech::Address& from)
    {
        if (!open_.load(std::memory_order_acquire)) {
            return;
        }
        std::error_code pec;
        auto compound = RtcpCompoundPacket::parse(data, len, pec);
        if (!compound) {
            parse_errors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        rtcp_received_.fetch_add(1, std::memory_order_relaxed);

        if (handler_ && facade_ != nullptr) {
            handler_->on_rtcp(*facade_, *compound, from);
        }
    }

    // -------------------------------------------------------------------------
    //  发送
    // -------------------------------------------------------------------------

    SendResult send_payload(netmech::ConstBuf payload, bool marker,
                            const std::optional<std::uint32_t>& ts_override,
                            const std::optional<netmech::Address>& dst_override)
    {
        if (!open_.load(std::memory_order_acquire)) {
            return SendResult::closed;
        }
        if (payload.data == nullptr || payload.size == 0) {
            return SendResult::invalid;
        }
        // MTU：头按最坏情况估（固定头 12 字节，本实现不加 CSRC/扩展）
        if (12 + payload.size > cfg_.max_packet_size) {
            return SendResult::invalid;
        }

        RtpPacketInfo info;
        info.payload_type = cfg_.payload_type;
        info.marker       = marker;
        info.sequence     = seq_.fetch_add(1, std::memory_order_relaxed);
        info.ssrc         = ssrc_.load(std::memory_order_acquire);
        info.timestamp    = ts_override.has_value() ? *ts_override : next_timestamp();

        // 组装到堆缓冲：send_to 是"已受理"，缓冲要活到真正发出，用 owner 交给库
        auto wire = std::make_shared<std::vector<std::uint8_t>>(12 + payload.size);
        std::error_code bec;
        const std::size_t written = build_rtp_packet(
            wire->data(), wire->size(), info,
            span<const std::uint8_t>(static_cast<const std::uint8_t*>(payload.data),
                                     payload.size),
            bec);
        if (bec || written == 0) {
            return SendResult::invalid;
        }

        netmech::Address dst;
        if (dst_override.has_value()) {
            dst = *dst_override;
        } else if (cfg_.remote.has_value()) {
            dst = *cfg_.remote;
        }
        if (dst.port() == 0) {
            return SendResult::invalid;   // 既没配 remote 也没给 dst
        }

        if (!rtp_ep_->send_to(dst, netmech::ConstBuf{wire, wire->data(), written})) {
            return SendResult::closed;
        }

        rtp_sent_.fetch_add(1, std::memory_order_relaxed);
        octet_sent_.fetch_add(payload.size, std::memory_order_relaxed);
        return SendResult::accepted;
    }

    // -------------------------------------------------------------------------
    //  RTCP
    // -------------------------------------------------------------------------

    bool send_rtcp(bool force)
    {
        if (!cfg_.enable_rtcp || !rtcp_ep_ || !open_.load(std::memory_order_acquire)) {
            return false;
        }
        if (!force && !scheduler_.due(steady_now_sec())) {
            return true;   // 未到期
        }

        const auto blocks = tracker_.build_report_blocks();

        std::vector<std::uint8_t> buf(cfg_.max_packet_size, 0);
        std::size_t off = 0;
        std::error_code ec;

        SenderReport sr;
        sr.ssrc = ssrc_.load(std::memory_order_acquire);
        {
            const auto now_wall = std::chrono::system_clock::now();
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                  now_wall.time_since_epoch()).count();
            const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   now_wall.time_since_epoch()).count() % 1000000000LL;
            const NtpTime ntp = make_ntp_time(secs, static_cast<std::uint32_t>(nanos));
            sr.ntp_msw       = ntp.seconds;
            sr.ntp_lsw       = ntp.fraction;
            sr.rtp_timestamp = current_timestamp();
            sr.packet_count  = static_cast<std::uint32_t>(rtp_sent_.load());
            sr.octet_count   = static_cast<std::uint32_t>(octet_sent_.load());
        }
        sr.reports = blocks;

        const std::size_t n_sr = build_rtcp_sender_report(buf.data(), buf.size(), sr, ec);
        if (ec) {
            return false;
        }
        off += n_sr;

        // 复合包里附带 SDES(CNAME)：RFC 3550 要求 CNAME 稳定且可读
        SdesReport sdes;
        SdesChunk chunk;
        chunk.ssrc  = sr.ssrc;
        chunk.cname = "rtpmech-" + std::to_string(sr.ssrc);
        sdes.chunks.push_back(chunk);

        const std::size_t n_sdes =
            build_rtcp_sdes(buf.data() + off, buf.size() - off, sdes, ec);
        if (ec) {
            return false;
        }
        off += n_sdes;

        if (!send_on_rtcp(buf, off)) {
            return false;
        }
        // 告诉调度器"发出去了、多大"，它据此更新平均包大小并排下一拍
        scheduler_.on_rtcp_sent(off, steady_now_sec());
        return true;
    }

    void schedule_rtcp()
    {
        if (!cfg_.enable_rtcp || !rtcp_ep_ || !open_.load(std::memory_order_acquire)) {
            return;
        }
        const double now = steady_now_sec();
        // 距下次还剩多久（可能为负 = 已逾期，给个下限避免忙等）
        const double delay_sec = std::max(0.001, scheduler_.time_until_next(now));
        const auto interval =
            std::chrono::milliseconds(static_cast<std::int64_t>(delay_sec * 1000.0));

        auto weak = weak_from_this();
        rtcp_ep_->set_timer(interval, weak, [weak] {
            if (auto s = weak.lock()) {
                (void)s->send_rtcp(/*force=*/false);
                s->schedule_rtcp();      // 下一拍（间隔随成员数变化）
            }
        });
    }

    void send_bye(const std::string& reason)
    {
        if (!cfg_.enable_rtcp || !rtcp_ep_) {
            return;
        }
        std::vector<std::uint8_t> buf(cfg_.max_packet_size, 0);
        std::error_code ec;

        ByeReport bye;
        bye.ssrcs.push_back(ssrc_.load(std::memory_order_acquire));
        if (!reason.empty()) {
            bye.reason = reason;
        }

        const std::size_t n = build_rtcp_bye(buf.data(), buf.size(), bye, ec);
        if (ec || n == 0) {
            return;
        }
        (void)send_on_rtcp(buf, n);
    }

    // -------------------------------------------------------------------------
    //  关闭
    // -------------------------------------------------------------------------

    void close() noexcept
    {
        if (closed_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        const bool was_open = open_.exchange(false, std::memory_order_acq_rel);

        if (was_open) {
            send_bye({});            // 尽力发，不关心结果
        }
        if (rtcp_ep_) {
            rtcp_ep_->clear_timer();
            rtcp_ep_->close();
            rtcp_ep_.reset();
        }
        if (rtp_ep_) {
            rtp_ep_->close();
            rtp_ep_.reset();
        }

        if (was_open && handler_) {
            handler_->on_close(std::error_code{});
        }
    }

    // -------------------------------------------------------------------------
    //  查询
    // -------------------------------------------------------------------------

    std::uint16_t local_rtp_port() const noexcept  { return rtp_port_; }
    std::uint16_t local_rtcp_port() const noexcept { return rtcp_port_; }

    std::uint32_t local_ssrc() const noexcept
    {
        return ssrc_.load(std::memory_order_acquire);
    }

    bool is_open() const noexcept { return open_.load(std::memory_order_acquire); }

    RtpSessionStats stats() const
    {
        RtpSessionStats st;
        st.rtp_sent       = rtp_sent_.load(std::memory_order_relaxed);
        st.rtp_received   = rtp_received_.load(std::memory_order_relaxed);
        st.rtcp_sent      = rtcp_sent_.load(std::memory_order_relaxed);
        st.rtcp_received  = rtcp_received_.load(std::memory_order_relaxed);
        st.send_rejected  = send_rejected_.load(std::memory_order_relaxed);
        st.parse_errors   = parse_errors_.load(std::memory_order_relaxed);
        st.active_sources = tracker_.size();
        st.rtt_us         = tracker_.rtt_us();
        return st;
    }

    const RtpSessionConfig& config() const noexcept { return cfg_; }

private:
    // -------------------------------------------------------------------------
    //  端口
    // -------------------------------------------------------------------------

    /// 建立 RTP 端点。端口 0 时交给系统分配，但要保证偶数 —— 奇数就丢弃重来。
    bool open_rtp_endpoint(std::error_code& ec)
    {
        const std::uint16_t want = cfg_.local.port();

        if (want != 0) {
            // 显式端口：向上取偶，绑不上就是失败（协商场景不重试）
            return try_bind_rtp(round_up_even(want), ec);
        }

        // 端口 0：系统分配 + 偶数校验
        for (int i = 0; i < kMaxAutoPortAttempts; ++i) {
            if (!try_bind_rtp(0, ec)) {
                return false;
            }
            if ((rtp_port_ & 1u) == 0) {
                return true;      // 偶数，可用
            }
            // 系统给了奇数：丢弃重来
            rtp_ep_->close();
            rtp_ep_.reset();
            rtp_port_ = 0;
        }
        ec = make_error_code(RtpErrc::invalid_argument);
        return false;
    }

    bool try_bind_rtp(std::uint16_t port, std::error_code& ec)
    {
        netmech::UdpConfig cfg;
        cfg.bind                = netmech::Address(cfg_.local.address_string(), port);
        cfg.recv_capacity       = std::max<std::size_t>(cfg_.recv_capacity, 64);
        cfg.multicast_group     = cfg_.multicast_group;
        cfg.multicast_interface = cfg_.multicast_interface;
        cfg.multicast_hops      = cfg_.multicast_hops;
        cfg.multicast_loopback  = cfg_.multicast_loopback;

        rtp_ep_ = netmech::create_udp_endpoint(
            cfg, std::make_shared<RtpSink>(weak_from_this()), nullptr, ec);
        if (!rtp_ep_) {
            return false;
        }
        rtp_port_ = rtp_ep_->local_address().port();
        return true;
    }

    // -------------------------------------------------------------------------
    //  辅助
    // -------------------------------------------------------------------------

    /// 把 RTCP 报文发到对端的 RTCP 端口（remote.port() + 1）。
    bool send_on_rtcp(std::vector<std::uint8_t>& buf, std::size_t len)
    {
        if (!rtcp_ep_ || !cfg_.remote.has_value()) {
            return false;
        }
        const netmech::Address dst(cfg_.remote->address_string(),
                                   static_cast<std::uint16_t>(cfg_.remote->port() + 1));

        // 缓冲随 owner 交给库，保证活到真正发出
        auto holder = std::make_shared<std::vector<std::uint8_t>>(std::move(buf));
        if (!rtcp_ep_->send_to(dst, netmech::ConstBuf{holder, holder->data(), len})) {
            return false;
        }
        rtcp_sent_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void on_ssrc_collision(const netmech::Address& from)
    {
        // 同一来源重复出现不算冲突（自己的包回环）
        const std::string key = from.to_string();
        if (last_collision_from_ == key) {
            return;
        }
        last_collision_from_ = key;

        const std::uint32_t old_ssrc = ssrc_.load(std::memory_order_acquire);
        // 已经用这个 SSRC 发过数据 → 先发 BYE 告知对端
        if (rtp_sent_.load(std::memory_order_relaxed) > 0) {
            send_bye({});
        }
        const std::uint32_t new_ssrc = gen_random32();
        ssrc_.store(new_ssrc, std::memory_order_release);

        std::printf("[rtpmech] SSRC collision from %s: %08X -> %08X\n",
                    from.to_string().c_str(), old_ssrc, new_ssrc);
    }

    std::uint32_t next_timestamp()
    {
        if (ts_increment_ != 0) {
            const std::uint32_t ts = ts_current_;
            ts_current_ += ts_increment_;
            return ts;
        }
        // 按真实发送时刻（仅适用于"采集即发、无独立时间基"）
        const double elapsed = std::chrono::duration<double>(
            steady_clock_t::now() - first_send_time_).count();
        return ts_base_ +
               static_cast<std::uint32_t>(elapsed * static_cast<double>(cfg_.clock_rate));
    }

    std::uint32_t current_timestamp() const
    {
        if (ts_increment_ != 0) {
            return ts_current_;
        }
        const double elapsed = std::chrono::duration<double>(
            steady_clock_t::now() - first_send_time_).count();
        return ts_base_ +
               static_cast<std::uint32_t>(elapsed * static_cast<double>(cfg_.clock_rate));
    }

    RtpSessionConfig cfg_;
    RtpSession*      facade_ = nullptr;

    std::shared_ptr<netmech::UdpEndpoint> rtp_ep_;
    std::shared_ptr<netmech::UdpEndpoint> rtcp_ep_;
    std::shared_ptr<RtpHandler>           handler_;

    std::uint16_t rtp_port_  = 0;
    std::uint16_t rtcp_port_ = 0;

    std::atomic<std::uint32_t> ssrc_{0};
    std::atomic<std::uint16_t> seq_{0};
    std::atomic<bool>          open_{false};
    std::atomic<bool>          closed_{false};

    std::uint32_t              ts_base_      = 0;
    std::uint32_t              ts_current_   = 0;
    std::uint32_t              ts_increment_ = 0;
    steady_clock_t::time_point first_send_time_{};

    // 协议组件：只在本人 io 线程上访问 → 无锁
    SourceTracker tracker_;
    RtcpScheduler scheduler_;

    std::atomic<std::uint64_t> rtp_sent_{0};
    std::atomic<std::uint64_t> rtp_received_{0};
    std::atomic<std::uint64_t> rtcp_sent_{0};
    std::atomic<std::uint64_t> rtcp_received_{0};
    std::atomic<std::uint64_t> send_rejected_{0};
    std::atomic<std::uint64_t> parse_errors_{0};
    std::atomic<std::uint64_t> octet_sent_{0};

    std::string last_collision_from_;
};

// =============================================================================
//  RtpSession 门面
// =============================================================================

RtpSession::RtpSession() = default;

RtpSession::~RtpSession()
{
    if (impl_) {
        impl_->close();
    }
}

std::shared_ptr<RtpSession> RtpSession::create(const RtpSessionConfig&     cfg,
                                               std::shared_ptr<RtpHandler> handler,
                                               std::error_code&            ec)
{
    ec.clear();

    // clock_rate 为 0 会让时间戳换算除零；payload_type 0 是合法的（PCMU），不校验
    if (cfg.clock_rate == 0) {
        ec = make_error_code(RtpErrc::invalid_argument);
        return nullptr;
    }
    if (cfg.recv_capacity == 0 || cfg.max_packet_size < 64) {
        ec = make_error_code(RtpErrc::invalid_argument);
        return nullptr;
    }

    auto session = std::shared_ptr<RtpSession>(new RtpSession());
    session->impl_ = std::make_shared<Impl>(cfg);
    session->impl_->set_facade(session.get());

    ec = session->impl_->open(std::move(handler));
    if (ec) {
        return nullptr;
    }
    return session;
}

SendResult RtpSession::send(netmech::ConstBuf payload, bool marker)
{
    if (!impl_) {
        return SendResult::closed;
    }
    return impl_->send_payload(payload, marker, std::nullopt, std::nullopt);
}

SendResult RtpSession::send_with_timestamp(netmech::ConstBuf payload, bool marker,
                                           std::uint32_t timestamp)
{
    if (!impl_) {
        return SendResult::closed;
    }
    return impl_->send_payload(payload, marker, timestamp, std::nullopt);
}

SendResult RtpSession::send_to(const netmech::Address& dst,
                               netmech::ConstBuf payload, bool marker)
{
    if (!impl_) {
        return SendResult::closed;
    }
    return impl_->send_payload(payload, marker, std::nullopt, dst);
}

bool RtpSession::send_rtcp_now()
{
    return impl_ && impl_->send_rtcp(/*force=*/true);
}

void RtpSession::send_bye(const std::string& reason, bool close_after)
{
    if (!impl_) {
        return;
    }
    if (impl_->is_open()) {
        impl_->send_bye(reason);
    }
    if (close_after) {
        impl_->close();
    }
}

std::uint16_t RtpSession::local_rtp_port() const noexcept
{
    return impl_ ? impl_->local_rtp_port() : 0;
}

std::uint16_t RtpSession::local_rtcp_port() const noexcept
{
    return impl_ ? impl_->local_rtcp_port() : 0;
}

std::uint32_t RtpSession::local_ssrc() const noexcept
{
    return impl_ ? impl_->local_ssrc() : 0;
}

RtpSessionStats RtpSession::stats() const
{
    return impl_ ? impl_->stats() : RtpSessionStats{};
}

bool RtpSession::is_open() const noexcept
{
    return impl_ && impl_->is_open();
}

const RtpSessionConfig& RtpSession::config() const noexcept
{
    static const RtpSessionConfig kDefault{};
    return impl_ ? impl_->config() : kDefault;
}

void RtpSession::close() noexcept
{
    if (impl_) {
        impl_->close();
    }
}

} // namespace rtpmech
