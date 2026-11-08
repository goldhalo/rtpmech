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
#include "net_bridge.hpp"
#include "rtpmech/session.hpp"

#include <atomic>
#include <array>
#include <rtpmech/rtcp_packet.hpp>
#include <rtpmech/rtp_packet.hpp>

#include <netmech/net.hpp>
#include <rtpmech/packetizer.hpp>

#include "rtcp_scheduler.hpp"
#include "source_tracker.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>
#include <optional>
#include <memory>
#include <map>
#include <mutex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

namespace rtpmech {

namespace {

using steady_clock_t = std::chrono::steady_clock;

/// 生成 SSRC / 初始时间戳（RFC 3550 §5.1 要求不可预测）。
/// 源失活超时：RFC 3550 §6.3.5 建议 5 个报告间隔。
/// 默认 RTCP 间隔 5s ⇒ 25s；这里固定成常量，不随会话带宽调整
/// （带宽变了只会让间隔更长，超时放宽一点没有坏处）。
constexpr std::int64_t kSourceTimeoutUs = 25 * 1000 * 1000;

std::string make_default_cname(std::uint32_t ssrc)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "rtpmech-%08x", ssrc);
    return buf;
}

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
        : Impl(config, config.local_ssrc != 0 ? config.local_ssrc : gen_random32())
    {
    }

    /// @param[in] ssrc 本会话的 SSRC（配置值或随机生成，**已经定好**）。
    ///
    /// 为什么要拆一层委托构造：以前 `ssrc_` 是随机的，而传给 `tracker_` 的是
    /// `config.local_ssrc != 0 ? ... : 0` —— 一旦用随机 SSRC，tracker 就认为
    /// 本端 SSRC 未知（0），`on_receiver_report()` 第一条判据
    /// `local_ssrc_ == 0` 直接把所有 RR 跳过 ⇒ RTT 永远算不出来。
    /// 两个对象必须拿到**同一个**值，所以在这里一次算好。
    Impl(const RtpSessionConfig& config, std::uint32_t ssrc)
        : cfg_(config)
        , ssrc_(ssrc)
        , ts_base_(gen_random32())
        , ts_current_(ts_base_)
        , ts_increment_(config.timestamp_increment)
        , tracker_(ssrc, config.clock_rate)
        , scheduler_(make_scheduler_params(config))
    {
        // CNAME 与本会话绑定，**不跟 SSRC 走**：SSRC 因冲突换号时它保持不变，
        // 对端才能把前后两条流归并到同一个逻辑实体（RFC 3550 §6.5.1）。
        cname_ = cfg_.cname.empty() ? make_default_cname(ssrc) : cfg_.cname;
    }

    ~Impl()
    {
        // 引用计数已归零：没有别的线程还会碰它，直接在析构线程收尾。
        if (!closed_.exchange(true, std::memory_order_acq_rel)) {
            open_.store(false, std::memory_order_release);
            do_close();
        }
    }

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
                // ★ 只有会话**已经 open** 时，端点关闭才代表"连接没了"。
                //   端口搜索阶段会主动丢弃奇数端口的端点，那时的 on_close
                //   属于内部动作，不能把会话判死。
                if (self->is_open()) {
                    Impl::request_close(self);
                }
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
            rtcp_cfg.bind          = netmech::Address(cfg_.local.host(), rtcp_port);
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
        // 初始只有本端一个成员（还没有远端源、也还没发过包 ⇒ 不是 sender）。
        // 真正的值每拍由 send_rtcp() 按 tracker_ 的实际内容刷新 ——
        // 这里以前写死 (1, 1)，等于告诉调度器"场上两个人、两个都在发"，
        // 带宽份额从一开始就分错了。
        scheduler_.update_members(1, 0);
        scheduler_.reset(steady_now_sec());

        first_send_time_ = steady_clock_t::now();
        open_.store(true, std::memory_order_release);

        // 没有 remote 时排定时器只会「每秒上千次尝试发送并失败」（见 rtcp_failed）。
        // 帧级/包级发送都可以不配 remote（用 send_to），但 RTCP 没有显式目标，
        // 因此只在有 remote 时启动自动调度。
        if (cfg_.enable_rtcp && cfg_.remote.has_value()) {
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

        // SSRC 相同的包有两种可能：
        //   (a) 自己发出的包被回环回来（组播 loopback / 自己发给自己）—— 忽略；
        //   (b) 真的 SSRC 冲突（RFC 3550 §8.2）—— 换号。
        //
        // 判据用「序号是否落在最近发出的窗口内」，而不是比来源地址：组播回环
        // 的源地址是本机某张网卡的 IP，与 bind 的 0.0.0.0 永远对不上。
        // 以前没有这个判断，组播发送端会把自己的包当成冲突，SSRC 每包一换。
        if (pkt->ssrc() == ssrc_.load(std::memory_order_acquire)) {
            if (is_self_loopback_seq(pkt->sequence())) {
                self_loopback_ignored_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            on_ssrc_collision(from);
            return;
        }

        rtp_received_.fetch_add(1, std::memory_order_relaxed);
        tracker_.on_rtp_received(*pkt, steady_now_us());
        sync_tracker_stats();

        // 回调里 pkt 是借用视图，返回即失效
        if (handler_ && facade_ != nullptr) {
            handler_->on_packet(*facade_, *pkt, detail::from_address(from));
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

        // RFC 3550 §6.1：复合包第一个报文必须是 SR 或 RR。顺序错说明对方
        // 实现有问题（或数据被伪造/错位），整包丢弃，不进 tracker。
        const auto& pkts = compound->packets();
        if (pkts.empty() ||
            (pkts.front().type() != RtcpType::sender_report &&
             pkts.front().type() != RtcpType::receiver_report)) {
            parse_errors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!compound->has_cname()) {
            // CNAME 缺失属于协议违规，但不少设备/网关会省；只计数不丢包，
            // 避免为一条可容忍的违规把整条 RTCP 反馈链断掉。
            rtcp_without_cname_.fetch_add(1, std::memory_order_relaxed);
        }
        rtcp_received_.fetch_add(1, std::memory_order_relaxed);

        // ★ 先把报文喂给源跟踪器。
        //
        //   这一步以前**完全缺失** —— 收到 RTCP 只是 parse 完递给应用回调，
        //   tracker_ 一个成员都没调。于是 RFC 3550 §6 的"双向报告闭环"整条断掉：
        //     · 收不到对端 SR ⇒ 我方报告块的 LSR/DLSR 恒 0 ⇒ **对端**算不出 RTT；
        //     · 收不到对端 RR ⇒ 我方 rtt_us() 恒 nullopt；
        //     · BYE 不移除源 ⇒ 已退出的源永远留在 map 和报告块里（31 个上限一到，
        //       活着的源反而被挤出去）。
        //   单测测不出来：test_source_tracker 直接 new tracker 测算法，
        //   test_session 只看 rtcp_sent/received 计数，从不看报文内容。
        const std::int64_t now_us = steady_now_us();
        for (const RtcpPacket& pkt : compound->packets()) {
            if (const SenderReport* sr = pkt.as_sender_report()) {
                tracker_.on_sender_report(*sr, now_us);
            } else if (const ReceiverReport* rr = pkt.as_receiver_report()) {
                tracker_.on_receiver_report(*rr, now_us);
            } else if (const ByeReport* bye = pkt.as_bye()) {
                tracker_.on_bye(*bye);
            }
        }
        sync_tracker_stats();

        if (handler_ && facade_ != nullptr) {
            handler_->on_rtcp(*facade_, *compound, detail::from_address(from));
        }
    }

    // -------------------------------------------------------------------------
    //  发送
    // -------------------------------------------------------------------------

    /// 对外入口：同步校验 + 投递到 io 线程。
    SendResult post_send(rtpmech::ConstBuf payload, bool marker,
                         std::optional<std::uint32_t> ts_override,
                         std::optional<Endpoint>       dst_override)
    {
        if (!open_.load(std::memory_order_acquire)) {
            return SendResult::closed;
        }
        if (payload.data == nullptr || payload.size == 0) {
            return SendResult::invalid;
        }
        // MTU：头按最坏情况估（固定头 12 字节，本实现不加 CSRC/扩展）。
        // 这条必须在**投递之前**判 —— 否则调用方拿到的是 accepted，
        // 而实际上包已经被丢掉了。
        if (12 + payload.size > cfg_.max_packet_size) {
            return SendResult::invalid;
        }
        if (dst_override) {
            if (dst_override->port() == 0) {
                return SendResult::invalid;
            }
        } else if (!cfg_.remote.has_value() || cfg_.remote->port() == 0) {
            return SendResult::invalid;
        }

        auto self = weak_from_this().lock();
        if (!self) {
            return SendResult::closed;
        }
        auto fn = [self, payload, marker, ts_override, dst_override]() mutable {
            (void)self->send_payload(payload, marker, ts_override, dst_override);
        };
        if (post_) {
            post_(std::move(fn), /*allow_inline=*/true);
        } else {
            fn();
        }
        return SendResult::accepted;
    }

    /// 对外入口：提交"立刻发一次 RTCP"。
    bool post_rtcp_now()
    {
        auto self = weak_from_this().lock();
        if (!self) {
            return false;
        }
        auto fn = [self] { (void)self->send_rtcp(/*force=*/true); };
        if (post_) {
            post_(std::move(fn), /*allow_inline=*/true);
        } else {
            fn();
        }
        return true;
    }

    /// 对外入口：发 BYE（可选顺带关闭会话）。
    ///
    /// 与 `post_send` / `post_rtcp_now` 同构 —— 以前门面直接就地调
    /// `send_bye()`，而它要读 `rtcp_ep_`、发 UDP、动 `ssrc_`，这些都在
    /// io 线程上被 `do_close()` 改，是标准的"检查后再用"竞态。
    ///
    /// `allow_inline=false`：BYE 必须排在已投递的发送之后（先让数据出去、再告别），
    /// 而且在 io 线程上调用时也不能插到前面去。`close_after` 在同一任务里串着做，
    /// 保证"BYE 一定先于关闭"。
    void post_bye(std::string reason, bool close_after)
    {
        auto self = weak_from_this().lock();
        if (!self) {
            return;
        }
        auto fn = [self, reason = std::move(reason), close_after]() mutable {
            if (self->open_.load(std::memory_order_acquire)) {
                self->send_bye(reason);
            }
            if (close_after) {
                Impl::request_close(self);
            }
        };
        if (post_) {
            post_(std::move(fn), /*allow_inline=*/false);
        } else {
            fn();
        }
    }

    SendResult send_payload(rtpmech::ConstBuf payload, bool marker,
                            const std::optional<std::uint32_t>& ts_override,
                            const std::optional<Endpoint>& dst_override)
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

        // ★ 先解析目标并确认端点仍在，再取序号：
        //   否则"参数非法/已关闭"的发送也会消耗序号，对端看到空洞。
        netmech::Address dst;
        if (dst_override.has_value()) {
            dst = detail::to_address(*dst_override);
        } else if (cfg_.remote.has_value()) {
            dst = detail::to_address(*cfg_.remote);
        }
        if (dst.port() == 0) {
            return SendResult::invalid;   // 既没配 remote 也没给 dst
        }
        if (!rtp_ep_) {
            return SendResult::closed;
        }

        // 显式时间戳也要推进内部游标：否则之后切回 send() 会拿到旧值。
        if (ts_override.has_value()) {
            advance_ts_from(*ts_override);
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
        if (!rtp_ep_->send_to(dst, netmech::ConstBuf{wire, wire->data(), written})) {
            return SendResult::closed;
        }

        note_sent_seq(info.sequence);
        rtp_sent_.fetch_add(1, std::memory_order_relaxed);
        octet_sent_.fetch_add(payload.size, std::memory_order_relaxed);
        return SendResult::accepted;
    }

    // -------------------------------------------------------------------------
    //  帧级发送（整帧一个时间戳）
    // -------------------------------------------------------------------------

    /// 同步校验 + 投递到 io 线程。frame 已由门面复制进 shared_ptr。
    SendResult post_send_frame(std::shared_ptr<std::vector<std::uint8_t>> frame,
                               Packetizer* packetizer,
                               std::optional<std::uint32_t> ts_override)
    {
        if (!open_.load(std::memory_order_acquire)) {
            return SendResult::closed;
        }
        if (!packetizer || !frame || frame->empty()) {
            return SendResult::invalid;
        }
        if (!cfg_.remote.has_value() || cfg_.remote->port() == 0) {
            return SendResult::invalid;   // 帧级 API 只发 remote
        }

        auto self = weak_from_this().lock();
        if (!self) {
            return SendResult::closed;
        }
        auto fn = [self, frame = std::move(frame), packetizer, ts_override]() mutable {
            (void)self->send_frame_now(
                span<const std::uint8_t>(frame->data(), frame->size()),
                *packetizer, ts_override);
        };
        if (post_) {
            post_(std::move(fn), /*allow_inline=*/true);
        } else {
            fn();
        }
        return SendResult::accepted;
    }

    /// io 线程上执行：取**一次**时间戳，所有分片共用。
    SendResult send_frame_now(span<const std::uint8_t> frame, Packetizer& packetizer,
                              std::optional<std::uint32_t> ts_override)
    {
        if (!open_.load(std::memory_order_acquire) || !rtp_ep_) {
            return SendResult::closed;
        }
        const std::uint32_t ssrc = ssrc_.load(std::memory_order_acquire);
        // ★ 整帧只取一次时间戳：所有分片共用（RFC 3550 / RFC 6184）。
        const std::uint32_t ts = ts_override.has_value() ? *ts_override : next_timestamp();
        if (ts_override.has_value()) {
            advance_ts_from(ts);   // 与 send_with_timestamp 保持同一游标语义
        }
        const netmech::Address dst = detail::to_address(*cfg_.remote);

        bool        failed = false;
        std::size_t octets = 0;

        packetizer.on_frame(frame, [&](span<const std::uint8_t> payload, bool marker) {
            if (failed || payload.empty()) {
                return;
            }
            if (payload.size() + kRtpFixedHeaderSize > cfg_.max_packet_size) {
                failed = true;   // 分片器与 session 的 MTU 配置不一致
                return;
            }
            RtpPacketInfo info;
            info.payload_type = cfg_.payload_type;
            info.marker       = marker;
            info.sequence     = seq_.fetch_add(1, std::memory_order_relaxed);
            info.ssrc         = ssrc;
            info.timestamp    = ts;

            auto wire = std::make_shared<std::vector<std::uint8_t>>(
                kRtpFixedHeaderSize + payload.size());
            std::error_code bec;
            const std::size_t written =
                build_rtp_packet(wire->data(), wire->size(), info, payload, bec);
            if (bec || written == 0 ||
                !rtp_ep_->send_to(dst, netmech::ConstBuf{wire, wire->data(), written})) {
                failed = true;
                return;
            }
            note_sent_seq(info.sequence);
            rtp_sent_.fetch_add(1, std::memory_order_relaxed);
            octets += payload.size();
        });

        octet_sent_.fetch_add(octets, std::memory_order_relaxed);
        if (failed) {
            send_rejected_.fetch_add(1, std::memory_order_relaxed);
            return SendResult::closed;
        }
        return SendResult::accepted;
    }

    // -------------------------------------------------------------------------
    //  RTCP
    // -------------------------------------------------------------------------

    /// RTCP 发送失败：计一次失败并**推进调度**。
    ///
    /// 不推进的话 `next_send_sec_` 停在过去，`schedule_rtcp()` 会按 1ms 重排，
    /// 变成每秒上千次无效唤醒 —— 无 `remote` / `max_packet_size` 配小时必现。
    bool rtcp_failed() noexcept
    {
        rtcp_send_failures_.fetch_add(1, std::memory_order_relaxed);
        scheduler_.on_rtcp_sent(0, steady_now_sec());
        return false;
    }

    bool send_rtcp(bool force)
    {
        if (!cfg_.enable_rtcp || !open_.load(std::memory_order_acquire)) {
            return false;
        }
        if (!force && !scheduler_.due(steady_now_sec())) {
            return true;   // 未到期
        }

        // 顺手清理失活源（RFC 3550 §6.3.5：5 个报告间隔无活动即可超时）。
        // 以前这一步从没被调用过 —— 长期会话里 map 只增不减，报告块也会被
        // 僵尸源占满（31 个的上限一到，活着的源反而被挤出去）。
        (void)tracker_.expire_inactive(steady_now_us(), kSourceTimeoutUs);

        // 参与者数喂给调度器：它据此算共享带宽份额与自适应间隔。
        // 以前写死 (1, 1) —— 调度算法本身是对的（rtcp_scheduler.cpp），
        // 但没有输入，退化成固定间隔。members 含本端自己。
        scheduler_.update_members(tracker_.size() + 1,
                                  tracker_.sender_count() + 1);
        sync_tracker_stats();

        const auto blocks = tracker_.build_report_blocks();

        std::vector<std::uint8_t> buf(cfg_.max_packet_size, 0);
        std::size_t off = 0;
        std::error_code ec;

        const std::uint32_t local_ssrc = ssrc_.load(std::memory_order_acquire);

        // ★ RFC 3550 §6.4：自上次报告以来**没发过数据**的一方应当发 RR。
        //   以前无条件发 SR —— 只收不发的会话（典型：监控端 / 纯播放端）会把
        //   自己伪装成发送者，污染对端的 sender_count，进而算错 25%/75%
        //   的 RTCP 带宽配额。build_rtcp_receiver_report 早就写好了，只是没人用。
        const bool is_sender = rtp_sent_.load(std::memory_order_relaxed) > 0;

        NtpTime local_sr_ntp{};
        if (is_sender) {
            SenderReport sr;
            sr.ssrc = local_ssrc;
            {
                const auto now_wall = std::chrono::system_clock::now();
                const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                      now_wall.time_since_epoch()).count();
                const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       now_wall.time_since_epoch()).count() % 1000000000LL;
                local_sr_ntp     = make_ntp_time(secs, static_cast<std::uint32_t>(nanos));
                sr.ntp_msw       = local_sr_ntp.seconds;
                sr.ntp_lsw       = local_sr_ntp.fraction;
                sr.rtp_timestamp = current_timestamp();
                sr.packet_count  = static_cast<std::uint32_t>(rtp_sent_.load());
                sr.octet_count   = static_cast<std::uint32_t>(octet_sent_.load());
            }
            sr.reports = blocks;

            const std::size_t n = build_rtcp_sender_report(buf.data(), buf.size(), sr, ec);
            if (ec) {
                return rtcp_failed();
            }
            off += n;
        } else {
            ReceiverReport rr;
            rr.ssrc    = local_ssrc;
            rr.reports = blocks;

            const std::size_t n =
                build_rtcp_receiver_report(buf.data(), buf.size(), rr, ec);
            if (ec) {
                return rtcp_failed();
            }
            off += n;
        }

        // 复合包里附带 SDES(CNAME)：RFC 3550 要求 CNAME 稳定且可读。
        // cname_ 在构造时定好，**不随 SSRC 变化**。
        SdesReport sdes;
        SdesChunk chunk;
        chunk.ssrc  = local_ssrc;
        chunk.cname = cname_;
        sdes.chunks.push_back(chunk);

        const std::size_t n_sdes =
            build_rtcp_sdes(buf.data() + off, buf.size() - off, sdes, ec);
        if (ec) {
            return rtcp_failed();
        }
        off += n_sdes;

        if (!send_on_rtcp(buf, off)) {
            return rtcp_failed();
        }

        // ★ 记下"我刚发过 SR"，供对端 RR 里的 lsr 匹配后算 RTT。
        //   以前 on_local_sr_sent 有实现、**没有调用点** —— 这是 RTT 永远
        //   nullopt 的最后一块拼图。
        //
        //   传的是 **LSR 形式**（NTP 中间 32 位），不是高 32 位：对端在 RR 里
        //   回填的 rb.lsr 就是中间 32 位（§6.4.1），两边必须同构，否则永远匹配不上。
        if (is_sender) {
            const std::uint32_t sr_lsr =
                (local_sr_ntp.seconds << 16) | (local_sr_ntp.fraction >> 16);
            tracker_.on_local_sr_sent(sr_lsr, steady_now_us());
            sync_tracker_stats();
        }

        // 告诉调度器"发出去了、多大"，它据此更新平均包大小并排下一拍
        scheduler_.on_rtcp_sent(off, steady_now_sec());
        return true;
    }

    /// 把 tracker 里"对外可见"的统计刷到原子镜像（**只在 io 线程调**）。
    ///
    /// `tracker_` 是 std::map，按约定只在 io 线程上碰；而 `stats()` 是公开
    /// 方法、典型用法就是在另一个线程打日志/采集 —— 直接 `tracker_.size()`
    /// 是与收包路径（插树）并发的无同步读，属于 UB。所以每次 tracker 变化后
    /// 把这两个数复制到原子变量，`stats()` 只读原子。
    ///
    /// 快照语义：两个字段各自独立更新，不保证属于同一瞬间（统计用途足够）。
    void sync_tracker_stats() noexcept
    {
        active_sources_.store(tracker_.size(), std::memory_order_relaxed);
        if (const auto rtt = tracker_.rtt_us()) {
            rtt_us_.store(*rtt, std::memory_order_relaxed);
            rtt_valid_.store(true, std::memory_order_relaxed);
        }
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

    /// 对外入口：幂等、可任意线程调。
    ///
    /// `open_` 在这里**同步**置 false（is_open()/send() 立刻反映已关），
    /// 收尾工作一律用 `asio::post` 投递到 io 线程 ——
    /// **不能用 dispatch**：从 io 线程内部调用时 dispatch 会就地执行、插到
    /// 已排队的 send 前面，把还没发出去的包连带端点一起丢掉。
    static void request_close(std::shared_ptr<Impl> self) noexcept
    {
        if (!self) {
            return;
        }
        if (self->closed_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        self->open_.store(false, std::memory_order_release);

        auto fn = [self] { self->do_close(); };
        if (self->post_) {
            // ★ 一律排队（不内联）：close 必须排在已投递的发送之后，
            //   否则从 io 线程内部调用时会把还没发出去的包连端点一起丢掉。
            self->post_(std::move(fn), /*allow_inline=*/false);
        } else {
            fn();
        }
    }

    /// 收尾（**只在 io 线程上执行**）。
    void do_close() noexcept
    {
        send_bye({});            // 尽力发，不关心结果

        if (rtcp_ep_) {
            rtcp_ep_->clear_timer();
            rtcp_ep_->close();
            rtcp_ep_.reset();
        }
        if (rtp_ep_) {
            rtp_ep_->close();
            rtp_ep_.reset();
        }

        if (handler_) {
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
        st.rtcp_send_failures    = rtcp_send_failures_.load(std::memory_order_relaxed);
        st.self_loopback_ignored = self_loopback_ignored_.load(std::memory_order_relaxed);
        st.ssrc_collisions       = ssrc_collisions_.load(std::memory_order_relaxed);
        st.rtcp_without_cname    = rtcp_without_cname_.load(std::memory_order_relaxed);
        // ★ 只读原子镜像，不碰 tracker_（std::map，只在 io 线程上访问）。
        //   以前这里直接 `tracker_.size()` / `rtt_us()` —— 与收包路径插树并发，
        //   是无同步读 map，属于 UB。测试没发现是因为都在流量停止后才查。
        st.active_sources = active_sources_.load(std::memory_order_relaxed);
        if (rtt_valid_.load(std::memory_order_relaxed)) {
            st.rtt_us = rtt_us_.load(std::memory_order_relaxed);
        }
        return st;
    }

    const RtpSessionConfig& config() const noexcept { return cfg_; }

private:
    // -------------------------------------------------------------------------
    //  端口
    // -------------------------------------------------------------------------

    /// 建立 RTP 端点。端口 0 时交给系统分配，但要保证偶数 —— 奇数就丢弃重来。
    /// 进程级端口分配器（按区间共享，游标跨会话保持）。
    ///
    /// 以前的做法是"每个会话都从 `local.port()` 重新扫一遍"：区间里已有 N 路
    /// 在用，第 N+1 路就要空转 N 次探测；而且刚释放的端口会被下一路立刻抢回
    /// （对端可能还留着旧流的状态）。改用带游标的分配器：从上次的位置继续，
    /// 走到区间尾回绕。
    static std::shared_ptr<netmech::PortAllocator> allocator_for(std::uint16_t lo,
                                                                std::uint16_t hi)
    {
        static std::mutex mu;
        static std::map<std::pair<std::uint16_t, std::uint16_t>,
                        std::shared_ptr<netmech::PortAllocator>> registry;

        const std::pair<std::uint16_t, std::uint16_t> key{lo, hi};
        std::lock_guard<std::mutex> lk(mu);
        auto it = registry.find(key);
        if (it != registry.end()) {
            return it->second;
        }
        auto alloc = std::make_shared<netmech::PortAllocator>(lo, hi);
        registry.emplace(key, alloc);
        return alloc;
    }

    bool open_rtp_endpoint(std::error_code& ec)
    {
        const std::uint16_t want = cfg_.local.port();

        if (want != 0 && cfg_.rtp_port_end > want) {
            // 给了区间：用分配器挑一对（游标续着走，不每次从头扫）
            auto alloc = allocator_for(static_cast<std::uint16_t>(round_up_even(want)),
                                       cfg_.rtp_port_end);
            // 探测与真正 bind 之间有极小窗口，撞上了就再要一对（最多 8 次）。
            //
            // 这里**不调 `alloc->release()`**：`PortAllocator` 刻意不维护引用计数
            // （见其 `release()` 的实现——只做参数校验，什么都不改），"端口可用"
            // 完全靠实时探测。而且 acquire 成功会把游标推到本组之后，所以下一轮
            // 拿的是**下一对**，不会反复撞同一对。报告里"没 release 会蚕食区间"
            // 的说法基于"release 会记账"的假设，实际不成立。
            for (int attempt = 0; attempt < 8; ++attempt) {
                const auto first = alloc->acquire(/*count=*/2, /*alignment=*/2);
                if (!first.has_value()) {
                    break;
                }
                if (try_bind_rtp(*first, ec)) {
                    goto bound;
                }
            }
            if (!ec) {
                ec = make_error_code(RtpErrc::invalid_argument);   // 区间里没有可用配对了
            }
            return false;
        }

        if (want != 0) {
            // 显式端口：向上取偶，绑不上就是失败（协商场景不重试）
            if (!try_bind_rtp(round_up_even(want), ec)) {
                return false;
            }
        } else {
            // 端口 0：系统分配 + 偶数校验
            bool bound = false;
            for (int i = 0; i < kMaxAutoPortAttempts; ++i) {
                if (!try_bind_rtp(0, ec)) {
                    return false;
                }
                if ((rtp_port_ & 1u) == 0) {
                    bound = true;
                    break;        // 偶数，可用
                }
                // 系统给了奇数：丢弃重来
                rtp_ep_->close();
                rtp_ep_.reset();
                rtp_port_ = 0;
            }
            if (!bound) {
                ec = make_error_code(RtpErrc::invalid_argument);
                return false;
            }
        }

    bound:
        // ★ 必须等端口**定下来**再设 post_：这段以前放进了 try_bind_rtp，
        //   而那是"端口搜索循环"里的函数 —— 每次尝试都会重设，且上一轮
        //   丢弃的端点会触发 on_close，把刚建好的会话误判成"端点死了"。
        //
        //   之后所有公开 API 都通过它投递到本端点的 io 线程：
        //   发送允许内联（已经在线程上就直接干），关闭一律排队（FIFO）。
        post_ = [ep = rtp_ep_](std::function<void()> fn, bool allow_inline) {
            ep->post(std::move(fn), allow_inline);
        };
        return true;
    }

    bool try_bind_rtp(std::uint16_t port, std::error_code& ec)
    {
        netmech::UdpConfig cfg;
        cfg.bind                = netmech::Address(cfg_.local.host(), port);
        cfg.recv_capacity       = std::max<std::size_t>(cfg_.recv_capacity, 64);
        if (cfg_.multicast_group) {
            cfg.multicast_group = detail::to_address(*cfg_.multicast_group);
        }
        if (cfg_.multicast_interface) {
            cfg.multicast_interface = detail::to_address(*cfg_.multicast_interface);
        }
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
        const netmech::Address dst(cfg_.remote->host(),
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
        // 同一来源**针对同一个 SSRC** 重复出现才算"已处理过"（自己的包回环）。
        //
        //   键里带上当前 SSRC：以前只用地址做键，于是"从同一台设备发起的第二次
        //   真实冲突"会被永久忽略 —— 对方换了号、我们这边也换过号之后再撞，
        //   本该再处理一次。带 ssrc 之后，换了号就是一个新事件。
        const std::uint32_t cur_ssrc = ssrc_.load(std::memory_order_acquire);
        const std::string key =
            from.to_string() + "#" + std::to_string(cur_ssrc);
        if (last_collision_from_ == key) {
            return;
        }
        last_collision_from_ = key;

        // 已经用这个 SSRC 发过数据 → 先发 BYE 告知对端
        if (rtp_sent_.load(std::memory_order_relaxed) > 0) {
            send_bye({});
        }
        const std::uint32_t new_ssrc = gen_random32();
        ssrc_.store(new_ssrc, std::memory_order_release);
        ssrc_collisions_.fetch_add(1, std::memory_order_relaxed);
        // ★ tracker 里的 local_ssrc 也要跟着换：它用这个值把"自己发的包"和
        //   "报告我的 RR 块"挑出来。不同步的话，换号之后 RTT 匹配就断了，
        //   而且对端报告块里针对旧号的条目会被当成别的源。
        tracker_.set_local_ssrc(new_ssrc);
        // 库内不再打印日志：换号已通过 stats().ssrc_collisions 暴露，
        // 需要细节的调用方可以在自己的日志里对比 local_ssrc() 变化。
    }

    // ---- 最近发出的 RTP 序号（识别"自己发包的回环"）----
    // 只在一个 io 线程上访问：发送与收包回调同在 RTP 端点的 io 线程。
    static constexpr std::size_t kRecentSeqRing = 128;
    std::array<std::uint16_t, kRecentSeqRing> recent_seq_{};
    std::size_t recent_seq_pos_   = 0;
    std::size_t recent_seq_count_ = 0;

    void note_sent_seq(std::uint16_t s) noexcept
    {
        recent_seq_[recent_seq_pos_] = s;
        recent_seq_pos_ = (recent_seq_pos_ + 1) % kRecentSeqRing;
        if (recent_seq_count_ < kRecentSeqRing) {
            ++recent_seq_count_;
        }
    }

    [[nodiscard]] bool is_self_loopback_seq(std::uint16_t seq) const noexcept
    {
        for (std::size_t i = 0; i < recent_seq_count_; ++i) {
            if (recent_seq_[i] == seq) {
                return true;
            }
        }
        return false;
    }

    /// 显式指定时间戳发送后，把内部游标推进到"这一帧之后"。
    /// 否则下一次不传时间戳的 send() 会复用很久以前的 ts_current_。
    void advance_ts_from(std::uint32_t ts) noexcept
    {
        if (ts_increment_ != 0) {
            ts_current_ = ts + ts_increment_;
        }
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

    // ★ 线程模型（唯一的规矩，别再加第二条）：
    //
    //   可变状态（两个端点、source_tracker、调度器、RTP 序号/时间戳）**只在
    //   RTP 端点所属的 io 线程上碰**。所有公开 API（send* / send_rtcp_now /
    //   send_bye / close）都先把活儿 post_ 到那个线程上，于是全程无锁。
    //
    //   因此下面这些成员"看起来是裸的"，但访问它们的线程只有一个：
    //     · send_bye() 由 io 线程调（门面 post 过来 / 冲突处理内部调）；
    //     · send_rtcp() 由定时器回调调（同一个 io 线程）；
    //     · do_close() 也是 post 过来的。
    //   唯一从别的线程读的是 `stats()` —— 它只读 atomic 镜像
    //   （active_sources_ / rtt_us_），不碰 tracker_。
    //
    //   ⚠ 加新的公开方法时，记得先 post_；直接碰上面这些成员就是数据竞争。
    std::function<void(std::function<void()>, bool)> post_;
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
    std::atomic<std::uint64_t> rtcp_send_failures_{0};
    std::atomic<std::uint64_t> self_loopback_ignored_{0};
    std::atomic<std::uint64_t> ssrc_collisions_{0};
    std::atomic<std::uint64_t> rtcp_without_cname_{0};

    // ---- 对外统计的原子镜像（见 sync_tracker_stats） ----
    //   tracker_ 只能在 io 线程碰；stats() 从任意线程调，所以这两个数单独存。
    std::atomic<std::size_t>   active_sources_{0};
    std::atomic<std::uint32_t> rtt_us_{0};
    std::atomic<bool>          rtt_valid_{false};

    /// SDES CNAME：构造时定好，**不随 SSRC 变化**（RFC 3550 §6.5.1）。
    std::string cname_;

    std::string last_collision_from_;
};

// =============================================================================
//  RtpSession 门面
// =============================================================================

RtpSession::RtpSession() = default;

RtpSession::~RtpSession()
{
    if (impl_) {
        Impl::request_close(impl_);
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
    // SDES item 的 len 是单字节：> 255 无法表达。在这里拦住，比在 send_rtcp 里
    // 每拍都失败一次好 —— 那种情况下 RTCP 会静默停发，日志里什么都看不到。
    if (cfg.cname.size() > 255) {
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

SendResult RtpSession::send(ConstBuf payload, bool marker)
{
    if (!impl_) {
        return SendResult::closed;
    }
    return impl_->post_send(payload, marker, std::nullopt, std::nullopt);
}

SendResult RtpSession::send_with_timestamp(ConstBuf payload, bool marker,
                                           std::uint32_t timestamp)
{
    if (!impl_) {
        return SendResult::closed;
    }
    return impl_->post_send(payload, marker, timestamp, std::nullopt);
}

SendResult RtpSession::send_to(const Endpoint& dst, ConstBuf payload, bool marker)
{
    if (!impl_) {
        return SendResult::closed;
    }
    return impl_->post_send(payload, marker, std::nullopt, dst);
}

SendResult RtpSession::send_frame(span<const std::uint8_t> frame,
                                  Packetizer& packetizer)
{
    if (!impl_) {
        return SendResult::closed;
    }
    if (frame.empty()) {
        return SendResult::invalid;
    }
    auto data = std::make_shared<std::vector<std::uint8_t>>(frame.begin(), frame.end());
    return impl_->post_send_frame(std::move(data), &packetizer, std::nullopt);
}

SendResult RtpSession::send_frame_with_timestamp(span<const std::uint8_t> frame,
                                                 Packetizer& packetizer,
                                                 std::uint32_t timestamp)
{
    if (!impl_) {
        return SendResult::closed;
    }
    if (frame.empty()) {
        return SendResult::invalid;
    }
    auto data = std::make_shared<std::vector<std::uint8_t>>(frame.begin(), frame.end());
    return impl_->post_send_frame(std::move(data), &packetizer, timestamp);
}

bool RtpSession::send_rtcp_now()
{
    if (!impl_ || !impl_->is_open()) {
        return false;
    }
    return impl_->post_rtcp_now();
}

void RtpSession::send_bye(const std::string& reason, bool close_after)
{
    if (!impl_) {
        return;
    }
    if (impl_->is_open()) {
        // ★ 投递到 io 线程 —— 与 send / send_rtcp_now 同构。
        //
        //   以前这里就地 `impl_->send_bye(reason)`：它读 rtcp_ep_、发 UDP、
        //   动 ssrc_，而 do_close() 在 io 线程上 `rtcp_ep_.reset()` ——
        //   两个线程并发 copy/reset 同一个 shared_ptr，也是"检查后再用"的
        //   标准竞态（头文件写着 send_bye 可从任意线程调）。
        impl_->post_bye(reason, close_after);
        return;
    }
    if (close_after) {
        Impl::request_close(impl_);
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
        Impl::request_close(impl_);
    }
}

} // namespace rtpmech
