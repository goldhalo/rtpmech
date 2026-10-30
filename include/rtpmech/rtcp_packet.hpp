// =============================================================================
//  rtpmech/rtcp_packet.hpp —— RTCP 报文（RFC 3550 第 6 节）
// =============================================================================
//  文件    : rtpmech/rtcp_packet.hpp
//  用途    : RTCP 复合包及其成员报文（SR / RR / SDES / BYE / APP）的解析与构造。
//  依赖    : 标准库（不需要网络层）
//  编码    : UTF-8
//
//  ── 部分实现参考自 rtplib ───────────────────────────────────────────────────
//  RTCP 报文的字段布局与解析/构造逻辑参考自 rtplib (JRTPLIB)：
//      Copyright (c) 1999-2017 Jori Liesenborgs, MIT License
//      https://github.com/j0r1/JRTPLIB
//  本项目不链接 rtplib，仅参考其实现并重写为 C++17 风格。
//  详见项目根目录 NOTICE 文件。
//
//  ── RTCP 复合包 ─────────────────────────────────────────────────────────────
//  RFC 3550 规定 RTCP 以**复合包**形式发送：一个 UDP 数据报内含多个 RTCP 报文，
//  每个报文自带通用头（V/P/RC/PT/Length）。其中 Length 字段是
//  **"32 位字数量减一"**（即 (packet_bytes / 4) - 1），这是容易出错的地方。
//
//  典型复合包结构：
//      [SR 或 RR]  [SDES(CNAME)]  [可选的其他报文...]
//
//  解析时按通用头逐个切分，直到耗尽整个数据报。
// =============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "rtpmech/rtp_packet.hpp"   // 复用 RtpErrc / span / 字节序工具声明
#include "rtpmech/span.hpp"

namespace rtpmech {

// -----------------------------------------------------------------------------
// RTCP 报文类型（RFC 3550 Table 1）
// -----------------------------------------------------------------------------

/// RTCP 报文的 PT 字段取值。
enum class RtcpType : std::uint8_t {
    sender_report    = 200,  ///< SR
    receiver_report  = 201,  ///< RR
    sdes             = 202,  ///< SDES（源描述）
    bye              = 203,  ///< BYE
    app              = 204,  ///< APP（应用自定义）
    rtpfb            = 205,  ///< RTP 反馈（RFC 4585）
    payload_specific = 206,  ///< 载荷特定反馈
};

/// 把 RTCP 类型转成可读名字（诊断用）。
[[nodiscard]] const char* rtcp_type_name(RtcpType t) noexcept;

/// 通用头长度（V/P/RC + PT + Length）。
inline constexpr std::size_t kRtcpCommonHeaderSize = 4;

/// 所有 RTCP 报文长度必须是 32 位字的整数倍。
inline constexpr std::size_t kRtcpWordSize = 4;

// -----------------------------------------------------------------------------
// 描述块（SDES 用）
// -----------------------------------------------------------------------------

/// SDES 描述项的类型（RFC 3550 第 6.5 节）。
enum class SdesType : std::uint8_t {
    end       = 0,  ///< 列表结束
    cname     = 1,  ///< 规范名称（**必须**）
    name      = 2,  ///< 用户真实姓名
    email     = 3,  ///< 邮箱
    phone     = 4,  ///< 电话
    loc       = 5,  ///< 地理位置
    tool      = 6,  ///< 应用/工具名
    note      = 7,  ///< 备注
    priv      = 8,  ///< 私有扩展
};

/// 一个 SR/RR 报告块。
struct ReportBlock {
    std::uint32_t ssrc          = 0;  ///< 被报告方的 SSRC
    std::uint8_t  fraction_lost = 0;  ///< 自上次报告以来丢失比例（0..255，即 0..100% × 2.56）
    std::int32_t  packets_lost  = 0;  ///< 累计丢失数（24 位有符号）
    std::uint32_t highest_seq   = 0;  ///< 已收到的最大扩展序列号
    std::uint32_t jitter        = 0;  ///< 到达间隔抖动（时间戳单位）
    std::uint32_t lsr           = 0;  ///< 上一个 SR 的 NTP 时间中间 32 位
    std::uint32_t dlsr          = 0;  ///< 从上一个 SR 到现在的延迟（1/65536 秒）
};

/// SDES 中的一个描述块（对应一个 SSRC 的若干条目）。
struct SdesChunk {
    std::uint32_t ssrc = 0;
    /// CNAME（RFC 3550 要求每个复合包必须包含）
    std::string   cname;
    /// 其他可选条目（原始类型 + 值）
    std::vector<std::pair<SdesType, std::string>> items;
};

// -----------------------------------------------------------------------------
// 各类 RTCP 报文
// -----------------------------------------------------------------------------

/// 发送者报告（SR）。
struct SenderReport {
    std::uint32_t ssrc             = 0;
    std::uint32_t ntp_msw          = 0;  ///< NTP 时间戳高 32 位（秒）
    std::uint32_t ntp_lsw          = 0;  ///< NTP 时间戳低 32 位（小数部分）
    std::uint32_t rtp_timestamp    = 0;
    std::uint32_t packet_count     = 0;
    std::uint32_t octet_count      = 0;
    std::vector<ReportBlock> reports;
};

/// 接收者报告（RR）。
struct ReceiverReport {
    std::uint32_t ssrc = 0;
    std::vector<ReportBlock> reports;
};

/// 源描述（SDES）。
struct SdesReport {
    std::vector<SdesChunk> chunks;
};

/// 会话结束（BYE）。
struct ByeReport {
    std::vector<std::uint32_t> ssrcs;
    std::string                reason;   ///< 可选
};

/// 应用自定义（APP）。
struct AppReport {
    std::uint8_t  subtype = 0;
    std::uint32_t ssrc    = 0;
    std::array<char, 4> name{};          ///< 4 字节 ASCII 标识
    std::vector<std::uint8_t> data;
};

// -----------------------------------------------------------------------------
// 解析结果（variant 风格的轻量联合）
// -----------------------------------------------------------------------------

/// 一个已解析的 RTCP 报文。
///
/// 使用显式类型标签而非 std::variant —— 因为要携带解析出的具体结构体，
/// variant 会更冗长且增加编译期开销。
class RtcpPacket {
public:
    /// 报文类型。
    [[nodiscard]] RtcpType type() const noexcept { return type_; }

    /// 通用头里的 padding 位。
    [[nodiscard]] bool has_padding() const noexcept { return padding_; }

    /// 原始字节视图。
    [[nodiscard]] span<const std::uint8_t> raw() const noexcept
    {
        return span<const std::uint8_t>(raw_, raw_len_);
    }

    /// 若为 SR，返回其内容；否则返回 nullptr。
    [[nodiscard]] const SenderReport*   as_sender_report() const noexcept;
    /// 若为 RR，返回其内容；否则返回 nullptr。
    [[nodiscard]] const ReceiverReport* as_receiver_report() const noexcept;
    /// 若为 SDES，返回其内容；否则返回 nullptr。
    [[nodiscard]] const SdesReport*     as_sdes() const noexcept;
    /// 若为 BYE，返回其内容；否则返回 nullptr。
    [[nodiscard]] const ByeReport*      as_bye() const noexcept;
    /// 若为 APP，返回其内容；否则返回 nullptr。
    [[nodiscard]] const AppReport*      as_app() const noexcept;

    /// 解析单个 RTCP 报文（内部使用；通常通过 RtcpCompoundPacket 解析）。
    [[nodiscard]] static std::optional<RtcpPacket>
    parse_single(const std::uint8_t* data, std::size_t len,
                 std::size_t& consumed, std::error_code& ec) noexcept;

private:
    RtcpPacket() = default;
    void clear_payload() noexcept;

    RtcpType      type_    = static_cast<RtcpType>(0);
    bool          padding_ = false;
    const std::uint8_t* raw_ = nullptr;
    std::size_t   raw_len_   = 0;

    std::optional<SenderReport>   sr_;
    std::optional<ReceiverReport> rr_;
    std::optional<SdesReport>     sdes_;
    std::optional<ByeReport>      bye_;
    std::optional<AppReport>      app_;

    friend class RtcpCompoundPacket;
};

/// 一个 RTCP 复合包（多个 RTCP 报文的序列）。
class RtcpCompoundPacket {
public:
    /// 解析整个数据报为复合包。
    ///
    /// @param[in] data 数据报字节。
    /// @param[in] len  长度。
    /// @param[out] ec  错误码。
    ///
    /// @return 成功返回复合包；任一成员报文非法则返回 std::nullopt。
    ///
    /// @note 严格遵循 RFC 3550：长度必须是 4 的倍数、每个报文的
    ///       Length 字段必须是 (bytes/4 - 1)、且不得越界。
    [[nodiscard]] static std::optional<RtcpCompoundPacket>
    parse(const std::uint8_t* data, std::size_t len,
          std::error_code& ec) noexcept;

    /// 成员报文列表（按出现顺序）。
    [[nodiscard]] const std::vector<RtcpPacket>& packets() const noexcept
    {
        return packets_;
    }

    /// 复合包内是否包含 CNAME（RFC 3550 要求**必须**有）。
    [[nodiscard]] bool has_cname() const noexcept;

private:
    RtcpCompoundPacket() = default;
    std::vector<RtcpPacket> packets_;
};

// -----------------------------------------------------------------------------
// 构造
// -----------------------------------------------------------------------------

/// 计算某类 RTCP 报文所需的缓冲区大小。
[[nodiscard]] std::size_t rtcp_sender_report_size(const SenderReport&) noexcept;
[[nodiscard]] std::size_t rtcp_receiver_report_size(const ReceiverReport&) noexcept;
[[nodiscard]] std::size_t rtcp_sdes_size(const SdesReport&) noexcept;
[[nodiscard]] std::size_t rtcp_bye_size(const ByeReport&) noexcept;
[[nodiscard]] std::size_t rtcp_app_size(const AppReport&) noexcept;

/// 构造各类 RTCP 报文到输出缓冲区。
///
/// @return 写入字节数；失败返回 0 并设置 ec。
///
/// @note 长度自动补齐到 32 位字，Length 字段自动填写为 (bytes/4 - 1)。
[[nodiscard]] std::size_t build_rtcp_sender_report(
    std::uint8_t* out, std::size_t cap, const SenderReport&, std::error_code&) noexcept;

[[nodiscard]] std::size_t build_rtcp_receiver_report(
    std::uint8_t* out, std::size_t cap, const ReceiverReport&, std::error_code&) noexcept;

[[nodiscard]] std::size_t build_rtcp_sdes(
    std::uint8_t* out, std::size_t cap, const SdesReport&, std::error_code&) noexcept;

[[nodiscard]] std::size_t build_rtcp_bye(
    std::uint8_t* out, std::size_t cap, const ByeReport&, std::error_code&) noexcept;

[[nodiscard]] std::size_t build_rtcp_app(
    std::uint8_t* out, std::size_t cap, const AppReport&, std::error_code&) noexcept;

// -----------------------------------------------------------------------------
// NTP 时间辅助（RTCP SR 使用）
// -----------------------------------------------------------------------------

/// NTP 时间戳（64 位定点，高 32 位秒、低 32 位小数）。
struct NtpTime {
    std::uint32_t seconds  = 0;
    std::uint32_t fraction = 0;
};

/// 由 Unix 时间（秒 + 纳秒）构造 NTP 时间戳。
///
/// @note NTP 纪元是 1900-01-01，Unix 纪元是 1970-01-01，相差 2208988800 秒。
[[nodiscard]] NtpTime make_ntp_time(std::int64_t unix_sec,
                                    std::uint32_t nano_sec) noexcept;

/// 由 NTP 时间戳还原 Unix 时间（秒 + 纳秒）。
void split_ntp_time(const NtpTime& ntp, std::int64_t& unix_sec,
                    std::uint32_t& nano_sec) noexcept;

/// NTP 纪元与 Unix 纪元相差的秒数。
inline constexpr std::uint64_t kNtpUnixEpochOffset = 2208988800ULL;

} // namespace rtpmech
