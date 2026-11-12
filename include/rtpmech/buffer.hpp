// =============================================================================
//  rtpmech - 缓冲区视图（可带持有者）
// =============================================================================
//  文件    : rtpmech/buffer.hpp
//  用途    : 发送接口的载荷入参。**不依赖 netmech** —— 之前这里用的是
//            netmech::ConstBuf，等于把 netmech 的类型钉进了本库的公共 API。
//  编码    : UTF-8
//
//  ★ 为什么带 owner
//    发送是异步的：`send()` 返回 accepted 之后，库还要在本线程之外把数据写出去。
//    如果载荷是临时 vector，调用方得自己保证它活到发送完成。把 owner 塞进来
//    就能让库代持（`owner` 是 shared_ptr<void>，指向什么无所谓，只要活着）。
// =============================================================================
#pragma once

#include <cstddef>
#include <memory>

namespace rtpmech {

/// 只读缓冲区视图。
///
/// @par 两种用法
/// @code
/// // 1) 调用方保证生命周期：数据必须活到**发送真正执行**（io 线程），
/// //    不是 send() 返回 —— send() 只是把视图投递到 io 线程，调用内
/// //    不拷贝。栈缓冲仅当你能阻塞等待发送完成时才可用；否则一律用
/// //    下面的 owner 形式（与 session.hpp 的 send 文档一致）。
/// sess.send(rtpmech::ConstBuf{nullptr, data, len}, true);
///
/// // 2) 异步安全（推荐）：把数据的持有者一起传进来，库代持到发出
/// auto pkt = std::make_shared<std::vector<std::uint8_t>>(...);
/// sess.send(rtpmech::ConstBuf{pkt, pkt->data(), pkt->size()}, true);
/// @endcode
struct ConstBuf {
    std::shared_ptr<void> owner;        ///< 可选：让库替你持有这段内存
    const void*           data = nullptr;
    std::size_t           size = 0;
};

/// 可变版本（接收侧用不到，留给将来的零拷贝接口）。
struct Buf {
    std::shared_ptr<void> owner;
    void*                 data = nullptr;
    std::size_t           size = 0;
};

} // namespace rtpmech
