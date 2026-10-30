// ============================================================================
//  test_rtcp_packet.cpp —— RTCP 报文解析与构造测试
//
//  验证：
//    * SR / RR / SDES / BYE / APP 的构造 → 解析往返
//    * 复合包（多个报文串联）解析
//    * Length 字段的 "(bytes/4)-1" 编码正确性
//    * 各类非法输入
//    * NTP 时间转换
// ============================================================================
#include <rtpmech/rtcp_packet.hpp>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "test_framework.hpp"

using namespace rtpmech;

namespace {

/// 构造到 vector 并返回实际长度。
template <typename Fn>
std::vector<std::uint8_t> build_into(std::size_t cap, Fn&& fn, std::error_code& ec)
{
    std::vector<std::uint8_t> buf(cap, 0);
    const std::size_t n = fn(buf.data(), buf.size(), ec);
    buf.resize(ec ? 0 : n);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. SR 构造 → 解析
// ---------------------------------------------------------------------------
NETMECH_TEST(rtcp_sender_report_roundtrip)
{
    SenderReport sr;
    sr.ssrc          = 0x11223344;
    sr.ntp_msw       = 0xAABBCCDD;
    sr.ntp_lsw       = 0xEEFF0011;
    sr.rtp_timestamp = 90000;
    sr.packet_count  = 1234;
    sr.octet_count   = 567890;

    std::error_code ec;
    std::vector<std::uint8_t> buf(128);
    const std::size_t n =
        build_rtcp_sender_report(buf.data(), buf.size(), sr, ec);
    REQUIRE(!ec);
    CHECK_EQ(n, 28u);          // 4 头 + 24 体，无报告块

    // Length = 28/4 - 1 = 6
    CHECK_EQ(buf[2], 0);
    CHECK_EQ(buf[3], 6);

    auto compound = RtcpCompoundPacket::parse(buf.data(), n, ec);
    REQUIRE(!ec);
    REQUIRE(compound.has_value());
    REQUIRE((compound->packets().size()) == (std::size_t{1}));

    const auto& pkt = compound->packets()[0];
    CHECK(pkt.type() == RtcpType::sender_report);
    const SenderReport* parsed = pkt.as_sender_report();
    REQUIRE(parsed != nullptr);
    CHECK_EQ(parsed->ssrc, 0x11223344u);
    CHECK_EQ(parsed->ntp_msw, 0xAABBCCDDu);
    CHECK_EQ(parsed->ntp_lsw, 0xEEFF0011u);
    CHECK_EQ(parsed->rtp_timestamp, 90000u);
    CHECK_EQ(parsed->packet_count, 1234u);
    CHECK_EQ(parsed->octet_count, 567890u);
    CHECK_EQ(parsed->reports.size(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// 2. SR 带报告块
// ---------------------------------------------------------------------------
NETMECH_TEST(rtcp_sender_report_with_blocks)
{
    SenderReport sr;
    sr.ssrc = 1;
    ReportBlock rb;
    rb.ssrc          = 0xDEADBEEF;
    rb.fraction_lost = 64;          // 25%
    rb.packets_lost  = 42;
    rb.highest_seq   = 1000;
    rb.jitter        = 300;
    rb.lsr           = 0x12345678;
    rb.dlsr          = 65536;
    sr.reports.push_back(rb);

    std::error_code ec;
    std::vector<std::uint8_t> buf(128);
    const std::size_t n = build_rtcp_sender_report(buf.data(), buf.size(), sr, ec);
    REQUIRE(!ec);
    CHECK_EQ(n, 52u);          // 4 + 24 + 24

    auto compound = RtcpCompoundPacket::parse(buf.data(), n, ec);
    REQUIRE(compound.has_value());
    const SenderReport* p = compound->packets()[0].as_sender_report();
    REQUIRE(p != nullptr);
    REQUIRE((p->reports.size()) == (std::size_t{1}));
    CHECK_EQ(p->reports[0].ssrc, 0xDEADBEEFu);
    CHECK_EQ(p->reports[0].fraction_lost, 64);
    CHECK_EQ(p->reports[0].packets_lost, 42);
    CHECK_EQ(p->reports[0].highest_seq, 1000u);
    CHECK_EQ(p->reports[0].jitter, 300u);
    CHECK_EQ(p->reports[0].lsr, 0x12345678u);
    CHECK_EQ(p->reports[0].dlsr, 65536u);
}

// ---------------------------------------------------------------------------
// 3. RR 构造 → 解析
// ---------------------------------------------------------------------------
NETMECH_TEST(rtcp_receiver_report_roundtrip)
{
    ReceiverReport rr;
    rr.ssrc = 0x55667788;
    ReportBlock rb;
    rb.ssrc         = 0x99AABBCC;
    rb.packets_lost = -5;          // 负数（重复包导致）应正确保留
    rr.reports.push_back(rb);

    std::error_code ec;
    std::vector<std::uint8_t> buf(128);
    const std::size_t n =
        build_rtcp_receiver_report(buf.data(), buf.size(), rr, ec);
    REQUIRE(!ec);
    CHECK_EQ(n, 32u);          // 4 + 4 + 24

    auto compound = RtcpCompoundPacket::parse(buf.data(), n, ec);
    REQUIRE(compound.has_value());
    const ReceiverReport* p = compound->packets()[0].as_receiver_report();
    REQUIRE(p != nullptr);
    CHECK_EQ(p->ssrc, 0x55667788u);
    REQUIRE((p->reports.size()) == (std::size_t{1}));
    CHECK_EQ(p->reports[0].packets_lost, -5);
}

// ---------------------------------------------------------------------------
// 4. SDES 构造 → 解析
// ---------------------------------------------------------------------------
NETMECH_TEST(rtcp_sdes_roundtrip)
{
    SdesReport sdes;
    SdesChunk c;
    c.ssrc  = 0x01020304;
    c.cname = "user@host";
    c.items.emplace_back(SdesType::tool, "rtpmech");
    sdes.chunks.push_back(c);

    std::error_code ec;
    std::vector<std::uint8_t> buf(256);
    const std::size_t n = build_rtcp_sdes(buf.data(), buf.size(), sdes, ec);
    REQUIRE(!ec);
    CHECK(n > 0);
    CHECK_EQ(n % 4, 0u);       // 必须 4 字节对齐

    auto compound = RtcpCompoundPacket::parse(buf.data(), n, ec);
    REQUIRE(compound.has_value());
    CHECK(compound->has_cname());

    const SdesReport* p = compound->packets()[0].as_sdes();
    REQUIRE(p != nullptr);
    REQUIRE((p->chunks.size()) == (std::size_t{1}));
    CHECK_EQ(p->chunks[0].ssrc, 0x01020304u);
    CHECK_EQ(p->chunks[0].cname, std::string("user@host"));
    REQUIRE((p->chunks[0].items.size()) == (std::size_t{1}));
    CHECK(p->chunks[0].items[0].first == SdesType::tool);
    CHECK_EQ(p->chunks[0].items[0].second, std::string("rtpmech"));
}

// ---------------------------------------------------------------------------
// 5. 多块 SDES 对齐
// ---------------------------------------------------------------------------
NETMECH_TEST(rtcp_sdes_multiple_chunks_aligned)
{
    SdesReport sdes;
    for (std::uint32_t i = 1; i <= 3; ++i) {
        SdesChunk c;
        c.ssrc  = i;
        c.cname = "c" + std::to_string(i);   // 长度 2，会触发补齐
        sdes.chunks.push_back(c);
    }

    std::error_code ec;
    std::vector<std::uint8_t> buf(256);
    const std::size_t n = build_rtcp_sdes(buf.data(), buf.size(), sdes, ec);
    REQUIRE(!ec);
    CHECK_EQ(n % 4, 0u);

    auto compound = RtcpCompoundPacket::parse(buf.data(), n, ec);
    REQUIRE(compound.has_value());
    const SdesReport* p = compound->packets()[0].as_sdes();
    REQUIRE(p != nullptr);
    REQUIRE((p->chunks.size()) == (std::size_t{3}));
    CHECK_EQ(p->chunks[0].cname, std::string("c1"));
    CHECK_EQ(p->chunks[1].cname, std::string("c2"));
    CHECK_EQ(p->chunks[2].cname, std::string("c3"));
}

// ---------------------------------------------------------------------------
// 6. BYE 构造 → 解析
// ---------------------------------------------------------------------------
NETMECH_TEST(rtcp_bye_roundtrip)
{
    ByeReport bye;
    bye.ssrcs = {0xAAAAAAAA, 0xBBBBBBBB};
    bye.reason = "shutdown";

    std::error_code ec;
    std::vector<std::uint8_t> buf(128);
    const std::size_t n = build_rtcp_bye(buf.data(), buf.size(), bye, ec);
    REQUIRE(!ec);
    CHECK_EQ(n % 4, 0u);

    auto compound = RtcpCompoundPacket::parse(buf.data(), n, ec);
    REQUIRE(compound.has_value());
    const ByeReport* p = compound->packets()[0].as_bye();
    REQUIRE(p != nullptr);
    REQUIRE((p->ssrcs.size()) == (std::size_t{2}));
    CHECK_EQ(p->ssrcs[0], 0xAAAAAAAAu);
    CHECK_EQ(p->ssrcs[1], 0xBBBBBBBBu);
    CHECK_EQ(p->reason, std::string("shutdown"));
}

// ---------------------------------------------------------------------------
// 7. APP 构造 → 解析
// ---------------------------------------------------------------------------
NETMECH_TEST(rtcp_app_roundtrip)
{
    AppReport app;
    app.subtype = 3;
    app.ssrc    = 0x12345678;
    app.name    = {'T', 'E', 'S', 'T'};
    app.data    = {0x01, 0x02, 0x03, 0x04, 0x05};

    std::error_code ec;
    std::vector<std::uint8_t> buf(128);
    const std::size_t n = build_rtcp_app(buf.data(), buf.size(), app, ec);
    REQUIRE(!ec);
    CHECK_EQ(n % 4, 0u);

    auto compound = RtcpCompoundPacket::parse(buf.data(), n, ec);
    REQUIRE(compound.has_value());
    const AppReport* p = compound->packets()[0].as_app();
    REQUIRE(p != nullptr);
    CHECK_EQ(static_cast<int>(p->subtype), 3);
    CHECK_EQ(p->ssrc, 0x12345678u);
    CHECK_EQ(p->name[0], 'T');
    CHECK_EQ(p->name[3], 'T');
    // ★ APP 数据长度：协议层面不可恢复（RFC 3550 未提供长度字段）
    //
    //   发送 5 字节 → 补齐到 8 字节 → 解析得到 8 字节。
    //   这不是 bug，而是 RTCP 包长必须 4 字节对齐导致的固有局限。
    //   本实现如实返回补齐后的长度（与 rtplib 一致），而不是猜测哪些零
    //   是补齐 —— 猜测会误删用户数据中真实的末尾零。
    REQUIRE((p->data.size()) == (std::size_t{8}));
    CHECK_EQ(static_cast<int>(p->data[0]), 1);
    CHECK_EQ(static_cast<int>(p->data[4]), 5);
    CHECK_EQ(static_cast<int>(p->data[5]), 0);   // 补齐零
    CHECK_EQ(static_cast<int>(p->data[7]), 0);
}

// ---------------------------------------------------------------------------
// 8. 复合包：SR + SDES 串联
// ---------------------------------------------------------------------------
NETMECH_TEST(rtcp_compound_sr_plus_sdes)
{
    SenderReport sr;
    sr.ssrc = 0x11111111;

    SdesReport sdes;
    SdesChunk c;
    c.ssrc  = 0x11111111;
    c.cname = "test";
    sdes.chunks.push_back(c);

    std::error_code ec;
    std::vector<std::uint8_t> buf(256);
    const std::size_t n_sr =
        build_rtcp_sender_report(buf.data(), buf.size(), sr, ec);
    REQUIRE(!ec);
    const std::size_t n_sdes = build_rtcp_sdes(
        buf.data() + n_sr, buf.size() - n_sr, sdes, ec);
    REQUIRE(!ec);

    const std::size_t total = n_sr + n_sdes;
    auto compound = RtcpCompoundPacket::parse(buf.data(), total, ec);
    REQUIRE(compound.has_value());
    REQUIRE((compound->packets().size()) == (std::size_t{2}));
    CHECK(compound->packets()[0].type() == RtcpType::sender_report);
    CHECK(compound->packets()[1].type() == RtcpType::sdes);
    CHECK(compound->packets()[1].as_sdes() != nullptr);
    CHECK(compound->has_cname());
}

// ---------------------------------------------------------------------------
// 9. 非法输入：长度不是 4 的倍数
// ---------------------------------------------------------------------------
NETMECH_TEST(rtcp_rejects_unaligned_length)
{
    std::uint8_t buf[28] = {0};
    buf[0] = 0x80;      // V=2
    buf[1] = 200;       // SR
    buf[2] = 0;
    buf[3] = 4;         // Length=4 → 但总长只有 28 而非 20 的整数倍

    std::error_code ec;
    auto compound = RtcpCompoundPacket::parse(buf, 27, ec);   // 27 非 4 倍数
    CHECK(ec);
    CHECK(!compound.has_value());
}

// ---------------------------------------------------------------------------
// 10. 非法输入：Length 越界
// ---------------------------------------------------------------------------
NETMECH_TEST(rtcp_rejects_length_overflow)
{
    std::uint8_t buf[8] = {0};
    buf[0] = 0x80;
    buf[1] = 200;
    buf[2] = 0xFF;      // Length 巨大
    buf[3] = 0xFF;

    std::error_code ec;
    auto compound = RtcpCompoundPacket::parse(buf, sizeof(buf), ec);
    CHECK(ec);
    CHECK(!compound.has_value());
}

// ---------------------------------------------------------------------------
// 11. 非法输入：版本号不是 2
// ---------------------------------------------------------------------------
NETMECH_TEST(rtcp_rejects_bad_version)
{
    std::uint8_t buf[8] = {0};
    buf[0] = 0x40;      // V=1
    buf[1] = 200;
    buf[2] = 0;
    buf[3] = 1;

    std::error_code ec;
    auto compound = RtcpCompoundPacket::parse(buf, sizeof(buf), ec);
    CHECK(ec);
    CHECK(!compound.has_value());
}

// ---------------------------------------------------------------------------
// 12. 缓冲区不足
// ---------------------------------------------------------------------------
NETMECH_TEST(rtcp_build_buffer_too_small)
{
    SenderReport sr;
    std::error_code ec;
    std::uint8_t small[8] = {0};
    const std::size_t n = build_rtcp_sender_report(small, sizeof(small), sr, ec);
    CHECK_EQ(n, 0u);
    CHECK(ec);
    CHECK(ec == RtpErrc::buffer_too_small);
}

// ---------------------------------------------------------------------------
// 13. 报告块数量超限（RC 只有 5 位，最多 31）
// ---------------------------------------------------------------------------
NETMECH_TEST(rtcp_rejects_too_many_blocks)
{
    ReceiverReport rr;
    rr.reports.resize(32);
    std::error_code ec;
    const std::size_t sz = rtcp_receiver_report_size(rr);
    CHECK_EQ(sz, 0u);

    std::vector<std::uint8_t> buf(1024);
    const std::size_t n = build_rtcp_receiver_report(buf.data(), buf.size(), rr, ec);
    CHECK_EQ(n, 0u);
    CHECK(ec == RtpErrc::invalid_argument);
}

// ---------------------------------------------------------------------------
// 14. NTP 时间转换
// ---------------------------------------------------------------------------
NETMECH_TEST(rtcp_ntp_time_conversion)
{
    // Unix 时间 0（1970-01-01）→ NTP 秒应为 2208988800
    const NtpTime t0 = make_ntp_time(0, 0);
    CHECK_EQ(t0.seconds, static_cast<std::uint32_t>(kNtpUnixEpochOffset));
    CHECK_EQ(t0.fraction, 0u);

    // 往返：带小数部分
    const std::int64_t  sec  = 1700000000;
    const std::uint32_t nano = 500000000;   // 0.5 秒
    const NtpTime t1 = make_ntp_time(sec, nano);
    CHECK_EQ(t1.fraction, 0x80000000u);     // 0.5 × 2^32

    std::int64_t  back_sec  = 0;
    std::uint32_t back_nano = 0;
    split_ntp_time(t1, back_sec, back_nano);
    CHECK_EQ(back_sec, sec);
    // 允许 1ns 误差（定点转换精度）
    CHECK(back_nano >= nano - 1 && back_nano <= nano + 1);
}

// ---------------------------------------------------------------------------
// 15. 未知类型的 RTCP 报文不应导致解析失败
// ---------------------------------------------------------------------------
NETMECH_TEST(rtcp_unknown_type_preserved)
{
    // 构造一个 PT=207（未定义）的报文，Length=1 → 8 字节
    std::uint8_t buf[8] = {0};
    buf[0] = 0x80;
    buf[1] = 207;
    buf[2] = 0;
    buf[3] = 1;
    buf[4] = 0xAA;
    buf[5] = 0xBB;

    std::error_code ec;
    auto compound = RtcpCompoundPacket::parse(buf, sizeof(buf), ec);
    REQUIRE(!ec);
    REQUIRE(compound.has_value());
    REQUIRE((compound->packets().size()) == (std::size_t{1}));
    CHECK_EQ(static_cast<int>(compound->packets()[0].type()), 207);
    CHECK_EQ(compound->packets()[0].raw().size(), std::size_t{8});
}

// ---------------------------------------------------------------------------
// SDES 块**无 padding** 时的边界（cname 长度使 chunk 恰好 4 字节对齐）
//
// 历史 bug：解析循环用 `off + 2 <= body_avail` 做条件，而 END 只占 1 字节。
// 当 END 正好落在块的最后 1 个字节时，循环根本进不去，合法的 SDES 被判非法。
// 触发条件：cname 17 字节 → chunk = 4+2+17+1 = 24（已对齐）→ 无 padding。
// 而 cname 18 字节 → chunk 25 → align4 = 28 → 有 2 字节 padding → 能解析成功。
// 所以症状是"时好时坏"，取决于 SSRC 的十进制位数。
// ---------------------------------------------------------------------------
NETMECH_TEST(sdes_chunk_without_padding_parses)
{
    SdesReport sdes;
    SdesChunk chunk;
    chunk.ssrc  = 0x2121A1F6;
    chunk.cname = "rtpmech-555852278";      // 17 字节，chunk 恰好 24（无 padding）
    sdes.chunks.push_back(chunk);

    std::uint8_t buf[128] = {};
    std::error_code ec;
    const std::size_t n = build_rtcp_sdes(buf, sizeof(buf), sdes, ec);
    REQUIRE(!ec);
    REQUIRE(n == 28);                       // 头 4 + chunk 24

    auto pkt = RtcpCompoundPacket::parse(buf, n, ec);
    CHECK_MSG(!ec, ("无 padding 的 SDES 解析失败: " + ec.message()).c_str());
    REQUIRE(pkt.has_value());

    const SdesReport* parsed = nullptr;
    for (const auto& p : pkt->packets()) {
        if (const auto* s = p.as_sdes()) {
            parsed = s;
            break;
        }
    }
    REQUIRE(parsed != nullptr);
    REQUIRE(parsed->chunks.size() == 1u);
    CHECK_EQ(parsed->chunks[0].ssrc, 0x2121A1F6u);
    CHECK(parsed->chunks[0].cname == "rtpmech-555852278");
}
