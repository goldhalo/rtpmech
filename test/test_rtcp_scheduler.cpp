// ============================================================================
//  test_rtcp_scheduler.cpp —— RTCP 调度测试（RFC 3550 §6.2）
//
//  验证：
//    * 基础间隔公式 T = avg_size × n / bandwidth
//    * 最小间隔 5 秒限制
//    * 随机抖动落在 [0.5T, 1.5T)
//    * 参与者数量变化的影响
//    * 平均包大小的 1/16 指数平均
// ============================================================================
#include "rtcp_scheduler.hpp"

#include "test_framework.hpp"

using namespace rtpmech;

// ---------------------------------------------------------------------------
// 1. 默认参数合理
// ---------------------------------------------------------------------------
NETMECH_TEST(sched_default_params_valid)
{
    RtcpSchedulerParams p;
    CHECK(p.valid());
    CHECK(p.rtcp_bandwidth_fraction > 0.049 && p.rtcp_bandwidth_fraction < 0.051);
    CHECK_EQ(p.min_interval_sec, 5.0);
    CHECK(p.use_half_at_startup);
}

// ---------------------------------------------------------------------------
// 2. 首次可立即发送
// ---------------------------------------------------------------------------
NETMECH_TEST(sched_first_send_immediate)
{
    RtcpScheduler s;
    s.reset(100.0);
    CHECK(s.due(100.0));
}

// ---------------------------------------------------------------------------
// 3. 确定间隔：参与者越多，间隔越长（线性）
// ---------------------------------------------------------------------------
NETMECH_TEST(sched_interval_scales_with_members)
{
    RtcpSchedulerParams p;
    p.min_interval_sec = 0.0;      // 关掉最小间隔便于观察公式
    p.use_half_at_startup = false;

    RtcpScheduler s(p);
    s.set_session_bandwidth(100000.0);   // 100 KB/s → RTCP 5 KB/s

    s.update_members(1, 0);
    const double t1 = s.deterministic_interval();

    s.update_members(10, 0);
    const double t10 = s.deterministic_interval();

    // T = avg_size × n / bw，n 从 1 → 10，间隔应约 10 倍
    CHECK(t10 > t1);
    CHECK(t10 > t1 * 5.0);    // 至少 5 倍（考虑最小间隔影响）
}

// ---------------------------------------------------------------------------
// 4. 最小间隔 5 秒生效
// ---------------------------------------------------------------------------
NETMECH_TEST(sched_min_interval_enforced)
{
    RtcpSchedulerParams p;
    p.min_interval_sec = 5.0;
    p.use_half_at_startup = false;

    RtcpScheduler s(p);
    s.set_session_bandwidth(1000000.0);   // 很大的带宽 → 理论间隔极小
    s.update_members(1, 0);

    const double t = s.deterministic_interval();
    CHECK(t >= 5.0);          // 必须被抬升到最小间隔
}

// ---------------------------------------------------------------------------
// 5. 带宽越小，间隔越长
// ---------------------------------------------------------------------------
NETMECH_TEST(sched_lower_bandwidth_longer_interval)
{
    RtcpSchedulerParams p;
    p.min_interval_sec = 0.0;
    p.use_half_at_startup = false;

    RtcpScheduler s1(p);
    s1.set_session_bandwidth(100000.0);
    s1.update_members(5, 0);
    const double fast = s1.deterministic_interval();

    RtcpScheduler s2(p);
    s2.set_session_bandwidth(10000.0);   // 带宽小 10 倍
    s2.update_members(5, 0);
    const double slow = s2.deterministic_interval();

    CHECK(slow > fast);
}

// ---------------------------------------------------------------------------
// 6. 随机抖动：实际间隔应落在 [0.5T, 1.5T) 区间内
// ---------------------------------------------------------------------------
NETMECH_TEST(sched_jitter_within_bounds)
{
    RtcpSchedulerParams p;
    p.min_interval_sec = 15.0;       // 保证 T 稳定为 15
    p.use_half_at_startup = false;

    // 多次采样验证抖动分布
    bool all_in_bounds = true;
    double min_seen = 1e9;
    double max_seen = 0.0;

    for (int i = 0; i < 50; ++i) {
        RtcpScheduler s(p);
        s.set_session_bandwidth(1000.0);
        s.update_members(1, 0);
        s.reset(0.0);

        // 触发一次调度
        s.on_rtcp_sent(100, 0.0);
        const double delta = s.time_until_next(0.0);

        // 注意：deterministic_interval 会受规范化影响，这里只验证范围
        // T ≈ 15（最小间隔），抖动后应在 [7.5, 22.5)
        if (delta < 7.4 || delta > 22.6) {
            all_in_bounds = false;
        }
        if (delta < min_seen) min_seen = delta;
        if (delta > max_seen) max_seen = delta;
    }

    CHECK(all_in_bounds);
    // 抖动应确实产生变化（不是恒定值）
    CHECK(max_seen - min_seen > 0.5);
}

// ---------------------------------------------------------------------------
// 7. 平均包大小：1/16 指数平均
// ---------------------------------------------------------------------------
NETMECH_TEST(sched_avg_packet_size_ema)
{
    RtcpSchedulerParams p;
    p.initial_avg_packet_size = 100;
    p.min_interval_sec = 0.0;

    RtcpScheduler s(p);
    s.set_session_bandwidth(100000.0);
    s.update_members(1, 0);

    const double before = s.avg_packet_size();
    CHECK_EQ(before, 100.0);

    // 发送一个 400 字节的包
    // 新 avg = 100 × 15/16 + 400 × 1/16 = 93.75 + 25 = 118.75
    s.on_rtcp_sent(400, 0.0);
    const double after = s.avg_packet_size();
    CHECK(after > before);
    CHECK(after < 200.0);          // 不会突增到 400
    CHECK(after > 110.0 && after < 130.0);
}

// ---------------------------------------------------------------------------
// 8. due() 与 time_until_next() 一致
// ---------------------------------------------------------------------------
NETMECH_TEST(sched_due_matches_remaining_time)
{
    RtcpSchedulerParams p;
    p.min_interval_sec = 10.0;
    p.use_half_at_startup = false;

    RtcpScheduler s(p);
    s.set_session_bandwidth(1000.0);
    s.update_members(1, 0);
    s.on_rtcp_sent(100, 0.0);

    const double remaining = s.time_until_next(0.0);
    CHECK(remaining > 0.0);
    CHECK(!s.due(0.0));
    CHECK(s.due(remaining));
    CHECK(s.due(remaining + 1.0));
}

// ---------------------------------------------------------------------------
// 9. 参与者数量变化立即影响间隔
// ---------------------------------------------------------------------------
NETMECH_TEST(sched_members_change_takes_effect)
{
    RtcpSchedulerParams p;
    p.min_interval_sec = 0.0;
    p.use_half_at_startup = false;

    RtcpScheduler s(p);
    s.set_session_bandwidth(100000.0);
    s.update_members(2, 0);
    const double t2 = s.deterministic_interval();

    s.update_members(20, 0);
    const double t20 = s.deterministic_interval();

    CHECK(t20 > t2);
}

// ---------------------------------------------------------------------------
// 10. 非法带宽参数被安全处理
// ---------------------------------------------------------------------------
NETMECH_TEST(sched_handles_zero_bandwidth)
{
    RtcpScheduler s;
    s.set_session_bandwidth(0.0);      // 非法：回退到默认
    s.update_members(1, 0);

    const double t = s.deterministic_interval();
    CHECK(t > 0.0);
    CHECK(t < 1e6);                    // 不应出现无穷大
}

// ---------------------------------------------------------------------------
// 11. 参与者 0 被归一化为 1
// ---------------------------------------------------------------------------
NETMECH_TEST(sched_zero_members_normalized)
{
    RtcpScheduler s;
    s.update_members(0, 0);
    CHECK_EQ(s.members(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// 12. 发送者数量不超过参与者数量
// ---------------------------------------------------------------------------
NETMECH_TEST(sched_senders_capped_at_members)
{
    RtcpScheduler s;
    s.update_members(3, 100);
    CHECK_EQ(s.senders(), std::size_t{3});
}
