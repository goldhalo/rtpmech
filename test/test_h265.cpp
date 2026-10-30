// ============================================================================
//  test_h265.cpp —— H.265 RTP 分片（RFC 7798）
//
//  与 H264 的关键差异：NAL 头 2 字节、FU 头 3 字节（2 字节 PayloadHdr
//  + 1 字节 FU header）、参数集是 VPS/SPS/PPS 三个。
// ============================================================================
#include <rtpmech/h265.hpp>

#include <cstring>
#include <vector>

#include "test_framework.hpp"

using namespace rtpmech;
using namespace netmech_test;

namespace {

using Bytes = std::vector<std::uint8_t>;

struct Out {
    std::vector<Bytes> payloads;
    std::vector<bool>  markers;
};

void collect(H265Packetizer& p, span<const std::uint8_t> frame, Out& out)
{
    p.on_frame(frame, [&out](span<const std::uint8_t> payload, bool marker) {
        out.payloads.emplace_back(payload.begin(), payload.end());
        out.markers.push_back(marker);
    });
}

/// 构造 H265 NAL（2 字节头 + body）。
Bytes make_nal(std::uint8_t type, std::size_t body_len, std::uint8_t layer = 0,
               std::uint8_t tid = 1)
{
    Bytes n;
    n.reserve(2 + body_len);
    n.push_back(static_cast<std::uint8_t>((type & 0x3F) << 1 | ((layer >> 5) & 0x01)));
    n.push_back(static_cast<std::uint8_t>(((layer & 0x1F) << 3) | (tid & 0x07)));
    for (std::size_t i = 0; i < body_len; ++i) {
        n.push_back(static_cast<std::uint8_t>(i & 0xFF));
    }
    return n;
}

Bytes frame_of(const std::vector<Bytes>& nals)
{
    Bytes f;
    for (const auto& n : nals) {
        f.push_back(0); f.push_back(0); f.push_back(0); f.push_back(1);
        f.insert(f.end(), n.begin(), n.end());
    }
    return f;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. 小 NAL → 单包
// ---------------------------------------------------------------------------
NETMECH_TEST(h265_small_nal_single_packet)
{
    H265Packetizer p;
    const auto nal = make_nal(1, 100);   // type=1（TRAIL_R）
    const auto frame = frame_of({nal});

    Out out;
    collect(p, span<const std::uint8_t>(frame.data(), frame.size()), out);

    REQUIRE(out.payloads.size() == 1u);
    CHECK_EQ(out.payloads[0].size(), nal.size());
    CHECK(std::memcmp(out.payloads[0].data(), nal.data(), nal.size()) == 0);
    CHECK(out.markers[0]);
}

// ---------------------------------------------------------------------------
// 2. 大 NAL → FU 分片，头 3 字节、S/E 位对
// ---------------------------------------------------------------------------
NETMECH_TEST(h265_large_nal_fu)
{
    H265Packetizer::Config cfg;
    cfg.max_packet_size = 1400;          // payload 上限 1388
    H265Packetizer p(cfg);

    const auto nal = make_nal(19, 5000); // type=19（IDR_W_RADL）
    const auto frame = frame_of({nal});

    Out out;
    collect(p, span<const std::uint8_t>(frame.data(), frame.size()), out);

    // 5000 字节数据（去掉 2 字节头 = 4998），每片 1385 → ceil(4998/1385) = 4
    REQUIRE(out.payloads.size() == 4u);

    // 第一片：PayloadHdr type=49(FU)，FU header S=1 E=0，FuType=19
    const auto& f0 = out.payloads.front();
    CHECK_EQ((f0[0] >> 1) & 0x3F, 49);         // FU 类型
    CHECK(f0[2] & 0x80);                        // S=1
    CHECK(!(f0[2] & 0x40));                     // E=0
    CHECK_EQ(f0[2] & 0x3F, 19);                 // 原 NAL type

    // 最后一片：S=0 E=1
    const auto& fl = out.payloads.back();
    CHECK(!(fl[2] & 0x80));
    CHECK(fl[2] & 0x40);

    // marker 只在最后一片
    for (std::size_t i = 0; i + 1 < out.payloads.size(); ++i) {
        CHECK(!out.markers[i]);
    }
    CHECK(out.markers.back());

    // 数据能拼回去（跳过 3 字节分片头）
    Bytes rebuilt;
    for (const auto& pkt : out.payloads) {
        rebuilt.insert(rebuilt.end(), pkt.begin() + 3, pkt.end());
    }
    REQUIRE(rebuilt.size() == nal.size() - 2);
    CHECK(std::memcmp(rebuilt.data(), nal.data() + 2, rebuilt.size()) == 0);
}

// ---------------------------------------------------------------------------
// 3. 参数集 VPS/SPS/PPS + IDR：4 个包，只有最后一个 marker
// ---------------------------------------------------------------------------
NETMECH_TEST(h265_parameter_sets_and_idr)
{
    H265Packetizer p;
    const auto vps = make_nal(32, 15);
    const auto sps = make_nal(33, 20);
    const auto pps = make_nal(34, 8);
    const auto idr = make_nal(19, 50);
    const auto frame = frame_of({vps, sps, pps, idr});

    Out out;
    collect(p, span<const std::uint8_t>(frame.data(), frame.size()), out);

    REQUIRE(out.payloads.size() == 4u);
    CHECK(!out.markers[0]);
    CHECK(!out.markers[1]);
    CHECK(!out.markers[2]);
    CHECK(out.markers[3]);                       // 只有 IDR 是最后一个

    // 类型对得上（6 位）
    CHECK_EQ((out.payloads[0][0] >> 1) & 0x3F, 32);   // VPS
    CHECK_EQ((out.payloads[1][0] >> 1) & 0x3F, 33);   // SPS
    CHECK_EQ((out.payloads[2][0] >> 1) & 0x3F, 34);   // PPS
    CHECK_EQ((out.payloads[3][0] >> 1) & 0x3F, 19);   // IDR
}

// ---------------------------------------------------------------------------
// 4. LayerId / TID 在分片头里保留
// ---------------------------------------------------------------------------
NETMECH_TEST(h265_layer_and_tid_preserved)
{
    H265Packetizer::Config cfg;
    cfg.max_packet_size = 200;                   // 逼出 FU
    H265Packetizer p(cfg);

    const auto nal = make_nal(1, 500, /*layer=*/2, /*tid=*/3);
    const auto frame = frame_of({nal});

    Out out;
    collect(p, span<const std::uint8_t>(frame.data(), frame.size()), out);

    REQUIRE(out.payloads.size() >= 2u);

    // 每片的 PayloadHdr 应保留 layer/tid
    for (const auto& pkt : out.payloads) {
        const std::uint8_t layer = static_cast<std::uint8_t>(
            ((pkt[0] & 0x01) << 5) | ((pkt[1] >> 3) & 0x1F));
        const std::uint8_t tid = static_cast<std::uint8_t>(pkt[1] & 0x07);
        CHECK_EQ(layer, 2);
        CHECK_EQ(tid, 3);
    }
}

// ---------------------------------------------------------------------------
// 5. 参数集带内发送可关闭
// ---------------------------------------------------------------------------
NETMECH_TEST(h265_can_suppress_parameter_sets)
{
    H265Packetizer::Config cfg;
    cfg.send_parameter_sets_inband = false;
    H265Packetizer p(cfg);

    const auto vps = make_nal(32, 15);
    const auto idr = make_nal(19, 50);
    const auto frame = frame_of({vps, idr});

    Out out;
    collect(p, span<const std::uint8_t>(frame.data(), frame.size()), out);

    REQUIRE(out.payloads.size() == 1u);
    CHECK_EQ((out.payloads[0][0] >> 1) & 0x3F, 19);
    CHECK(out.markers[0]);
}

// ---------------------------------------------------------------------------
// 6. 3 字节起始码
// ---------------------------------------------------------------------------
NETMECH_TEST(h265_three_byte_start_code)
{
    H265Packetizer p;
    const auto n1 = make_nal(1, 10);
    const auto n2 = make_nal(1, 10);

    Bytes frame;
    frame.push_back(0); frame.push_back(0); frame.push_back(1);
    frame.insert(frame.end(), n1.begin(), n1.end());
    frame.push_back(0); frame.push_back(0); frame.push_back(1);
    frame.insert(frame.end(), n2.begin(), n2.end());

    Out out;
    collect(p, span<const std::uint8_t>(frame.data(), frame.size()), out);

    REQUIRE(out.payloads.size() == 2u);
    CHECK_EQ(out.payloads[0].size(), n1.size());
    CHECK_EQ(out.payloads[1].size(), n2.size());
}

// ---------------------------------------------------------------------------
// 7. 太短的 NAL（< 2 字节）被丢弃
// ---------------------------------------------------------------------------
NETMECH_TEST(h265_rejects_too_short_nal)
{
    H265Packetizer p;
    const std::uint8_t one[1] = {0x40};

    Out out;
    collect(p, span<const std::uint8_t>(one, 1), out);
    CHECK(out.payloads.empty());
}
