// =============================================================================
//  rtpmech - 会话对外接口（讨论稿 v0.4）
// =============================================================================
//  本文件是**接口草案**，用于讨论，不是可用代码。定稿后再落到 include/。
//
//  相对现有实现的改动（已确认）：
//    1. 回调用虚函数 `RtpHandler`，与 netmech 的 `ConnHandler` 同构
//    2. 地址类型用 `netmech::Address`，公共头不暴露 asio
//    3. 双端口：RTP 与 RTCP 各占一个，**RTCP 恒为 RTP + 1**（RFC 3550）
//    4. `RtpSession` 内部持有两个 `netmech::UdpEndpoint`（组合，不继承）
//    5. 发送用 `SendResult` 枚举
//    6. **RTCP 自动发送**：链路在就按协议节奏自动发 SR/RR，应用层不参与
//    7. 端口绑定失败**直接失败**，不重试（协商结果已定）
//
//  待确认项见文末。
// =============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <netmech/common.hpp>
#include <netmech/udp.hpp>

namespace rtpmech {

class RtpPacket;              // 只读包视图（现有类型，接口不变）
class RtcpCompoundPacket;     // RTCP 复合包视图（现有类型）
class RtpSession;             // 回调里以引用形式传出，见 RtpHandler

// =============================================================================
//  发送结果
// =============================================================================

/// 一次发送尝试的结果。
///
/// 比 bool 多一档，因为"队列满"和"连接没了"的处置方式完全不同：
/// 前者等一会儿重试，后者要重建会话。靠 `stats()` 差值去猜不可靠。
enum class SendResult {
    accepted,      ///< 已受理，将按序发出
    closed,        ///< 会话已关闭，重试无意义
    invalid,       ///< 参数问题：payload 为空 / 超过 MTU / 未配置 remote
};

// 没有 "backpressure" 这一档：UDP 端点本身没有发送队列，`send_to` 把报文交给
// 内核后就返回"已受理"。真正的失败（内核发送缓冲满）发生在 io 线程里，计入
// `stats().datagrams_send_failed`。
//
// 应用层的码率自适应请走 RTCP 反馈（RR 里的丢包率），而不是靠这个返回值——
// 这正是 RFC 3550 设计的路径。

// =============================================================================
//  配置
// =============================================================================

struct RtpSessionConfig {
    // ---- 网络 ----

    /// 绑定地址。`address` 部分生效；`port` 是 **RTP 端口的起点**。
    ///
    /// - `port == 0`：由系统分配，并保证落在偶数端口（RFC 3550 §11）；
    /// - `port != 0`：从该端口起尝试绑定 `(p, p+1)`，**两个都成功才算成功**。
    netmech::Address local{"0.0.0.0", 0};

    /// 端口搜索上界（含）。仅在 `local.port() != 0` 时有意义。
    ///
    /// - `0`（默认）：**只试 `local.port()` 这一个**，失败即失败；
    /// - 非 0：失败则 `p += 2` 继续（保持偶数），直到 `p > rtp_port_end`。
    ///
    /// @note 每次尝试都是**整对** `(p, p+1)`——RTCP 恒为 RTP+1。
    /// @note 该字段只影响"未协商"的场景。**若端口来自 SIP/SDP 协商，
    ///       必须把 `rtp_port_end` 保持为 0**：协商已定，换端口对端就错了。
    std::uint16_t rtp_port_end = 0;

    /// 默认目标地址（`send()` 的收件人）。为空时 `send()` 返回 `invalid`，
    /// 必须用 `send_to()` 显式指定。
    ///
    /// @note RTCP 的目标自动推导为 `remote.port() + 1`。
    std::optional<netmech::Address> remote;

    // ---- 组播（只作用于 RTP 端点） ----

    std::optional<netmech::Address> multicast_group;
    std::optional<netmech::Address> multicast_interface;
    std::uint8_t multicast_hops     = 1;
    bool         multicast_loopback = true;

    // ---- RTP ----

    std::uint32_t local_ssrc = 0;       ///< 0 = 随机生成
    std::uint8_t  payload_type = 96;    ///< 动态载荷类型
    std::uint32_t clock_rate   = 90000; ///< 时钟频率（Hz）

    /// 每个 RTP 包的时间戳增量 —— **由采集端的采样帧率决定**。
    ///
    /// | 场景 | 取值 |
    /// |---|---|
    /// | 视频 25 fps @ 90 kHz（监控主流） | `3600` |
    /// | 视频 30 fps @ 90 kHz | `3000` |
    /// | 音频 20 ms @ 8 kHz | `160` |
    /// | 音频 20 ms @ 48 kHz | `960` |
    ///
    /// `0` 表示按**真实发送时刻**计算，**仅适用于"采集即发、无独立时间基"**
    /// 的场景（软采集、流转发）。
    ///
    /// @warning 硬件采集 / 硬件编码**必须显式配置本字段**。
    ///          RFC 3550 里 RTP 时间戳反映的是**采样时刻**，不是发送时刻；
    ///          用发送时刻算，会把编码延迟、线程调度、网络重传的抖动全部写进
    ///          时间戳，接收端按时间戳播放就会抖。而硬件采集的时间基是严格
    ///          等间隔的 —— 固定增量恰好把这个事实编码进去，播放反而更稳。
    ///
    /// @note 需要逐帧可变（可变帧率、多路共用一个时钟）时改用
    ///       `send_with_timestamp()` 显式指定，此时本字段不参与。
    std::uint32_t timestamp_increment = 3600;

    // ---- RTCP ----

    /// 是否启用 RTCP。启用后由库**自动**周期发送 SR/RR，应用层不参与。
    /// 关闭后只剩 RTP 单端口（P+1 不会被绑定，可能被别的进程占用）。
    bool enable_rtcp = true;

    /// 会话带宽（bps），用于计算 RTCP 发送间隔（RFC 3550 §6.2）。
    /// `0` 表示使用默认值。
    double session_bandwidth = 0.0;

    /// RTCP 发送间隔的**下限**（秒），RFC 3550 §6.2 建议 5 秒。
    ///
    /// 正常场景不要改。改小会让 RTCP 流量上升，200 路时尤其明显。
    /// 存在的意义：测试里需要验证"定时器确实在按间隔触发"，等 5 秒太慢。
    double rtcp_min_interval_sec = 5.0;

    // ---- 报文大小 ----

    /// 单个 RTP / RTCP 报文的**最大字节数**（含头部）。
    ///
    /// 超过时 `send()` 返回 `SendResult::invalid`，**不会发出**——
    /// 库不做 IP 分片，硬发出去只会让接收端丢掉整个包。
    ///
    /// 默认 1400：以太网 MTU 1500 − IP 头 20 − UDP 头 8 = 1472，留出
    /// VPN / PPPoE 的余量。
    ///
    /// @note 库**不做应用层分片**。超大编码帧请在应用层切成多个 RTP 包：
    ///       同一时间戳，除最后一个外 `marker = false`。
    std::size_t max_packet_size = 1400;

    // ---- 缓冲 ----

    std::size_t recv_capacity    = 2048;          ///< 单报文接收上限
    std::size_t send_queue_bytes = 1024 * 1024;   ///< 发送队列字节上限（反压阈值）
    std::size_t send_queue_count = 256;           ///< 发送队列笔数上限
};

// =============================================================================
//  事件
// =============================================================================

/// 一个 RTP 会话上的事件。全部在该会话的 io 线程上回调，**同一会话严格串行**。
class RtpHandler {
public:
    virtual ~RtpHandler() = default;

    /// 收到一个 RTP 报文。
    ///
    /// @param[in] sess 报文所属的会话。**一个 handler 可以服务多个会话**，
    ///                 靠这个参数区分；要在回调里回包（比如发 RTCP 反馈）
    ///                 也用它。
    /// @param[in] pkt  只读包视图。**有效期仅限本回调返回之前** —— 若需延后处理，
    ///                 必须自行拷贝 payload（`pkt.payload()`）。
    /// @param[in] from 真实来源地址。回包用它，而不是配置里的 remote
    ///                 （NAT 之后两者可能不一致）。
    ///
    /// @note 与 netmech 的 `borrow` 交付一致：不做拷贝、零分配。
    virtual void on_packet(RtpSession& sess, const RtpPacket& pkt,
                           const netmech::Address& from) = 0;

    /// 收到一个 RTCP 复合包（SR / RR / SDES / BYE 等）。
    ///
    /// **一般不需要实现**——SR/RR 的生成与解析、RTT 计算、成员管理都由库内部完成，
    /// 这里只是把收到的报文透出来供观测（打日志、做监控）。
    virtual void on_rtcp(RtpSession& sess, const RtcpCompoundPacket& compound,
                         const netmech::Address& from)
    {
        (void)sess;
        (void)compound;
        (void)from;
    }

    /// 会话关闭。本会话的最后一次回调。
    /// @param[in] ec 空 = 主动 close；非空 = 异常。
    /// @note 每个成功创建的会话**恰好一次**。
    virtual void on_close(std::error_code ec) { (void)ec; }
};

// =============================================================================
//  统计
// =============================================================================

struct RtpSessionStats {
    std::uint64_t rtp_sent      = 0;   ///< 已发出的 RTP 包
    std::uint64_t rtp_received  = 0;   ///< 收到的 RTP 包
    std::uint64_t rtcp_sent     = 0;   ///< 已发出的 RTCP 复合包（含自动发送的）
    std::uint64_t rtcp_received = 0;
    std::uint64_t send_rejected = 0;   ///< 被反压 / 已关闭 拒绝的发送次数
    std::uint64_t parse_errors  = 0;   ///< 非法包（不影响会话存活）

    std::size_t   active_sources = 0;  ///< 当前跟踪的远端源数量
    std::optional<std::uint32_t> rtt_us; ///< 最近一次测得的 RTT（微秒）
};

// =============================================================================
//  会话
// =============================================================================

/// RTP 会话。
///
/// @par 状态机
/// ```
///   create() 成功 ──► 活动 ──close()/send_bye(close_after)──► 已关闭
///                      │                                        ▲
///                      └──────── 致命错误（端点不可恢复）────────┘
/// ```
/// 只有两个稳定状态：**活动**与**已关闭**。没有"半开"。
///
/// @par 各接口在"已关闭"后的行为
/// | 接口 | 行为 |
/// |---|---|
/// | `send` / `send_with_timestamp` / `send_to` | 返回 `SendResult::closed`，不回调 |
/// | `send_rtcp_now` | 返回 `false` |
/// | `send_bye` | 静默忽略（已经关了，没有收件人） |
/// | `close` | **幂等**，直接返回 |
/// | `stats` / `local_*_port` / `local_ssrc` / `config` / `is_open` | **仍可调用**，返回关闭前的值（`is_open()` 为 false） |
///
/// 这条很重要：业务侧常在关闭后打日志、上报统计，这些调用不能崩也不能返回垃圾。
///
/// @par SSRC 冲突
///  库会自动处理（RFC 3550 §8.2），应用无需感知：
///  - 收到与本地 SSRC 相同、但来源地址不同的报文 → 判定冲突；
///  - 若此前已经用该 SSRC 发过数据 → 先发一个 RTCP BYE；
///  - 然后换一个新的随机 SSRC 继续。
///
///  同一来源地址重复出现**不算**冲突（那是自己发的包回环）。
class RtpSession {
public:
    /// 创建会话。
    ///
    /// @param[in]  cfg      配置
    /// @param[in]  handler  事件处理器（库强持有到 `on_close` 之后）
    /// @param[out] ec       失败原因
    /// @return 失败返回 nullptr
    ///
    /// @par 端口绑定
    ///  绑定 RTP 与 RTCP 两个端口（**RTCP = RTP + 1**），两个都成功才算成功；
    ///  任一失败则整体回滚并返回 `address_in_use`。搜索规则见
    ///  `RtpSessionConfig::rtp_port_end`。
    ///
    /// @par 线程模型
    ///  内部创建两个 `netmech::UdpEndpoint`：RTP 端点正常创建，RTCP 端点用
    ///  `create_udp_endpoint_on(rtp, ...)` **跟随 RTP 的 io 线程**。
    ///  RTCP 的定时器也挂在这个端点上，于是三者完全串行 ——
    ///  内部的 `source_tracker` / `rtcp_scheduler` 全部**无锁**。
    ///
    /// @par 协商场景（SIP/SDP）
    ///  端口必须先定下来写进 SDP，再绑定：
    ///  @code
    ///      auto base = netmech::find_free_port_run(5000, 6000, 2, 2);  // 探一对
    ///      // ... 把 *base / *base+1 写进 SDP 并完成协商 ...
    ///      cfg.local = {ip, *base};
    ///      cfg.rtp_port_end = 0;          // 协商已定，不搜别的端口
    ///      auto sess = RtpSession::create(cfg, handler, ec);
    ///      if (!sess) { /* 绑定失败就是失败，不重试 */ }
    ///  @endcode
    ///
    /// @note 调用前必须已 `netmech::initialize()`。
    [[nodiscard]] static std::shared_ptr<RtpSession>
    create(const RtpSessionConfig&    cfg,
           std::shared_ptr<RtpHandler> handler,
           std::error_code&            ec);

    ~RtpSession();
    RtpSession(const RtpSession&) = delete;
    RtpSession& operator=(const RtpSession&) = delete;

    // ---- 发送 ----

    /// 发一个 RTP 包到配置里的 `remote`。
    ///
    /// @par 为什么返回枚举而不是 bool
    ///  `closed` 意味着"重建会话"，`invalid` 意味着"参数错了重试也没用"，
    ///  处置方式不同；用 bool 分不出来。
    ///
    /// @par 缓冲区
    ///  返回 `accepted` 后 `payload` 指向的内存必须保持有效直到发送完成；
    ///  异步场景请用 `netmech::ConstBuf` 的 owner 形式让库代持。
    SendResult send(netmech::ConstBuf payload, bool marker);

    /// 指定时间戳发送（关键帧对齐等场景需要自己控制）。
    SendResult send_with_timestamp(netmech::ConstBuf payload, bool marker,
                                   std::uint32_t timestamp);

    /// 发到显式目标（覆盖配置里的 remote）。
    SendResult send_to(const netmech::Address& dst,
                       netmech::ConstBuf payload, bool marker);

    /// 立即发一个 RTCP 复合包（不等定时器）。
    ///
    /// @note 正常情况下**不需要调用**：RTCP 由库按 `session_bandwidth` 与
    ///       成员数自动调度。这个接口只给"想立刻发一次"的场景（调试、
    ///       成员变化后立即上报）。
    bool send_rtcp_now();

    /// 发 RTCP BYE。尽力发出，不关心结果（BYE 本身就是"我要走了"）。
    /// @param[in] close_after 发完是否关闭会话
    void send_bye(const std::string& reason = {}, bool close_after = false);

    // ---- 查询 ----

    [[nodiscard]] std::uint16_t local_rtp_port()  const noexcept;
    [[nodiscard]] std::uint16_t local_rtcp_port() const noexcept;
    [[nodiscard]] std::uint32_t local_ssrc()      const noexcept;
    [[nodiscard]] RtpSessionStats stats()         const;
    [[nodiscard]] bool            is_open()       const noexcept;
    [[nodiscard]] const RtpSessionConfig& config() const noexcept;

    // ---- 关闭 ----

    /// 关闭会话（先发 BYE，再关两个端点，最后停 RTCP 定时器）。幂等。
    void close() noexcept;

private:
    RtpSession();
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace rtpmech

// =============================================================================
//  设计定论
// =============================================================================
//  · `rtp_port_end` **保留**。
//    同一台主机上跑多个 RTP 服务时，部署者可以给每个实例规划一段端口范围，
//    由库在范围内找一对可用的 —— 比"每次启动碰运气"可控得多。
//    协商场景（SIP/SDP）则应保持为 0：端口已经定死在对端那里了。
//
//  · 单会话多接收者（jrtplib 的 AddDestination）**本期不做**，记入下版本：
//    `remote` 改成 `std::vector<Address>`，RTCP 对每个目标都发，
//    `send()` 变成"群发"。当前用 `send_to()` 逐个发即可满足。
//
//  · `send_bye(reason, close_after)` 保持现状：默认只发 BYE 不关会话。
//
//  · `on_rtcp` 保留（默认空实现）：内部已解析 SR/RR 并据此算 RTT，
//    透出来供观测（打日志、接监控）成本很低。
