// =============================================================================
//  rtpmech/rtcp_scheduler.hpp —— RTCP 发送调度（RFC 3550 第 6.2 节）
// =============================================================================
//  文件    : rtpmech/rtcp_scheduler.hpp
//  用途    : 计算下一次 RTCP 报文的发送时刻，把 RTCP 流量限制在会话带宽的
//            固定比例内，并随参与者数量自适应。
//  依赖    : 标准库
//  编码    : UTF-8
//
//  ── 部分实现参考自 rtplib ───────────────────────────────────────────────────
//  调度算法参考自 rtplib (JRTPLIB) 的 RTCPScheduler：
//      Copyright (c) 1999-2017 Jori Liesenborgs, MIT License
//      https://github.com/j0r1/JRTPLIB
//  本项目不链接 rtplib，仅参考其实现并重写为 C++17 风格。
//  详见项目根目录 NOTICE 文件。
//
//  ── 为什么需要调度（RFC 3550 §6.2）────────────────────────────────────────
//  RTCP 必须限制在**会话带宽的 5%** 以内（推荐值），否则大量参与者会把
//  网络压垮。具体规则：
//
//  1. **带宽分配**
//       RTCP 总带宽 = 会话带宽 × 0.05
//       其中 25% 留给发送者、75% 留给接收者。
//       若发送者比例 ≤ 25%，则接收者使用全部 75%；
//       若发送者比例 > 25%，发送者获得的份额按比例放大。
//
//  2. **基础间隔**
//       T = avg_rtcp_size × n / rtcp_bandwidth
//       其中 n 是参与者数量，avg_rtcp_size 是 RTCP 包平均大小。
//
//  3. **随机抖动**（防止同步）
//       Td = T × (0.5 + rand(0, 0.5) / 2^32)   → [0.5T, 1.5T]
//
//  4. **最小间隔**
//       T 不得小于 5 秒（首次发送除外，首次可减半）。
//
//  5. **规范化**（RFC 3550 附录 A.7）
//       若 T 小于最小间隔，则把 T 置为最小间隔，同时按比例
//       调整平均包大小，使长期平均速率仍然正确。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <random>

namespace rtpmech {

// -----------------------------------------------------------------------------
// 调度参数
// -----------------------------------------------------------------------------

/// RTCP 调度器的可调参数。
struct RtcpSchedulerParams {
    /// RTCP 占会话带宽的比例（RFC 3550 推荐 5%）。
    double rtcp_bandwidth_fraction = 0.05;

    /// 发送者占 RTCP 带宽的比例（RFC 3550 §6.2 推荐 25%）。
    double sender_bandwidth_fraction = 0.25;

    /// RTCP 发送间隔下限（秒）。RFC 3550 推荐 5 秒。
    double min_interval_sec = 5.0;

    /// 首次发送是否允许间隔减半（RFC 3550 §6.2 "initial" 标志）。
    bool use_half_at_startup = true;

    /// 初始平均 RTCP 包大小估计（字节）。会随实际发送自适应。
    std::size_t initial_avg_packet_size = 100;

    /// 输入参数合法性检查。
    [[nodiscard]] bool valid() const noexcept
    {
        return rtcp_bandwidth_fraction > 0.0 &&
               rtcp_bandwidth_fraction < 1.0 &&
               sender_bandwidth_fraction > 0.0 &&
               sender_bandwidth_fraction < 1.0 &&
               min_interval_sec >= 0.0 &&
               initial_avg_packet_size > 0;
    }
};

// -----------------------------------------------------------------------------
// 调度器
// -----------------------------------------------------------------------------

/// RTCP 发送调度器。
///
/// 用法：
/// ```cpp
/// RtcpScheduler sched;
/// sched.set_session_bandwidth(100000);     // 100 KB/s
/// sched.update_members(5, 2);              // 5 个参与者，其中 2 个发送者
///
/// // 每次发出 RTCP 包后：
/// sched.on_rtcp_sent(packet_size, now_sec);
///
/// // 每次准备发 RTP 前检查：
/// if (sched.due(now_sec)) {
///     ... 构造并发送 RTCP ...
/// }
/// ```
///
/// @note 非线程安全，应在单一线程（或 strand）上使用。
class RtcpScheduler {
public:
    explicit RtcpScheduler(RtcpSchedulerParams params = {}) noexcept;

    /// 设置会话带宽（字节/秒）。RTCP 使用其中 `rtcp_bandwidth_fraction` 的部分。
    void set_session_bandwidth(double bytes_per_sec) noexcept
    {
        session_bandwidth_ = bytes_per_sec > 0 ? bytes_per_sec : 0;
    }

    /// 当前会话带宽。
    [[nodiscard]] double session_bandwidth() const noexcept
    {
        return session_bandwidth_;
    }

    /// 更新参与者数量与其中发送者数量。
    ///
    /// @param[in] members 参与者总数（含自己）。
    /// @param[in] senders 其中正在发送 RTP 数据的数量。
    ///
    /// @note 参与者数量变化会立即影响下一次间隔计算（RFC 3550 要求）。
    void update_members(std::size_t members, std::size_t senders) noexcept;

    /// 当前参与者数量。
    [[nodiscard]] std::size_t members() const noexcept { return members_; }

    /// 当前发送者数量。
    [[nodiscard]] std::size_t senders() const noexcept { return senders_; }

    /// 记录一次 RTCP 发送，用于更新平均包大小并重算下次时刻。
    ///
    /// @param[in] packet_size 本次发出的 RTCP 复合包字节数。
    /// @param[in] now_sec     当前时刻（秒，单调时钟）。
    void on_rtcp_sent(std::size_t packet_size, double now_sec) noexcept;

    /// 是否到了该发 RTCP 的时刻。
    ///
    /// @param[in] now_sec 当前时刻（秒，单调时钟）。
    ///
    /// @note 返回 true 后**不会**自动重置；调用方发送成功后需调用
    ///       `on_rtcp_sent()`。若发送失败可再次检查 `due()`。
    [[nodiscard]] bool due(double now_sec) const noexcept
    {
        return now_sec >= next_send_sec_;
    }

    /// 距离下次发送还剩多少秒（可能为负，表示已逾期）。
    [[nodiscard]] double time_until_next(double now_sec) const noexcept
    {
        return next_send_sec_ - now_sec;
    }

    /// 计算理论上的"确定性间隔"（不含随机抖动）。诊断/测试用。
    [[nodiscard]] double deterministic_interval() const noexcept;

    /// 平均 RTCP 包大小（字节）。
    [[nodiscard]] double avg_packet_size() const noexcept
    {
        return avg_packet_size_;
    }

    /// 重置到初始状态（保留参数与带宽设置）。
    ///
    /// @param[in] now_sec 当前时刻。
    void reset(double now_sec) noexcept;

    /// 修改参数（会重算下次发送时刻）。
    void set_params(const RtcpSchedulerParams& p, double now_sec) noexcept;

    /// 当前参数。
    [[nodiscard]] const RtcpSchedulerParams& params() const noexcept
    {
        return params_;
    }

private:
    /// 按 RFC 3550 §6.2 与附录 A.7 计算下一间隔（含抖动与规范化）。
    void schedule_next(double now_sec) noexcept;

    RtcpSchedulerParams params_;
    double   session_bandwidth_ = 0.0;   ///< 字节/秒；0 = 未设置（用默认）
    std::size_t members_    = 1;
    std::size_t senders_    = 0;
    /// 平均 RTCP 包大小。声明为 mutable 的原因：
    /// RFC 3550 附录 A.7 的"规范化"要求：当计算出的间隔小于最小间隔时，
    /// 需**就地放大** avg_packet_size 以保持长期平均带宽 —— 这发生在
    /// `deterministic_interval()`（逻辑上的查询）内部。
    /// 把它当作缓存/自适应状态而非逻辑状态，因此 mutable。
    mutable double avg_packet_size_;
    double   next_send_sec_ = 0.0;
    bool     first_send_    = true;
    std::mt19937 rng_;
};

// -----------------------------------------------------------------------------
// 带宽常量
// -----------------------------------------------------------------------------

/// RFC 3550 推荐的 RTCP 带宽占比。
inline constexpr double kRtcpBandwidthFractionDefault = 0.05;

/// RFC 3550 推荐的发送者带宽占比。
inline constexpr double kRtcpSenderFractionDefault = 0.25;

/// RFC 3550 推荐的最小 RTCP 间隔（秒）。
inline constexpr double kRtcpMinIntervalSecDefault = 5.0;

/// 未显式设置会话带宽时使用的默认值（字节/秒）。
///
/// RFC 3550 §6.2 建议单会话带宽至少 10 kbps（这里取 10000 B/s 便于计算）。
inline constexpr double kRtcpDefaultSessionBandwidth = 10000.0;

} // namespace rtpmech
