// ============================================================================
//  test_rtp_packet.cpp —— RTP 包解析与构造测试
//
//  验证：
//    * 构造 → 解析 的往返一致性
//    * 各类边界情况（扩展头、CSRC、padding、空载荷）
//    * 非法输入的处理（长度不足、版本错误、padding 非法）
//    * 字节序（网络序）正确性
// ============================================================================
#include <rtpmech/rtp_packet.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "test_framework.hpp"

using namespace rtpmech;

// ---------------------------------------------------------------------------
// 1. 最小包：12 字节头，无载荷、无扩展、无 CSRC
// ---------------------------------------------------------------------------
NETMECH_TEST(rtp_parse_minimal_header)
{
    // RFC 3550 最小 RTP 头（版本 2）
    const std::uint8_t raw[12] = {
        0x80,        // V=2, P=0, X=0, CC=0
        0x60,        // M=0, PT=96
        0x12, 0x34,  // sequence = 0x1234
        0x00, 0x00, 0x01, 0x00,   // timestamp = 256
        0xDE, 0xAD, 0xBE, 0xEF    // ssrc
    };

    std::error_code ec;
    auto pkt = RtpPacket::parse(raw, sizeof(raw), ec);
    REQUIRE(!ec);
    REQUIRE(pkt.has_value());

    CHECK_EQ(pkt->version(), 2);
    CHECK(!pkt->has_padding());
    CHECK(!pkt->has_extension());
    CHECK_EQ(pkt->csrc_count(), 0u);
    CHECK(!pkt->marker());
    CHECK_EQ(pkt->payload_type(), 96);
    CHECK_EQ(pkt->sequence(), 0x1234);
    CHECK_EQ(pkt->timestamp(), 256u);
    CHECK_EQ(pkt->ssrc(), 0xDEADBEEFu);
    CHECK_EQ(pkt->payload().size(), 0u);
}

// ---------------------------------------------------------------------------
// 2. 带载荷
// ---------------------------------------------------------------------------
NETMECH_TEST(rtp_parse_with_payload)
{
    std::uint8_t raw[14] = {
        0x80, 0x60, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0A,
        0x11, 0x22, 0x33, 0x44,
        'h', 'i'
    };

    std::error_code ec;
    auto pkt = RtpPacket::parse(raw, sizeof(raw), ec);
    REQUIRE(pkt.has_value());
    CHECK_EQ(pkt->payload().size(), 2u);
    CHECK_EQ(pkt->payload()[0], static_cast<std::uint8_t>('h'));
    CHECK_EQ(pkt->payload()[1], static_cast<std::uint8_t>('i'));
}

// ---------------------------------------------------------------------------
// 3. marker 位与 payload type 高位
// ---------------------------------------------------------------------------
NETMECH_TEST(rtp_marker_and_payload_type)
{
    const std::uint8_t raw[12] = {
        0x80, 0xE0,   // M=1, PT=96 (0x60 | 0x80)
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    std::error_code ec;
    auto pkt = RtpPacket::parse(raw, sizeof(raw), ec);
    REQUIRE(pkt.has_value());
    CHECK(pkt->marker());
    CHECK_EQ(pkt->payload_type(), 96);
}

// ---------------------------------------------------------------------------
// 4. CSRC 列表
// ---------------------------------------------------------------------------
NETMECH_TEST(rtp_parse_csrc_list)
{
    // CC=2 → 头 12 + 8 = 20 字节
    const std::uint8_t raw[20] = {
        0x82, 0x60,                    // V=2, CC=2
        0x00, 0x05,
        0x00, 0x00, 0x00, 0x64,
        0xAA, 0xBB, 0xCC, 0xDD,        // ssrc
        0x00, 0x00, 0x00, 0x01,        // csrc[0]
        0x00, 0x00, 0x00, 0x02         // csrc[1]
    };

    std::error_code ec;
    auto pkt = RtpPacket::parse(raw, sizeof(raw), ec);
    REQUIRE(pkt.has_value());
    CHECK_EQ(pkt->csrc_count(), 2u);
    REQUIRE(pkt->csrc_list().size() == 2);
    CHECK_EQ(pkt->csrc_list()[0], 1u);
    CHECK_EQ(pkt->csrc_list()[1], 2u);
    CHECK_EQ(pkt->payload().size(), 0u);
}

// ---------------------------------------------------------------------------
// 5. 扩展头
// ---------------------------------------------------------------------------
NETMECH_TEST(rtp_parse_extension)
{
    // X=1, 扩展 1 个 32 位字（4 字节）
    const std::uint8_t raw[20] = {
        0x90, 0x60,                    // V=2, X=1
        0x00, 0x07,
        0x00, 0x00, 0x00, 0x01,
        0x01, 0x02, 0x03, 0x04,
        0xBE, 0xEF,                    // extension profile = 0xBEEF
        0x00, 0x01,                    // extension length = 1 word
        0xAB, 0xCD, 0xEF, 0x12         // extension data（4 字节）
    };

    std::error_code ec;
    auto pkt = RtpPacket::parse(raw, sizeof(raw), ec);
    REQUIRE(pkt.has_value());
    CHECK(pkt->has_extension());
    CHECK_EQ(pkt->extension_profile(), 0xBEEFu);
    CHECK_EQ(pkt->extension_data().size(), 4u);
    CHECK_EQ(pkt->payload().size(), 0u);
}

// ---------------------------------------------------------------------------
// 6. padding
// ---------------------------------------------------------------------------
NETMECH_TEST(rtp_parse_padding)
{
    // P=1，载荷 2 字节 + 2 字节 padding（最后一字节 = 2）
    const std::uint8_t raw[16] = {
        0xA0, 0x60,                    // V=2, P=1
        0x00, 0x09,
        0x00, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x03,
        'a', 'b',
        0x00, 0x02                     // padding：1 个填充 + 长度字节 2
    };

    std::error_code ec;
    auto pkt = RtpPacket::parse(raw, sizeof(raw), ec);
    REQUIRE(pkt.has_value());
    CHECK(pkt->has_padding());
    CHECK_EQ(pkt->payload().size(), 2u);   // padding 已扣除
    CHECK_EQ(pkt->payload()[0], static_cast<std::uint8_t>('a'));
    CHECK_EQ(pkt->size(), 14u);            // raw 不含 padding
}

// ---------------------------------------------------------------------------
// 7. 非法输入：长度不足
// ---------------------------------------------------------------------------
NETMECH_TEST(rtp_parse_rejects_short_packet)
{
    std::error_code ec;
    const std::uint8_t raw[5] = {0x80, 0x60, 0, 0, 0};
    auto pkt = RtpPacket::parse(raw, sizeof(raw), ec);
    CHECK(ec);
    CHECK(!pkt.has_value());
    CHECK(ec == RtpErrc::invalid_packet);
}

// ---------------------------------------------------------------------------
// 8. 非法输入：版本号不是 2
// ---------------------------------------------------------------------------
NETMECH_TEST(rtp_parse_rejects_bad_version)
{
    const std::uint8_t raw[12] = {
        0x40, 0x60,   // V=1（非法）
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    std::error_code ec;
    auto pkt = RtpPacket::parse(raw, sizeof(raw), ec);
    CHECK(ec);
    CHECK(ec == RtpErrc::unsupported_version);
    CHECK(!pkt.has_value());
}

// ---------------------------------------------------------------------------
// 9. 非法输入：CSRC 数量超出实际长度
// ---------------------------------------------------------------------------
NETMECH_TEST(rtp_parse_rejects_truncated_csrc)
{
    const std::uint8_t raw[12] = {
        0x83, 0x60,   // CC=3 但后面没有 CSRC 数据
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    std::error_code ec;
    auto pkt = RtpPacket::parse(raw, sizeof(raw), ec);
    CHECK(ec);
    CHECK(!pkt.has_value());
}

// ---------------------------------------------------------------------------
// 10. 非法输入：padding 长度非法
// ---------------------------------------------------------------------------
NETMECH_TEST(rtp_parse_rejects_bad_padding)
{
    std::uint8_t raw[12] = {
        0xA0, 0x60,   // P=1
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    raw[11] = 200;   // padding 长度远大于包长
    std::error_code ec;
    auto pkt = RtpPacket::parse(raw, sizeof(raw), ec);
    CHECK(ec);
    CHECK(!pkt.has_value());
}

// ---------------------------------------------------------------------------
// 11. 构造 → 解析 往返
// ---------------------------------------------------------------------------
NETMECH_TEST(rtp_build_parse_roundtrip)
{
    RtpPacketInfo info;
    info.payload_type = 96;
    info.marker       = true;
    info.sequence     = 0xABCD;
    info.timestamp    = 90000;
    info.ssrc         = 0x11223344;

    const std::string payload_str = "hello-rtp";
    span<const std::uint8_t> payload(
        reinterpret_cast<const std::uint8_t*>(payload_str.data()),
        payload_str.size());

    std::error_code ec;
    const std::size_t need = rtp_packet_size(info, ec) + payload.size();
    REQUIRE(!ec);

    std::vector<std::uint8_t> buf(need + 8);
    const std::size_t written =
        build_rtp_packet(buf.data(), buf.size(), info, payload, ec);
    REQUIRE(!ec);
    CHECK_EQ(written, need);

    auto pkt = RtpPacket::parse(buf.data(), written, ec);
    REQUIRE(pkt.has_value());
    CHECK_EQ(pkt->payload_type(), 96);
    CHECK(pkt->marker());
    CHECK_EQ(pkt->sequence(), 0xABCD);
    CHECK_EQ(pkt->timestamp(), 90000u);
    CHECK_EQ(pkt->ssrc(), 0x11223344u);
    CHECK_EQ(pkt->payload().size(), payload_str.size());
    CHECK(std::memcmp(pkt->payload().data(), payload_str.data(),
                      payload_str.size()) == 0);
}

// ---------------------------------------------------------------------------
// 12. 构造：CSRC + 扩展 + padding 的复合往返
// ---------------------------------------------------------------------------
NETMECH_TEST(rtp_build_complex_roundtrip)
{
    const std::uint32_t csrc_arr[2] = {0xAAAA0001u, 0xBBBB0002u};
    const std::uint8_t  ext_arr[5]  = {1, 2, 3, 4, 5};   // 5 字节 → 补齐到 8

    RtpPacketInfo info;
    info.payload_type      = 97;
    info.sequence          = 100;
    info.timestamp         = 48000;
    info.ssrc              = 0x01020304;
    info.csrc_list         = span<const std::uint32_t>(csrc_arr, 2);
    info.extension_profile = 0x1234;
    info.extension_data    = span<const std::uint8_t>(ext_arr, 5);
    info.use_padding       = true;

    const std::string body = "xyz";
    span<const std::uint8_t> payload(
        reinterpret_cast<const std::uint8_t*>(body.data()), body.size());

    std::error_code ec;
    std::vector<std::uint8_t> buf(256);
    const std::size_t written =
        build_rtp_packet(buf.data(), buf.size(), info, payload, ec);
    REQUIRE(!ec);
    CHECK(written > 0);

    auto pkt = RtpPacket::parse(buf.data(), written, ec);
    REQUIRE(pkt.has_value());
    CHECK_EQ(pkt->csrc_count(), 2u);
    CHECK_EQ(pkt->csrc_list()[0], 0xAAAA0001u);
    CHECK_EQ(pkt->csrc_list()[1], 0xBBBB0002u);
    CHECK(pkt->has_extension());
    CHECK_EQ(pkt->extension_profile(), 0x1234);
    CHECK_EQ(pkt->extension_data().size(), 8u);   // 5 字节补齐到 8
    CHECK_EQ(pkt->extension_data()[0], 1u);
    CHECK_EQ(pkt->extension_data()[4], 5u);
    CHECK_EQ(pkt->extension_data()[5], 0u);       // 补零
    CHECK(pkt->has_padding());
    CHECK_EQ(pkt->payload().size(), 3u);
}

// ---------------------------------------------------------------------------
// 13. 构造：缓冲区不足应报错
// ---------------------------------------------------------------------------
NETMECH_TEST(rtp_build_buffer_too_small)
{
    RtpPacketInfo info;
    std::error_code ec;
    std::uint8_t small[4] = {0};
    const std::uint8_t data[10] = {0};

    const std::size_t written = build_rtp_packet(
        small, sizeof(small), info, span<const std::uint8_t>(data, 10), ec);
    CHECK_EQ(written, 0u);
    CHECK(ec);
    CHECK(ec == RtpErrc::buffer_too_small);
}

// ---------------------------------------------------------------------------
// 14. 构造：CSRC 超过 15 个应报错
// ---------------------------------------------------------------------------
NETMECH_TEST(rtp_build_rejects_too_many_csrc)
{
    std::uint32_t csrc_arr[16] = {0};   // 超过 kMaxCsrcCount(15)

    RtpPacketInfo info;
    info.csrc_list = span<const std::uint32_t>(csrc_arr, 16);

    std::error_code ec;
    (void)rtp_packet_size(info, ec);
    CHECK(ec);
    CHECK(ec == RtpErrc::invalid_argument);
}

// ---------------------------------------------------------------------------
// 15. 字节序验证：构造的字节必须是大端
// ---------------------------------------------------------------------------
NETMECH_TEST(rtp_build_network_byte_order)
{
    RtpPacketInfo info;
    info.sequence  = 0x0102;
    info.timestamp = 0x03040506;
    info.ssrc      = 0x0A0B0C0D;

    std::error_code ec;
    std::uint8_t buf[16] = {0};
    const std::size_t n = build_rtp_packet(
        buf, sizeof(buf), info, span<const std::uint8_t>(), ec);
    REQUIRE(!ec);
    CHECK_EQ(n, 12u);

    // 序列号 0x0102 → 字节 01 02
    CHECK_EQ(buf[2], 0x01);
    CHECK_EQ(buf[3], 0x02);
    // 时间戳 0x03040506 → 03 04 05 06
    CHECK_EQ(buf[4], 0x03);
    CHECK_EQ(buf[5], 0x04);
    CHECK_EQ(buf[6], 0x05);
    CHECK_EQ(buf[7], 0x06);
    // SSRC 0x0A0B0C0D → 0A 0B 0C 0D
    CHECK_EQ(buf[8], 0x0A);
    CHECK_EQ(buf[9], 0x0B);
    CHECK_EQ(buf[10], 0x0C);
    CHECK_EQ(buf[11], 0x0D);
}
