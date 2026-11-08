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
#include <rtpmech/h264.hpp>
#include <netmech/runtime.hpp>

#include <netmech/net.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>

#include "test_framework.hpp"

using namespace rtpmech;
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
    void on_packet(RtpSession&, const RtpPacket& pkt, const Endpoint&) override
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
    void on_packet(RtpSession&, const RtpPacket&, const Endpoint&) override {}
};

RtpSessionConfig base_cfg()
{
    RtpSessionConfig cfg;
    cfg.local        = Endpoint("127.0.0.1", 0);
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
// 2b. CNAME 超过 255 字节：构造期就拒绝
//
//     SDES item 的长度字段是单字节。放过去的话，send_rtcp 每次都会在
//     build_rtcp_sdes 里失败并 return false —— RTCP 静默停发，应用侧只看到
//     "对端收不到报告"，日志里什么都没有。
// ---------------------------------------------------------------------------
NETMECH_TEST(session_rejects_overlong_cname)
{
    std::error_code ec;

    auto ok_cfg   = base_cfg();
    ok_cfg.cname  = std::string(255, 'c');
    auto ok_sess  = RtpSession::create(ok_cfg, std::make_shared<NullHandler>(), ec);
    CHECK_MSG(!ec, "255 字节 CNAME 应当允许");
    CHECK(ok_sess != nullptr);
    if (ok_sess) {
        ok_sess->close();
    }

    auto bad_cfg  = base_cfg();
    bad_cfg.cname = std::string(256, 'c');
    auto bad_sess = RtpSession::create(bad_cfg, std::make_shared<NullHandler>(), ec);
    CHECK(bad_sess == nullptr);
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
    tx_cfg.remote = Endpoint("127.0.0.1", rx->local_rtp_port());
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

    cfg.remote = Endpoint("127.0.0.1", rx->local_rtp_port());
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

    CHECK(!sess->is_open());                 // 这个是同步的：close() 立刻翻标志

    // on_close 是**投递到 io 线程**才回调的（close() 只标记 + 排队），
    // 所以这里要等一下 —— 但"恰好一次"仍然要成立（后两次 close 被幂等挡掉）。
    for (int i = 0; i < 200 && handler->closed.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));   // 给"多回调"留出观察窗口
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
    CHECK_EQ(sess->send_to(Endpoint("127.0.0.1", 9999),
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

    cfg.remote = Endpoint("127.0.0.1", rx->local_rtp_port());
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

    cfg.remote = Endpoint("127.0.0.1", rx->local_rtp_port());
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

    cfg.remote = Endpoint("127.0.0.1", rx->local_rtp_port());
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
    tx_cfg.remote = Endpoint("127.0.0.1", rx->local_rtp_port());
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
    auto base = netmech::find_free_port_run(40000, 50000, 2, 2);
    REQUIRE(base.has_value());

    // 占住 base+1（RTCP 位置）
    // 占端口用的是 netmech 的 UDP 端点（测试自己造一个"被占用"的环境）
    struct Null : netmech::UdpHandler {
        void on_read(netmech::UdpEndpoint&, std::error_code, netmech::Buf, std::size_t,
                     const netmech::Address&) override {}
    };
    netmech::UdpConfig blocker_cfg;
    blocker_cfg.bind = netmech::Address("127.0.0.1", static_cast<std::uint16_t>(*base + 1));
    std::error_code ec;
    auto blocker = create_udp_endpoint(blocker_cfg, std::make_shared<Null>(), nullptr, ec);
    REQUIRE(!ec);
    REQUIRE(blocker != nullptr);

    // 用 base 建会话：RTP 能绑，但 RTCP 绑不上 → 整体失败
    auto cfg = base_cfg();
    cfg.local = Endpoint("127.0.0.1", *base);

    auto sess = RtpSession::create(cfg, std::make_shared<NullHandler>(), ec);
    CHECK(sess == nullptr);
    CHECK(static_cast<bool>(ec));

    // ★ 关键：RTP 端口必须已回滚，否则重试永远绑不上
    netmech::UdpConfig rtp_cfg;
    rtp_cfg.bind = netmech::Address("127.0.0.1", *base);
    auto after = netmech::create_udp_endpoint(rtp_cfg, std::make_shared<Null>(), nullptr, ec);
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
    auto base = netmech::find_free_port_run(40000, 50000, 2, 2);
    REQUIRE(base.has_value());

    auto cfg = base_cfg();
    cfg.local        = Endpoint("127.0.0.1", *base);
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
    tx_cfg.remote = Endpoint("127.0.0.1", rx->local_rtp_port());
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

// ---------------------------------------------------------------------------
// 端口区间：同一个区间里连续建会话，应当拿到**不同的**端口对
// （分配器带游标，不会每次都从区间起点重扫）
// ---------------------------------------------------------------------------
NETMECH_TEST(session_port_range_allocates_distinct_pairs)
{
    const auto base = netmech::find_free_port_run(41000, 42000, 4, 2);
    REQUIRE(base.has_value());

    auto cfg = base_cfg();
    cfg.local        = Endpoint("127.0.0.1", *base);
    cfg.rtp_port_end = static_cast<std::uint16_t>(*base + 3);

    std::error_code ec;
    auto a = RtpSession::create(cfg, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);
    REQUIRE(a != nullptr);

    auto b = RtpSession::create(cfg, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);
    REQUIRE(b != nullptr);

    CHECK(a->local_rtp_port() != b->local_rtp_port());
    CHECK_EQ((a->local_rtp_port() % 2u), 0u);
    CHECK_EQ((b->local_rtp_port() % 2u), 0u);
    // 两对都要落在区间内
    CHECK(a->local_rtp_port() >= *base);
    CHECK(b->local_rtp_port() >= *base);
    CHECK(a->local_rtp_port() <= static_cast<std::uint16_t>(*base + 3));
    CHECK(b->local_rtp_port() <= static_cast<std::uint16_t>(*base + 3));

    a->close();
    b->close();
}

// ---------------------------------------------------------------------------
// 10. RTT 闭环：一方发数据（SR）、另一方只收（RR），发送方必须测出 RTT
//
//     这条钉的是"tracker 有没有真正接进会话"。修之前：
//       · on_rtcp_datagram 从来没调过 tracker 的 SR/RR/BYE；
//       · on_local_sr_sent 有实现、无调用点；
//       · tracker 的 local_ssrc 拿的是 0（随机 SSRC 时），on_receiver_report
//         第一条判据就把所有 RR 跳过；
//       · on_local_sr_sent 存的是"NTP 高 32 位"，而对端回填的 LSR 是"中间
//         32 位"，两边不同构。
//     四者任一没修，rtt_us 都永远是 nullopt —— 而旧测试只看计数，全绿。
// ---------------------------------------------------------------------------
NETMECH_TEST(session_rtt_measured_when_only_one_side_sends)
{
    // 要 4 个连续端口：(p, p+1) 给 A，(p+2, p+3) 给 B
    const auto base = netmech::find_free_port_run(40000, 50000, 4, 2);
    REQUIRE(base.has_value());

    std::error_code ec;

    auto cfg_a       = base_cfg();
    cfg_a.local      = Endpoint("127.0.0.1", *base);
    cfg_a.remote     = Endpoint("127.0.0.1", static_cast<std::uint16_t>(*base + 2));
    cfg_a.enable_rtcp = true;
    auto a = RtpSession::create(cfg_a, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);

    auto cfg_b       = base_cfg();
    cfg_b.local      = Endpoint("127.0.0.1", static_cast<std::uint16_t>(*base + 2));
    cfg_b.remote     = Endpoint("127.0.0.1", *base);
    cfg_b.enable_rtcp = true;
    auto b = RtpSession::create(cfg_b, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);

    // A 发几个 RTP ⇒ 此后 A 是"发送者"（发 SR 而不是 RR）
    const std::uint8_t payload[4] = {0x11, 0x22, 0x33, 0x44};
    for (int i = 0; i < 3; ++i) {
        a->send(ConstBuf{nullptr, payload, sizeof payload}, i == 2);
        std::this_thread::sleep_for(20ms);
    }

    // A 发 SR → B 收到后回 RR（带 lsr）→ A 算出 RTT
    a->send_rtcp_now();
    std::this_thread::sleep_for(100ms);
    b->send_rtcp_now();
    std::this_thread::sleep_for(100ms);

    const auto sa = a->stats();
    CHECK_MSG(sa.rtt_us.has_value(),
              "A 侧 RTT 未测出：tracker 未接通 / local_ssrc=0 / LSR 不同构");
    if (sa.rtt_us.has_value()) {
        // 回环上 RTT 是亚毫秒级；给一个宽松上限，只用来发现"算出了个天文数字"
        CHECK_MSG(*sa.rtt_us < 2000000u,
                  ("RTT 异常大: " + std::to_string(*sa.rtt_us)).c_str());
    }

    a->close();
    b->close();
}

// ---------------------------------------------------------------------------
// 11. 双向都在发：SR 里的报告块同样要能算 RTT
//
//     RFC 3550 §6.4.1 规定 SR = 发送者信息 + 0..31 个报告块，与 RR 的报告块
//     等价。只处理 RR 的话，"双方都在发"的会话（对讲、双向视频）永远算不出
//     RTT —— 因为对端发的是 SR。
// ---------------------------------------------------------------------------
NETMECH_TEST(session_rtt_measured_between_two_senders)
{
    const auto base = netmech::find_free_port_run(40000, 50000, 4, 2);
    REQUIRE(base.has_value());

    std::error_code ec;

    auto cfg_a        = base_cfg();
    cfg_a.local       = Endpoint("127.0.0.1", *base);
    cfg_a.remote      = Endpoint("127.0.0.1", static_cast<std::uint16_t>(*base + 2));
    cfg_a.enable_rtcp = true;
    auto a = RtpSession::create(cfg_a, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);

    auto cfg_b        = base_cfg();
    cfg_b.local       = Endpoint("127.0.0.1", static_cast<std::uint16_t>(*base + 2));
    cfg_b.remote      = Endpoint("127.0.0.1", *base);
    cfg_b.enable_rtcp = true;
    auto b = RtpSession::create(cfg_b, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);

    const std::uint8_t payload[4] = {0x55, 0x66, 0x77, 0x88};
    for (int i = 0; i < 3; ++i) {
        a->send(ConstBuf{nullptr, payload, sizeof payload}, i == 2);
        b->send(ConstBuf{nullptr, payload, sizeof payload}, i == 2);
        std::this_thread::sleep_for(20ms);
    }

    // 交替催：A 发 SR → B 发 SR（报告块里 lsr = A 的 SR）→ A 算出 RTT；再反过来
    for (int round = 0; round < 3; ++round) {
        a->send_rtcp_now();
        std::this_thread::sleep_for(60ms);
        b->send_rtcp_now();
        std::this_thread::sleep_for(60ms);
    }

    const auto sa = a->stats();
    const auto sb = b->stats();

    CHECK_MSG(sa.rtt_us.has_value(), "A 侧 RTT 未测出（SR 报告块没参与计算）");
    CHECK_MSG(sb.rtt_us.has_value(), "B 侧 RTT 未测出（SR 报告块没参与计算）");

    a->close();
    b->close();
}

// ---------------------------------------------------------------------------
// 15. 帧级 API：一整帧的所有分片必须共用同一个 RTP 时间戳
//
//     以前 README 教"packetizer 每个分片回调里调一次 send()"，时间戳会逐分片
//     前进 3600 —— 接收端把一帧当成多个采样时刻。
// ---------------------------------------------------------------------------
NETMECH_TEST(session_send_frame_shares_timestamp)
{
    std::error_code ec;

    auto collector = std::make_shared<Collector>();
    auto rx = RtpSession::create(base_cfg(), collector, ec);
    REQUIRE(!ec);

    auto cfg = base_cfg();
    cfg.remote = Endpoint("127.0.0.1", rx->local_rtp_port());
    auto tx = RtpSession::create(cfg, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);

    // 一帧 = AUD + 一个 > MTU 的 IDR（必然 FU-A 成多包）
    std::vector<std::uint8_t> frame;
    const std::uint8_t aud[] = {0x00, 0x00, 0x00, 0x01, 0x09, 0xF0};
    const std::uint8_t idr[] = {0x00, 0x00, 0x00, 0x01, 0x65};
    frame.insert(frame.end(), aud, aud + sizeof aud);
    frame.insert(frame.end(), idr, idr + sizeof idr);
    frame.resize(frame.size() + 3000, 0x5A);

    H264Packetizer pkt;
    CHECK_EQ(tx->send_frame(span<const std::uint8_t>(frame.data(), frame.size()), pkt),
             SendResult::accepted);

    REQUIRE(wait_for([&] { return collector->count.load() >= 3; }));

    std::lock_guard<std::mutex> lk(collector->mtx);
    REQUIRE(collector->timestamps.size() >= 3u);

    // 所有分片时间戳相同
    for (std::size_t i = 0; i < collector->timestamps.size(); ++i) {
        CHECK_EQ(collector->timestamps[i], collector->timestamps[0]);
    }
    // 序号连续
    for (std::size_t i = 1; i < collector->seqs.size(); ++i) {
        CHECK_EQ(collector->seqs[i],
                 static_cast<std::uint16_t>(collector->seqs[i - 1] + 1));
    }

    tx->close();
    rx->close();
}

// ---------------------------------------------------------------------------
// 16. 自回环不能被误判成 SSRC 冲突
//
//     组播 loopback / "发给自己" 都会收到 SSRC 与本地相同的包。以前只看
//     SSRC 相同就判冲突 → 发 BYE + 换号，接着自己的下一包再触发……
// ---------------------------------------------------------------------------
NETMECH_TEST(session_ignores_self_loopback)
{
    std::error_code ec;
    const auto base = netmech::find_free_port_run(42000, 45000, 2, 2);
    REQUIRE(base.has_value());

    RtpSessionConfig cfg = base_cfg();
    cfg.local          = Endpoint("127.0.0.1", static_cast<std::uint16_t>(*base));
    cfg.remote         = Endpoint("127.0.0.1", static_cast<std::uint16_t>(*base));
    cfg.enable_rtcp    = false;
    cfg.rtp_port_end   = 0;

    auto sess = RtpSession::create(cfg, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);
    const std::uint32_t ssrc0 = sess->local_ssrc();

    const std::uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(sess->send(ConstBuf{nullptr, payload, sizeof payload}, false),
                 SendResult::accepted);
    }

    REQUIRE(wait_for([&] { return sess->stats().self_loopback_ignored >= 5u; }));
    CHECK_EQ(sess->local_ssrc(), ssrc0);   // 不应因自回环换号
    CHECK_EQ(sess->stats().rtp_received, std::uint64_t{0});   // 自己包不回调

    sess->close();
}

// ---------------------------------------------------------------------------
// 17. 没有 remote 时，RTCP 定时器不能 1ms 忙循环
//
//     以前 send_rtcp 失败路径不推进调度，定时器按 1ms 重排：
//     每秒上千次唤醒去尝试一个注定失败的发送。
// ---------------------------------------------------------------------------
NETMECH_TEST(session_rtcp_without_remote_does_not_spin)
{
    std::error_code ec;
    auto cfg = base_cfg();
    cfg.enable_rtcp = true;   // 开了 RTCP，但没有 remote

    auto sess = RtpSession::create(cfg, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);

    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    const auto st = sess->stats();
    CHECK_EQ(st.rtcp_sent, std::uint64_t{0});
    CHECK(st.rtcp_send_failures <= 2u);   // 忙循环时 250ms 会有上百次

    sess->close();
}

// ---------------------------------------------------------------------------
// 18. 显式时间戳也要推进内部游标（否则之后 send() 会拿到旧时间戳）
// ---------------------------------------------------------------------------
NETMECH_TEST(session_explicit_timestamp_advances_cursor)
{
    std::error_code ec;
    auto collector = std::make_shared<Collector>();
    auto rx = RtpSession::create(base_cfg(), collector, ec);
    REQUIRE(!ec);

    auto cfg = base_cfg();
    cfg.remote = Endpoint("127.0.0.1", rx->local_rtp_port());
    cfg.timestamp_increment = 3600;
    auto tx = RtpSession::create(cfg, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);

    const std::uint8_t payload[8] = {};
    CHECK_EQ(tx->send_with_timestamp(ConstBuf{nullptr, payload, sizeof payload}, false,
                                     /*timestamp=*/100000),
             SendResult::accepted);
    REQUIRE(wait_for([&] { return collector->count.load() >= 1; }));

    CHECK_EQ(tx->send(ConstBuf{nullptr, payload, sizeof payload}, false),
             SendResult::accepted);
    REQUIRE(wait_for([&] { return collector->count.load() >= 2; }));

    std::lock_guard<std::mutex> lk(collector->mtx);
    REQUIRE(collector->timestamps.size() >= 2u);
    CHECK_EQ(collector->timestamps[0], 100000u);
    CHECK_EQ(collector->timestamps[1], 103600u);   // 100000 + 3600

    tx->close();
    rx->close();
}

// ---------------------------------------------------------------------------
// 19. RTCP 复合包：第一个报文不是 SR/RR 时必须丢弃（RFC 3550 §6.1）
// ---------------------------------------------------------------------------
namespace {
class RtcpCountHandler : public RtpHandler {
public:
    void on_packet(RtpSession&, const RtpPacket&, const Endpoint&) override {}
    void on_rtcp(RtpSession&, const RtcpCompoundPacket&, const Endpoint&) override
    {
        count.fetch_add(1);
    }
    std::atomic<int> count{0};
};

/// 发裸 RTCP 用的最简接收者（netmech 要求 handler 非空才会创建端点）。
class DiscardUdpHandler : public netmech::UdpHandler {
public:
    void on_read(netmech::UdpEndpoint&, std::error_code, netmech::Buf,
                 std::size_t, const netmech::Address&) override
    {
    }
};

/// 从任意 UDP 端点把裸字节发给指定端口（模拟对端发 RTCP）。
///
/// 返回端点本身：`send_to` 是异步的，端点必须在包真正发出前保持存活，
/// 否则刚创建就析构会把待发数据一起丢掉。
std::shared_ptr<netmech::UdpEndpoint>
send_raw_udp(std::uint16_t port, const std::vector<std::uint8_t>& bytes)
{
    netmech::UdpConfig uc;
    uc.bind = netmech::Address("127.0.0.1", 0);
    std::error_code ec;
    auto ep = netmech::create_udp_endpoint(
        uc, std::make_shared<DiscardUdpHandler>(), nullptr, ec);
    if (!ep) {
        return nullptr;
    }
    (void)ep->send_to(netmech::Address("127.0.0.1", port),
                      netmech::ConstBuf{nullptr, bytes.data(), bytes.size()});
    return ep;
}
} // namespace

NETMECH_TEST(session_rejects_rtcp_without_sr_rr_first)
{
    std::error_code ec;
    auto handler = std::make_shared<RtcpCountHandler>();
    auto cfg = base_cfg();
    cfg.enable_rtcp = true;
    auto sess = RtpSession::create(cfg, handler, ec);
    REQUIRE(!ec);

    // 构造一个"以 SDES 开头"的复合包（顺序违规）
    std::vector<std::uint8_t> buf(256);
    SdesReport sdes;
    SdesChunk chunk;
    chunk.ssrc  = 0x01020304;
    chunk.cname = "bad";
    sdes.chunks.push_back(chunk);

    std::error_code bec;
    const auto n = build_rtcp_sdes(buf.data(), buf.size(), sdes, bec);
    REQUIRE(!bec);
    REQUIRE(n > 0);
    buf.resize(n);

    auto raw_ep = send_raw_udp(sess->local_rtcp_port(), buf);

    REQUIRE(wait_for([&] { return sess->stats().parse_errors >= 1u; }, 1500ms));
    if (raw_ep) { raw_ep->close(); }
    CHECK_EQ(sess->stats().rtcp_received, std::uint64_t{0});
    CHECK_EQ(handler->count.load(), 0);

    sess->close();
}

// ---------------------------------------------------------------------------
// 20. RTCP 复合包：缺少 SDES/CNAME 只计数、不丢包（保互操作）
// ---------------------------------------------------------------------------
NETMECH_TEST(session_counts_rtcp_without_cname)
{
    std::error_code ec;
    auto handler = std::make_shared<RtcpCountHandler>();
    auto cfg = base_cfg();
    cfg.enable_rtcp = true;
    auto sess = RtpSession::create(cfg, handler, ec);
    REQUIRE(!ec);

    // 只有一个 SR、没有 SDES(CNAME)
    std::vector<std::uint8_t> buf(256);
    SenderReport sr;
    sr.ssrc = 0x0A0B0C0D;
    std::error_code bec;
    const auto n = build_rtcp_sender_report(buf.data(), buf.size(), sr, bec);
    REQUIRE(!bec);
    REQUIRE(n > 0);
    buf.resize(n);

    auto raw_ep = send_raw_udp(sess->local_rtcp_port(), buf);

    REQUIRE(wait_for([&] { return sess->stats().rtcp_received >= 1u; }, 1500ms));
    if (raw_ep) { raw_ep->close(); }
    CHECK(sess->stats().rtcp_without_cname >= 1u);
    CHECK_EQ(handler->count.load(), 1);

    sess->close();
}

// ---------------------------------------------------------------------------
// 21. 注入 Runtime：io 线程数由应用决定，RTP/RTCP 端点跑在该 Runtime 上
// ---------------------------------------------------------------------------
NETMECH_TEST(session_accepts_injected_runtime)
{
    netmech::Runtime::Options opt;
    opt.io_threads = 1;
    auto rt = netmech::Runtime::create(opt);
    REQUIRE(rt != nullptr);
    CHECK_EQ(rt->io_threads(), std::size_t{1});

    std::error_code ec;
    auto collector = std::make_shared<Collector>();

    auto rx_cfg      = base_cfg();
    rx_cfg.enable_rtcp = false;
    rx_cfg.runtime     = rt;
    auto rx = RtpSession::create(rx_cfg, collector, ec);
    REQUIRE(!ec);

    auto tx_cfg        = base_cfg();
    tx_cfg.enable_rtcp = false;
    tx_cfg.runtime     = rt;
    tx_cfg.remote      = Endpoint("127.0.0.1", rx->local_rtp_port());
    auto tx = RtpSession::create(tx_cfg, std::make_shared<NullHandler>(), ec);
    REQUIRE(!ec);

    const std::uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK_EQ(tx->send(ConstBuf{nullptr, payload, sizeof payload}, false),
             SendResult::accepted);
    REQUIRE(wait_for([&] { return collector->count.load() >= 1; }));
    CHECK_EQ(collector->count.load(), 1);

    tx->close();
    rx->close();
    rt->shutdown();
    CHECK(!rt->running());
}
