// =============================================================================
//  rtp_bench —— 多路 RTP 压测
// =============================================================================
//  用法：rtp_bench [路数] [fps] [包字节] [时长秒]
//        默认 200 路 / 25 fps / 1000 字节 / 10 秒
//
//  每路建一对会话（发送端 + 接收端），模拟 N 个摄像头同时推流。
//  这正好是"为什么不用 jrtplib"那个场景 —— 后者的每会话一线程模型在这里
//  会变成 N 条线程 + 每秒 N×100 次空转唤醒。
//
//  输出：建链耗时、收包总数/速率、丢包率、CPU、RSS。
// =============================================================================
#include <rtpmech/rtpmech.hpp>

#include <netmech/net.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace rtpmech;
using netmech::Address;

namespace {

using namespace std::chrono_literals;

/// 只计数，不做任何处理 —— 测的是库本身的开销。
class Counter : public RtpHandler {
public:
    void on_packet(RtpSession&, const RtpPacket&, const Address&) override
    {
        received.fetch_add(1, std::memory_order_relaxed);
    }
    std::atomic<long> received{0};
};

class Silent : public RtpHandler {
public:
    void on_packet(RtpSession&, const RtpPacket&, const Address&) override {}
};

/// 当前进程 CPU 时间（秒）。Linux 用 getrusage，其它平台返回 0。
double process_cpu_sec()
{
#if defined(__linux__)
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        return static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
               static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
    }
#endif
    return 0.0;
}

/// 当前 RSS（MB）。
double process_rss_mb()
{
#if defined(__linux__)
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        long total = 0;
        long resident = 0;
        const int got = std::fscanf(f, "%ld %ld", &total, &resident);
        std::fclose(f);
        if (got == 2) {
            return static_cast<double>(resident) *
                   static_cast<double>(::sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
        }
    }
#endif
    return 0.0;
}

} // namespace

int main(int argc, char** argv)
{
    const int streams  = (argc > 1) ? std::atoi(argv[1]) : 200;
    const int fps      = (argc > 2) ? std::atoi(argv[2]) : 25;
    const int pkt_size = (argc > 3) ? std::atoi(argv[3]) : 1000;
    const int seconds  = (argc > 4) ? std::atoi(argv[4]) : 10;

    if (streams <= 0 || fps <= 0 || pkt_size <= 0 || seconds <= 0) {
        std::fprintf(stderr, "参数必须 > 0\n");
        return 2;
    }

    // io 线程数：给足但不过量（4~8 是库建议的范围）
    const unsigned hw = std::thread::hardware_concurrency();
    const std::size_t io_threads = std::max<std::size_t>(4, std::min<std::size_t>(hw, 8));
    if (!netmech::initialize(io_threads)) {
        std::fprintf(stderr, "initialize failed\n");
        return 1;
    }

    std::printf("rtp_bench: %d 路 × %d fps × %d B, %d 秒, io 线程 %zu\n",
                streams, fps, pkt_size, seconds, io_threads);

    // ---- 建链 ----
    const double rss0 = process_rss_mb();
    const auto t_create0 = std::chrono::steady_clock::now();

    std::vector<std::shared_ptr<RtpSession>> txs;
    std::vector<std::shared_ptr<RtpSession>> rxs;    // 接收端也必须持有！
    std::vector<std::shared_ptr<Counter>>    counters;
    txs.reserve(streams);
    rxs.reserve(streams);
    counters.reserve(streams);

    RtpSessionConfig base;
    base.local        = Address("127.0.0.1", 0);
    base.payload_type = 96;
    base.clock_rate   = 90000;
    base.timestamp_increment = static_cast<std::uint32_t>(90000 / fps);
    base.max_packet_size = std::max(1400, pkt_size + 64);

    int created = 0;
    for (int i = 0; i < streams; ++i) {
        std::error_code ec;

        // 接收端
        auto counter = std::make_shared<Counter>();
        auto rx = RtpSession::create(base, counter, ec);
        if (!rx) {
            std::fprintf(stderr, "第 %d 路 rx 创建失败: %s\n", i, ec.message().c_str());
            break;
        }

        // 发送端：指向接收端
        auto tx_cfg = base;
        tx_cfg.remote = Address("127.0.0.1", rx->local_rtp_port());
        auto tx = RtpSession::create(tx_cfg, std::make_shared<Silent>(), ec);
        if (!tx) {
            std::fprintf(stderr, "第 %d 路 tx 创建失败: %s\n", i, ec.message().c_str());
            break;
        }

        rxs.push_back(rx);      // ★ 少了这句，rx 每轮析构，会话当场关闭
        txs.push_back(tx);
        counters.push_back(counter);
        ++created;
    }

    const double create_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_create0).count();
    const double rss1 = process_rss_mb();

    std::printf("建链: %d / %d 路，耗时 %.1f ms（%.2f ms/路），RSS %.1f MB (+%.1f)\n",
                created, streams, create_ms, created ? create_ms / created : 0.0,
                rss1, rss1 - rss0);

    if (created == 0) {
        netmech::shutdown();
        return 1;
    }

    // ---- 发送 ----
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(pkt_size), 0x5A);
    const auto interval = std::chrono::microseconds(1000000 / fps);
    const int total_frames = fps * seconds;

    const double cpu0 = process_cpu_sec();
    const auto t_run0 = std::chrono::steady_clock::now();

    auto next = std::chrono::steady_clock::now();
    long sent = 0;
    for (int f = 0; f < total_frames; ++f) {
        for (auto& tx : txs) {
            if (tx->send({nullptr, payload.data(), payload.size()}, true)
                == SendResult::accepted) {
                ++sent;
            }
        }
        next += interval;
        std::this_thread::sleep_until(next);
    }

    // 等接收侧消化（回环有积压）
    std::this_thread::sleep_for(500ms);

    const double run_sec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_run0).count();
    const double cpu_sec = process_cpu_sec() - cpu0;
    const double rss2 = process_rss_mb();

    long received = 0;
    for (auto& c : counters) {
        received += c->received.load(std::memory_order_relaxed);
    }

    // ---- 统计 ----
    std::printf("\n--- 结果 ---\n");
    std::printf("时长          : %.2f s\n", run_sec);
    std::printf("发出          : %ld 包（%.0f pkt/s）\n", sent, sent / run_sec);
    std::printf("收到          : %ld 包（%.0f pkt/s）\n", received, received / run_sec);
    std::printf("丢包率        : %.2f %%\n",
                sent > 0 ? 100.0 * static_cast<double>(sent - received) /
                               static_cast<double>(sent) : 0.0);
    std::printf("吞吐          : %.1f MB/s（载荷）\n",
                received * static_cast<double>(pkt_size) / run_sec / (1024.0 * 1024.0));
    std::printf("CPU           : %.2f s（%.1f %% of 1 核，%.1f %% of 全部）\n",
                cpu_sec, 100.0 * cpu_sec / run_sec,
                100.0 * cpu_sec / run_sec / static_cast<double>(hw));
    std::printf("RSS           : %.1f MB -> %.1f MB (+%.1f)\n", rss1, rss2, rss2 - rss1);
    std::printf("每路内存      : %.1f KB\n",
                created ? (rss2 - rss0) * 1024.0 / created : 0.0);

    for (auto& tx : txs) {
        tx->close();
    }
    for (auto& rx : rxs) {
        rx->close();
    }
    txs.clear();
    rxs.clear();
    counters.clear();
    netmech::shutdown();
    return 0;
}
