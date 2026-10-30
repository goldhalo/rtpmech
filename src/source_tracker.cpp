// =============================================================================
//  rtpmech/source_tracker.cpp —— 源跟踪实现
// =============================================================================
//  ★ 部分实现参考自 rtplib (JRTPLIB)
//      Copyright (c) 1999-2017 Jori Liesenborgs
//      MIT License — https://github.com/j0r1/JRTPLIB
//    本项目不链接 rtplib，仅参考其实现并重写为 C++17 风格。
//    详见项目根目录 NOTICE 文件。
//
//  算法依据 RFC 3550：
//    * 附录 A.1  序列号回绕与有效性检查
//    * 附录 A.3  expected / lost / fraction lost
//    * 附录 A.8  到达间隔抖动
//    * 第 6.4.1 节  LSR / DLSR 与 RTT
// =============================================================================

#include "source_tracker.hpp"

#include <algorithm>
#include <chrono>

namespace rtpmech {

// -----------------------------------------------------------------------------
// 时钟
// -----------------------------------------------------------------------------

std::int64_t steady_now_us() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// -----------------------------------------------------------------------------
// SourceStats
// -----------------------------------------------------------------------------

std::int64_t SourceStats::cumulative_lost() const noexcept
{
    if (received == 0) {
        return 0;
    }
    const std::int64_t expected =
        static_cast<std::int64_t>(ext_max_seq) -
        static_cast<std::int64_t>(base_seq) + 1;
    return expected - static_cast<std::int64_t>(received);
}

std::uint8_t SourceStats::fraction_lost() const noexcept
{
    // RFC 3550 A.3：丢包比例基于**本区间**计算，而不是累计值。
    //
    //   expected  = ext_max_seq - base_seq + 1   （首包到当前最大序号的总数）
    //   exp_i     = 当前 expected - 上次采样时的 expected
    //   rec_i     = 当前 received - 上次采样时的 received
    //   lost_i    = exp_i - rec_i
    //   fraction  = (lost_i << 8) / exp_i        （上限 255）
    const std::int64_t cur_expected =
        static_cast<std::int64_t>(ext_max_seq) -
        static_cast<std::int64_t>(base_seq) + 1;

    const std::int64_t exp_i =
        cur_expected - static_cast<std::int64_t>(expected_prior);
    const std::int64_t rec_i =
        static_cast<std::int64_t>(received) -
        static_cast<std::int64_t>(received_prior);

    if (exp_i <= 0) {
        return 0;
    }
    const std::int64_t lost_i = exp_i - rec_i;
    if (lost_i <= 0) {
        // 可能因重复包导致负数；按 RFC 规定置 0
        return 0;
    }
    const std::int64_t frac = (lost_i << 8) / exp_i;
    return static_cast<std::uint8_t>(std::min<std::int64_t>(frac, 255));
}

double SourceStats::jitter_ms(std::uint32_t clock_rate) const noexcept
{
    if (clock_rate == 0) {
        return 0.0;
    }
    // jitter_units 是"时间戳单位 × 16"，先还原再加到毫秒
    const double units = static_cast<double>(jitter_units) / 16.0;
    return units * 1000.0 / static_cast<double>(clock_rate);
}

// -----------------------------------------------------------------------------
// SourceTracker
// -----------------------------------------------------------------------------

SourceTracker::SourceTracker(std::uint32_t local_ssrc,
                             std::uint32_t clock_rate) noexcept
    : local_ssrc_(local_ssrc)
    , clock_rate_(clock_rate)
{
}

SourceStats& SourceTracker::get_or_create(std::uint32_t ssrc,
                                          std::int64_t now_us)
{
    auto it = sources_.find(ssrc);
    if (it != sources_.end()) {
        return it->second;
    }
    SourceStats st;
    st.ssrc             = ssrc;
    st.last_activity_us = now_us;
    auto res = sources_.emplace(ssrc, std::move(st));
    return res.first->second;
}

void SourceTracker::on_rtp_received(const RtpPacket& pkt,
                                    std::int64_t arrival_us)
{
    // 自己的包不计入
    if (local_ssrc_ != 0 && pkt.ssrc() == local_ssrc_) {
        return;
    }

    SourceStats& st = get_or_create(pkt.ssrc(), arrival_us);
    st.last_activity_us = arrival_us;
    st.is_sender        = true;

    const std::uint16_t seq = pkt.sequence();

    // ---- 序列号跟踪（RFC 3550 A.1）----
    if (st.received == 0) {
        // 首个包：初始化基准
        st.max_seq     = seq;
        st.cycles      = 0;
        st.base_seq    = seq;
        st.ext_max_seq = seq;
        st.received    = 1;
    } else {
        const std::uint16_t prev = st.max_seq;
        std::int32_t delta = 0;

        if (seq == prev) {
            // 重复包：只计入 received，不影响 max_seq
            ++st.received;
        } else if (seq < prev) {
            // 可能乱序或回绕
            if (static_cast<std::uint32_t>(prev) - static_cast<std::uint32_t>(seq) >
                32768U) {
                // 回绕：进入下一个周期
                ++st.cycles;
                delta = static_cast<std::int32_t>(seq) + 65536 - prev;
            } else {
                // 乱序（旧包）
                delta = static_cast<std::int32_t>(seq) - prev;
            }
        } else {
            // seq > prev
            if (static_cast<std::uint32_t>(seq) - static_cast<std::uint32_t>(prev) >
                32768U) {
                // 明显倒退（可能是大幅乱序或误判），按旧包处理
                delta = static_cast<std::int32_t>(seq) - prev;
            } else {
                delta = static_cast<std::int32_t>(seq) - prev;
            }
        }

        if (delta > 0) {
            st.max_seq     = seq;
            st.ext_max_seq = static_cast<std::uint64_t>(st.cycles) * 65536ULL + seq;
        }
        ++st.received;
    }

    // ---- 抖动（RFC 3550 A.8）----
    //
    //   D(i,j) = (Rj - Ri) - (Sj - Si)
    //       R: 到达时刻（换算到 RTP 时间戳单位）
    //       S: RTP 时间戳
    //   J(i)   = J(i-1) + (|D(i-1,i)| - J(i-1)) / 16
    //
    //   关键：R 必须换算成**时间戳单位**才能与 S 相减，
    //   因此需要 clock_rate（如视频 90000 Hz）。
    //   若未知 clock_rate，则无法计算抖动 —— 这是 RTP 的固有约束。
    //
    //   实现用定点：jitter_units 保存 J × 16，全程整数运算。
    if (clock_rate_ > 0 && st.has_prev_ts && st.has_prev_arrival) {
        // 到达间隔（us）换算到时间戳单位
        const std::int64_t arrival_delta_us = arrival_us - st.last_arrival_us;
        if (arrival_delta_us > 0) {
            const std::int64_t arrival_delta_ts =
                (arrival_delta_us * static_cast<std::int64_t>(clock_rate_)) / 1000000LL;

            // 发送间隔（时间戳单位，处理 32 位回绕）
            const std::int64_t ts_delta =
                static_cast<std::int64_t>(
                    static_cast<std::uint32_t>(pkt.timestamp() - st.last_rtp_ts));

            // D = 到达间隔 - 发送间隔
            const std::int64_t d = arrival_delta_ts - ts_delta;
            const std::int64_t abs_d = d < 0 ? -d : d;

            // J += (|D| - J) / 16
            st.jitter_units += (abs_d - st.jitter_units) / 16;
        }
    }

    st.last_rtp_ts      = pkt.timestamp();
    st.has_prev_ts      = true;
    st.last_arrival_us  = arrival_us;
    st.has_prev_arrival = true;
}

void SourceTracker::on_sender_report(const SenderReport& sr,
                                     std::int64_t arrival_us)
{
    if (local_ssrc_ != 0 && sr.ssrc == local_ssrc_) {
        return;
    }

    SourceStats& st = get_or_create(sr.ssrc, arrival_us);
    st.last_activity_us = arrival_us;

    NtpTime ntp;
    ntp.seconds  = sr.ntp_msw;
    ntp.fraction = sr.ntp_lsw;

    st.last_sr_ntp          = ntp;
    st.last_sr_rtp_ts       = sr.rtp_timestamp;
    st.last_sr_packet_count = sr.packet_count;
    st.last_sr_octet_count  = sr.octet_count;
    st.last_sr_recv_us      = arrival_us;
}

void SourceTracker::on_receiver_report(const ReceiverReport& rr,
                                       std::int64_t now_us)
{
    for (const ReportBlock& rb : rr.reports) {
        // 只关心"报告我的 SSRC"的块
        if (local_ssrc_ == 0 || rb.ssrc != local_ssrc_) {
            continue;
        }
        if (!has_local_sr_ || rb.lsr != local_last_sr_ntp_msw_) {
            continue;   // lsr 未匹配我们的最近一次 SR，无法计算 RTT
        }

        // DLSR 单位是 1/65536 秒 → 微秒：dlsr * 1e6 / 65536
        const std::uint64_t dlsr_us =
            (static_cast<std::uint64_t>(rb.dlsr) * 1000000ULL) / 65536ULL;

        const std::int64_t elapsed_us = now_us - local_last_sr_sent_us_;
        if (elapsed_us < 0) {
            continue;
        }
        const std::int64_t rtt =
            elapsed_us - static_cast<std::int64_t>(dlsr_us);
        if (rtt < 0 || rtt > static_cast<std::int64_t>(0xFFFFFFFFLL)) {
            continue;
        }
        local_rtt_us_ = static_cast<std::uint32_t>(rtt);
        has_rtt_      = true;
    }
}

void SourceTracker::on_bye(const ByeReport& bye)
{
    for (std::uint32_t ssrc : bye.ssrcs) {
        sources_.erase(ssrc);
    }
}

void SourceTracker::on_local_sr_sent(std::uint32_t ntp_msw,
                                     std::int64_t sent_us)
{
    local_last_sr_ntp_msw_ = ntp_msw;
    local_last_sr_sent_us_ = sent_us;
    has_local_sr_          = true;
}

std::size_t SourceTracker::expire_inactive(std::int64_t now_us,
                                           std::int64_t timeout_us)
{
    std::size_t removed = 0;
    for (auto it = sources_.begin(); it != sources_.end();) {
        if (now_us - it->second.last_activity_us > timeout_us) {
            it = sources_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

const SourceStats* SourceTracker::find(std::uint32_t ssrc) const noexcept
{
    auto it = sources_.find(ssrc);
    return it == sources_.end() ? nullptr : &it->second;
}

std::vector<SourceStats> SourceTracker::snapshot() const
{
    std::vector<SourceStats> out;
    out.reserve(sources_.size());
    for (const auto& kv : sources_) {
        out.push_back(kv.second);
    }
    return out;
}

std::size_t SourceTracker::sender_count() const noexcept
{
    std::size_t n = 0;
    for (const auto& kv : sources_) {
        if (kv.second.is_sender) {
            ++n;
        }
    }
    return n;
}

void SourceTracker::clear() noexcept
{
    sources_.clear();
    local_last_sr_ntp_msw_ = 0;
    local_last_sr_sent_us_ = 0;
    has_local_sr_          = false;
    local_rtt_us_          = 0;
    has_rtt_               = false;
}

std::vector<ReportBlock> SourceTracker::build_report_blocks() const
{
    // 按最近活跃排序，最多 31 个（RTCP RC 只有 5 位）
    std::vector<const SourceStats*> sorted;
    sorted.reserve(sources_.size());
    for (const auto& kv : sources_) {
        if (kv.second.received > 0 || kv.second.is_sender) {
            sorted.push_back(&kv.second);
        }
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const SourceStats* a, const SourceStats* b) {
                  return a->last_activity_us > b->last_activity_us;
              });
    if (sorted.size() > 31) {
        sorted.resize(31);
    }

    std::vector<ReportBlock> blocks;
    blocks.reserve(sorted.size());
    for (const SourceStats* st : sorted) {
        ReportBlock rb;
        rb.ssrc          = st->ssrc;
        rb.fraction_lost = st->fraction_lost();

        // packets_lost 是 24 位有符号
        const std::int64_t lost = st->cumulative_lost();
        if (lost > 0x7FFFFFLL) {
            rb.packets_lost = 0x7FFFFF;
        } else if (lost < -0x800000LL) {
            rb.packets_lost = -0x800000;
        } else {
            rb.packets_lost = static_cast<std::int32_t>(lost);
        }

        rb.highest_seq = static_cast<std::uint32_t>(st->ext_max_seq);
        rb.jitter      = st->jitter();

        // LSR = 最近 SR 的 NTP 时间中间 32 位
        if (st->last_sr_ntp.has_value()) {
            rb.lsr = (st->last_sr_ntp->seconds << 16) |
                     (st->last_sr_ntp->fraction >> 16);
        } else {
            rb.lsr = 0;
        }
        // DLSR = 从收到 SR 到现在的延迟，单位 1/65536 秒
        if (st->last_sr_recv_us > 0) {
            const std::int64_t now = steady_now_us();
            const std::int64_t d   = now - st->last_sr_recv_us;
            if (d > 0) {
                const std::uint64_t dlsr =
                    (static_cast<std::uint64_t>(d) * 65536ULL) / 1000000ULL;
                rb.dlsr = static_cast<std::uint32_t>(
                    dlsr > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : dlsr);
            }
        }
        blocks.push_back(rb);
    }
    return blocks;
}

} // namespace rtpmech
