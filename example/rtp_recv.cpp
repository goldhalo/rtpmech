// =============================================================================
//  rtp_recv —— 接收端示例
// =============================================================================
//  用法：rtp_recv [port]
//        默认 port = 5000（RTCP 自动用 5001）
//
//  收到 RTP 包时打印 序号 / 时间戳 / SSRC / 载荷长度 / 来源。
//  RTCP 由库自动处理（发 SR/RR），应用不需要关心。
//  Ctrl+C 退出；退出时会发 RTCP BYE。
// =============================================================================
#include <rtpmech/rtpmech.hpp>

#include <netmech/net.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace rtpmech;
using netmech::Address;

namespace {

std::atomic<bool> g_stop{false};

void on_sigint(int) { g_stop.store(true); }

class Printer : public RtpHandler {
public:
    void on_packet(RtpSession&, const RtpPacket& pkt, const Address& from) override
    {
        // ★ pkt 只在本次回调内有效：想延后处理必须自己拷 payload
        std::printf("seq=%-5u ts=%-10u ssrc=%08X pt=%-3u len=%-5zu from=%s\n",
                    pkt.sequence(), pkt.timestamp(), pkt.ssrc(),
                    pkt.payload_type(), pkt.payload().size(),
                    from.to_string().c_str());
        count_.fetch_add(1);
    }

    int count() const { return count_.load(); }

private:
    std::atomic<int> count_{0};
};

} // namespace

int main(int argc, char** argv)
{
    const std::uint16_t port =
        (argc > 1) ? static_cast<std::uint16_t>(std::atoi(argv[1])) : 5000;

    if (!netmech::initialize(2)) {
        std::fprintf(stderr, "netmech initialize failed\n");
        return 1;
    }
    std::signal(SIGINT, on_sigint);

    RtpSessionConfig cfg;
    cfg.local        = Address("0.0.0.0", port);
    cfg.payload_type = 96;
    cfg.clock_rate   = 90000;

    auto printer = std::make_shared<Printer>();
    std::error_code ec;
    auto sess = RtpSession::create(cfg, printer, ec);
    if (!sess) {
        std::fprintf(stderr, "create: %s\n", ec.message().c_str());
        netmech::shutdown();
        return 1;
    }

    std::printf("listening  rtp=%u  rtcp=%u  ssrc=%08X\n",
                sess->local_rtp_port(), sess->local_rtcp_port(),
                sess->local_ssrc());
    std::printf("按 Ctrl+C 退出\n\n");

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    const auto st = sess->stats();
    std::printf("\n--- 统计 ---\n");
    std::printf("收到 RTP   : %d （库计数 %llu）\n",
                printer->count(), (unsigned long long)st.rtp_received);
    std::printf("收到 RTCP  : %llu\n", (unsigned long long)st.rtcp_received);
    std::printf("发出 RTCP  : %llu （SR/RR，由库自动发送）\n",
                (unsigned long long)st.rtcp_sent);
    std::printf("非法包     : %llu\n", (unsigned long long)st.parse_errors);
    std::printf("远端源数   : %zu\n", st.active_sources);
    if (st.rtt_us) {
        std::printf("RTT        : %.1f ms\n", *st.rtt_us / 1000.0);
    }

    sess->close();          // 会发 BYE
    netmech::shutdown();
    return 0;
}
