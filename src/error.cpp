// =============================================================================
//  rtpmech/error.cpp —— 错误码类别实现
// =============================================================================
//  本文件为 rtpmech 原创实现（不含 rtplib 派生代码）。
// =============================================================================

#include "rtpmech/error.hpp"

namespace rtpmech {

namespace {

/// rtpmech 的错误类别。
///
/// 用函数内静态保证唯一实例（std::error_category 的相等比较依赖
/// 地址唯一，因此不能每次返回新对象）。
class RtpCategoryImpl final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override
    {
        return "rtpmech";
    }

    [[nodiscard]] std::string message(int ev) const override
    {
        switch (static_cast<RtpErrc>(ev)) {
        case RtpErrc::ok:
            return "ok";
        case RtpErrc::invalid_packet:
            return "invalid RTP/RTCP packet";
        case RtpErrc::unsupported_version:
            return "unsupported RTP version (expected 2)";
        case RtpErrc::buffer_too_small:
            return "output buffer too small";
        case RtpErrc::invalid_argument:
            return "invalid argument";
        case RtpErrc::session_closed:
            return "session closed";
        case RtpErrc::send_failed:
            return "send failed (queue full or not accepted)";
        }
        return "unknown rtpmech error";
    }
};

} // namespace

const std::error_category& rtp_category() noexcept
{
    static const RtpCategoryImpl category;
    return category;
}

} // namespace rtpmech
