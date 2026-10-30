// =============================================================================
//  rtpmech - 汇总头（讨论稿）
// =============================================================================
//  文件    : rtpmech/rtpmech.hpp
//  用途    : 一次性引入全部公共接口。业务代码通常只需要
//              #include <rtpmech/rtpmech.hpp>
//  依赖    : netmech（Address / Buf / ConstBuf / UdpEndpoint）
//  编码    : UTF-8
//
//  ★ 公共头 8 个
//  ---------------------------------------------------------------------------
//    span.hpp          轻量 span（C++17 没有 std::span）
//    error.hpp         RtpErrc 错误码
//    rtp_packet.hpp    RtpPacket（只读视图）+ RtpPacketInfo + 构造
//    rtcp_packet.hpp   RtcpCompoundPacket + 各子包（SR/RR/SDES/BYE/APP）
//    session.hpp       RtpSession / RtpHandler / Config / SendResult / Stats
//    packetizer.hpp    Packetizer（载荷分片接口）
//    h264.hpp          H264Packetizer（RFC 6184）
//    h265.hpp          H265Packetizer（RFC 7798）
//    rtpmech.hpp       本文件（汇总）
//
//    source_tracker / rtcp_scheduler 是**实现细节**，不对外：
//    它们的结果通过 `stats()` 的字段暴露（active_sources / rtt_us）。
//
//  ★ 三条契约
//  ---------------------------------------------------------------------------
//   1. 收到回调里的 `pkt` 是**借用视图** —— 回调返回即失效，要延后处理请自己拷
//      payload；
//   2. 同一会话的全部回调（on_packet / on_rtcp / on_close）**串行**，不需要加锁；
//   3. RTCP 由库自动发 —— 应用只处理 on_packet，不要自己算间隔、不要自己发 SR/RR。
// =============================================================================
#pragma once

// 基础类型与错误码
#include <rtpmech/span.hpp>
#include <rtpmech/error.hpp>

// 报文
#include <rtpmech/rtp_packet.hpp>
#include <rtpmech/rtcp_packet.hpp>

// 会话
#include <rtpmech/session.hpp>

// 载荷分片
#include <rtpmech/packetizer.hpp>
#include <rtpmech/h264.hpp>
#include <rtpmech/h265.hpp>
