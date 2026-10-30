// =============================================================================
//  rtpmech - 载荷分片接口
// =============================================================================
//  文件    : rtpmech/packetizer.hpp
//  用途    : 把"一帧/一个访问单元"切成若干个 RTP payload。
//
//  ★ 与 RtpSession 的关系
//  ---------------------------------------------------------------------------
//  **不侵入**。`RtpSession::send()` 的契约始终是"一个 send = 一个 RTP 包"，
//  超过 max_packet_size 直接返回 invalid。Packetizer 是在它**之上**的一层
//  适配工具：
//
//      帧数据 ──Packetizer──► 1..N 个 payload ──send()──► RTP 包
//
//  应用可以完全不用它（自己切），也可以自己实现它（PS/TS 或私有封装）。
//
//  ★ 为什么分片不在 RtpSession 里做
//  ---------------------------------------------------------------------------
//  载荷格式决定怎么切：H.264 按 NAL 边界（RFC 6184）、H.265 按 NAL（RFC 7798）、
//  PS/TS 按字节数（容器层）。塞进会话会把"RTP 报文层"和"载荷适配层"耦合死。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include <rtpmech/span.hpp>

namespace rtpmech {

/// 载荷分片器。
///
/// 实现方负责把一段数据切成能塞进单个 RTP 包的 payload；库只保证
/// `RtpSession::send()` 收到一个 payload 就发一个包。
class Packetizer {
public:
    /// 输出回调。
    ///
    /// @param[in] payload 一块 RTP payload（**不含 RTP 头**）。指向分片器内部缓冲，
    ///                    只在本次回调内有效 —— 要留就自己拷。
    /// @param[in] marker  是否是"一帧的最后一个包"。直接传给 `RtpSession::send()`
    ///                    的 marker 参数。
    using Sink = std::function<void(span<const std::uint8_t> payload, bool marker)>;

    virtual ~Packetizer() = default;

    /// 输入一个访问单元（AU）。
    ///
    /// @param[in] frame 访问单元字节。
    ///                  - H.264/H.265：一帧，Annex-B 格式（`00 00 00 01` / `00 00 01`
    ///                    起始码分隔的多个 NAL）；
    ///                  - PS/TS：一帧编码数据。
    /// @param[in] sink  输出回调。**必须在返回前调用完** —— 实现方不持有 frame。
    ///
    /// @note 返回后 sink 即失效，实现方不应保存它。
    virtual void on_frame(span<const std::uint8_t> frame, const Sink& sink) = 0;
};

} // namespace rtpmech
