// =============================================================================
//  rtpmech/rtcp_packet.cpp —— RTCP 报文解析与构造实现
// =============================================================================
//  ★ 部分实现参考自 rtplib (JRTPLIB)
//      Copyright (c) 1999-2017 Jori Liesenborgs
//      MIT License — https://github.com/j0r1/JRTPLIB
//    本项目不链接 rtplib，仅参考其实现并重写为 C++17 风格。
//    详见项目根目录 NOTICE 文件。
//
//  与 rtplib 的实现差异：
//    * 不使用 C 位域（端序无关）；
//    * 解析结果为**值类型**（SenderReport / ReceiverReport 等），
//      rtplib 则用带虚函数的类层次 + 内存管理器；
//    * 长度字段的 "(bytes/4)-1" 编码集中在一处处理，避免散落。
// =============================================================================

#include "rtpmech/rtcp_packet.hpp"

#include <cstring>

namespace rtpmech {

// -----------------------------------------------------------------------------
// 字节序辅助
// -----------------------------------------------------------------------------

namespace {

inline std::uint16_t rd16(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

inline std::uint32_t rd32(const std::uint8_t* p) noexcept
{
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

inline void wr16(std::uint8_t* p, std::uint16_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

inline void wr32(std::uint8_t* p, std::uint32_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

/// 32 位字对齐（向上取整）。
inline std::size_t align4(std::size_t n) noexcept
{
    return (n + 3) & ~static_cast<std::size_t>(3);
}

/// 读 24 位有符号整数（packets_lost 用）。
inline std::int32_t rd24s(const std::uint8_t* p) noexcept
{
    std::int32_t v = (static_cast<std::int32_t>(p[0]) << 16) |
                     (static_cast<std::int32_t>(p[1]) << 8) |
                     static_cast<std::int32_t>(p[2]);
    // 24 位符号扩展
    if (v & 0x800000) {
        v |= static_cast<std::int32_t>(0xFF000000);
    }
    return v;
}

inline void wr24s(std::uint8_t* p, std::int32_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>(v & 0xFF);
}

} // namespace

// -----------------------------------------------------------------------------
// 类型名
// -----------------------------------------------------------------------------

const char* rtcp_type_name(RtcpType t) noexcept
{
    switch (t) {
    case RtcpType::sender_report:    return "SR";
    case RtcpType::receiver_report:  return "RR";
    case RtcpType::sdes:             return "SDES";
    case RtcpType::bye:              return "BYE";
    case RtcpType::app:              return "APP";
    case RtcpType::rtpfb:            return "RTPFB";
    case RtcpType::payload_specific: return "PSFB";
    }
    return "UNKNOWN";
}

// -----------------------------------------------------------------------------
// RtcpPacket 访问器
// -----------------------------------------------------------------------------

const SenderReport* RtcpPacket::as_sender_report() const noexcept
{
    return sr_ ? &*sr_ : nullptr;
}

const ReceiverReport* RtcpPacket::as_receiver_report() const noexcept
{
    return rr_ ? &*rr_ : nullptr;
}

const SdesReport* RtcpPacket::as_sdes() const noexcept
{
    return sdes_ ? &*sdes_ : nullptr;
}

const ByeReport* RtcpPacket::as_bye() const noexcept
{
    return bye_ ? &*bye_ : nullptr;
}

const AppReport* RtcpPacket::as_app() const noexcept
{
    return app_ ? &*app_ : nullptr;
}

void RtcpPacket::clear_payload() noexcept
{
    sr_.reset();
    rr_.reset();
    sdes_.reset();
    bye_.reset();
    app_.reset();
}

// -----------------------------------------------------------------------------
// 单个 RTCP 报文解析
// -----------------------------------------------------------------------------

namespace {

/// 解析报告块列表（SR/RR 共用）。
bool parse_report_blocks(const std::uint8_t* p, std::size_t count,
                         std::size_t avail, std::vector<ReportBlock>& out) noexcept
{
    const std::size_t need = count * 24;   // 每个报告块 24 字节
    if (avail < need) {
        return false;
    }
    out.clear();
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* b = p + i * 24;
        ReportBlock rb;
        rb.ssrc          = rd32(b + 0);
        rb.fraction_lost = b[4];
        rb.packets_lost  = rd24s(b + 5);
        rb.highest_seq   = rd32(b + 8);
        rb.jitter        = rd32(b + 12);
        rb.lsr           = rd32(b + 16);
        rb.dlsr          = rd32(b + 20);
        out.push_back(rb);
    }
    return true;
}

} // namespace

std::optional<RtcpPacket>
RtcpPacket::parse_single(const std::uint8_t* data, std::size_t len,
                         std::size_t& consumed, std::error_code& ec) noexcept
{
    ec.clear();
    consumed = 0;

    // ---- 通用头：至少 4 字节 ----
    if (!data || len < kRtcpCommonHeaderSize) {
        ec = make_error_code(RtpErrc::invalid_packet);
        return std::nullopt;
    }

    const std::uint8_t b0 = data[0];
    const std::uint8_t version = static_cast<std::uint8_t>((b0 >> 6) & 0x03);
    const bool         padding = ((b0 >> 5) & 0x01) != 0;
    const std::uint8_t count   = static_cast<std::uint8_t>(b0 & 0x1F);  // RC

    if (version != kRtpVersion) {
        ec = make_error_code(RtpErrc::unsupported_version);
        return std::nullopt;
    }

    const std::uint8_t pt = data[1];

    // Length 字段：32 位字数量减一
    const std::uint16_t length_words = rd16(data + 2);
    const std::size_t   packet_len =
        (static_cast<std::size_t>(length_words) + 1) * kRtcpWordSize;

    if (packet_len < kRtcpCommonHeaderSize || len < packet_len) {
        ec = make_error_code(RtpErrc::invalid_packet);
        return std::nullopt;
    }

    RtcpPacket pkt;
    pkt.type_    = static_cast<RtcpType>(pt);
    pkt.padding_ = padding;

    // padding 处理：末尾 padding 字节不计入有效长度
    std::size_t body_end = packet_len;
    if (padding) {
        const std::uint8_t pad = data[packet_len - 1];
        if (pad == 0 || static_cast<std::size_t>(pad) > packet_len - kRtcpCommonHeaderSize) {
            ec = make_error_code(RtpErrc::invalid_packet);
            return std::nullopt;
        }
        body_end = packet_len - pad;
    }

    pkt.raw_     = data;
    pkt.raw_len_ = body_end;

    const std::uint8_t* body = data + kRtcpCommonHeaderSize;
    const std::size_t   body_avail = body_end - kRtcpCommonHeaderSize;

    switch (pkt.type_) {
    case RtcpType::sender_report: {
        // SR: 24 字节发送者信息 + RC 个报告块
        if (body_avail < 24) {
            ec = make_error_code(RtpErrc::invalid_packet);
            return std::nullopt;
        }
        SenderReport sr;
        sr.ssrc          = rd32(body + 0);
        sr.ntp_msw       = rd32(body + 4);
        sr.ntp_lsw       = rd32(body + 8);
        sr.rtp_timestamp = rd32(body + 12);
        sr.packet_count  = rd32(body + 16);
        sr.octet_count   = rd32(body + 20);
        if (!parse_report_blocks(body + 24, count, body_avail - 24, sr.reports)) {
            ec = make_error_code(RtpErrc::invalid_packet);
            return std::nullopt;
        }
        pkt.sr_ = std::move(sr);
        break;
    }

    case RtcpType::receiver_report: {
        // RR: 4 字节 SSRC + RC 个报告块
        if (body_avail < 4) {
            ec = make_error_code(RtpErrc::invalid_packet);
            return std::nullopt;
        }
        ReceiverReport rr;
        rr.ssrc = rd32(body + 0);
        if (!parse_report_blocks(body + 4, count, body_avail - 4, rr.reports)) {
            ec = make_error_code(RtpErrc::invalid_packet);
            return std::nullopt;
        }
        pkt.rr_ = std::move(rr);
        break;
    }

    case RtcpType::sdes: {
        // SDES: RC 个块，每块 = SSRC(4) + [类型(1)+长度(1)+值]... + END(1)
        SdesReport sdes;
        std::size_t off = 0;
        for (std::uint8_t c = 0; c < count; ++c) {
            if (off + 4 > body_avail) {
                ec = make_error_code(RtpErrc::invalid_packet);
                return std::nullopt;
            }
            SdesChunk chunk;
            chunk.ssrc = rd32(body + off);
            off += 4;

            bool ended = false;
            // ★ 循环条件必须是 off < body_avail（不是 off+2 <= body_avail）：
            //   END 只占 1 字节，当它恰好落在块的最后 1 个字节时（块长正好 4 字节
            //   对齐、无 padding），用 off+2 判断会直接跳过循环，导致把合法的
            //   SDES 判成非法。实测：cname 长度使 chunk 恰好 24 字节时必现。
            while (off < body_avail) {
                const std::uint8_t item_type = body[off];

                if (item_type == static_cast<std::uint8_t>(SdesType::end)) {
                    off += 1;   // END 只有 1 字节
                    ended = true;
                    break;
                }
                if (off + 2 > body_avail) {
                    ec = make_error_code(RtpErrc::invalid_packet);
                    return std::nullopt;
                }
                const std::uint8_t item_len = body[off + 1];
                if (off + 2 + item_len > body_avail) {
                    ec = make_error_code(RtpErrc::invalid_packet);
                    return std::nullopt;
                }
                std::string value(
                    reinterpret_cast<const char*>(body + off + 2), item_len);

                if (item_type == static_cast<std::uint8_t>(SdesType::cname)) {
                    chunk.cname = std::move(value);
                } else {
                    chunk.items.emplace_back(
                        static_cast<SdesType>(item_type), std::move(value));
                }
                off += 2 + item_len;
            }
            if (!ended) {
                ec = make_error_code(RtpErrc::invalid_packet);
                return std::nullopt;
            }
            // 每个块补齐到 32 位字边界（相对于整个包起点，即 body 偏移 4）
            const std::size_t abs = kRtcpCommonHeaderSize + off;
            off = align4(abs) - kRtcpCommonHeaderSize;

            sdes.chunks.push_back(std::move(chunk));
        }
        pkt.sdes_ = std::move(sdes);
        break;
    }

    case RtcpType::bye: {
        // BYE: RC 个 SSRC + 可选 reason（长度字节 + 文本）
        ByeReport bye;
        const std::size_t need = static_cast<std::size_t>(count) * 4;
        if (body_avail < need) {
            ec = make_error_code(RtpErrc::invalid_packet);
            return std::nullopt;
        }
        bye.ssrcs.reserve(count);
        for (std::uint8_t i = 0; i < count; ++i) {
            bye.ssrcs.push_back(rd32(body + i * 4));
        }
        if (body_avail > need) {
            const std::size_t reason_off = need;
            const std::uint8_t reason_len = body[reason_off];
            if (reason_off + 1 + reason_len <= body_avail) {
                bye.reason.assign(
                    reinterpret_cast<const char*>(body + reason_off + 1),
                    reason_len);
            }
        }
        pkt.bye_ = std::move(bye);
        break;
    }

    case RtcpType::app: {
        // APP: SSRC(4) + name(4) + data
        if (body_avail < 8) {
            ec = make_error_code(RtpErrc::invalid_packet);
            return std::nullopt;
        }
        AppReport app;
        app.subtype = count;   // APP 的 subtype 使用 RC 字段
        app.ssrc    = rd32(body + 0);
        std::memcpy(app.name.data(), body + 4, 4);

        // ★ APP 报文的数据长度：协议层面**不可恢复**
        //
        //   RFC 3550 的 APP 报文没有数据长度字段 —— 数据长度由通用头的
        //   Length 推出，但 RTCP 包长必须 4 字节对齐，发送方会把不足 4 字节
        //   的部分用零补齐。
        //
        //   因此解析端只能拿到"补齐后的长度"。例如发送 5 字节数据：
        //       构造: [头4][ssrc4][name4][data5][pad3] = 20 字节
        //       解析: 只能得到 8 字节 data（5 真实 + 3 补齐零）
        //
        //   本实现**如实**返回补齐后的全部字节（与 rtplib 行为一致），
        //   而不是猜测哪个零是补齐 —— 猜测会在"数据本身以零结尾"时
        //   错误地截断用户数据，那是更严重的问题。
        //
        //   调用方若需要精确长度，应自行在 data 内加长度前缀。
        app.data.assign(body + 8, body + 8 + (body_avail - 8));
        pkt.app_ = std::move(app);
        break;
    }

    default:
        // 未知类型：保留 raw 视图，便于上层诊断；不算错误
        break;
    }

    consumed = packet_len;
    return pkt;
}

// -----------------------------------------------------------------------------
// 复合包解析
// -----------------------------------------------------------------------------

std::optional<RtcpCompoundPacket>
RtcpCompoundPacket::parse(const std::uint8_t* data, std::size_t len,
                          std::error_code& ec) noexcept
{
    ec.clear();

    if (!data || len < kRtcpCommonHeaderSize) {
        ec = make_error_code(RtpErrc::invalid_packet);
        return std::nullopt;
    }
    // RFC 3550：整个复合包长度必须是 32 位字的整数倍
    if (len % kRtcpWordSize != 0) {
        ec = make_error_code(RtpErrc::invalid_packet);
        return std::nullopt;
    }

    RtcpCompoundPacket compound;
    std::size_t offset = 0;

    while (offset < len) {
        std::size_t consumed = 0;
        auto pkt = RtcpPacket::parse_single(data + offset, len - offset,
                                            consumed, ec);
        if (!pkt) {
            return std::nullopt;
        }
        if (consumed == 0) {
            ec = make_error_code(RtpErrc::invalid_packet);
            return std::nullopt;
        }
        compound.packets_.push_back(std::move(*pkt));
        offset += consumed;
    }

    if (compound.packets_.empty()) {
        ec = make_error_code(RtpErrc::invalid_packet);
        return std::nullopt;
    }
    return compound;
}

bool RtcpCompoundPacket::has_cname() const noexcept
{
    for (const auto& p : packets_) {
        if (const SdesReport* s = p.as_sdes()) {
            for (const auto& c : s->chunks) {
                if (!c.cname.empty()) {
                    return true;
                }
            }
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// 构造辅助
// -----------------------------------------------------------------------------

namespace {

/// 写 RTCP 通用头。`body_bytes` 是头之后的字节数（尚未对齐）。
inline void write_common_header(std::uint8_t* out, std::uint8_t count,
                                std::uint8_t pt, std::size_t total_bytes) noexcept
{
    out[0] = static_cast<std::uint8_t>(
        (static_cast<std::uint8_t>(kRtpVersion) << 6) |
        static_cast<std::uint8_t>(count & 0x1F));
    out[1] = pt;
    // Length = (总字节数 / 4) - 1
    wr16(out + 2, static_cast<std::uint16_t>(total_bytes / kRtcpWordSize - 1));
}

inline void write_report_block(std::uint8_t* p, const ReportBlock& rb) noexcept
{
    wr32(p + 0, rb.ssrc);
    p[4] = rb.fraction_lost;
    wr24s(p + 5, rb.packets_lost);
    wr32(p + 8, rb.highest_seq);
    wr32(p + 12, rb.jitter);
    wr32(p + 16, rb.lsr);
    wr32(p + 20, rb.dlsr);
}

inline std::size_t sdes_chunk_bytes(const SdesChunk& c)
{
    std::size_t n = 4;   // SSRC
    if (!c.cname.empty()) {
        n += 2 + c.cname.size();
    }
    for (const auto& it : c.items) {
        n += 2 + it.second.size();
    }
    n += 1;              // END
    return align4(n);
}

} // namespace

// -----------------------------------------------------------------------------
// 大小计算
// -----------------------------------------------------------------------------

std::size_t rtcp_sender_report_size(const SenderReport& sr) noexcept
{
    if (sr.reports.size() > 31) {
        return 0;   // RC 只有 5 位
    }
    return kRtcpCommonHeaderSize + 24 + sr.reports.size() * 24;
}

std::size_t rtcp_receiver_report_size(const ReceiverReport& rr) noexcept
{
    if (rr.reports.size() > 31) {
        return 0;
    }
    return kRtcpCommonHeaderSize + 4 + rr.reports.size() * 24;
}

std::size_t rtcp_sdes_size(const SdesReport& sdes) noexcept
{
    if (sdes.chunks.size() > 31) {
        return 0;
    }
    std::size_t n = kRtcpCommonHeaderSize;
    for (const auto& c : sdes.chunks) {
        n += sdes_chunk_bytes(c);
    }
    return n;
}

std::size_t rtcp_bye_size(const ByeReport& bye) noexcept
{
    if (bye.ssrcs.size() > 31) {
        return 0;
    }
    std::size_t n = kRtcpCommonHeaderSize + bye.ssrcs.size() * 4;
    if (!bye.reason.empty()) {
        n += 1 + bye.reason.size();
    }
    return align4(n);
}

std::size_t rtcp_app_size(const AppReport& app) noexcept
{
    return align4(kRtcpCommonHeaderSize + 8 + app.data.size());
}

// -----------------------------------------------------------------------------
// 构造
// -----------------------------------------------------------------------------

std::size_t build_rtcp_sender_report(std::uint8_t* out, std::size_t cap,
                                     const SenderReport& sr,
                                     std::error_code& ec) noexcept
{
    ec.clear();
    const std::size_t need = rtcp_sender_report_size(sr);
    if (need == 0) {
        ec = make_error_code(RtpErrc::invalid_argument);
        return 0;
    }
    if (!out || cap < need) {
        ec = make_error_code(RtpErrc::buffer_too_small);
        return 0;
    }

    write_common_header(out, static_cast<std::uint8_t>(sr.reports.size()),
                        static_cast<std::uint8_t>(RtcpType::sender_report), need);

    std::uint8_t* body = out + kRtcpCommonHeaderSize;
    wr32(body + 0, sr.ssrc);
    wr32(body + 4, sr.ntp_msw);
    wr32(body + 8, sr.ntp_lsw);
    wr32(body + 12, sr.rtp_timestamp);
    wr32(body + 16, sr.packet_count);
    wr32(body + 20, sr.octet_count);

    std::uint8_t* rb = body + 24;
    for (const auto& r : sr.reports) {
        write_report_block(rb, r);
        rb += 24;
    }
    return need;
}

std::size_t build_rtcp_receiver_report(std::uint8_t* out, std::size_t cap,
                                       const ReceiverReport& rr,
                                       std::error_code& ec) noexcept
{
    ec.clear();
    const std::size_t need = rtcp_receiver_report_size(rr);
    if (need == 0) {
        ec = make_error_code(RtpErrc::invalid_argument);
        return 0;
    }
    if (!out || cap < need) {
        ec = make_error_code(RtpErrc::buffer_too_small);
        return 0;
    }

    write_common_header(out, static_cast<std::uint8_t>(rr.reports.size()),
                        static_cast<std::uint8_t>(RtcpType::receiver_report), need);

    std::uint8_t* body = out + kRtcpCommonHeaderSize;
    wr32(body + 0, rr.ssrc);

    std::uint8_t* rb = body + 4;
    for (const auto& r : rr.reports) {
        write_report_block(rb, r);
        rb += 24;
    }
    return need;
}

std::size_t build_rtcp_sdes(std::uint8_t* out, std::size_t cap,
                            const SdesReport& sdes, std::error_code& ec) noexcept
{
    ec.clear();
    const std::size_t need = rtcp_sdes_size(sdes);
    if (need == 0) {
        ec = make_error_code(RtpErrc::invalid_argument);
        return 0;
    }
    if (!out || cap < need) {
        ec = make_error_code(RtpErrc::buffer_too_small);
        return 0;
    }

    write_common_header(out, static_cast<std::uint8_t>(sdes.chunks.size()),
                        static_cast<std::uint8_t>(RtcpType::sdes), need);

    std::size_t off = kRtcpCommonHeaderSize;
    for (const auto& c : sdes.chunks) {
        const std::size_t start = off;
        wr32(out + off, c.ssrc);
        off += 4;

        if (!c.cname.empty()) {
            out[off]     = static_cast<std::uint8_t>(SdesType::cname);
            out[off + 1] = static_cast<std::uint8_t>(c.cname.size());
            std::memcpy(out + off + 2, c.cname.data(), c.cname.size());
            off += 2 + c.cname.size();
        }
        for (const auto& it : c.items) {
            out[off]     = static_cast<std::uint8_t>(it.first);
            out[off + 1] = static_cast<std::uint8_t>(it.second.size());
            std::memcpy(out + off + 2, it.second.data(), it.second.size());
            off += 2 + it.second.size();
        }
        // END
        out[off] = static_cast<std::uint8_t>(SdesType::end);
        off += 1;

        // 补齐到 32 位字边界
        const std::size_t aligned = align4(off);
        std::memset(out + off, 0, aligned - off);
        off = aligned;
        (void)start;
    }
    return off;
}

std::size_t build_rtcp_bye(std::uint8_t* out, std::size_t cap,
                           const ByeReport& bye, std::error_code& ec) noexcept
{
    ec.clear();
    const std::size_t need = rtcp_bye_size(bye);
    if (need == 0) {
        ec = make_error_code(RtpErrc::invalid_argument);
        return 0;
    }
    if (!out || cap < need) {
        ec = make_error_code(RtpErrc::buffer_too_small);
        return 0;
    }

    write_common_header(out, static_cast<std::uint8_t>(bye.ssrcs.size()),
                        static_cast<std::uint8_t>(RtcpType::bye), need);

    std::size_t off = kRtcpCommonHeaderSize;
    for (std::uint32_t s : bye.ssrcs) {
        wr32(out + off, s);
        off += 4;
    }
    if (!bye.reason.empty()) {
        out[off] = static_cast<std::uint8_t>(bye.reason.size());
        std::memcpy(out + off + 1, bye.reason.data(), bye.reason.size());
        off += 1 + bye.reason.size();
    }
    // 补齐
    std::memset(out + off, 0, need - off);
    return need;
}

std::size_t build_rtcp_app(std::uint8_t* out, std::size_t cap,
                           const AppReport& app, std::error_code& ec) noexcept
{
    ec.clear();
    const std::size_t need = rtcp_app_size(app);
    if (!out || cap < need) {
        ec = make_error_code(RtpErrc::buffer_too_small);
        return 0;
    }

    write_common_header(out, static_cast<std::uint8_t>(app.subtype & 0x1F),
                        static_cast<std::uint8_t>(RtcpType::app), need);

    std::uint8_t* body = out + kRtcpCommonHeaderSize;
    wr32(body, app.ssrc);
    std::memcpy(body + 4, app.name.data(), 4);

    if (!app.data.empty()) {
        std::memcpy(body + 8, app.data.data(), app.data.size());
    }
    // 补齐
    const std::size_t filled = kRtcpCommonHeaderSize + 8 + app.data.size();
    std::memset(out + filled, 0, need - filled);
    return need;
}

// -----------------------------------------------------------------------------
// NTP 时间
// -----------------------------------------------------------------------------

NtpTime make_ntp_time(std::int64_t unix_sec, std::uint32_t nano_sec) noexcept
{
    NtpTime t{};
    const std::uint64_t ntp_sec =
        static_cast<std::uint64_t>(unix_sec) + kNtpUnixEpochOffset;
    t.seconds = static_cast<std::uint32_t>(ntp_sec);

    // 小数部分：(nano / 1e9) * 2^32
    const std::uint64_t frac =
        (static_cast<std::uint64_t>(nano_sec) << 32) / 1000000000ULL;
    t.fraction = static_cast<std::uint32_t>(frac);
    return t;
}

void split_ntp_time(const NtpTime& ntp, std::int64_t& unix_sec,
                    std::uint32_t& nano_sec) noexcept
{
    unix_sec = static_cast<std::int64_t>(
        static_cast<std::uint64_t>(ntp.seconds) - kNtpUnixEpochOffset);

    // 小数部分：(fraction / 2^32) * 1e9
    const std::uint64_t nano =
        (static_cast<std::uint64_t>(ntp.fraction) * 1000000000ULL) >> 32;
    nano_sec = static_cast<std::uint32_t>(nano);
}

} // namespace rtpmech
