// ============================================================================
//  test_h264.cpp —— H.264 RTP 分片（RFC 6184）
//
//  重点验三件事：
//    1. 小 NAL 走单包，大 NAL 走 FU-A
//    2. FU-A 的 S/E 位、类型、分片数据能拼回原 NAL
//    3. 一帧多 NAL 时，只有最后一个包 marker=true
// ============================================================================
#include <rtpmech/h264.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "test_framework.hpp"

using namespace rtpmech;
using namespace netmech_test;

namespace {

using Bytes = std::vector<std::uint8_t>;

/// 收集分片结果。
struct Out {
    std::vector<Bytes> payloads;
    std::vector<bool>  markers;
};

void collect(H264Packetizer& p, span<const std::uint8_t> frame, Out& out)
{
    p.on_frame(frame, [&out](span<const std::uint8_t> payload, bool marker) {
        out.payloads.emplace_back(payload.begin(), payload.end());
        out.markers.push_back(marker);
    });
}

/// 构造一个 NAL（含 1 字节头）。
Bytes make_nal(std::uint8_t type, std::size_t body_len)
{
    Bytes n;
    n.reserve(1 + body_len);
    n.push_back(static_cast<std::uint8_t>(0x60 | (type & 0x1F)));   // NRI=3
    for (std::size_t i = 0; i < body_len; ++i) {
        n.push_back(static_cast<std::uint8_t>(i & 0xFF));
    }
    return n;
}

/// 拼成一个 Annex-B 帧（4 字节起始码）。
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
// 1. 小 NAL → 单包，marker 正确
// ---------------------------------------------------------------------------
NETMECH_TEST(h264_small_nal_single_packet)
{
    H264Packetizer p;
    const auto nal = make_nal(1, 100);              // type=1（非 IDR 片）
    const auto frame = frame_of({nal});

    Out out;
    collect(p, span<const std::uint8_t>(frame.data(), frame.size()), out);

    REQUIRE(out.payloads.size() == 1u);
    CHECK_EQ(out.payloads[0].size(), nal.size());   // 原样，无额外头
    CHECK(std::memcmp(out.payloads[0].data(), nal.data(), nal.size()) == 0);
    CHECK(out.markers[0]);
}

// ---------------------------------------------------------------------------
// 2. 大 NAL → FU-A 分片，S/E 位与数据拼接正确
// ---------------------------------------------------------------------------
NETMECH_TEST(h264_large_nal_fu_a)
{
    H264Packetizer::Config cfg;
    cfg.max_packet_size = 1400;                     // payload 上限 1388
    H264Packetizer p(cfg);

    const auto nal = make_nal(5, 5000);             // type=5（IDR），远超单包
    const auto frame = frame_of({nal});

    Out out;
    collect(p, span<const std::uint8_t>(frame.data(), frame.size()), out);

    // 5000 字节 → 每片 1386 数据 → ceil(5000/1386) = 4 片
    REQUIRE(out.payloads.size() == 4u);

    // 第一片：S=1, E=0, type=FU-A
    CHECK_EQ(out.payloads.front()[0] & 0x1F, 28);       // FU indicator type = 28
    CHECK_EQ(out.payloads.front()[0] & 0x60, 0x60);     // NRI 保留
    CHECK(out.payloads.front()[1] & 0x80);              // S=1
    CHECK(!(out.payloads.front()[1] & 0x40));           // E=0
    CHECK_EQ(out.payloads.front()[1] & 0x1F, 5);        // 原 NAL type

    // 最后一片：S=0, E=1
    CHECK(!(out.payloads.back()[1] & 0x80));            // S=0
    CHECK(out.payloads.back()[1] & 0x40);               // E=1

    // marker 只在最后一片
    for (std::size_t i = 0; i + 1 < out.payloads.size(); ++i) {
        CHECK(!out.markers[i]);
    }
    CHECK(out.markers.back());

    // 把分片数据拼回去，应该等于原 NAL 去掉头
    Bytes rebuilt;
    for (const auto& pkt : out.payloads) {
        rebuilt.insert(rebuilt.end(), pkt.begin() + 2, pkt.end());
    }
    REQUIRE(rebuilt.size() == nal.size() - 1);
    CHECK(std::memcmp(rebuilt.data(), nal.data() + 1, rebuilt.size()) == 0);
}

// ---------------------------------------------------------------------------
// 3. 一帧多 NAL（SPS+PPS+IDR）：只有最后一个包 marker=true
// ---------------------------------------------------------------------------
NETMECH_TEST(h264_multi_nal_frame)
{
    H264Packetizer p;
    const auto sps = make_nal(7, 20);
    const auto pps = make_nal(8, 10);
    const auto idr = make_nal(5, 50);
    const auto frame = frame_of({sps, pps, idr});

    Out out;
    collect(p, span<const std::uint8_t>(frame.data(), frame.size()), out);

    REQUIRE(out.payloads.size() == 3u);
    CHECK(!out.markers[0]);
    CHECK(!out.markers[1]);
    CHECK(out.markers[2]);                          // 只有 IDR 那个是最后一个

    // 类型对得上
    CHECK_EQ(out.payloads[0][0] & 0x1F, 7);         // SPS
    CHECK_EQ(out.payloads[1][0] & 0x1F, 8);         // PPS
    CHECK_EQ(out.payloads[2][0] & 0x1F, 5);         // IDR
}

// ---------------------------------------------------------------------------
// 4. 3 字节起始码也能拆
// ---------------------------------------------------------------------------
NETMECH_TEST(h264_three_byte_start_code)
{
    H264Packetizer p;
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
// 5. 无起始码：整帧当单 NAL
// ---------------------------------------------------------------------------
NETMECH_TEST(h264_no_start_code_treated_as_single_nal)
{
    H264Packetizer p;
    const auto nal = make_nal(1, 30);   // 直接给裸 NAL

    Out out;
    collect(p, span<const std::uint8_t>(nal.data(), nal.size()), out);

    REQUIRE(out.payloads.size() == 1u);
    CHECK_EQ(out.payloads[0].size(), nal.size());
    CHECK(out.markers[0]);
}

// ---------------------------------------------------------------------------
// 6. 参数集带内发送可关闭
// ---------------------------------------------------------------------------
NETMECH_TEST(h264_can_suppress_parameter_sets)
{
    H264Packetizer::Config cfg;
    cfg.send_parameter_sets_inband = false;
    H264Packetizer p(cfg);

    const auto sps = make_nal(7, 20);
    const auto idr = make_nal(5, 50);
    const auto frame = frame_of({sps, idr});

    Out out;
    collect(p, span<const std::uint8_t>(frame.data(), frame.size()), out);

    REQUIRE(out.payloads.size() == 1u);             // SPS 被跳过
    CHECK_EQ(out.payloads[0][0] & 0x1F, 5);         // 只剩 IDR
    CHECK(out.markers[0]);
}

// ---------------------------------------------------------------------------
// 7. 空帧不崩、不输出
// ---------------------------------------------------------------------------
NETMECH_TEST(h264_empty_frame)
{
    H264Packetizer p;
    Out out;
    const std::uint8_t dummy = 0;
    p.on_frame(span<const std::uint8_t>(&dummy, 0),
               [&out](span<const std::uint8_t>, bool) { out.payloads.emplace_back(); });
    CHECK(out.payloads.empty());
}
