// =============================================================================
//  rtpmech/rtp_packet.cpp —— RTP 包解析与构造实现
// =============================================================================
//  ★ 部分实现参考自 rtplib (JRTPLIB)
//      Copyright (c) 1999-2017 Jori Liesenborgs
//      MIT License — https://github.com/j0r1/JRTPLIB
//    本项目不链接 rtplib，仅参考其实现并重写为 C++17 风格。
//    详见项目根目录 NOTICE 文件。
//
//  与 rtplib 的实现差异：
//    * rtplib 用 C 位域 + #ifdef RTP_BIG_ENDIAN 表示头部字段；
//      本实现**不使用位域**，显式按位提取/写入 —— 端序无关、无宏分支。
//    * rtplib 的 RTPPacket 拥有自己的缓冲区（构造时分配）；
//      本实现解析时为**零分配只读视图**（RtpPacket 仅记录指针与长度）。
// =============================================================================

#include "rtpmech/rtp_packet.hpp"

#include <cstring>
#include <string>

namespace rtpmech {

// 错误类别实现见 rtpmech/error.cpp
// -----------------------------------------------------------------------------
// 字节序辅助（网络字节序 = 大端）
// -----------------------------------------------------------------------------

namespace {

inline std::uint16_t read_be16(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}

inline std::uint32_t read_be32(const std::uint8_t* p) noexcept
{
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

inline void write_be16(std::uint8_t* p, std::uint16_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

inline void write_be32(std::uint8_t* p, std::uint32_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

} // namespace

// -----------------------------------------------------------------------------
// 解析
// -----------------------------------------------------------------------------

std::optional<RtpPacket> RtpPacket::parse(const std::uint8_t* data,
                                          std::size_t len,
                                          std::error_code& ec) noexcept
{
    ec.clear();

    // ---- 1. 最小长度检查 ----
    if (!data || len < kRtpFixedHeaderSize) {
        ec = make_error_code(RtpErrc::invalid_packet);
        return std::nullopt;
    }

    RtpPacket pkt;
    pkt.raw_ = data;

    // ---- 2. 逐位拆解第一字节（不使用位域，端序无关）----
    //
    //   0                   1                   2                   3
    //   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    //  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //  |V=2|P|X|  CC   |M|     PT      |       sequence number         |
    //  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    const std::uint8_t b0 = data[0];
    pkt.version_   = static_cast<std::uint8_t>((b0 >> 6) & 0x03);
    pkt.padding_   = ((b0 >> 5) & 0x01) != 0;
    pkt.extension_ = ((b0 >> 4) & 0x01) != 0;
    pkt.csrc_count_ = static_cast<std::size_t>(b0 & 0x0F);

    if (pkt.version_ != kRtpVersion) {
        ec = make_error_code(RtpErrc::unsupported_version);
        return std::nullopt;
    }

    const std::uint8_t b1 = data[1];
    pkt.marker_       = ((b1 >> 7) & 0x01) != 0;
    pkt.payload_type_ = static_cast<std::uint8_t>(b1 & 0x7F);

    pkt.sequence_  = read_be16(data + 2);
    pkt.timestamp_ = read_be32(data + 4);
    pkt.ssrc_      = read_be32(data + 8);

    // ---- 3. CSRC 列表 ----
    std::size_t offset = kRtpFixedHeaderSize;
    const std::size_t csrc_bytes = pkt.csrc_count_ * kCsrcEntrySize;

    if (len < offset + csrc_bytes) {
        ec = make_error_code(RtpErrc::invalid_packet);
        return std::nullopt;
    }
    for (std::size_t i = 0; i < pkt.csrc_count_; ++i) {
        pkt.csrc_[i] = read_be32(data + offset + i * kCsrcEntrySize);
    }
    offset += csrc_bytes;

    // ---- 4. 扩展头 ----
    if (pkt.extension_) {
        if (len < offset + kRtpExtensionHeaderSize) {
            ec = make_error_code(RtpErrc::invalid_packet);
            return std::nullopt;
        }
        pkt.ext_profile_ = read_be16(data + offset);
        const std::uint16_t ext_words = read_be16(data + offset + 2);
        const std::size_t ext_bytes =
            static_cast<std::size_t>(ext_words) * 4;   // 长度单位是 32 位字

        offset += kRtpExtensionHeaderSize;
        if (len < offset + ext_bytes) {
            ec = make_error_code(RtpErrc::invalid_packet);
            return std::nullopt;
        }
        pkt.ext_data_ = data + offset;
        pkt.ext_len_  = ext_bytes;
        offset += ext_bytes;
    }

    // ---- 5. 载荷与 padding ----
    pkt.raw_len_ = len;

    std::size_t payload_end = len;
    if (pkt.padding_) {
        // 最后一个字节是 padding 长度（含它自己），且不应超过剩余空间
        const std::uint8_t pad = data[len - 1];
        if (pad == 0 || static_cast<std::size_t>(pad) > len - offset) {
            ec = make_error_code(RtpErrc::invalid_packet);
            return std::nullopt;
        }
        payload_end = len - pad;
        pkt.raw_len_ = payload_end;   // raw() 不含 padding
    }

    pkt.payload_     = data + offset;
    pkt.payload_len_ = payload_end - offset;

    return pkt;
}

// -----------------------------------------------------------------------------
// 构造
// -----------------------------------------------------------------------------

std::size_t rtp_packet_size(const RtpPacketInfo& info,
                            std::error_code& ec) noexcept
{
    ec.clear();

    if (info.csrc_list.size() > kMaxCsrcCount) {
        ec = make_error_code(RtpErrc::invalid_argument);
        return 0;
    }

    std::size_t size = kRtpFixedHeaderSize +
                       info.csrc_list.size() * kCsrcEntrySize;

    if (!info.extension_data.empty()) {
        // 扩展数据按 32 位字对齐（不足补零）
        const std::size_t ext_words = (info.extension_data.size() + 3) / 4;
        size += kRtpExtensionHeaderSize + ext_words * 4;
    }

    return size;
}

std::size_t build_rtp_packet(std::uint8_t* out, std::size_t capacity,
                             const RtpPacketInfo& info,
                             span<const std::uint8_t> payload,
                             std::error_code& ec) noexcept
{
    ec.clear();

    if (!out) {
        ec = make_error_code(RtpErrc::invalid_argument);
        return 0;
    }

    const std::size_t header_size = rtp_packet_size(info, ec);
    if (ec) {
        return 0;
    }

    std::size_t total = header_size + payload.size();

    // padding 使总长为 4 字节对齐
    std::size_t pad = 0;
    if (info.use_padding) {
        const std::size_t rem = total % 4;
        if (rem != 0) {
            pad = 4 - rem;
        }
    }
    total += pad;

    if (capacity < total) {
        ec = make_error_code(RtpErrc::buffer_too_small);
        return 0;
    }

    const bool has_ext = !info.extension_data.empty();

    // ---- 第一字节：V(2) P(1) X(1) CC(4) ----
    out[0] = static_cast<std::uint8_t>(
        (static_cast<std::uint8_t>(kRtpVersion) << 6) |
        (static_cast<std::uint8_t>(pad > 0 ? 1 : 0) << 5) |
        (static_cast<std::uint8_t>(has_ext ? 1 : 0) << 4) |
        static_cast<std::uint8_t>(info.csrc_list.size() & 0x0F));

    // ---- 第二字节：M(1) PT(7) ----
    out[1] = static_cast<std::uint8_t>(
        (static_cast<std::uint8_t>(info.marker ? 1 : 0) << 7) |
        static_cast<std::uint8_t>(info.payload_type & 0x7F));

    write_be16(out + 2, info.sequence);
    write_be32(out + 4, info.timestamp);
    write_be32(out + 8, info.ssrc);

    std::size_t offset = kRtpFixedHeaderSize;

    // ---- CSRC ----
    for (std::size_t i = 0; i < info.csrc_list.size(); ++i) {
        write_be32(out + offset + i * kCsrcEntrySize, info.csrc_list[i]);
    }
    offset += info.csrc_list.size() * kCsrcEntrySize;

    // ---- 扩展头 ----
    if (has_ext) {
        const std::size_t ext_words = (info.extension_data.size() + 3) / 4;
        write_be16(out + offset, info.extension_profile);
        write_be16(out + offset + 2, static_cast<std::uint16_t>(ext_words));
        offset += kRtpExtensionHeaderSize;

        std::memcpy(out + offset, info.extension_data.data(),
                    info.extension_data.size());
        // 补零到 32 位对齐
        const std::size_t padding_bytes =
            ext_words * 4 - info.extension_data.size();
        if (padding_bytes > 0) {
            std::memset(out + offset + info.extension_data.size(), 0,
                        padding_bytes);
        }
        offset += ext_words * 4;
    }

    // ---- 载荷 ----
    if (!payload.empty()) {
        std::memcpy(out + offset, payload.data(), payload.size());
        offset += payload.size();
    }

    // ---- padding ----
    if (pad > 0) {
        std::memset(out + offset, 0, pad - 1);
        out[offset + pad - 1] = static_cast<std::uint8_t>(pad);
        offset += pad;
    }

    return offset;
}

} // namespace rtpmech
