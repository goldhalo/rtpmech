// ============================================================================
//  test_source_tracker.cpp —— 源跟踪测试（丢包/抖动/序列号回绕/RTT）
//
//  验证 RFC 3550 的核心算法：
//    * 序列号回绕（附录 A.1）
//    * 累计丢包与丢包比例（附录 A.3）
//    * 到达间隔抖动（附录 A.8）
//    * LSR/DLSR 与 RTT（§6.4.1）
// ============================================================================
#include "source_tracker.hpp"

#include <cstring>
#include <optional>
#include <vector>

#include "test_framework.hpp"

using namespace rtpmech;

namespace {

/// RtpPacket 是只读视图，且**没有公开默认构造函数**（只能由 parse 产出）。
/// 因此测试中用 std::optional<RtpPacket> 持有解析结果，
/// 同时用成员 bytes 保证视图指向的缓冲在跟踪期间有效。
///
/// ⚠ 成员声明顺序很重要：bytes 必须在 pkt 之前声明，
///    否则 pkt 会在 bytes 构造前就被初始化（虽然用 optional 时是
///    emplace，但仍应保证语义清晰）。
struct OwnedPacket {
    explicit OwnedPacket(std::uint32_t ssrc, std::uint16_t seq, std::uint32_t ts)
        : bytes(12, 0)
    {
        bytes[0] = 0x80;                                    // V=2
        bytes[1] = 96;                                      // PT=96
        bytes[2] = static_cast<std::uint8_t>(seq >> 8);
        bytes[3] = static_cast<std::uint8_t>(seq & 0xFF);
        bytes[4] = static_cast<std::uint8_t>(ts >> 24);
        bytes[5] = static_cast<std::uint8_t>(ts >> 16);
        bytes[6] = static_cast<std::uint8_t>(ts >> 8);
        bytes[7] = static_cast<std::uint8_t>(ts);
        bytes[8]  = static_cast<std::uint8_t>(ssrc >> 24);
        bytes[9]  = static_cast<std::uint8_t>(ssrc >> 16);
        bytes[10] = static_cast<std::uint8_t>(ssrc >> 8);
        bytes[11] = static_cast<std::uint8_t>(ssrc);

        std::error_code ec;
        pkt = RtpPacket::parse(bytes.data(), bytes.size(), ec);
    }

    [[nodiscard]] const RtpPacket& get() const { return *pkt; }

    std::vector<std::uint8_t>    bytes;
    std::optional<RtpPacket>     pkt;
};
} // namespace

// ---------------------------------------------------------------------------
// 1. 基本收包统计
// ---------------------------------------------------------------------------
NETMECH_TEST(tracker_counts_packets)
{
    SourceTracker tr(0, 90000);
    for (std::uint16_t i = 0; i < 10; ++i) {
        OwnedPacket o(0x1234, i, i * 3000);
        tr.on_rtp_received(o.get(), static_cast<std::int64_t>(i) * 33000);
    }

    const SourceStats* st = tr.find(0x1234);
    REQUIRE(st != nullptr);
    CHECK_EQ(st->received, std::uint64_t{10});
    CHECK_EQ(st->cumulative_lost(), std::int64_t{0});
    CHECK_EQ(st->ext_max_seq, std::uint64_t{9});
}

// ---------------------------------------------------------------------------
// 2. 丢包统计（中间缺包）
// ---------------------------------------------------------------------------
NETMECH_TEST(tracker_detects_loss)
{
    SourceTracker tr(0, 90000);
    // 收到 seq 0,1,2,5,6,7 —— 缺 3,4
    const std::uint16_t seqs[] = {0, 1, 2, 5, 6, 7};
    for (int i = 0; i < 6; ++i) {
        OwnedPacket o(0xAA, seqs[i], static_cast<std::uint32_t>(i) * 3000);
        tr.on_rtp_received(o.get(), static_cast<std::int64_t>(i) * 33000);
    }

    const SourceStats* st = tr.find(0xAA);
    REQUIRE(st != nullptr);
    CHECK_EQ(st->received, std::uint64_t{6});
    // expected = 0..7 共 8 个，收到 6 个 → 丢 2
    CHECK_EQ(st->cumulative_lost(), std::int64_t{2});
}

// ---------------------------------------------------------------------------
// 3. 序列号回绕（RFC 3550 A.1）
// ---------------------------------------------------------------------------
NETMECH_TEST(tracker_handles_sequence_wraparound)
{
    SourceTracker tr(0, 90000);
    // 从 65534 开始，回绕到 0,1,2
    const std::uint16_t seqs[] = {65534, 65535, 0, 1, 2};
    for (int i = 0; i < 5; ++i) {
        OwnedPacket o(0xBB, seqs[i], static_cast<std::uint32_t>(i) * 3000);
        tr.on_rtp_received(o.get(), static_cast<std::int64_t>(i) * 33000);
    }

    const SourceStats* st = tr.find(0xBB);
    REQUIRE(st != nullptr);
    CHECK_EQ(st->received, std::uint64_t{5});
    // 无丢包：扩展序列号应连续
    CHECK_EQ(st->cumulative_lost(), std::int64_t{0});
    // 回绕一次，ext_max = 1×65536 + 2
    CHECK_EQ(st->ext_max_seq, std::uint64_t{65538});
    CHECK_EQ(st->cycles, std::uint32_t{1});
}

// ---------------------------------------------------------------------------
// 4. 重复包不应产生"负丢包"比例溢出
// ---------------------------------------------------------------------------
NETMECH_TEST(tracker_duplicate_packet_no_negative_fraction)
{
    SourceTracker tr(0, 90000);
    OwnedPacket a(0xCC, 0, 0);
    tr.on_rtp_received(a.get(), 0);
    tr.on_rtp_received(a.get(), 1000);   // 重复

    const SourceStats* st = tr.find(0xCC);
    REQUIRE(st != nullptr);
    // 首包计 1 次，再收到同一个包两次 => 共 3 次
    CHECK_EQ(st->received, std::uint64_t{3});
    // fraction 必须为 0（负数被钳制）
    CHECK_EQ(static_cast<int>(st->fraction_lost()), 0);
}

// ---------------------------------------------------------------------------
// 5. 抖动计算
// ---------------------------------------------------------------------------
NETMECH_TEST(tracker_computes_jitter)
{
    // clock_rate = 90000，包间隔 3000 时间戳单位 = 33.33 ms
    SourceTracker tr(0, 90000);

    // 包 0：ts=0，到达 0us
    OwnedPacket p0(0xDD, 0, 0);
    tr.on_rtp_received(p0.get(), 0);

    // 包 1：ts=3000，按时到达 33333us → D≈0，抖动应保持很小
    OwnedPacket p1(0xDD, 1, 3000);
    tr.on_rtp_received(p1.get(), 33333);

    // 包 2：ts=6000，迟到 10ms 到达 76666us（应为 66666us）→ 产生抖动
    OwnedPacket p2(0xDD, 2, 6000);
    tr.on_rtp_received(p2.get(), 76666);

    const SourceStats* st = tr.find(0xDD);
    REQUIRE(st != nullptr);
    // 抖动应 > 0（因为第三个包延迟了）
    CHECK(st->jitter() > 0);
    // 抖动单位是 1/16，所以原始 units 更大
    CHECK(st->jitter_units > 0);
}

// ---------------------------------------------------------------------------
// 6. 自己发的包不计入统计
// ---------------------------------------------------------------------------
NETMECH_TEST(tracker_ignores_local_ssrc)
{
    SourceTracker tr(0x9999, 90000);
    OwnedPacket o(0x9999, 0, 0);
    tr.on_rtp_received(o.get(), 0);

    CHECK_EQ(tr.size(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// 7. SR 记录与报告块生成
// ---------------------------------------------------------------------------
NETMECH_TEST(tracker_records_sender_report)
{
    SourceTracker tr(0, 90000);
    OwnedPacket o(0xEEEE, 0, 0);
    tr.on_rtp_received(o.get(), 1000000);

    SenderReport sr;
    sr.ssrc          = 0xEEEE;
    sr.ntp_msw       = 0x11112222;
    sr.ntp_lsw       = 0x33334444;
    sr.rtp_timestamp = 90000;
    sr.packet_count  = 100;
    sr.octet_count   = 12000;
    tr.on_sender_report(sr, 1000000);

    const SourceStats* st = tr.find(0xEEEE);
    REQUIRE(st != nullptr);
    REQUIRE(st->last_sr_ntp.has_value());
    CHECK_EQ(st->last_sr_ntp->seconds, 0x11112222u);
    CHECK_EQ(st->last_sr_packet_count, 100u);

    auto blocks = tr.build_report_blocks();
    REQUIRE(blocks.size() == 1);
    CHECK_EQ(blocks[0].ssrc, 0xEEEEu);
    // LSR = NTP 中间 32 位
    CHECK_EQ(blocks[0].lsr, (0x11112222u << 16) | (0x33334444u >> 16));
}

// ---------------------------------------------------------------------------
// 8. BYE 移除源
// ---------------------------------------------------------------------------
NETMECH_TEST(tracker_bye_removes_source)
{
    SourceTracker tr(0, 90000);
    OwnedPacket o(0xFFFF, 0, 0);
    tr.on_rtp_received(o.get(), 0);
    CHECK_EQ(tr.size(), std::size_t{1});

    ByeReport bye;
    bye.ssrcs = {0xFFFF};
    tr.on_bye(bye);
    CHECK_EQ(tr.size(), std::size_t{0});
    CHECK(tr.find(0xFFFF) == nullptr);
}

// ---------------------------------------------------------------------------
// 9. 超时清理
// ---------------------------------------------------------------------------
NETMECH_TEST(tracker_expires_inactive)
{
    SourceTracker tr(0, 90000);
    OwnedPacket o(0x1111, 0, 0);
    tr.on_rtp_received(o.get(), 1000000);   // 1 秒时刻

    // 最后一次活动在 1.0 秒，超时阈值 5 秒 => 6.0 秒后过期
    // 3.0 秒：未超时（间隔 2 秒 < 5 秒）
    CHECK_EQ(tr.expire_inactive(3000000, 5000000), std::size_t{0});
    // 5.5 秒：仍未超时（间隔 4.5 秒 < 5 秒）
    CHECK_EQ(tr.expire_inactive(5500000, 5000000), std::size_t{0});
    // 6.5 秒：已超时（间隔 5.5 秒 > 5 秒）→ 被清理
    CHECK_EQ(tr.expire_inactive(6500000, 5000000), std::size_t{1});
    CHECK_EQ(tr.size(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// 10. 报告块数量上限 31
// ---------------------------------------------------------------------------
NETMECH_TEST(tracker_report_blocks_capped_at_31)
{
    SourceTracker tr(0, 90000);
    for (std::uint32_t i = 1; i <= 40; ++i) {
        OwnedPacket o(i, 0, 0);
        tr.on_rtp_received(o.get(), static_cast<std::int64_t>(i));
    }
    CHECK_EQ(tr.size(), std::size_t{40});

    auto blocks = tr.build_report_blocks();
    CHECK_EQ(blocks.size(), std::size_t{31});   // RTCP RC 只有 5 位
}

// ---------------------------------------------------------------------------
// 11. 发送者计数
// ---------------------------------------------------------------------------
NETMECH_TEST(tracker_sender_count)
{
    SourceTracker tr(0, 90000);
    OwnedPacket a(1, 0, 0);
    tr.on_rtp_received(a.get(), 0);
    OwnedPacket b(2, 0, 0);
    tr.on_rtp_received(b.get(), 0);

    CHECK_EQ(tr.sender_count(), std::size_t{2});
    CHECK_EQ(tr.size(), std::size_t{2});
}
