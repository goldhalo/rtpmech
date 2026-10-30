# rtpmech API 参考

> 目标读者：使用本库的开发者。按头文件分节，每节列签名、返回值、失败原因与约束。
> 底层网络能力（`Address` / `Buf` / `ConstBuf` / `UdpEndpoint`）见 [netmech 的 API 文档](../../netmech/docs/API.md)。

---

## 0. 通用约定

### 命名空间与头文件

所有接口都在 `namespace rtpmech`。业务代码通常只需要：

```cpp
#include <rtpmech/rtpmech.hpp>
```

### 错误处理

| 位置 | 形式 |
|---|---|
| 可能失败的操作（`create` / `parse` / `build_*`） | `std::error_code&` 出参 |
| 异步完成回调 | 参数 `std::error_code ec`，空表示成功 |
| 纯查询 / 关闭 | `noexcept`，无返回值 |

错误码用 `RtpErrc`，可以直接比较：

```cpp
if (ec == RtpErrc::invalid_packet) { ... }
```

> ⚠️ `parse()` 系列**既返回 `optional` 又设置 `ec`**：`optional` 为空时 `ec` 一定有值；
> 但**不要**只看 `ec` 判断成功——有些实现路径会清空 `ec` 却返回空 optional。

### 线程模型

与 netmech 一致：

- **同一会话的全部回调串行**（`on_packet` / `on_rtcp` / `on_close`），不需要加锁。
- 不同会话可能在不同 io 线程上并发，**跨会话共享的状态要自己加锁**。
- `send()` / `send_bye()` / `close()` / `stats()` 可从任意线程调用。

### 三条贯穿全库的契约

1. **`on_packet` 里的 `pkt` 是借用视图** —— 回调返回即失效，要延后处理必须自己拷 payload。
2. **RTCP 由库自动发送** —— 不要自己算间隔、不要自己发 SR/RR。
3. **时间戳反映采样时刻** —— `timestamp_increment` 由帧率决定，不是发送时刻。

---

## 1. `rtpmech/span.hpp`

C++17 没有 `std::span`，这里给一个只读/只写的最小实现。

```cpp
template <typename T>
class span {
public:
    constexpr span() noexcept;
    constexpr span(T* data, std::size_t len) noexcept;

    template <typename Container> constexpr span(Container& c) noexcept;
    template <typename Container> constexpr span(const Container& c) noexcept;
    template <typename U, std::size_t N> constexpr span(T (&arr)[N]) noexcept;
    template <typename U> constexpr span(const span<U>& other) noexcept;

    [[nodiscard]] constexpr T*        data() const noexcept;
    [[nodiscard]] constexpr std::size_t size() const noexcept;
    [[nodiscard]] constexpr std::size_t size_bytes() const noexcept;
    [[nodiscard]] constexpr bool      empty() const noexcept;
    [[nodiscard]] constexpr T&        operator[](std::size_t i) const noexcept;
    [[nodiscard]] constexpr iterator  begin() const noexcept;
    [[nodiscard]] constexpr iterator  end() const noexcept;
};
```

只提供**读**操作。构造时不做生命周期检查——span 不持有数据，源对象必须活得比它久。

> 用途：`RtpPacket::payload()` 返回 `span<const uint8_t>`，避免每包一次 vector 分配。
> C++20 环境会自动走 `std::span`（通过 `if constexpr` 探测）。

---

## 2. `rtpmech/error.hpp`

### 2.1 `RtpErrc`

```cpp
enum class RtpErrc {
    ok = 0,
    invalid_packet,      ///< 包长度不足或字段非法
    unsupported_version, ///< RTP/RTCP 版本号不是 2
    buffer_too_small,    ///< 构造时输出缓冲不足
    invalid_argument,    ///< 参数非法（CSRC 过多、clock_rate 为 0 等）
    session_closed,      ///< 会话已关闭
    send_failed,         ///< 底层发送失败
};
```

### 2.2 `rtp_category()` / `make_error_code()`

```cpp
[[nodiscard]] const std::error_category& rtp_category() noexcept;
[[nodiscard]] std::error_code make_error_code(RtpErrc e) noexcept;
```

`RtpErrc` 被注册为 `is_error_code_enum`，所以可以直接赋给 `std::error_code`：

```cpp
std::error_code ec = RtpErrc::invalid_packet;   // 隐式转换
```

---

## 3. `rtpmech/rtp_packet.hpp`

RTP 报文的解析与构造。

### 3.1 `RtpPacket` —— 只读视图

```cpp
class RtpPacket {
public:
    [[nodiscard]] static std::optional<RtpPacket>
    parse(const std::uint8_t* data, std::size_t len, std::error_code& ec);

    // ---- 固定头 ----
    [[nodiscard]] std::uint8_t  version() const noexcept;
    [[nodiscard]] bool          has_padding() const noexcept;
    [[nodiscard]] bool          has_extension() const noexcept;
    [[nodiscard]] std::size_t   csrc_count() const noexcept;
    [[nodiscard]] bool          marker() const noexcept;
    [[nodiscard]] std::uint8_t  payload_type() const noexcept;
    [[nodiscard]] std::uint16_t sequence() const noexcept;
    [[nodiscard]] std::uint32_t timestamp() const noexcept;
    [[nodiscard]] std::uint32_t ssrc() const noexcept;

    // ---- 可变部分 ----
    [[nodiscard]] span<const std::uint32_t> csrc_list() const noexcept;
    [[nodiscard]] std::uint16_t             extension_profile() const noexcept;
    [[nodiscard]] span<const std::uint8_t>  extension_data() const noexcept;

    // ---- 载荷 ----
    [[nodiscard]] span<const std::uint8_t> payload() const noexcept;
    [[nodiscard]] span<const std::uint8_t> raw() const noexcept;
};
```

**不拷贝任何字节**——`payload()` / `raw()` 直接指向传入的 `data`。
**有效期由调用方决定**：在 `on_packet` 回调里就是"回调返回之前"。

`parse` 失败原因：`invalid_packet`（长度不足、padding 越界、CSRC 数超出剩余长度）、
`unsupported_version`（版本号不是 2）。

> 不使用 C 位域解析——位域的内存布局是**实现定义**的，跨编译器/跨架构会错。
> 这里全部用显式移位，端序无关。

### 3.2 `RtpPacketInfo` —— 构造参数

```cpp
struct RtpPacketInfo {
    std::uint8_t  payload_type = 96;
    bool          marker       = false;
    std::uint16_t sequence     = 0;
    std::uint32_t timestamp    = 0;
    std::uint32_t ssrc         = 0;

    std::uint16_t            extension_profile = 0;   ///< 仅当 extension_data 非空时使用
    span<const std::uint8_t> extension_data{};        ///< 扩展头数据（可为空）
    span<const std::uint32_t> csrc_list{};            ///< CSRC 列表（最多 15 个）
    bool                     use_padding = false;     ///< 末尾补齐到 4 字节
};
```

### 3.3 `build_rtp_packet`

```cpp
[[nodiscard]] std::size_t
build_rtp_packet(std::uint8_t* out, std::size_t capacity,
                 const RtpPacketInfo& info,
                 span<const std::uint8_t> payload,
                 std::error_code& ec) noexcept;
```

| 返回值 | 含义 |
|---|---|
| `> 0` | 写入的字节数 |
| `0` | 失败，`ec` 说明原因 |

失败原因：`buffer_too_small`（`capacity` 不够）、`invalid_argument`（CSRC 超过 15 个）。

> 一般**不需要直接调用**——`RtpSession::send()` 内部会组包。这个接口是给
> "自己管理 socket"或"生成离线 pcap"的场景用的。

---

## 4. `rtpmech/rtcp_packet.hpp`

RTCP 复合包的解析与构造。

### 4.1 各类报文结构

```cpp
struct ReportBlock {          // SR / RR 里的报告块（24 字节）
    std::uint32_t ssrc          = 0;   ///< 被报告方的 SSRC
    std::uint8_t  fraction_lost = 0;   ///< 自上次报告以来丢失比例（0..255 = 0..100%）
    std::int32_t  packets_lost  = 0;   ///< 累计丢失数（24 位有符号）
    std::uint32_t highest_seq   = 0;   ///< 收到的最大扩展序列号
    std::uint32_t jitter        = 0;   ///< 到达间隔抖动（时间戳单位）
    std::uint32_t lsr           = 0;   ///< 上一个 SR 的 NTP 时间中间 32 位
    std::uint32_t dlsr          = 0;   ///< 从上一个 SR 到现在的延迟（1/65536 秒）
};

struct SenderReport {         // SR（PT=200）
    std::uint32_t ssrc          = 0;
    std::uint32_t ntp_msw       = 0;   ///< NTP 秒
    std::uint32_t ntp_lsw       = 0;   ///< NTP 小数部分
    std::uint32_t rtp_timestamp = 0;
    std::uint32_t packet_count  = 0;
    std::uint32_t octet_count   = 0;
    std::vector<ReportBlock> reports;  ///< 最多 31 个
};

struct ReceiverReport { ... };   // RR（PT=201），结构同上但无发送统计
struct SdesChunk      { ... };   // SSRC + CNAME/NAME/EMAIL 等条目
struct SdesReport     { ... };   // SDES（PT=202），可含多个 chunk
struct ByeReport      { ... };   // BYE（PT=203），SSRC 列表 + 可选 reason
struct AppReport      { ... };   // APP（PT=204）
struct NtpTime        { ... };   // NTP 时间戳（秒 + 小数）
```

### 4.2 `RtcpCompoundPacket`

```cpp
class RtcpCompoundPacket {
public:
    [[nodiscard]] static std::optional<RtcpCompoundPacket>
    parse(const std::uint8_t* data, std::size_t len, std::error_code& ec);

    [[nodiscard]] const std::vector<RtcpPacket>& packets() const noexcept;
    [[nodiscard]] bool has_cname() const noexcept;
};
```

一个 UDP 报文里可以装**多个** RTCP 子包（这就是"复合包"）。`packets()` 按顺序给出。

`RtcpPacket` 提供 `as_sender_report()` / `as_receiver_report()` / `as_sdes()` /
`as_bye()` / `as_app()`，返回对应类型的指针（类型不符时为 `nullptr`）。

```cpp
auto compound = RtcpCompoundPacket::parse(data, len, ec);
for (const auto& p : compound->packets()) {
    if (const auto* sr = p.as_sender_report()) {
        std::printf("SR from %08X, packets=%u\n", sr->ssrc, sr->packet_count);
    }
}
```

### 4.3 构造

```cpp
[[nodiscard]] std::size_t build_rtcp_sender_report  (std::uint8_t*, std::size_t, const SenderReport&,   std::error_code&) noexcept;
[[nodiscard]] std::size_t build_rtcp_receiver_report(std::uint8_t*, std::size_t, const ReceiverReport&, std::error_code&) noexcept;
[[nodiscard]] std::size_t build_rtcp_sdes           (std::uint8_t*, std::size_t, const SdesReport&,     std::error_code&) noexcept;
[[nodiscard]] std::size_t build_rtcp_bye            (std::uint8_t*, std::size_t, const ByeReport&,      std::error_code&) noexcept;
[[nodiscard]] std::size_t build_rtcp_app            (std::uint8_t*, std::size_t, const AppReport&,      std::error_code&) noexcept;
```

长度自动补齐到 32 位字，`Length` 字段自动填 `(bytes/4 - 1)`。返回 0 表示失败。

> 同样**一般不需要直接调用**——会话内部会构造 SR/RR/SDES/BYE。
> 这个接口是给"自定义 RTCP 行为"或离线生成用的。

### 4.4 NTP 时间

```cpp
[[nodiscard]] NtpTime make_ntp_time(std::int64_t unix_seconds,
                                    std::uint32_t nanoseconds) noexcept;
```

RTCP SR 里的时间戳是 NTP 格式（1900 年起的秒数 + 2^32 分之一秒）。用这个转换。

---

## 5. `rtpmech/session.hpp`

核心接口。

### 5.1 `RtpSessionConfig`

```cpp
struct RtpSessionConfig {
    // ---- 网络 ----
    netmech::Address local{"0.0.0.0", 0};   ///< port 是 RTP 起点；0 = 系统分配
    std::uint16_t    rtp_port_end = 0;      ///< 端口搜索上界（0 = 只试一个）
    std::optional<netmech::Address> remote; ///< send() 的默认目标

    // ---- 组播（只作用于 RTP 端点） ----
    std::optional<netmech::Address> multicast_group;
    std::optional<netmech::Address> multicast_interface;
    std::uint8_t multicast_hops     = 1;
    bool         multicast_loopback = true;

    // ---- RTP ----
    std::uint32_t local_ssrc          = 0;      ///< 0 = 随机
    std::uint8_t  payload_type        = 96;
    std::uint32_t clock_rate          = 90000;
    std::uint32_t timestamp_increment = 3600;   ///< 25 fps @ 90 kHz；0 = 按发送时刻

    // ---- RTCP ----
    bool   enable_rtcp           = true;
    double session_bandwidth     = 0.0;
    double rtcp_min_interval_sec = 5.0;         ///< RFC 3550 建议 5 秒

    // ---- 报文与缓冲 ----
    std::size_t max_packet_size  = 1400;        ///< 单包上限（含头），超了不发出
    std::size_t recv_capacity    = 2048;        ///< 单报文接收上限
    std::size_t send_queue_bytes = 1024 * 1024;
    std::size_t send_queue_count = 256;
};
```

**`timestamp_increment` 的取舍**（最容易搞错的一处）：

| 取值 | 含义 | 适用 |
|---|---|---|
| 非 0（默认 `3600`） | 每包固定递增 | **硬件采集 / 硬件编码**（监控、IPC） |
| `0` | 按真实发送时刻算 | 软采集、流转发（"采集即发、无独立时间基"） |

RFC 3550 的 timestamp 是**采样时刻**，不是发送时刻。用发送时刻算会把编码延迟、
线程调度、网络重传的抖动全写进时间戳，接收端按它播放就会抖。

**`rtp_port_end` 的实际用途**：一台机器跑多路时，给每个实例划一段端口区间
（实例 A 用 5000–5099、实例 B 用 5100–5199），库在各自区间内找一对可用的。
**协商场景（SIP/SDP）必须保持为 0** —— 端口已经定死在对端那里了。

### 5.2 `SendResult`

```cpp
enum class SendResult {
    accepted,   ///< 已受理，将按序发出
    closed,     ///< 会话已关闭，重试无意义
    invalid,    ///< 参数问题：payload 为空 / 超过 MTU / 未配置 remote
};
```

> **没有 `backpressure`**：UDP 端点没有发送队列，`send_to` 交给内核后即返回
> "已受理"。内核发送缓冲满会体现在 `stats().send_rejected` 上；码率自适应应该走
> RTCP 反馈（RR 里的丢包率），这正是 RFC 3550 设计的路径。

### 5.3 `RtpHandler`

```cpp
class RtpHandler {
public:
    virtual ~RtpHandler() = default;

    virtual void on_packet(RtpSession& sess, const RtpPacket& pkt,
                           const netmech::Address& from) = 0;

    virtual void on_rtcp(RtpSession& sess, const RtcpCompoundPacket& compound,
                         const netmech::Address& from) {}

    virtual void on_close(std::error_code ec) {}
};
```

| 回调 | 说明 |
|---|---|
| `on_packet` | 每个 RTP 包一次。**`pkt` 只在本次回调内有效**；`from` 是真实来源地址，回包用它 |
| `on_rtcp` | 收到 RTCP 复合包。**一般不用实现**——SR/RR 的解析与 RTT 计算库内部已完成，这里只供观测 |
| `on_close` | 会话关闭，恰好一次 |

`sess` 参数的意义：**一个 handler 可以服务多个会话**，靠它区分；要在回调里回包也用它。

### 5.4 `RtpSessionStats`

```cpp
struct RtpSessionStats {
    std::uint64_t rtp_sent      = 0;
    std::uint64_t rtp_received  = 0;
    std::uint64_t rtcp_sent     = 0;   ///< 含自动发送的 SR/RR
    std::uint64_t rtcp_received = 0;
    std::uint64_t send_rejected = 0;   ///< 被拒绝的发送次数
    std::uint64_t parse_errors  = 0;   ///< 非法包（不影响会话存活）

    std::size_t   active_sources = 0;  ///< 当前跟踪的远端源数量
    std::optional<std::uint32_t> rtt_us;
};
```

### 5.5 `RtpSession`

#### 创建

```cpp
[[nodiscard]] static std::shared_ptr<RtpSession>
create(const RtpSessionConfig& cfg, std::shared_ptr<RtpHandler> handler,
       std::error_code& ec);
```

| 项 | 说明 |
|---|---|
| 端口 | 绑定 RTP 与 RTCP（**RTCP = RTP + 1**），两个都成功才算成功；任一失败整体回滚 |
| 失败回滚 | RTCP 失败会**关掉已绑的 RTP 端口**，保证可以重试 |
| 线程 | RTP 端点正常创建，RTCP 端点用 `create_udp_endpoint_on(rtp, ...)` 跟随它的 io 线程 |
| 前置条件 | 必须已 `netmech::initialize()` |

失败原因：`clock_rate == 0` / `recv_capacity == 0` / `max_packet_size < 64`（参数非法）、
端口被占（系统错误码）。

#### 发送

```cpp
SendResult send(netmech::ConstBuf payload, bool marker);
SendResult send_with_timestamp(netmech::ConstBuf payload, bool marker,
                               std::uint32_t timestamp);
SendResult send_to(const netmech::Address& dst, netmech::ConstBuf payload, bool marker);
```

| 接口 | 说明 |
|---|---|
| `send` | 发到 `cfg.remote`，时间戳按 `timestamp_increment` 递增 |
| `send_with_timestamp` | 显式指定时间戳（可变帧率），此时 `timestamp_increment` 不参与 |
| `send_to` | 显式指定目标，覆盖 `cfg.remote` |

**缓冲区**：返回 `accepted` 后 `payload` 指向的内存必须保持有效直到发送完成；
异步场景用 `netmech::ConstBuf` 的 owner 形式让库代持。

**`marker`**：一般用于"一帧的最后一个包"。一帧一包时恒为 `true`。

#### RTCP

```cpp
bool send_rtcp_now();                                     // 强制发一次（调试用）
void send_bye(const std::string& reason = {}, bool close_after = false);
```

`send_rtcp_now()` 正常情况下**不需要调用**——RTCP 由库按 `session_bandwidth` 与
成员数自动调度。

#### 查询

```cpp
std::uint16_t local_rtp_port()  const noexcept;
std::uint16_t local_rtcp_port() const noexcept;
std::uint32_t local_ssrc()      const noexcept;
RtpSessionStats stats()         const;
bool            is_open()       const noexcept;
const RtpSessionConfig& config() const noexcept;
```

#### 关闭

```cpp
void close() noexcept;
```

先发 BYE，再关两个端点，最后停 RTCP 定时器。**幂等**。

### 5.6 生命周期与状态机

只有两个稳定状态：**活动** 与 **已关闭**。

```
  create() 成功 ──► 活动 ──close() / send_bye(close_after)──► 已关闭
                     │                                          ▲
                     └────── 致命错误（端点不可恢复）────────────┘
```

**关闭后各接口的行为**：

| 接口 | 行为 |
|---|---|
| `send` / `send_with_timestamp` / `send_to` | 返回 `SendResult::closed`，不回调 |
| `send_rtcp_now` | 返回 `false` |
| `send_bye` | 静默忽略 |
| `close` | 幂等，直接返回 |
| `stats` / `local_*_port` / `local_ssrc` / `config` / `is_open` | **仍可调用**，返回关闭前的值 |

最后一行是硬要求：业务侧常在关闭后打日志、上报统计，这些调用不能崩、不能返回垃圾。

### 5.7 SSRC 冲突

库自动处理（RFC 3550 §8.2），应用无需感知：

1. 收到与本地 SSRC 相同、但**来源地址不同**的报文 → 判定冲突；
2. 若此前已用该 SSRC 发过数据 → 先发一个 RTCP BYE；
3. 换一个新的随机 SSRC 继续。

同一来源地址重复出现**不算**冲突（那是自己发的包回环）。

---

## 6. `rtpmech/rtpmech.hpp`

汇总头，包含上面 5 个：

```cpp
#include <rtpmech/span.hpp>
#include <rtpmech/error.hpp>
#include <rtpmech/rtp_packet.hpp>
#include <rtpmech/rtcp_packet.hpp>
#include <rtpmech/session.hpp>
```

业务代码只需要 `#include <rtpmech/rtpmech.hpp>`。

> `source_tracker` / `rtcp_scheduler` 是**实现细节**，不对外。它们的结果通过
> `stats().active_sources` / `rtt_us` 暴露。
