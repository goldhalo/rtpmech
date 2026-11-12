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

#include <rtpmech/export.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>
#include <cstddef>

#include "rtpmech/rtcp_packet.hpp"
#include "rtpmech/rtp_packet.hpp"

namespace rtpmech {

// -----------------------------------------------------------------------------
// 单个源的接收统计
// -----------------------------------------------------------------------------

/// 一个远端源（SSRC）的接收状态与统计。
///
/// 由 `SourceTracker` 在收到 RTP 包时更新。
struct RTPMECH_API SourceStats {
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
class RTPMECH_API SourceTracker {
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
    /// @note 纯接收者（只发 RR、不发 RTP）也会进成员表：否则广播场景
    ///       成员数被严重低估，RTCP 合计带宽会击穿 5% 上限（RFC 3550 §6.2）。
    void on_receiver_report(const ReceiverReport& rr, std::int64_t now_us);

    /// 收到 SDES 时调用：把各 chunk 的 SSRC 纳入成员表（不置 is_sender）。
    /// 捕获还没发过 SR/RR 的成员，保证成员数完整。
    void on_sdes(const SdesReport& sdes, std::int64_t now_us);

    /// 收到 BYE 时移除对应源。
    void on_bye(const ByeReport& bye);

    /// 记录本地发出的 SR（用于后续 RTT 计算）。
    ///
    /// @param[in] sr_lsr    发出的 SR 的 **LSR 形式** —— NTP 时间戳的中间 32 位，
    ///                      即 `(seconds << 16) | (fraction >> 16)`。
    ///                      **不是** NTP 的高 32 位：对端在 RR 里回填的 `lsr`
    ///                      就是这个中间 32 位（RFC 3550 §6.4.1），两边必须同构，
    ///                      否则匹配永远失败、RTT 永远是 nullopt。
    /// @param[in] sent_us   发送时刻（steady_clock 微秒）。
    void on_local_sr_sent(std::uint32_t sr_lsr, std::int64_t sent_us);

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

    /// 更新本地 SSRC（SSRC 冲突换号时用）。
    ///
    /// 构造时如果拿不到 SSRC（传 0），`on_receiver_report()` 会把所有 RR 跳过
    /// —— 它靠这个值挑"报告我的块"。所以会话层必须在**确定 SSRC 时**同步过来，
    /// 换号时也要同步，否则 RTT 匹配立刻断掉。
    void set_local_ssrc(std::uint32_t ssrc) noexcept { local_ssrc_ = ssrc; }

    /// 清空全部状态。
    void clear() noexcept;

    /// 生成用于 RR/SR 的报告块列表（按最近活跃排序）。
    ///
    /// @param[in] limit 最多生成几个（RTCP RC 只有 5 位，硬上限 31）。
    ///                  调用方按 MTU 预算传入更小的值（RFC 3550 §6.2：
    ///                  装不下时截断报告块数量，而不是整包放弃）。
    [[nodiscard]] std::vector<ReportBlock> build_report_blocks(std::size_t limit = 31) const;

    /// 报告成功发出后调用：对**被报告的源**采样 expected/received，
    /// 作为下一区间 fraction lost 的基线（RFC 3550 A.3）。
    ///
    /// 不调用的话 expected_prior/received_prior 永远是 0，fraction lost
    /// 退化成累计丢包率：早期一次丢包突发后即使链路恢复干净也居高不下。
    void commit_report_sampling(const std::vector<ReportBlock>& reported);

private:
    SourceStats& get_or_create(std::uint32_t ssrc, std::int64_t now_us);

    /// 从一组报告块里挑出"报告本端 SSRC"的那块算 RTT。
    ///
    /// SR 和 RR 都携带报告块（RFC 3550 §6.4.1），所以两个入口共用这一份逻辑
    /// —— 只处理 RR 的话，双方都在发的会话永远算不出 RTT。
    void handle_report_blocks(const std::vector<ReportBlock>& blocks,
                              std::int64_t now_us);

    std::uint32_t                local_ssrc_;
    std::uint32_t                clock_rate_;
    std::map<std::uint32_t, SourceStats> sources_;

    // 本地 SR 记录（用于 RTT）
    /// 最近一次本地 SR 的 LSR 形式（NTP 中间 32 位），用来匹配对端 RR 里的 lsr。
    std::uint32_t local_last_sr_lsr_ = 0;
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
