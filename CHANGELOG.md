# Changelog

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

---

## [未发布]

### 新增

**CMake 安装与导出（find_package）**

- `cmake --install` 现在装出完整的一套：头文件、静态库、`lib/cmake/rtpmech/` 下的
  `rtpmechConfig.cmake` / `rtpmechConfigVersion.cmake` / `rtpmechTargets.cmake`，
  以及 `share/doc/rtpmech` 下的 LICENSE / NOTICE
- 使用者只需要两行：
  ```cmake
  find_package(rtpmech REQUIRED)
  target_link_libraries(app PRIVATE rtpmech::rtpmech)
  ```
- Config 里 `find_dependency(netmech)`：`librtpmech.a + libnetmech.a + pthread`
  这一串不用再手写

- 默认安装前缀：Linux `/usr/local`，Windows `C:/Program Files/rtpmech`

**动态库与 Runtime 注入（2026-11）**

- `RTPMECH_LIBRARY_TYPE=STATIC|SHARED` + `RTPMECH_API` 导出宏（Windows
  `dllexport/dllimport`，Linux hidden 可见性）；静态链入的 netmech 符号用
  `-Wl,--exclude-libs,ALL` 隐藏；CI 增加 `Linux / gcc / Shared` job
- `RtpSessionConfig::runtime`：显式注入 `netmech::Runtime`，io 线程数由应用决定；
  netmech submodule 升级到支持 UDP 工厂 Runtime 重载的版本

### 修复（2026-11 审查）

- **RTP 时间戳**：新增 `send_frame()` / `send_frame_with_timestamp()` —— 一整帧
  的所有分片共用同一个时间戳（之前 README 教逐分片 `send()`，时间戳会逐个前进）
- **抖动**：`jitter_units` 更新漏乘 16，上报值只有 RFC 3550 A.8 的 1/16
- **RTCP 定时器**：发送失败路径不推进调度 → 1ms 忙循环；无 `remote` 时不再启动定时器
- **SSRC 自回环**：组播 loopback / 发给自己不再被误判为冲突（按最近发送序号识别）
- **RTCP 复合包**：校验首个报文必须是 SR/RR；缺 SDES/CNAME 只计数不丢包
- **其它**：显式时间戳推进游标、发送失败不空耗序号、扩展头字数截断校验、
  SDES PRIV 跳过解析、`span` 空视图 `nullptr+0` UB

### 测试

- 用例 90 → 101、断言 461 → 536；新增 Packetizer+Session 集成（同帧同时间戳）、
  抖动 RFC 精度、SSRC 自回环、RTCP 结构校验、Runtime 注入、扩展头截断、SDES PRIV 等用例

---

## [0.9.0] — 首个版本

功能完整，Windows / Linux x86_64 / Linux aarch64 三平台验证过。
尚未在真实项目中长期运行，1.0 之前 API 可能微调。

### 新增

**会话**

- `RtpSession`：一路 RTP 流 = 一个会话，内部持两个 `netmech::UdpEndpoint`
  （RTP / RTCP），**跟随同一条 io 线程**，因此内部状态全部无锁。
- `RtpHandler`：`on_packet` / `on_rtcp` / `on_close`，回调串行。
- `SendResult`：`accepted` / `closed` / `invalid`。比 bool 多一档，
  因为"会话没了"和"参数错了"的处置方式不同（一个要重建，一个重试也没用）。

  > 刻意**没有** `backpressure`：UDP 端点没有发送队列，`send_to` 交给内核就返回
  > "已受理"。内核发送缓冲满会体现在 `stats().datagrams_send_failed` 上；
  > 码率自适应应该走 RTCP 反馈（RFC 3550 的设计路径），而不是发送返回值。
- 端口成对绑定：RTP 用偶数端口，**RTCP 恒为 RTP + 1**；任一失败整体回滚。
- 支持 `rtp_port_end` 在区间内搜一对端口（一台机器跑多路时由部署者规划）。

**RTCP 自动发送**

- 链路活着就按 RFC 3550 §6.2 的节奏自动发 SR / RR，间隔随会话带宽与成员数变化，
  含 0.5~1.5 的随机抖动因子。应用只处理 `on_packet`。
- 复合包里附带 SDES(CNAME)。
- `close()` 发 BYE。

**协议**

- RTP：固定头 / 扩展头 / CSRC / padding 的解析与构造，端序无关（不用位域）。
- RTCP：SR / RR / SDES / BYE / APP 的解析与构造。
- 源跟踪：序列号回绕、丢包率、抖动、RTT（RFC 3550 §6.4 / A.8）。
- **SSRC 冲突自愈**：收到同 SSRC 但不同来源的包时，先发 BYE 再换号（§8.2）。

**时间戳**

- 默认按**采样帧率**递增（25 fps @ 90 kHz = 3600），而不是"发送时刻"。
  用发送时刻会把编码延迟、线程调度、网络重传的抖动全写进时间戳，接收端播放会抖。
- `timestamp_increment = 0` 时退化为按真实发送时刻算，
  仅适用"采集即发、无独立时间基"的场景。
- `send_with_timestamp()` 支持逐帧可变（可变帧率）。

**其他**

- MTU 保护：超过 `max_packet_size`（默认 1400）直接返回 `invalid`，不发出，不做 IP 分片。
- 零分配解析：`RtpPacket` 是只读视图，回调里直接读，不拷贝。

**载荷分片（新增）**

- `Packetizer` 抽象接口：把一帧切成若干个 RTP payload，不侵入 `RtpSession`。
- `H264Packetizer`（RFC 6184）：单 NAL 包 + FU-A 分片，参数集带内发送可关。
- `H265Packetizer`（RFC 7798）：单 NAL 包 + FU 分片，VPS/SPS/PPS 可关。
  **与 H264 不共用代码**——NAL 头 1→2 字节、分片头 2→3 字节、参数集 2→3 个。
- PS/TS 不在本库职责内（容器封装，与 RTP 正交），应用自己实现 `Packetizer`。

### 已知限制

- 单会话只支持**一个**接收目标（`remote` + `send_to`）。多目标群发留到下版本。
- RTCP 反馈报文（REMB / NACK / PLI）未处理，目前只用 SR/RR 算 RTT。
- 组播场景下 RTCP 走单播（RFC 允许，但组播场景通常应发到同一组播组）。
- 库**不做应用层分片**。超大编码帧需在应用层切分成多个 RTP 包
  （同一时间戳，除最后一个外 `marker = false`）。

### 验证情况

| 平台 | 编译器 | 结果 |
|---|---|---|
| Windows 10/11 x64 | MSVC 14.44 | 69 用例 / 344 断言全绿，零警告 |
| Linux x86_64（WSL2） | GCC 15.2 | 69 用例 / 344 断言全绿，零警告 |
| Linux aarch64（qemu） | Arm GNU 10.2 交叉 | 69 用例 / 344 断言全绿，零警告 |

示例 `rtp_loopback` 在三个环境均 PASS（端口成对、序列号连续、时间戳间隔、收发计数）。
