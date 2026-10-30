// ============================================================================
//  test_session.cpp —— RtpSession 契约测试
//
//  按新接口（netmech 0.9 + UdpEndpoint）重写。测的是**对外契约**：
//    * 端口成对（RTCP = RTP + 1）、RTP 偶数
//    * 收发往返、序列号递增、时间戳按增量
//    * 关闭幂等、关闭后各接口的行为
//    * RTCP 自动发送
//    * MTU 拒绝、无 peer 拒绝
//    * 活跃时析构不崩
// ============================================================================
#include <rtpmech/rtpmech.hpp>

#include <netmech/net.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "test_framework.hpp"

using namespace rtpmech;
using namespace netmech;
using namespace netmech_test;

// CHECK_EQ 失败时要打印两侧的值，给 SendResult 补一个输出算子
// （放在测试里而不是公共头，避免 session.hpp 引 <ostream>）
namespace rtpmech {
inline std::ostream& operator<<(std::ostream& os, SendResult r)
{
    switch (r) {
    case SendResult::accepted:     return os << "accepted";
    case SendResult::closed:       return os << "closed";
    case SendResult::invalid:      return os << "invalid";
    }
    return os << "SendResult(" << static_cast<int>(r) << ")";
}
} // namespace rtpmech

namespace {

using namespace std::chrono_literals;

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout = 3000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

/// 收集 RTP 包，记录序列号 / 时间戳 / 载荷长度。
class Collector : public RtpHandler {
public:
    void on_packet(RtpSession&, const RtpPacket& pkt, const Address&) override
    {
        std::lock_guard<std::mutex> lk(mtx);
        seqs.push_back(pkt.sequence());
        timestamps.push_back(pkt.timestamp());
        sizes.push_back(pkt.payload().size());
        count.fetch_add(1);
    }
    void on_close(std::error_code) override { closed.fetch_add(1); }

    std::mutex                 mtx;
    std::vector<std::uint16_t> seqs;
    std::vector<std::uint32_t> timestamps;
    std::vector<std::size_t>   sizes;
    std::atomic<int>           count{0};
    std::atomic<int>           closed{0};
};

/// 只发不收的 handler。
class NullHandler : public RtpHandler {
public:
    void on_packet(RtpSession&, const RtpPacket&, const Address&) override {}
};

RtpSessionConfig base_cfg()
{
    RtpSessionConfig cfg;
    cfg.local        = Address("127.0.0.1", 0);
    cfg.payload_type = 96;
    cfg.clock_rate   = 90000;
    return cfg;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. 端口成对：RTCP = RTP + 1，且 RTP 为偶数（RFC 3550 §11）
// ---------------------------------------------------------------------------
NETMECH_TEST(session_creates_paired_ports)
{
    std::error_code ec;
    auto sess = RtpSession::create(base_cfg(), std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);
    REQUIRE(sess != nullptr);

    const auto rtp  = sess->local_rtp_port();
    const auto rtcp = sess->local_rtcp_port();

    CHECK(rtp != 0);

    CHECK_EQ(rtp % 2u, 0u);                                  // 偶数
    CHECK_EQ(rtcp, static_cast<std::uint16_t>(rtp + 1));     // 恒为 +1
    CHECK(sess->is_open());
    CHECK(sess->local_ssrc() != 0);

    sess->close();
}

// ---------------------------------------------------------------------------
// 2. clock_rate 为 0 会被拒绝
// ---------------------------------------------------------------------------
NETMECH_TEST(session_rejects_zero_clock_rate)
{
    auto cfg = base_cfg();
    cfg.clock_rate = 0;

    std::error_code ec;
    auto sess = RtpSession::create(cfg, std::make_shared<NullHandler>(), ec);
    CHECK(sess == nullptr);
    CHECK(ec == RtpErrc::invalid_argument);
}

// ---------------------------------------------------------------------------
// 3. 收发往返：字段与载荷都正确
// ---------------------------------------------------------------------------
NETMECH_TEST(session_send_receive_roundtrip)
{
    std::error_code ec;

    auto collector = std::make_shared<Collector>();
    auto rx = RtpSession::create(base_cfg(), collector, ec);
    REQUIRE(!ec);

    auto tx_cfg = base_cfg();
    tx_cfg.remote = Address("127.0.0.1", rx->local_rtp_port());
    auto tx = RtpSession::create(tx_cfg, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);

    const std::uint8_t payload[160] = {0xA5};
    CHECK_EQ(tx->send({nullptr, payload, sizeof(payload)}, /*marker=*/true),
             SendResult::accepted);

    REQUIRE(wait_for([&] { return collector->count.load() == 1; }));
    {
        std::lock_guard<std::mutex> lk(collector->mtx);
        REQUIRE(collector->sizes.size() == 1u);
        CHECK_EQ(collector->sizes[0], sizeof(payload));
    }

    tx->close();
    rx->close();
}

// ---------------------------------------------------------------------------
// 4. 序列号连续 + 时间戳按固定增量
// ---------------------------------------------------------------------------
NETMECH_TEST(session_sequence_and_timestamp_increment)
{
    std::error_code ec;

    auto cfg = base_cfg();
    cfg.timestamp_increment = 3600;      // 25 fps @ 90 kHz

    auto collector = std::make_shared<Collector>();
    auto rx = RtpSession::create(cfg, collector, ec);
    REQUIRE(!ec);

    cfg.remote = Address("127.0.0.1", rx->local_rtp_port());
    auto tx = RtpSession::create(cfg, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);

    const std::uint8_t payload[8] = {};
    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(tx->send({nullptr, payload, sizeof(payload)}, false),
                 SendResult::accepted);
    }

    REQUIRE(wait_for([&] { return collector->count.load() >= 5; }));
    {
        std::lock_guard<std::mutex> lk(collector->mtx);
        REQUIRE(collector->seqs.size() >= 5u);
        for (std::size_t i = 1; i < 5; ++i) {
            CHECK_EQ(collector->seqs[i],
                     static_cast<std::uint16_t>(collector->seqs[i - 1] + 1));
        }
        CHECK_EQ(collector->timestamps[1] - collector->timestamps[0], 3600u);
    }

    tx->close();
    rx->close();
}

// ---------------------------------------------------------------------------
// 5. 关闭幂等；关闭后查询仍返回关闭前的值
// ---------------------------------------------------------------------------
NETMECH_TEST(session_close_is_idempotent)
{
    std::error_code ec;
    auto handler = std::make_shared<Collector>();
    auto sess = RtpSession::create(base_cfg(), handler, ec);
    REQUIRE(!ec);

    const auto rtp_port = sess->local_rtp_port();
    const auto ssrc     = sess->local_ssrc();

    sess->close();
    sess->close();       // 幂等
    sess->close();

    CHECK(!sess->is_open());
    CHECK_EQ(handler->closed.load(), 1);     // on_close 恰好一次

    // 业务侧常在关闭后打日志 / 上报统计，这些调用不能崩、不能返回垃圾
    CHECK_EQ(sess->local_rtp_port(), rtp_port);
    CHECK_EQ(sess->local_ssrc(), ssrc);
    CHECK_EQ(sess->stats().rtp_sent, 0u);
    (void)sess->config();
}

// ---------------------------------------------------------------------------
// 6. 关闭后 send 返回 closed
// ---------------------------------------------------------------------------
NETMECH_TEST(session_send_after_close_returns_closed)
{
    std::error_code ec;
    auto sess = RtpSession::create(base_cfg(), std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);
    sess->close();

    const std::uint8_t payload[4] = {};
    CHECK_EQ(sess->send({nullptr, payload, sizeof(payload)}, false), SendResult::closed);
    CHECK_EQ(sess->send_to(Address("127.0.0.1", 9999),
                           {nullptr, payload, sizeof(payload)}, false),
             SendResult::closed);
    CHECK(!sess->send_rtcp_now());
}

// ---------------------------------------------------------------------------
// 7. 没有 remote 也没有 dst → invalid
// ---------------------------------------------------------------------------
NETMECH_TEST(session_send_without_peer_is_invalid)
{
    std::error_code ec;
    auto sess = RtpSession::create(base_cfg(), std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);
    REQUIRE(!sess->config().remote.has_value());

    const std::uint8_t payload[4] = {};
    CHECK_EQ(sess->send({nullptr, payload, sizeof(payload)}, false), SendResult::invalid);

    sess->close();
}

// ---------------------------------------------------------------------------
// 8. 超过 MTU → invalid，且一个字节都不发
// ---------------------------------------------------------------------------
NETMECH_TEST(session_rejects_oversized_payload)
{
    std::error_code ec;

    auto cfg = base_cfg();
    cfg.max_packet_size = 200;

    auto collector = std::make_shared<Collector>();
    auto rx = RtpSession::create(cfg, collector, ec);
    REQUIRE(!ec);

    cfg.remote = Address("127.0.0.1", rx->local_rtp_port());
    auto tx = RtpSession::create(cfg, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);

    std::vector<std::uint8_t> big(400, 0);
    CHECK_EQ(tx->send({nullptr, big.data(), big.size()}, false), SendResult::invalid);

    std::this_thread::sleep_for(120ms);
    CHECK_EQ(collector->count.load(), 0);

    tx->close();
    rx->close();
}

// ---------------------------------------------------------------------------
// 9. RTCP 能发出去（强制发一次验证通路；自动调度由间隔决定，不在此断言）
// ---------------------------------------------------------------------------
NETMECH_TEST(session_sends_rtcp)
{
    std::error_code ec;

    auto cfg = base_cfg();
    cfg.enable_rtcp = true;
    cfg.session_bandwidth = 1000.0;

    auto collector = std::make_shared<Collector>();
    auto rx = RtpSession::create(cfg, collector, ec);
    REQUIRE(!ec);

    cfg.remote = Address("127.0.0.1", rx->local_rtp_port());
    auto tx = RtpSession::create(cfg, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);

    CHECK(tx->send_rtcp_now());
    CHECK(wait_for([&] { return tx->stats().rtcp_sent >= 1; }));

    tx->close();
    rx->close();
}

// ---------------------------------------------------------------------------
// 9b. RTCP **自动**发送：定时器按间隔触发，对端确实收到
//
//     这里把 rtcp_min_interval_sec 调到 0.2 秒（默认 5 秒）——
//     只为把"定时器在按间隔跑"这件事验证出来，正常场景不要这么设。
// ---------------------------------------------------------------------------
NETMECH_TEST(session_rtcp_auto_sends_periodically)
{
    std::error_code ec;

    auto cfg = base_cfg();
    cfg.enable_rtcp            = true;
    cfg.session_bandwidth      = 100000.0;   // 带宽足够 → 间隔由下限决定
    cfg.rtcp_min_interval_sec  = 0.2;        // 测试专用

    auto collector = std::make_shared<Collector>();
    auto rx = RtpSession::create(cfg, collector, ec);
    REQUIRE(!ec);

    cfg.remote = Address("127.0.0.1", rx->local_rtp_port());
    auto tx = RtpSession::create(cfg, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);

    // 等 1.5 秒：0.2 秒间隔 × 0.5~1.5 随机因子 → 至少能发 3 次
    std::this_thread::sleep_for(1500ms);

    const auto tx_st = tx->stats();
    const auto rx_st = rx->stats();

    CHECK_MSG(tx_st.rtcp_sent >= 2,
              ("自动发送次数不足: " + std::to_string(tx_st.rtcp_sent)).c_str());
    CHECK_MSG(rx_st.rtcp_received >= 2,
              ("对端收到的 RTCP 不足: " + std::to_string(rx_st.rtcp_received)).c_str());

    tx->close();
    rx->close();
}

// ---------------------------------------------------------------------------
// 9c. SSRC 冲突：收到同 SSRC 但不同来源的包 → 发 BYE 并换号（RFC 3550 §8.2）
//
//     做法：让发送端**故意用与接收端相同的 SSRC**。接收端收到后应判定冲突，
//     换一个新的 SSRC，并且这个"别人的包"不能被交付给 on_packet。
// ---------------------------------------------------------------------------
NETMECH_TEST(session_ssrc_collision_changes_ssrc)
{
    std::error_code ec;

    auto cfg = base_cfg();
    cfg.local_ssrc = 0x11223344;              // 固定 SSRC

    auto collector = std::make_shared<Collector>();
    auto rx = RtpSession::create(cfg, collector, ec);
    REQUIRE(!ec);
    REQUIRE(rx->local_ssrc() == 0x11223344u);

    // 发送端故意用同一个 SSRC
    auto tx_cfg = cfg;
    tx_cfg.remote = Address("127.0.0.1", rx->local_rtp_port());
    auto tx = RtpSession::create(tx_cfg, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);
    REQUIRE(tx->local_ssrc() == 0x11223344u);

    const std::uint8_t payload[8] = {};
    CHECK_EQ(tx->send({nullptr, payload, sizeof(payload)}, false), SendResult::accepted);

    // 接收端应当发现冲突并换号
    CHECK_MSG(wait_for([&] { return rx->local_ssrc() != 0x11223344u; }, 2000ms),
              "收到同 SSRC 的包后没有换号");

    // 冲突包属于"另一个实体"，不应交付给应用
    std::this_thread::sleep_for(100ms);
    CHECK_EQ(collector->count.load(), 0);

    tx->close();
    rx->close();
}

// ---------------------------------------------------------------------------
// 9d. 端口占用：RTCP 端口（P+1）被占 → create 失败，且 RTP 端口已回滚
// ---------------------------------------------------------------------------
NETMECH_TEST(session_fails_when_rtcp_port_occupied)
{
    // 探一对可用端口
    auto base = find_free_port_run(40000, 50000, 2, 2);
    REQUIRE(base.has_value());

    // 占住 base+1（RTCP 位置）
    struct Null : UdpHandler {
        void on_read(UdpEndpoint&, std::error_code, Buf, std::size_t,
                     const Address&) override {}
    };
    UdpConfig blocker_cfg;
    blocker_cfg.bind = Address("127.0.0.1", static_cast<std::uint16_t>(*base + 1));
    std::error_code ec;
    auto blocker = create_udp_endpoint(blocker_cfg, std::make_shared<Null>(), nullptr, ec);
    REQUIRE(!ec);
    REQUIRE(blocker != nullptr);

    // 用 base 建会话：RTP 能绑，但 RTCP 绑不上 → 整体失败
    auto cfg = base_cfg();
    cfg.local = Address("127.0.0.1", *base);

    auto sess = RtpSession::create(cfg, std::make_shared<NullHandler>(), ec);
    CHECK(sess == nullptr);
    CHECK(static_cast<bool>(ec));

    // ★ 关键：RTP 端口必须已回滚，否则重试永远绑不上
    UdpConfig rtp_cfg;
    rtp_cfg.bind = Address("127.0.0.1", *base);
    auto after = create_udp_endpoint(rtp_cfg, std::make_shared<Null>(), nullptr, ec);
    CHECK_MSG(!ec, ("RTCP 失败后 RTP 端口没回滚: " + ec.message()).c_str());
    if (after) {
        after->close();
    }
    blocker->close();
}

// ---------------------------------------------------------------------------
// 9e. 协商流程：探出端口 → 按其绑定 → 实际端口与探测值一致
// ---------------------------------------------------------------------------
NETMECH_TEST(session_binds_exactly_probed_port)
{
    auto base = find_free_port_run(40000, 50000, 2, 2);
    REQUIRE(base.has_value());

    auto cfg = base_cfg();
    cfg.local        = Address("127.0.0.1", *base);
    cfg.rtp_port_end = 0;               // 协商已定，不搜别的

    std::error_code ec;
    auto sess = RtpSession::create(cfg, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);

    CHECK_EQ(sess->local_rtp_port(), *base);
    CHECK_EQ(sess->local_rtcp_port(), static_cast<std::uint16_t>(*base + 1));

    sess->close();
}

// ---------------------------------------------------------------------------
// 10. 统计初值与增长
// ---------------------------------------------------------------------------
NETMECH_TEST(session_stats_initial_and_grow)
{
    std::error_code ec;

    auto collector = std::make_shared<Collector>();
    auto rx = RtpSession::create(base_cfg(), collector, ec);
    REQUIRE(!ec);

    const auto st0 = rx->stats();
    CHECK_EQ(st0.rtp_received, 0u);
    CHECK_EQ(st0.parse_errors, 0u);

    auto tx_cfg = base_cfg();
    tx_cfg.remote = Address("127.0.0.1", rx->local_rtp_port());
    auto tx = RtpSession::create(tx_cfg, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);

    const std::uint8_t payload[16] = {};
    CHECK_EQ(tx->send({nullptr, payload, sizeof(payload)}, false), SendResult::accepted);
    CHECK(wait_for([&] { return rx->stats().rtp_received >= 1; }));

    CHECK(tx->stats().rtp_sent >= 1u);
    CHECK(rx->stats().active_sources >= 1u);   // 跟踪到了对端源

    tx->close();
    rx->close();
}

// ---------------------------------------------------------------------------
// 11. 活跃时析构：不崩，handler 收到 on_close
// ---------------------------------------------------------------------------
NETMECH_TEST(session_destroy_while_active)
{
    std::error_code ec;
    auto handler = std::make_shared<Collector>();
    {
        auto sess = RtpSession::create(base_cfg(), handler, ec);
        REQUIRE(!ec);
        REQUIRE(sess->is_open());
        // 故意不 close，直接离开作用域
    }
    CHECK(wait_for([&] { return handler->closed.load() == 1; }, 1000ms));
}
