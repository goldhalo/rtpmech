// =============================================================================
//  rtpmech/source_tracker.hpp —— RTP 参与者源跟踪
// =============================================================================
//  文件    : rtpmech/source_tracker.hpp
//  用途    : 跟踪 RTP 会话中各参与者的接收统计（丢包、抖动、RTT），
//            为 RTCP RR/SR 提供报告数据。
//  依赖    : 标准库 + rtcp_packet.hpp
//  编码    : UTF-8
//
//  ── 部分实现参考自 rtplib ───────────────────────────────────────────────────
//  源跟踪与统计逻辑参考自 rtplib (JRTPLIB)：
//      Copyright (c) 1999-2017 Jori Liesenborgs, MIT License
//      https://github.com/j0r1/JRTPLIB
//  本项目不链接 rtplib，仅参考其实现并重写为 C++17 风格。
//  详见项目根目录 NOTICE 文件。
//
//  ── 核心算法（RFC 3550）────────────────────────────────────────────────────
//
//  1. **序列号回绕**（RFC 3550 附录 A.1）
//     RTP 序列号是 16 位，会从 65535 回绕到 0。直接用算术差值会得到
//     巨大错误。正确做法是维护"扩展序列号"（64 位计数）：
//         if (seq < prev && prev - seq > 32768)  → 视为回绕，周期 +1
//         if (seq > prev && seq - prev > 32768)  → 视为乱序倒退，周期 -1
//
//  2. **抖动**（RFC 3550 附录 A.8）
//     D(i,j) = (Rj - Ri) - (Sj - Si)       （到达间隔差 - 发送间隔差）
//     J(i)   = J(i-1) + (|D(i-1,i)| - J(i-1)) / 16
//     用定点数实现：J 以 1/16 为单位累加，避免了浮点。
//
//  3. **丢包**（RFC 3550 附录 A.3）
//     expected = extended_max - extended_base + 1
//     lost     = expected - received
//     fraction = (lost_interval << 8) / expected_interval
//     （若 lost 为负，fraction 置 0 —— 可能是重复包导致）
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "rtpmech/rtcp_packet.hpp"
#include "rtpmech/rtp_packet.hpp"

namespace rtpmech {

// -----------------------------------------------------------------------------
// 单个源的接收统计
// -----------------------------------------------------------------------------

/// 一个远端源（SSRC）的接收状态与统计。
///
/// 由 `SourceTracker` 在收到 RTP 包时更新。
struct SourceStats {
    std::uint32_t ssrc = 0;

    // ---- 序列号跟踪（RFC 3550 A.1）----
    std::uint16_t max_seq        = 0;     ///< 已见的最大 16 位序列号
    std::uint32_t cycles         = 0;     ///< 回绕周期数
    std::uint64_t ext_max_seq    = 0;     ///< 扩展后的最大序列号
    std::uint64_t base_seq       = 0;     ///< 收到的第一个包的扩展序列号
    std::uint64_t received       = 0;     ///< 实际收到的包数
    std::uint64_t expected_prior = 0;     ///< 上次报告时的 expected
    std::uint64_t received_prior = 0;     ///< 上次报告时的 received

    // ---- 抖动（RFC 3550 A.8）----
    /// 到达间隔抖动，**以 1/16 时间戳单位**存储（RTCP 上报时右移 4 位）。
    std::int64_t jitter_units   = 0;
    std::uint32_t last_rtp_ts   = 0;      ///< 上一个包的时间戳
    bool          has_prev_ts   = false;
    /// 上一次包的到达时刻（用 steady_clock 的 us 计时，避免系统时钟跳变）。
    std::int64_t  last_arrival_us = 0;
    bool          has_prev_arrival = false;

    // ---- 发送者报告信息（来自 RTCP SR）----
    std::optional<NtpTime>    last_sr_ntp;      ///< 最近收到的 SR 的 NTP 时间
    std::uint32_t             last_sr_rtp_ts = 0;
    std::uint32_t             last_sr_packet_count = 0;
    std::uint32_t             last_sr_octet_count = 0;
    /// 收到该 SR 的本地时刻（us），用于计算 DLSR。
    std::int64_t              last_sr_recv_us = 0;

    // ---- 活跃时间 ----
    std::int64_t last_activity_us = 0;     ///< 最后一次收到包的时刻
    bool         is_sender        = false; ///< 是否在最近收到过 RTP 数据包

    // ---- 派生统计 ----

    /// 累计丢包数（可为负 —— 表示收到重复包）。
    [[nodiscard]] std::int64_t cumulative_lost() const noexcept;

    /// 抖动（时间戳单位，向下取整）。
    [[nodiscard]] std::uint32_t jitter() const noexcept
    {
        return static_cast<std::uint32_t>(jitter_units >> 4);
    }

    /// 抖动（毫秒），需要时钟频率。
    [[nodiscard]] double jitter_ms(std::uint32_t clock_rate) const noexcept;

    /// 本轮区间的丢包比例（0..255，RTCP fraction lost 格式）。
    [[nodiscard]] std::uint8_t fraction_lost() const noexcept;
};

// -----------------------------------------------------------------------------
// 源跟踪器
// -----------------------------------------------------------------------------

/// 跟踪一个 RTP 会话中的所有远端源。
///
/// 线程安全：**非线程安全**。调用方应保证在单一线程（或 strand）上使用。
/// 这与 netmech 的"同一连接回调串行"约定一致。
class SourceTracker {
public:
    /// @param[in] local_ssrc 本次会话的本地 SSRC（自己的包不计入统计）。
    /// @param[in] clock_rate RTP 时钟频率（如视频 90000、音频 8000/48000）。
    ///                       **抖动计算必需** —— 它是把到达时刻换算成
    ///                       RTP 时间戳单位的依据。为 0 时抖动恒为 0。
    explicit SourceTracker(std::uint32_t local_ssrc = 0,
                           std::uint32_t clock_rate = 90000) noexcept;

    /// 设置/修改 RTP 时钟频率。
    void set_clock_rate(std::uint32_t clock_rate) noexcept { clock_rate_ = clock_rate; }

    /// 当前 RTP 时钟频率。
    [[nodiscard]] std::uint32_t clock_rate() const noexcept { return clock_rate_; }

    /// 收到一个 RTP 包时调用，更新对应源的统计。
    ///
    /// @param[in] pkt 已解析的 RTP 包。
    /// @param[in] arrival_us 到达时刻（steady_clock 微秒）。
    void on_rtp_received(const RtpPacket& pkt, std::int64_t arrival_us);

    /// 收到一个 RTCP SR 时调用，记录发送者信息。
    ///
    /// @param[in] sr         发送者报告。
    /// @param[in] arrival_us 到达时刻（steady_clock 微秒）。
    void on_sender_report(const SenderReport& sr, std::int64_t arrival_us);

    /// 收到一个 RTCP RR 时调用，可用于计算 RTT。
    ///
    /// @param[in] rr        接收者报告。
    /// @param[in] now_us    当前时刻（steady_clock 微秒）。
    ///
    /// @note RTT 计算：若 RR 的 lsr 匹配我们最近发出的 SR，
    ///       RTT = (now - sr_sent_time) - dlsr（dlsr 单位 1/65536 秒）。
    void on_receiver_report(const ReceiverReport& rr, std::int64_t now_us);

    /// 收到 BYE 时移除对应源。
    void on_bye(const ByeReport& bye);

    /// 记录本地发出的 SR（用于后续 RTT 计算）。
    ///
    /// @param[in] ntp_msw   发出的 SR 的 NTP 高 32 位。
    /// @param[in] sent_us   发送时刻（steady_clock 微秒）。
    void on_local_sr_sent(std::uint32_t ntp_msw, std::int64_t sent_us);

    /// 清除超过 `timeout_us` 未活动的源（RFC 3550 建议 5 个报告间隔）。
    ///
    /// @return 被移除的源数量。
    std::size_t expire_inactive(std::int64_t now_us, std::int64_t timeout_us);

    /// 查询某个源的统计；不存在返回 nullptr。
    [[nodiscard]] const SourceStats* find(std::uint32_t ssrc) const noexcept;

    /// 所有源的快照（按 SSRC 排序）。
    [[nodiscard]] std::vector<SourceStats> snapshot() const;

    /// 当前跟踪的源数量。
    [[nodiscard]] std::size_t size() const noexcept { return sources_.size(); }

    /// 其中"发送者"的数量。
    [[nodiscard]] std::size_t sender_count() const noexcept;

    /// 本地 SSRC。
    [[nodiscard]] std::uint32_t local_ssrc() const noexcept { return local_ssrc_; }

    /// 清空全部状态。
    void clear() noexcept;

    /// 生成用于 RR/SR 的报告块列表（最多 31 个，按最近活跃排序）。
    [[nodiscard]] std::vector<ReportBlock> build_report_blocks() const;

private:
    SourceStats& get_or_create(std::uint32_t ssrc, std::int64_t now_us);

    std::uint32_t                local_ssrc_;
    std::uint32_t                clock_rate_;
    std::map<std::uint32_t, SourceStats> sources_;

    // 本地 SR 记录（用于 RTT）
    std::uint32_t local_last_sr_ntp_msw_ = 0;
    std::int64_t  local_last_sr_sent_us_ = 0;
    bool          has_local_sr_          = false;
    std::uint32_t local_rtt_us_          = 0;
    bool          has_rtt_               = false;

public:
    /// 最近计算出的 RTT（微秒）；尚无数据时返回 std::nullopt。
    [[nodiscard]] std::optional<std::uint32_t> rtt_us() const noexcept
    {
        return has_rtt_ ? std::optional<std::uint32_t>(local_rtt_us_) : std::nullopt;
    }
};

// -----------------------------------------------------------------------------
// 时钟辅助
// -----------------------------------------------------------------------------

/// 取当前单调时钟的微秒数（steady_clock，不受系统时间调整影响）。
[[nodiscard]] std::int64_t steady_now_us() noexcept;

} // namespace rtpmech
