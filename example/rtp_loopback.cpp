// =============================================================================
//  rtp_loopback —— 同进程自测
// =============================================================================
//  一个进程里建两个会话：A 发、B 收，验证整条链路（端口成对、时间戳增量、
//  收发、统计、关闭）。跑完自己判断成败，可直接用于 CI。
//
//  用法：rtp_loopback [frames]
//  退出码：0 = 全部符合预期；1 = 有断言失败
// =============================================================================
#include <rtpmech/rtpmech.hpp>

#include <netmech/net.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace rtpmech;
using netmech::Address;

namespace {

using namespace std::chrono_literals;

class Collector : public RtpHandler {
public:
    void on_packet(RtpSession&, const RtpPacket& pkt, const Address&) override
    {
        std::lock_guard<std::mutex> lk(mtx);
        seqs.push_back(pkt.sequence());
        timestamps.push_back(pkt.timestamp());
        count.fetch_add(1);
    }

    std::mutex                 mtx;
    std::vector<std::uint16_t> seqs;
    std::vector<std::uint32_t> timestamps;
    std::atomic<int>           count{0};
};

class Silent : public RtpHandler {
public:
    void on_packet(RtpSession&, const RtpPacket&, const Address&) override {}
};

int g_fail = 0;

void check(bool cond, const char* what)
{
    if (!cond) {
        std::printf("  [FAIL] %s\n", what);
        ++g_fail;
    } else {
        std::printf("  [ ok ] %s\n", what);
    }
}

} // namespace

int main(int argc, char** argv)
{
    const int frames = (argc > 1) ? std::atoi(argv[1]) : 50;
    if (frames <= 0) {
        std::fprintf(stderr, "frames 必须 > 0\n");
        return 2;
    }

    if (!netmech::initialize(2)) {
        std::fprintf(stderr, "netmech initialize failed\n");
        return 1;
    }

    std::error_code ec;

    // ---- 接收端 ----
    RtpSessionConfig rx_cfg;
    rx_cfg.local        = Address("127.0.0.1", 0);
    rx_cfg.payload_type = 96;
    rx_cfg.clock_rate   = 90000;

    auto collector = std::make_shared<Collector>();
    auto rx = RtpSession::create(rx_cfg, collector, ec);
    if (!rx) {
        std::fprintf(stderr, "create receiver: %s\n", ec.message().c_str());
        return 1;
    }

    // ---- 发送端：25 fps @ 90 kHz → 增量 3600 ----
    RtpSessionConfig tx_cfg = rx_cfg;
    tx_cfg.remote              = Address("127.0.0.1", rx->local_rtp_port());
    tx_cfg.timestamp_increment = 3600;

    auto tx = RtpSession::create(tx_cfg, std::make_shared<Silent>(), ec);
    if (!tx) {
        std::fprintf(stderr, "create sender: %s\n", ec.message().c_str());
        return 1;
    }

    std::printf("rx: rtp=%u rtcp=%u   tx: rtp=%u rtcp=%u\n",
                rx->local_rtp_port(), rx->local_rtcp_port(),
                tx->local_rtp_port(), tx->local_rtcp_port());

    // ---- 发 ----
    std::vector<std::uint8_t> payload(512, 0x5A);
    int accepted = 0;
    for (int i = 0; i < frames; ++i) {
        if (tx->send({nullptr, payload.data(), payload.size()}, true) == SendResult::accepted) {
            ++accepted;
        }
        std::this_thread::sleep_for(2ms);      // 压一下，避免一次灌满
    }

    // ---- 等收齐（最多 3 秒）----
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline
           && collector->count.load() < accepted) {
        std::this_thread::sleep_for(1ms);
    }

    // ---- 断言 ----
    std::printf("\n--- 检查 ---\n");

    check(accepted == frames, "所有帧都被受理");
    check(collector->count.load() == accepted, "收到的帧数 == 发出的帧数");
    check(rx->local_rtp_port() % 2 == 0, "RTP 端口为偶数");
    check(rx->local_rtcp_port() == rx->local_rtp_port() + 1, "RTCP 端口 = RTP + 1");

    {
        std::lock_guard<std::mutex> lk(collector->mtx);
        bool seq_ok = collector->seqs.size() >= 2;
        for (std::size_t i = 1; i < collector->seqs.size() && seq_ok; ++i) {
            if (collector->seqs[i] != static_cast<std::uint16_t>(collector->seqs[i - 1] + 1)) {
                seq_ok = false;
            }
        }
        check(seq_ok, "序列号逐包递增、无乱序");

        bool ts_ok = collector->timestamps.size() >= 2;
        for (std::size_t i = 1; i < collector->timestamps.size() && ts_ok; ++i) {
            if (collector->timestamps[i] - collector->timestamps[i - 1] != 3600u) {
                ts_ok = false;
            }
        }
        check(ts_ok, "时间戳间隔恒为 3600（25fps @ 90kHz）");
    }

    const auto st = rx->stats();
    std::printf("\n--- 统计 ---\n");
    std::printf("rtp_sent=%llu  rtp_received=%llu  rtcp_sent=%llu  rtcp_received=%llu\n",
                (unsigned long long)tx->stats().rtp_sent,
                (unsigned long long)st.rtp_received,
                (unsigned long long)st.rtcp_sent,
                (unsigned long long)st.rtcp_received);
    std::printf("active_sources=%zu  parse_errors=%llu\n",
                st.active_sources, (unsigned long long)st.parse_errors);

    check(st.parse_errors == 0, "没有解析错误");
    check(st.active_sources >= 1, "跟踪到了远端源");

    tx->close();
    rx->close();
    netmech::shutdown();

    std::printf("\n%s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
