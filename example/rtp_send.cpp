// =============================================================================
//  rtp_send —— 发送端示例
// =============================================================================
//  用法：rtp_send <dst_ip> <dst_port> [fps] [frames]
//        默认 fps = 25, frames = 250（10 秒）
//
//  按固定帧率发送固定大小的伪载荷。时间戳用固定增量（25fps @ 90kHz = 3600），
//  这是监控场景的正确做法 —— 不要用"发送时刻"算时间戳，否则编码延迟与
//  线程调度抖动都会被写进去，接收端播放会抖。
// =============================================================================
#include <rtpmech/rtpmech.hpp>

#include <netmech/net.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace rtpmech;
using netmech::Address;

namespace {

/// 只发不收：on_packet 是纯虚，必须实现，但发送端通常收不到包。
class Silent : public RtpHandler {
public:
    void on_packet(RtpSession&, const RtpPacket&, const Address&) override {}
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <dst_ip> <dst_port> [fps] [frames]\n", argv[0]);
        return 2;
    }
    const std::string dst_ip = argv[1];
    const std::uint16_t dst_port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    const int fps    = (argc > 3) ? std::atoi(argv[3]) : 25;
    const int frames = (argc > 4) ? std::atoi(argv[4]) : 250;

    if (fps <= 0 || frames <= 0) {
        std::fprintf(stderr, "fps 与 frames 必须 > 0\n");
        return 2;
    }

    if (!netmech::initialize(2)) {
        std::fprintf(stderr, "netmech initialize failed\n");
        return 1;
    }

    RtpSessionConfig cfg;
    cfg.local               = Address("0.0.0.0", 0);          // 系统分配，保证偶数
    cfg.remote              = Address(dst_ip, dst_port);
    cfg.payload_type        = 96;
    cfg.clock_rate          = 90000;
    // 关键：时间戳增量由**帧率**决定，不是"发送时刻"
    cfg.timestamp_increment = static_cast<std::uint32_t>(90000 / fps);
    cfg.session_bandwidth   = 1'000'000;                       // 1 Mbps，影响 RTCP 间隔

    std::error_code ec;
    auto sess = RtpSession::create(cfg, std::make_shared<Silent>(), ec);
    if (!sess) {
        std::fprintf(stderr, "create: %s\n", ec.message().c_str());
        netmech::shutdown();
        return 1;
    }

    std::printf("sending to %s:%u  rtp=%u rtcp=%u  ssrc=%08X  %d fps  %d frames\n",
                dst_ip.c_str(), dst_port,
                sess->local_rtp_port(), sess->local_rtcp_port(),
                sess->local_ssrc(), fps, frames);

    std::vector<std::uint8_t> payload(1024, 0x5A);
    const auto interval = std::chrono::microseconds(1000000 / fps);
    int accepted = 0;

    auto next = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        // 一帧一包：marker 置位表示"这是一帧的最后一个包"
        switch (sess->send({nullptr, payload.data(), payload.size()}, /*marker=*/true)) {
        case SendResult::accepted:
            ++accepted;
            break;
        case SendResult::closed:
            std::fprintf(stderr, "session closed\n");
            i = frames;
            break;
        case SendResult::invalid:
            std::fprintf(stderr, "invalid payload\n");
            i = frames;
            break;
        }

        next += interval;
        std::this_thread::sleep_until(next);
    }

    const auto st = sess->stats();
    std::printf("\n已发 %d 帧，库计数 rtp_sent=%llu rtcp_sent=%llu\n",
                accepted,
                (unsigned long long)st.rtp_sent,
                (unsigned long long)st.rtcp_sent);

    sess->close();      // 发 BYE
    netmech::shutdown();
    return 0;
}
