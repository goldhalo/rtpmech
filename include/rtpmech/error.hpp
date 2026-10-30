// =============================================================================
//  rtpmech/error.hpp —— 错误码
// =============================================================================
//  文件    : rtpmech/error.hpp
//  用途    : 提供 rtpmech 统一的错误码体系（基于 std::error_code）。
//  编码    : UTF-8
//
//  ── 设计说明 ────────────────────────────────────────────────────────────────
//  * 与 netmech 保持一致，全部使用 `std::error_code`，不用异常传递可预期的失败。
//  * `RtpErrc` 可以通过 `std::error_code` 隐式构造（见下方 is_error_code_enum
//    特化），因此调用方可以写：
//        std::error_code ec = RtpErrc::invalid_packet;
//        if (ec == RtpErrc::invalid_packet) { ... }
//
//  * 本头文件被 rtp_packet.hpp / session.hpp 等共同包含，避免各模块
//    各自定义错误码导致的重复与不一致。
// =============================================================================
#pragma once

#include <string>
#include <system_error>

namespace rtpmech {

// -----------------------------------------------------------------------------
// RtpErrc
// -----------------------------------------------------------------------------

/// rtpmech 错误码。
enum class RtpErrc {
    ok = 0,
    invalid_packet,      ///< 包长度不足或字段非法
    unsupported_version, ///< RTP/RTCP 版本号不是 2
    buffer_too_small,    ///< 构造时输出缓冲不足
    invalid_argument,    ///< 参数非法（如 CSRC 过多、clock_rate 为 0）
    session_closed,      ///< 会话已关闭
    send_failed,         ///< 底层发送失败（队列满 / 未受理）
};

/// rtpmech 的错误类别。
[[nodiscard]] const std::error_category& rtp_category() noexcept;

/// 由 `RtpErrc` 构造 `std::error_code`。
[[nodiscard]] inline std::error_code make_error_code(RtpErrc e) noexcept
{
    return std::error_code(static_cast<int>(e), rtp_category());
}

} // namespace rtpmech

// 让 `RtpErrc` 可以被隐式转换为 `std::error_code`。
namespace std {
template <>
struct is_error_code_enum<rtpmech::RtpErrc> : true_type {};
} // namespace std
