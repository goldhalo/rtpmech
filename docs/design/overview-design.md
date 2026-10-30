# rtpmech 概要设计

> 状态：讨论稿。本文定"怎么分层、线程怎么走、改哪里留哪里"，不写实现细节。
> 接口契约见 [`proposed/session.hpp`](proposed/session.hpp)。

---

## 1. 定位与范围

rtpmech 是 netmech 之上的 **RTP/RTCP 应用库**：

| 做什么 | 不做什么 |
|---|---|
| RTP 固定头 / 扩展头 / CSRC / padding 的解析与构造 | 编解码（H.264、Opus 等） |
| RTCP 复合包（SR/RR/SDES/BYE/APP）的构造与解析 | SDP 生成与协商 |
| 源跟踪、序列号/时间戳维护、RTT 计算 | 抖动缓冲、AEC、混音 |
| RTCP 按 RFC 3550 的节奏自动发送 | 传输层安全（SRTP） |

**依赖边界**：网络层全部由 netmech 提供（`UdpEndpoint`），rtpmech 不碰 socket、
不碰 asio、不起线程。

---

## 1.5 为什么不用现成的 RTP 库

社区里能直接用的主要是 jrtplib（MIT，1999 年起）。我们**用它做协议参照，但不用它的底层通信**。

### 1.5.1 差距不在协议层，在网络层

jrtplib 的协议实现是扎实的（RFC 3550 覆盖完整，SSRC 冲突、RTCP 调度都有）。问题在传输模型：

| 维度 | jrtplib | 本项目 |
|---|---|---|
| 线程 | **每个 `RTPSession` 一个 poll thread** | 固定 M 条 io 线程（4–8），N 路共享 |
| 驱动方式 | 轮询：`RTPTime::Wait(RTPTime(0, 10000))`，**固定 10 ms 一轮** | epoll / IOCP 事件驱动，数据到达即唤醒 |
| 加锁 | `rtpsession.cpp` 中 **65 处** lock（5 类 mutex） | 回调串行，**零锁** |
| 收包延迟 | 0–10 ms 量化误差（取决于到达时刻与唤醒时刻） | 微秒级 |

### 1.5.2 200 路时这些数字会变成什么

```
线程数        200
无效唤醒      200 × 100 次/秒 = 20,000 次/秒   ← 绝大多数是"没数据"的空转
每包加锁      收一个包要过 SOURCES_LOCK + SCHED_LOCK（有时还有 PACKSENT_LOCK）
```

这不是它的实现质量问题——是**1999 年的设计前提**（epoll 尚未出现，IOCP 刚有）。
在那个年代选择"轮询 + 每会话一线程"是合理取舍，代价现在才显出来。

### 1.5.3 分工

| 层 | 参照 jrtplib | 自己做 |
|---|---|---|
| RTP/RTCP 报文解析与构造 | ✅ 参考 | — |
| RFC 3550 的行为（时间戳语义、RTCP 间隔、SSRC 冲突、CNAME） | ✅ 参考 | — |
| **底层通信（socket / 线程 / 事件）** | ❌ 不用 | ✅ netmech |
| 会话层（持有关系、回调风格） | 部分参考 | ✅ 推模式、无锁 |

正好对应两个项目各自的职责：**netmech 解决"200 路怎么跑"，rtpmech 解决"协议怎么对"**。

### 1.5.4 许可

jrtplib 是 MIT。参照 RFC 行为不涉及代码复制；若借鉴了具体实现（如 SSRC 冲突的判定顺序），
在 `NOTICE` 中注明来源即可。

---

## 2. 现状与断点

现有实现规模：

| 文件 | 行数 | 状态 |
|---|---|---|
| `rtp_packet.h/.cpp` | — | 纯解析，**零 netmech 依赖** |
| `rtcp_packet.h/.cpp` | — | 同上 |
| `source_tracker.h/.cpp` | — | 纯统计，零依赖 |
| `rtcp_scheduler.h/.cpp` | — | 纯定时计算，零依赖 |
| `session.h/.cpp` | 277 + 996 | **依赖已删除的 netmech 旧接口** |
| 测试 | 64 用例 | `test_session.cpp` 的 11 个要重写，其余 53 个不动 |

**断点**：`session.cpp` 用了 4 个已经消失的接口

| 旧接口 | 现状 |
|---|---|
| `netmech::IoPool` | 移入 `src/`，不再是公共接口 |
| `netmech::make_datagram_channel` | 已删除 |
| `netmech::IDatagramChannel` | 已删除 |
| `netmech::DatagramConfig` | 已删除 |

所以 rtpmech **当前编译不过**。这次重构不是可选项。

---

## 2.5 公共接口清单

对外 **6 个头**，内部 **2 个**（收进 `src/`，不再对外暴露）。

| 头 | 内容 | 相对现状 |
|---|---|---|
| `span.hpp` | 轻量 span（C++17 没有 `std::span`） | **原样保留** |
| `error.hpp` | `RtpErrc` + `make_error_code` | **原样保留** |
| `rtp_packet.hpp` | `RtpPacket`（只读视图）+ `RtpPacketInfo` + `build_rtp_packet` | **原样保留**（15 用例） |
| `rtcp_packet.hpp` | `RtcpCompoundPacket` + `ReportBlock` / `SenderReport` / `ReceiverReport` / `SdesReport` / `ByeReport` / `AppReport` / `NtpTime` | **原样保留**（15 用例） |
| `session.hpp` | `RtpSession` / `RtpHandler` / `RtpSessionConfig` / `SendResult` / `RtpSessionStats` | **重写** |
| `rtpmech.hpp` | 汇总头（新增） | 新增 |

| 内部头（移入 `src/`） | 理由 |
|---|---|
| `source_tracker.hpp` | 实现细节。结果通过 `stats().active_sources` / `rtt_us` 暴露 |
| `rtcp_scheduler.hpp` | 实现细节。间隔计算不对外，应用无从干预（也不该干预） |

**为什么把这两个收进去**：它们的状态（成员表、下个报告时刻）属于会话内部。
暴露出去等于把"实现怎么算"变成契约的一部分，以后想改算法就是破坏性变更。

---

## 2.6 时间戳：反映**采样时刻**，不是发送时刻

这是最容易做错的一处，值得单独写。

### 2.6.1 RFC 的原文说的是什么

RFC 3550 §5.1：timestamp 是 "the sampling instant of the first octet in the RTP data
packet"。**采样时刻**，不是"这包什么时候发出去"。

### 2.6.2 两者在监控场景里不相等

| 环节 | 时间特性 |
|---|---|
| 传感器采样 | 硬件定时，25 fps → **严格 40 ms 一帧** |
| 编码 | 有几帧缓冲，延迟随 I 帧波动 |
| 发送 | 受调度、网络、重传影响，间隔不匀 |
| **时间戳该反映的** | **只有第一行** |

如果用发送时刻算时间戳，就把编码延迟、线程调度、网络重传的抖动**全都写进时间戳**，
接收端按时间戳播放 → 帧间隔忽长忽短 → 画面抖。

而固定增量（25 fps → 3600）恰好把"硬件采集严格等间隔"这个事实编码进去，
播放的时间轴是均匀的。**画面反而更稳。**

### 2.6.3 算错会怎样

`clock_rate = 90000`：

| 实际速率 | 应有增量 | 若误用 20 fps 的 4500 |
|---|---|---|
| 20 fps | 4500 | 1 秒涨 90000 ✓ |
| 25 fps | 3600 | 1 秒涨 112500 ✗（**快 25%**） |

接收端会把 1 秒的素材当成 1.25 秒播，音画立刻不同步。

### 2.6.4 接口的取值

| `timestamp_increment` | 含义 | 适用 |
|---|---|---|
| 非 0（**默认 3600**） | 每包固定递增 | 硬件采集 / 硬件编码（监控、IPC） |
| `0` | 按真实发送时刻算 | 软采集、流转发（"采集即发、无独立时间基"） |

默认给 3600 而不是 0，因为**默认场景就是监控视频**；让人落到"按发送时刻"才是错。

`send_with_timestamp()` 用于逐帧可变（可变帧率、多路共用一个时钟）。

### 2.6.5 顺带纠正 jrtplib 的一处

```cpp
double timestampunit = 90000;   // 硬编码
```

它赌的就是"视频是绝对主场景"，合理。代价是**音频会错**：`clock_rate = 8000` 时
SR 里的 RTP 时间戳差 11.25 倍，接收端算不出正确的唇音同步基准。

我们不复刻：SR 里用 `cfg.clock_rate`。

---

## 2.7 状态机

只有两个稳定状态：**活动** 与 **已关闭**。不存在"半开"。

```
  create() 成功 ──► 活动 ──close() / send_bye(close_after)──► 已关闭
                     │                                          ▲
                     └────── 致命错误（端点不可恢复）────────────┘
```

### 2.7.1 关闭后的接口行为

| 接口 | 行为 |
|---|---|
| `send` / `send_with_timestamp` / `send_to` | 返回 `SendResult::closed`，**不回调** |
| `send_rtcp_now` | 返回 `false` |
| `send_bye` | 静默忽略 |
| `close` | **幂等**，直接返回 |
| `stats` / `local_*_port` / `local_ssrc` / `config` / `is_open` | **仍可调用**，返回关闭前的值 |

最后一行是硬要求：业务侧常在关闭后打日志、上报统计，这些调用不能崩、不能返回垃圾。

### 2.7.2 create 失败的回滚

端口是成对绑定的，失败可能发生在"RTP 已绑、RTCP 未绑"的中间态：

```
绑 RTP 失败          → 返回，无泄漏
绑 RTP 成功、RTCP 失败 → 关掉 RTP，再返回 address_in_use
建 handler 失败       → 两个端点都关
```

**不允许出现"RTCP 失败但 RTP 还占着端口"** —— 那会让人重试时永远绑不上。

### 2.7.3 close 的在途发送

`close()` 立即停止接收、丢弃发送队列中未发出的数据，然后发 BYE。

理由：实时流里"关掉会话还慢慢发完队列"没有意义，只会拖住关闭路径。
BYE 用 `send_to` 发出后不等确认（UDP 也无从确认）。

---

## 2.8 接口走查（v0.4 更新）

用 `proposed/example_loopback.cpp` 把完整流程按接口写了一遍。

| 结论 | 说明 |
|---|---|
| ✔ 够用 | `create` / `send` / `on_packet` / `stats` / `close` 覆盖"发-收-看-关" |
| ✔ 够用 | `local_rtp_port()` / `local_rtcp_port()` 可直接用于日志与 SDP 回填 |
| ✘ **已补 #1** | 缺 `timestamp_increment`（见 2.7） |
| ✘ **已补 #2** | `on_packet` 只有 `pkt` 和 `from`，**拿不到会话引用** —— 一个 handler 服务多路时无法区分，也回不了包。已加 `RtpSession& sess` 首参 |
| ? 待确认 | 纯发送端被迫实现 `on_packet`（纯虚）。是否允许 `create(cfg, nullptr, ec)`？ |
| ? 待确认 | 是否需要内置"发送节拍器"？倾向**不加**（发送由采集/编码驱动） |

---

## 2.9 边界检查清单

写接口时对照 RFC 逐条过了一遍，标出"做了 / 没做 / 不做"：

| 项 | 状态 | 说明 |
|---|---|---|
| 序列号回绕 | ✅ 正确 | `uint16` 自然回绕，RFC 允许，接收端按模运算比较 |
| RTP 时间戳回绕 | ✅ 正确 | `uint32` 自然回绕，同上 |
| 端口偶数对齐 | ✅ 做 | 系统分配时保证；协商时由调用方保证 |
| RTCP 间隔抖动 | ✅ 做 | 复用 `rtcp_scheduler`（RFC 3550 §6.3.1 的 0.5~1.5 随机因子） |
| **SSRC 冲突检测** | ✅ **本版本做** | 参照 jrtplib：源表里发现"同 SSRC、不同来源地址"即冲突；若是自己的 SSRC → 先发 BYE，再换新号。同一来源重复出现不算冲突（那是自己的包回环） |
| **MTU** | ✅ **本版本做** | 参照 jrtplib 的 `SetMaximumPacketSize`：`cfg.max_packet_size`（默认 1400），超过则 `send` 返回 `invalid` 且不发。**不做 IP 分片，也不做应用层分片** |
| 组播下的 RTCP | ⏳ **下版本** | jrtplib 的做法：RTCP socket 也加入同一个组播组，RTCP 发到组地址的 `P+1`。当前只支持 RTP 入组播 / RTCP 单播 |

### 2.9.1 参照 jrtplib 的三段实现

| 项 | jrtplib 的做法 | 我们的做法 |
|---|---|---|
| **SSRC 冲突** | `RTPSourceTable::ProcessRawPacket` 里比对"同 SSRC 但不同地址" → `OnSSRCCollision` → 若 `IsOwnSSRC()` 则置 `owncollision`；会话层收到后：已发过数据就先 `BuildBYEPacket`，再 `SetNewSSRC` | 同机制。我们的 `source_tracker` 已有源表，只需加"地址比较 + 冲突标记"，会话层做"发 BYE + 换号" |
| **MTU** | `RTPSessionParams::maxpacksize`（默认值在构造函数里）+ 传给 `RTPPacketBuilder` 作构造缓冲上限，超了返回 `ERR_RTP_PACKBUILD_DATATOOLARGE` | `cfg.max_packet_size`，`send` 里先算"头 + payload"是否超限，超了直接 `SendResult::invalid` |
| **组播 RTCP** | RTP 与 RTCP 两个 socket **都加入**同一组播组（代码里专门判断 `rtpsock != rtcpsock` 避免重复加入） | 下版本：给 `UdpConfig` 的 RTCP 端点也配 `multicast_group` |

---

## 2.10 扩展性评估（会不会下版本推翻）

用户要求"不能下个版本彻底推倒重来"。逐项过：

| 项 | 本版本 | 下版本 | 接口是否要改 |
|---|---|---|---|
| SSRC 冲突 | 做 | — | **不可见**（纯内部） |
| MTU | 做 | — | 新增字段，已有接口不变 |
| 组播 RTCP | RTCP 单播 | RTCP 也入组 | 加 `std::optional<Address> rtcp_multicast_group`（默认空 = 沿用 RTP 组），**不影响现有调用** |
| 纯发送端（`create(cfg, nullptr, ec)`） | 做 | — | `create` 签名不变，只是 `handler` 允许为空 |
| 发送节拍器 | 不做 | 看需求 | 独立接口（如 `set_send_interval`），不碰 `send` |
| 多接收者（群发） | 不做 | 做 | `remote` 从 `optional<Address>` 改成 `vector<Address>` —— **这一项会破坏兼容** |

**结论**：除"多接收者"外，其余都能平滑演进。多接收者如果要提前兼容，有两种做法：

- **A**：现在就用 `std::vector<Address> remote`（单目标时只有一个元素），代价是 API 略显笨重；
- **B**：保持 `optional<Address>`，下版本加 `add_destination()` 而不改 `remote`（jrtplib 就是 `AddDestination` + 内部列表，`remote` 仍保留为"默认目标"）。

**建议 B** —— 与 jrtplib 一致，且 `send()` 的语义（"发到默认目标"）保持清晰。

---

## 3. 分层

```
┌─────────────────────────────────────────────────┐
│  应用层：on_packet / send()                     │
├─────────────────────────────────────────────────┤
│  RtpSession                                     │
│   ├─ source_tracker   （收包更新、RTT 计算）     │
│   ├─ rtcp_scheduler   （下次 RTCP 何时发）       │
│   └─ 序列号 / 时间戳 / SSRC                      │
├─────────────────────────────────────────────────┤
│  RtpPacket / RtcpCompoundPacket（零分配视图）    │
├─────────────────────────────────────────────────┤
│  netmech::UdpEndpoint ×2（RTP / RTCP）           │
├─────────────────────────────────────────────────┤
│  netmech（io 线程、socket、定时器）              │
└─────────────────────────────────────────────────┘
```

**关键约束**：`source_tracker` / `rtcp_scheduler` 是**普通成员**，不是线程安全的。
它们能被无锁访问，前提是"RTP 收包、RTCP 收包、RTCP 定时发送"三者同线程 ——
这由第 4 节保证。

---

## 4. 线程模型

### 4.1 三个执行体，一条线程

| 执行体 | 触发来源 |
|---|---|
| RTP 收包 | `rtp_ep->on_read` |
| RTCP 收包 | `rtcp_ep->on_read` |
| RTCP 定时发送 | `rtcp_ep->set_timer` 的回调 |

三者必须落在**同一个 io_context** 上。实现方式：

```cpp
rtp_ep_  = create_udp_endpoint(rtp_cfg, rtp_handler, ...);          // 任取一条 io 线程
rtcp_ep_ = create_udp_endpoint_on(*rtp_ep_, rtcp_cfg, ...);         // 跟随 rtp_ep_
rtcp_ep_->set_timer(interval, weak_from_this(), [this]{ send_rtcp(); });  // 跟随 rtcp_ep_
```

`create_udp_endpoint_on` 与端点级 `set_timer` 都是 netmech 0.9 提供的能力，
正是为这个场景加的。

### 4.2 跨线程入口

应用可能从任意线程调 `send()` / `close()`：

| 入口 | 处理 |
|---|---|
| `send()` | 组包后调 `ep->send_to()`，内部 `dispatch` 到 io 线程 |
| `close()` | 置原子标志 + `dispatch` 收尾 |
| `stats()` | 读原子计数器，不加锁 |

`send_to` 返回 `true` 只代表"已受理"（可能已发出，也可能排队）。
真实失败（发送缓冲满）计入 `datagrams_send_failed()`。

### 4.3 为什么不做成"任意线程安全"

如果给每个共享结构加锁，每收一个 RTP 包就要过 1~2 次 mutex。30 路视频、
每路 1000 pkt/s 时是 3 万次加锁/秒——虽然不致命，但完全可以避免。
同线程布局把这个成本降到零，代价只是创建时必须用 `_on` 跟随。

---

## 5. 生命周期

```
RtpSession::create(cfg, handler, ec)
  │
  ├─ 绑定 RTP 端口       失败 → 整体失败，回滚
  ├─ 绑定 RTCP 端口(P+1) 失败 → 关掉 RTP，整体失败
  ├─ 创建 rtcp timer
  └─ 启动接收

RtpSession::close()
  ├─ 发 BYE（尽力）
  ├─ 停 timer
  ├─ 关 rtcp_ep_ / rtp_ep_
  └─ 回调 on_close 一次

~RtpSession()  → 自动 close()（幂等）
```

**持有关系**：

| 谁 | 持有谁 | 强弱 |
|---|---|---|
| `RtpSession` | `RtpHandler` | **强**（活到 `on_close` 之后） |
| `RtpSession::Impl` | 两个 `UdpEndpoint` | 强 |
| `UdpEndpoint` 的定时器 | `RtpSession::Impl` | **弱**（`weak_from_this`） |
| handler / 业务对象 | `RtpSession` | 由用户决定（建议弱引用，避免环） |

定时器用弱引用的意义：`Impl` 销毁后 timer 自动停，不需要在析构里手工 `clear_timer`。

---

## 6. 端口策略

### 6.1 规则

| 场景 | 行为 |
|---|---|
| `local.port() == 0` | 系统分配，**保证偶数**（奇数则丢弃重绑，最多 N 次） |
| `local.port() != 0`，`rtp_port_end == 0` | 只试 `(p, p+1)`，失败即失败 |
| `local.port() != 0`，`rtp_port_end != 0` | 从 `p` 起每次 `p += 2` 试整对，直到 `> rtp_port_end` |

**RTCP 恒为 RTP+1**，不支持 +2 或强制指定（那不符合 RFC 3550，对端按惯例推算会错）。

### 6.2 协商场景（SIP/SDP）

端口必须在协商前确定，因此提供"先探测、再绑定"的两步用法：

```cpp
auto base = netmech::find_free_port_run(5000, 6000, /*count=*/2, /*align=*/2);
// → 写进 SDP：m=audio <base> RTP/AVP 96，a=rtcp:<base+1>
// → 协商完成后：
cfg.local = {ip, *base};
cfg.rtp_port_end = 0;                    // 协商已定，绝不搜别的端口
auto sess = RtpSession::create(cfg, ...);
if (!sess) { /* 被抢了就是失败，重试只会让对端对不上 */ }
```

探测与绑定之间有 TOCTOU 窗口，这是**接受的设计取舍**：协商定了端口，换端口
反而破坏协议。

### 6.3 同一主机多实例

`rtp_port_end` 存在的实际理由：一台机器上跑多路 RTP 服务时，部署者给每个实例
划一段端口区间（比如实例 A 用 5000–5099、实例 B 用 5100–5199），库在各自区间内
找一对可用的。

这比"每个实例都从 `port = 0` 让系统随机分配"更可控——运维能提前在防火墙上开洞、
在监控里按端口段定位实例。

```cpp
cfg.local        = {ip, 5000};
cfg.rtp_port_end = 5099;      // 在 5000..5099 里找一对
```

搜索步长是 **2**（保持 RTP 偶数），每次试的是 `(p, p+1)` 整对。

### 6.4 现有实现的处理要保留

`session.cpp` 里已有两段经过实测的端口逻辑，不能因为换接口丢掉：

1. **偶数保证**：系统分配得到奇数 → 释放，显式重绑（原实现最多 32 次）。
2. **RTCP 冲突整对重试**：`P+1` 被占 → 关掉 RTP，换 `P+2` 重绑整对。
   注释记录了"多轮测试约 1/60 概率出现"。

新实现改成 jrtplib 的"纯重试"更简洁：系统分配 → 检查奇偶 → 不合格就丢弃重来。
但**只在 `port == 0` 的未协商场景**启用重试。

---

## 7. RTCP 自动调度

### 7.1 行为

链路在就自动发，应用层不参与：

| 时机 | 发什么 |
|---|---|
| 收到 RTP（源有变化） | 记录，等下一拍 |
| 定时器到期 | 有发送过 → **SR**（含发送统计 + 报告块）；只收不发 → **RR** |
| 成员数 / 带宽变化 | 按 RFC 3550 §6.2 重算间隔，`set_timer` 改间隔 |
| `close()` | BYE |

间隔计算复用现有 `rtcp_scheduler`（12 个用例覆盖），不重写。

### 7.2 实现形态

```cpp
// Impl 持有
std::shared_ptr<netmech::UdpEndpoint> rtcp_ep_;
std::chrono::milliseconds             rtcp_interval_;

void schedule_rtcp()
{
    rtcp_interval_ = std::chrono::milliseconds(scheduler_.next_interval_ms());
    // 依赖对象是 Impl 自己：Impl 销毁 → 定时器自动停
    rtcp_ep_->set_timer(rtcp_interval_,
                        weak_from_this(),
                        [this] {
                            send_rtcp(/*force=*/false);
                            schedule_rtcp();   // 下一拍（间隔可能变了）
                        });
}
```

### 7.3 为什么不做成"应用层调 send_rtcp_now"

- 应用要自己维护定时器 → 又回到"自己起线程 / 加锁"的老路；
- RFC 的间隔算法要考虑成员数、带宽、随机抖动，不该让每个使用者重复实现。

`send_rtcp_now()` 保留给调试和"成员变化后立刻上报"。

---

## 8. 改造边界

### 8.1 原样保留（零改动）

| 文件 | 理由 |
|---|---|
| `rtp_packet.*` | 纯字节解析，无依赖；15 个用例继续有效 |
| `rtcp_packet.*` | 同上；15 个用例 |
| `source_tracker.*` | 纯统计；11 个用例 |
| `rtcp_scheduler.*` | 纯定时计算；12 个用例 |

### 8.2 重写（约 250 行）

| 位置 | 现状 | 改成 |
|---|---|---|
| `open()` 端口绑定 | `make_datagram_channel` + 32 次重试 | `create_udp_endpoint(_on)` + 保留偶数/整对规则 |
| `start_receive_rtp/rtcp` | `channel->async_receive` + **每包一次 `make_shared<vector>`** | `UdpHandler::on_read`（borrow，**零分配**） |
| `send_rtp` | `channel->async_send` + 弱捕获 | `ep->send_to()` → `SendResult` |
| `close` | `channel->close` | `ep->close()`；timer 改由端点托管 |
| RTCP 定时 | 自建 `steady_timer` + 上下文绑定 | `ep->set_timer()` |

### 8.3 顺带能拿到的改进

| 项 | 现状 | 改后 |
|---|---|---|
| 收包分配 | 每包 `make_shared<vector>` | 零分配（borrow） |
| RTCP 定时器生命周期 | 手工 cancel、有自我销毁风险 | 端点托管，`weak_from_this` 自动停 |
| 发送锁 | 无（但依赖单线程假设） | 无（由 netmech 保证同线程） |

---

## 9. 实施步骤

每步结束都必须**可编译**。

| 步 | 内容 | 结束标志 |
|---|---|---|
| 1 | `session.hpp` 换成 v0.3 接口；`session.cpp` 暂时留旧实现并加 `#if 0` | 编译不过（预期，旧实现依赖已删接口） |
| 2 | 重写 `session.cpp` 的网络层（端口/收发/关闭/timer） | **编译通过** |
| 3 | 重写 `test_session.cpp` 的 11 个用例到新接口 | 测试恢复全绿 |
| 4 | 补 RTCP 自动调度（定时器 + 间隔重算） | 新增用例覆盖自动发送 |
| 5 | 端到端功能测试（两个真实会话互通） | 见第 10 节 |

> 第 1 步故意让编译失败，是为了先把接口钉死，避免实现过程中反复改签名。

---

## 10. 功能测试（对外接口出发）

| 用例 | 验什么 |
|---|---|
| 双会话互通 | A 发 → B 的 `on_packet` 收到，字段正确 |
| 端口成对 | `local_rtcp_port() == local_rtp_port() + 1` |
| 端口占用 | 故意占住 `P+1`，`create` 返回失败且 RTP 端口已回滚 |
| 协商流程 | `find_free_port_run` → 绑定 → 实际端口与探测一致 |
| RTCP 自动发送 | 等 2~3 个间隔，`stats().rtcp_sent` 增长，对端收到 SR/RR |
| 发送失败可见 | 猛发到内核缓冲满时 `datagrams_send_failed` 增长，会话不崩 |
| 多线程发送 | 业务线程调 `send`，对端正常收到 |
| 关闭语义 | `close()` 后 `on_close` 恰好一次；在途数据不发 |
| 长跑 | 两会话持续 60 s，RSS 不涨、`active_sources` 稳定 |

---

## 10.5 多路压测数据

工具：`bench/rtp_bench.cpp`。每路建一对会话（发送端 + 接收端），模拟 N 个摄像头
同时推流，全部在**单进程**内跑回环。

环境：WSL2 / Ubuntu / GCC 15 / 16 物理核，io 线程固定 8 条，包长 1000 字节。

| 场景 | 建链 | 收发 | 丢包 | CPU（占 8 核） | 每路内存 |
|---|---|---|---|---|---|
| 200 路 @ 25 fps | 136 ms（0.68 ms/路） | 50k / 50k | **0 %** | **3.6 %** | 28.7 KB |
| 200 路 @ 50 fps | 117 ms（0.58 ms/路） | 80k / 80k | **0 %** | 4.5 % | 29.0 KB |
| 500 路 @ 25 fps | 215 ms（0.43 ms/路） | 100k / 100k | **0 %** | **7.0 %** | 26.5 KB |

几点值得记：

- **500 路 = 1000 个会话 = 2000 个 socket**，只占 7% CPU。io 线程数固定 8 条，
  不随路数增长——这正是"1 context = 1 线程"模型的价值。
- 建链耗时随路数**下降**（0.68 → 0.43 ms/路），因为端口分配与 io 线程预热是
  一次性成本，摊薄了。
- 每路内存稳定在 **27~29 KB**（两个 `UdpEndpoint` + 会话状态 + 源跟踪表）。
- 零丢包是回环环境下的结果；真实网络会有 MTU、拥塞、抖动的影响。

对比 jrtplib 的模型（每会话一条 poll thread、每 10 ms 醒一次）：

```
200 路 → 200 条线程
       → 200 × 100 次/秒 = 20,000 次/秒的唤醒（绝大多数是"没数据"的空转）
       → 收包路径上还要过 2~3 把锁
```

本项目的等价开销是"8 条线程 × epoll 唤醒"，且收包路径零锁。

> 注：这些数字是**库本身的开销**，不含解码。真实部署时 200 路 H.264 的解码
> 会远超这个量级——但那是编解码器的事，不该让网络库背。

---

## 11. 风险

| 风险 | 影响 | 处置 |
|---|---|---|
| 端口 `P+1` 常被占 | `create` 偶发失败 | 协商场景无解（协议决定）；非协商场景靠 `p += 2` 搜索 |
| RTCP 间隔算错 | 带宽占用超标 / 报告不及时 | 复用已验证的 `rtcp_scheduler`，不重写 |
| 借用视图被跨回调持有 | 读到已复用的缓冲（脏数据） | 文档明确；测试里加一条"回调外访问"的负例 |
| 同线程假设被破坏 | 数据竞争（UB） | 用 `_on` 强制同线程；测试验证 thread::id 一致 |
| aarch64 未验证 | 目标平台行为未知 | 与 netmech 一起在板子上补 |

---

## 12. 下版本待办

| 项 | 说明 |
|---|---|
| 单会话多接收者 | `remote` 改成 `std::vector<Address>`，`send()` 变群发，RTCP 对每个目标都发（jrtplib 的 `AddDestination` 语义） |
| RTCP 反馈处理 | 目前只算 RTT；REMBI / NACK / PLI 等反馈报文留给后续 |
