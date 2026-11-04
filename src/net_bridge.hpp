// =============================================================================
//  rtpmech - 内部：公共类型 ⇄ netmech 类型的桥
// =============================================================================
//  公共头里只有 rtpmech::Endpoint / rtpmech::ConstBuf（不引 netmech）；
//  真正调 netmech 的 UdpEndpoint 时才换成 netmech 的对应类型。
//  转换集中在这三个 inline 函数里，改动面小。
// =============================================================================
#pragma once

#include <netmech/common.hpp>

#include <rtpmech/buffer.hpp>
#include <rtpmech/endpoint.hpp>

namespace rtpmech {
namespace detail {

/// Endpoint → netmech::Address（bind / send_to 用）。
[[nodiscard]] inline netmech::Address to_address(const Endpoint& ep)
{
    return netmech::Address(ep.host(), ep.port());
}

/// netmech::Address → Endpoint（回调里把来源地址交给应用时用）。
[[nodiscard]] inline Endpoint from_address(const netmech::Address& a)
{
    return Endpoint{a.address_string(), a.port()};
}

/// ConstBuf → netmech::ConstBuf（发送时用）。
[[nodiscard]] inline netmech::ConstBuf to_constbuf(const ConstBuf& b)
{
    netmech::ConstBuf out;
    out.owner = b.owner;
    out.data  = b.data;
    out.size  = b.size;
    return out;
}

} // namespace detail
} // namespace rtpmech
