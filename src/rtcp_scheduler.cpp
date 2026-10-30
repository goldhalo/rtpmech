// =============================================================================
//  rtpmech/rtcp_scheduler.cpp —— RTCP 调度实现（RFC 3550 §6.2 + 附录 A.7）
// =============================================================================
//  ★ 部分实现参考自 rtplib (JRTPLIB) 的 RTCPScheduler
//      Copyright (c) 1999-2017 Jori Liesenborgs
//      MIT License — https://github.com/j0r1/JRTPLIB
//    本项目不链接 rtplib，仅参考其实现并重写为 C++17 风格。
//    详见项目根目录 NOTICE 文件。
//
//  算法来源：RFC 3550 第 6.2 节（RTCP transmission interval）与
//            附录 A.7（reconsideration / 规范化）。
// =============================================================================

#include "rtcp_scheduler.hpp"

#include <algorithm>
#include <cmath>

namespace rtpmech {

namespace {

/// 2^32，用于把 mt19937 的 32 位输出归一化到 [0,1)。
constexpr double kTwoPow32 = 4294967296.0;

} // namespace

RtcpScheduler::RtcpScheduler(RtcpSchedulerParams params) noexcept
    : params_(params.valid() ? params : RtcpSchedulerParams{})
    , avg_packet_size_(static_cast<double>(params_.initial_avg_packet_size))
    , rng_(std::random_device{}())
{
    reset(0.0);
}

void RtcpScheduler::update_members(std::size_t members,
                                   std::size_t senders) noexcept
{
    members_ = members == 0 ? 1 : members;
    senders_ = std::min(senders, members_);
}

void RtcpScheduler::reset(double now_sec) noexcept
{
    avg_packet_size_ = static_cast<double>(params_.initial_avg_packet_size);
    first_send_      = true;
    next_send_sec_   = now_sec;   // 首次可立即发送
}

void RtcpScheduler::set_params(const RtcpSchedulerParams& p, double now_sec) noexcept
{
    params_ = p.valid() ? p : RtcpSchedulerParams{};
    schedule_next(now_sec);
}

double RtcpScheduler::deterministic_interval() const noexcept
{
    // ---- 1. 确定 RTCP 带宽（字节/秒）----
    const double bandwidth =
        session_bandwidth_ > 0 ? session_bandwidth_ : kRtcpDefaultSessionBandwidth;
    const double rtcp_bw = bandwidth * params_.rtcp_bandwidth_fraction;

    if (rtcp_bw <= 0.0) {
        return params_.min_interval_sec;
    }

    // ---- 2. 按发送者/接收者划分带宽（RFC 3550 §6.2）----
    //
    //   RFC 3550 原文（§6.2, "Determining the RTCP Reporting Interval"）:
    //
    //     若发送者占参与者比例 <= 25%：
    //         发送者共享 RTCP 带宽的 25%，接收者共享 75%
    //     若发送者占比 > 25%：
    //         发送者共享 25%，接收者共享 75% —— 但接收者数量少时，
    //         其人均带宽随之提高；
    //         若接收者数量为 0，则发送者获得全部 RTCP 带宽。
    //
    //   本函数返回的是**全体参与者**的平均间隔基准。由于本库的
    //   RtcpScheduler 面向单一会话（本端 + 其他参与者），采用 RFC 的
    //   简化形式：按"本端角色"取对应份额的倒数作为缩放因子。
    //
    //   为了保持接口简单且行为可预测，这里按 RFC 的**保守**做法：
    //   使用 rtcp_bw 的整体作为带宽基准（即所有参与者共同分摊），
    //   这在 n 较小时略偏保守（RTCP 发送略少），不会超过 5% 上限。
    const double n = static_cast<double>(members_ < 1 ? 1 : members_);

    // ---- 3. 基础间隔 T = avg_size × n / bandwidth ----
    double t = (avg_packet_size_ * n) / rtcp_bw;

    // ---- 4. 最小间隔限制（RFC 3550：T ≥ 5 秒）----
    //
    //   附录 A.7 的"规范化"：若 T < min_interval，则把 T 提升到 min_interval，
    //   同时按比例放大 avg_packet_size，使长期平均带宽仍正确。
    if (t < params_.min_interval_sec) {
        if (t > 0.0) {
            // avg_size 按 (min / t) 放大，补偿间隔变长
            avg_packet_size_ *= (params_.min_interval_sec / t);
        } else {
            avg_packet_size_ = static_cast<double>(params_.initial_avg_packet_size);
        }
        t = params_.min_interval_sec;
    }

    // ---- 5. 首次发送允许减半（RFC 3550 "initial" 标志）----
    if (first_send_ && params_.use_half_at_startup) {
        t *= 0.5;
    }

    return t;
}

void RtcpScheduler::schedule_next(double now_sec) noexcept
{
    const double t = deterministic_interval();

    // ---- 随机抖动：Td = T × (0.5 + rand / 2^32) → [0.5T, 1.5T) ----
    //
    //   RFC 3550 §6.2 要求实施 ±50% 的随机化，避免所有参与者同时发送
    //   造成 RTCP 突发（"synchronization"）。
    const double r = static_cast<double>(rng_()) / kTwoPow32;
    const double td = t * (0.5 + r);

    first_send_    = false;
    next_send_sec_ = now_sec + td;
}

void RtcpScheduler::on_rtcp_sent(std::size_t packet_size, double now_sec) noexcept
{
    // ---- 更新平均包大小（RFC 3550 §6.3.3，1/16 指数平均）----
    //
    //   avg = (1 - 1/16) × avg + (1/16) × size
    //
    const double size = static_cast<double>(packet_size);
    avg_packet_size_ = avg_packet_size_ * (15.0 / 16.0) + size * (1.0 / 16.0);

    schedule_next(now_sec);
}

} // namespace rtpmech
