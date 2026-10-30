// =============================================================================
//  rtpmech/rtp_packet.hpp —— RTP 数据包
// =============================================================================
//  文件    : rtpmech/rtp_packet.hpp
//  用途    : RTP (RFC 3550) 固定头 + 扩展头 + CSRC 列表 + 载荷的解析与构造。
//  依赖    : 标准库 + netmech（仅用于地址类型；本文件本身不涉及网络 IO）
//  编码    : UTF-8
//
//  ── 部分实现参考自 rtplib ───────────────────────────────────────────────────
//  RTP 头的字段布局与解析/构造逻辑参考自 rtplib (JRTPLIB)：
//      Copyright (c) 1999-2017 Jori Liesenborgs, MIT License
//      https://github.com/j0r1/JRTPLIB
//  本项目不链接 rtplib，仅参考其实现并重写为 C++17 风格。
//  详见项目根目录 NOTICE 文件。
//
//  ── 与 rtplib 实现方式的差异 ────────────────────────────────────────────────
//  rtplib 用 C 位域（bit-field）表示版本/填充/扩展/CSRC 计数：
//      struct RTPHeader { uint8_t version:2; uint8_t padding:1; ... };
//  位域的**内存布局是实现定义的**，rtplib 需要 #ifdef RTP_BIG_ENDIAN 分支，
//  且在小端平台上的布局依赖编译器具体实现。
//
//  本实现**不使用位域**，改为显式按位提取/写入：
//      version   = (b0 >> 6) & 0x03;
//      padding   = (b0 >> 5) & 0x01;
//      extension = (b0 >> 4) & 0x01;
//      csrcCount =  b0       & 0x0F;
//  这样端序无关、无需宏分支、可移植性更好。
// =============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>

#include <netmech/net.hpp>

#include "rtpmech/error.hpp"
#include "rtpmech/span.hpp"

namespace rtpmech {

// 错误码定义见 rtpmech/error.hpp（已在上方包含）

// -----------------------------------------------------------------------------
// RTP 常量
// -----------------------------------------------------------------------------

/// RTP 固定头长度（字节）。
inline constexpr std::size_t kRtpFixedHeaderSize = 12;

/// 单个 CSRC 标识符长度。
inline constexpr std::size_t kCsrcEntrySize = 4;

/// 扩展头固定部分长度（profile 2 字节 + length 2 字节）。
inline constexpr std::size_t kRtpExtensionHeaderSize = 4;

/// 支持的 RTP 版本号。
inline constexpr std::uint8_t kRtpVersion = 2;

/// 最大 CSRC 数量（4 位字段）。
inline constexpr std::size_t kMaxCsrcCount = 15;

// -----------------------------------------------------------------------------
// RTP 包（只读视图）
// -----------------------------------------------------------------------------

/// 一个已解析的 RTP 包。
///
/// 本类是**只读视图**，不拥有内存 —— 它指向调用方提供的一段字节。
/// 因此：
///   * 源数据必须比本对象活得更久；
///   * 从网络回调里拿到数据后，应在回调内完成处理或复制出需要的内容。
///
/// @note 解析时不做任何内存分配（CSRC 列表以 span 视图暴露）。
class RtpPacket {
public:
    /// 解析结果：成功返回 RtpPacket，失败返回错误码。
    ///
    /// @param[in] data 原始数据报字节。
    /// @param[out] ec  错误码；成功时为空。
    ///
    /// @return 解析成功返回有效对象；失败返回 std::nullopt。
    ///
    /// @note 失败情形：长度 < 12、版本号 != 2、扩展头长度越界、
    ///       CSRC 数量导致长度越界、padding 长度非法。
    [[nodiscard]] static std::optional<RtpPacket>
    parse(const std::uint8_t* data, std::size_t len, std::error_code& ec) noexcept;

    /// 便捷重载（span 版本）。
    [[nodiscard]] static std::optional<RtpPacket>
    parse(span<const std::uint8_t> data, std::error_code& ec) noexcept
    {
        return parse(data.data(), data.size(), ec);
    }

    // ---- 固定头字段 ----

    /// RTP 版本号（应为 2）。
    [[nodiscard]] std::uint8_t version() const noexcept { return version_; }

    /// 是否置了 padding 位（末尾有填充字节，长度由最后一字节指定）。
    [[nodiscard]] bool has_padding() const noexcept { return padding_; }

    /// 是否有扩展头。
    [[nodiscard]] bool has_extension() const noexcept { return extension_; }

    /// CSRC 数量（0..15）。
    [[nodiscard]] std::size_t csrc_count() const noexcept { return csrc_count_; }

    /// 标记位（视频帧结束、音频会话开始等语义由 payload type 定义）。
    [[nodiscard]] bool marker() const noexcept { return marker_; }

    /// 载荷类型（7 位，0..127）。
    [[nodiscard]] std::uint8_t payload_type() const noexcept { return payload_type_; }

    /// 序列号（16 位，会回绕）。
    [[nodiscard]] std::uint16_t sequence() const noexcept { return sequence_; }

    /// 时间戳（media clock，单位由 clock_rate 决定）。
    [[nodiscard]] std::uint32_t timestamp() const noexcept { return timestamp_; }

    /// 同步源标识（SSRC）。
    [[nodiscard]] std::uint32_t ssrc() const noexcept { return ssrc_; }

    // ---- 可选字段 ----

    /// CSRC 列表（贡献源标识）。
    ///
    /// @return 长度为实际 CSRC 数量（0..15）的只读视图。
    ///
    /// @note 内部存储是固定 15 个元素的数组（避免解析时分配内存），
    ///       因此这里必须按 `csrc_count_` 截取 —— 否则会暴露未使用的槽位。
    [[nodiscard]] span<const std::uint32_t> csrc_list() const noexcept
    {
        return span<const std::uint32_t>(csrc_.data(), csrc_count_);
    }

    /// 扩展头 profile 标识；无扩展头时返回 0。
    [[nodiscard]] std::uint16_t extension_profile() const noexcept
    {
        return ext_profile_;
    }

    /// 扩展头数据（不含 4 字节 profile/length 头）。
    [[nodiscard]] span<const std::uint8_t> extension_data() const noexcept
    {
        return span<const std::uint8_t>(ext_data_, ext_len_);
    }

    // ---- 载荷 ----

    /// 载荷数据（已扣除 padding）。
    [[nodiscard]] span<const std::uint8_t> payload() const noexcept
    {
        return span<const std::uint8_t>(payload_, payload_len_);
    }

    /// 整个包的原始字节（不含 padding）。
    [[nodiscard]] span<const std::uint8_t> raw() const noexcept
    {
        return span<const std::uint8_t>(raw_, raw_len_);
    }

    /// 包的总长度（不含 padding）。
    [[nodiscard]] std::size_t size() const noexcept { return raw_len_; }

private:
    RtpPacket() = default;

    const std::uint8_t* raw_       = nullptr;
    std::size_t         raw_len_   = 0;
    const std::uint8_t* payload_   = nullptr;
    std::size_t         payload_len_ = 0;
    const std::uint8_t* ext_data_  = nullptr;
    std::size_t         ext_len_   = 0;

    std::uint8_t  version_       = 0;
    bool          padding_       = false;
    bool          extension_     = false;
    bool          marker_        = false;
    std::uint8_t  payload_type_  = 0;
    std::size_t   csrc_count_    = 0;
    std::uint16_t sequence_      = 0;
    std::uint32_t timestamp_     = 0;
    std::uint32_t ssrc_          = 0;
    std::uint16_t ext_profile_   = 0;

    // CSRC 列表在栈上（最多 15 个），解析时按主机序填充
    std::array<std::uint32_t, kMaxCsrcCount> csrc_{};
};

// -----------------------------------------------------------------------------
// 构造 RTP 包
// -----------------------------------------------------------------------------

/// 构造 RTP 包所需的字段。
struct RtpPacketInfo {
    std::uint8_t  payload_type = 96;
    bool          marker       = false;
    std::uint16_t sequence     = 0;
    std::uint32_t timestamp    = 0;
    std::uint32_t ssrc         = 0;

    /// 扩展头 profile（仅当 extension_data 非空时使用）。
    std::uint16_t extension_profile = 0;

    /// 扩展头数据（可为空）。
    span<const std::uint8_t> extension_data{};

    /// CSRC 列表（可为空，最多 15 个）。
    span<const std::uint32_t> csrc_list{};

    /// 是否在末尾添加 padding 使包长为 4 字节对齐。
    bool use_padding = false;
};

/// 计算构造该包所需的缓冲区大小。
///
/// @return 所需字节数；参数非法时返回 0 并设置 ec。
[[nodiscard]] std::size_t
rtp_packet_size(const RtpPacketInfo& info, std::error_code& ec) noexcept;

/// 构造 RTP 包到调用方提供的缓冲区。
///
/// @param[out] out     输出缓冲。
/// @param[in]  capacity 缓冲容量。
/// @param[in]  info     包头信息。
/// @param[in]  payload  载荷数据。
/// @param[out] ec       错误码。
///
/// @return 写入的字节数；失败返回 0。
///
/// @note 头中所有多字节字段按**网络字节序**（大端）写入。
[[nodiscard]] std::size_t
build_rtp_packet(std::uint8_t* out, std::size_t capacity,
                 const RtpPacketInfo& info,
                 span<const std::uint8_t> payload,
                 std::error_code& ec) noexcept;

} // namespace rtpmech
