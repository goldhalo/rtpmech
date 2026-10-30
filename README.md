# rtpmech

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey.svg)](#平台支持)

> 基于 [netmech](../netmech) 的 **RTP / RTCP** 库（C++17）。
> **协议按 RFC 3550 实现，网络层完全交给 netmech** —— 一个 `RtpSession` 就是一路流，
> 底层两个 `UdpEndpoint` 跟着同一条 io 线程跑，200 路也不会有线程爆炸。

---

## 特性

| | |
|---|---|
| **协议完整** | RTP 固定头 / 扩展头 / CSRC / padding；RTCP 的 SR / RR / SDES / BYE / APP |
| **RTCP 自动** | 链路在就按 RFC 3550 §6.2 的节奏自动发 SR/RR，应用只处理 `on_packet` |
| **时间戳按采样时刻** | 固定增量（25 fps @ 90 kHz = 3600），不把发送抖动写进时间戳 |
| **零分配解析** | `RtpPacket` 是只读视图，解析不分配内存 |
| **无锁** | 同一会话的回调全部串行，内部 `source_tracker` / `rtcp_scheduler` 不加锁 |
| **端序无关** | 不使用 C 位域（避免实现定义的内存布局） |
| **SSRC 冲突自愈** | 检测到冲突自动发 BYE 并换号（RFC 3550 §8.2） |

---

## 30 秒上手

接收端：

```cpp
#include <rtpmech/rtpmech.hpp>
#include <netmech/net.hpp>

using namespace rtpmech;

class Player : public RtpHandler {
public:
    void on_packet(RtpSession&, const RtpPacket& pkt, const netmech::Address& from) override
    {
        // ★ pkt 是借用视图：只在本次回调内有效，要延后处理请自己拷 payload
        std::printf("seq=%u ts=%u len=%zu from=%s\n",
                    pkt.sequence(), pkt.timestamp(),
                    pkt.payload().size(), from.to_string().c_str());
    }
    // on_rtcp / on_close 有默认空实现，多数场景不用管
};

int main() {
    netmech::initialize(2);

    RtpSessionConfig cfg;
    cfg.local      = netmech::Address("0.0.0.0", 5000);   // RTCP 自动用 5001
    cfg.clock_rate = 90000;

    std::error_code ec;
    auto sess = RtpSession::create(cfg, std::make_shared<Player>(), ec);
    if (!sess) {
        std::fprintf(stderr, "%s\n", ec.message().c_str());
        return 1;
    }
    std::printf("rtp=%u rtcp=%u\n", sess->local_rtp_port(), sess->local_rtcp_port());

    std::getchar();
    sess->close();          // 会发 RTCP BYE
    netmech::shutdown();
}
```

发送端：

```cpp
RtpSessionConfig cfg;
cfg.local               = netmech::Address("0.0.0.0", 0);      // 系统分配，保证偶数
cfg.remote              = netmech::Address("192.168.1.10", 5000);
cfg.timestamp_increment = 3600;                                  // 25 fps @ 90 kHz

auto sess = RtpSession::create(cfg, std::make_shared<Silent>(), ec);

while (running) {
    auto r = sess->send({nullptr, frame.data(), frame.size()}, /*marker=*/true);
    if (r == SendResult::closed) { break; }        // 会话没了，别发了
    std::this_thread::sleep_until(next_frame_time);
}
```

完整可跑的例子见 `example/`：

```bash
./rtp_loopback 50                    # 同进程自测，跑完自己判断成败
./rtp_recv 5000                      # 接收端
./rtp_send 127.0.0.1 5000 25 250     # 发送端（25fps，250 帧）
```

---

## 三个必须知道的约束

### ① 时间戳反映**采样时刻**，不是发送时刻

`timestamp_increment` 由**采集帧率**决定：

| 场景 | 取值 |
|---|---|
| 视频 25 fps @ 90 kHz（监控主流） | `3600`（默认） |
| 视频 30 fps @ 90 kHz | `3000` |
| 音频 20 ms @ 8 kHz | `160` |

用发送时刻算时间戳，会把**编码延迟、线程调度、网络重传的抖动**全写进去，
接收端按时间戳播放就会抖。硬件采集的时间基是严格等间隔的，固定增量恰好把这个
事实编码进去。

`timestamp_increment = 0` 表示按真实发送时刻算 —— **仅适用"采集即发、无独立时间基"**
（软采集、流转发）。

### ② 回调里的 `pkt` 是借用视图

```cpp
void on_packet(RtpSession&, const RtpPacket& pkt, const Address& from) override
{
    save(pkt.payload());          // ✘ 危险：回调返回后这块内存就归库了
    auto copy = std::vector<std::uint8_t>(pkt.payload().begin(), pkt.payload().end());  // ✔
}
```

零分配换来的代价。要送进解码器或丢到别的线程，**必须自己拷 payload**。

### ③ RTCP 不用你管

`enable_rtcp = true`（默认）时，库自己按 RFC 3550 的间隔发 SR/RR，
间隔随会话带宽与成员数变化。应用一般不需要调 `send_rtcp_now()` ——
那个接口是给"想立刻发一次"的场景（调试、成员变化后立即上报）留的。

---

## 载荷分片（H.264 / H.265）

`RtpSession::send()` 的契约始终是**一个 send = 一个 RTP 包**。要按标准把一帧切成
多个包，用 `Packetizer`：

```cpp
#include <rtpmech/rtpmech.hpp>

H264Packetizer pkt;      // 或 H265Packetizer（RFC 7798，格式不同）

pkt.on_frame(frame, [&](span<const std::uint8_t> payload, bool marker) {
    sess.send({nullptr, payload.data(), payload.size()}, marker);
});
```

| 分片器 | 标准 | 支持 |
|---|---|---|
| `H264Packetizer` | RFC 6184 | 单 NAL 包、FU-A 分片；参数集带内可关 |
| `H265Packetizer` | RFC 7798 | 单 NAL 包、FU 分片；VPS/SPS/PPS 可关 |

**H.264 和 H.265 的 RTP 格式不一样**，不能共用：

| 项 | H.264 | H.265 |
|---|---|---|
| NAL 头 | 1 字节 | **2 字节** |
| 分片类型 | FU-A（28） | FU（49） |
| 分片头 | 1 + 1 字节 | **2 + 1 字节** |
| 聚合 | STAP-A（24） | AP（48） |
| 参数集 | SPS/PPS | **VPS/SPS/PPS** |

**PS / TS 不归 rtpmech 管**。它们是容器封装（GB/T 28181、RFC 2250），跟 RTP
是正交的两层。需要的话自己实现 `rtpmech::Packetizer` 接口：

```cpp
class PsPacketizer : public rtpmech::Packetizer {
    void on_frame(span<const std::uint8_t> frame, const Sink& sink) override {
        // 封 PS → 按 1400 切块 → sink(chunk, is_last)
    }
};
```

---

## 构建

### 依赖

- C++17 编译器（MSVC 2019+ / GCC 9+）
- CMake ≥ 3.16
- **netmech**（底层网络库）

netmech 默认从 `thirdparty/netmech` 取（git submodule）。开发时也可以直接指向本地目录：

```bash
cmake -S . -B build -DRTPMECH_NETMECH_DIR=/path/to/netmech
```

### Linux / WSL

```bash
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release \
      -DRTPMECH_NETMECH_DIR=/path/to/netmech
cmake --build build-linux -j$(nproc)
ctest --test-dir build-linux --output-on-failure
```

### Windows（MSVC）

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      -DRTPMECH_NETMECH_DIR=C:/path/to/netmech
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### aarch64 交叉编译

```bash
cmake -S . -B build-aarch64 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DRTPMECH_NETMECH_DIR=/path/to/netmech
cmake --build build-aarch64 -j$(nproc)
```

产物最低 glibc 2.17。精简系统上可用
`-DCMAKE_EXE_LINKER_FLAGS="-static -static-libstdc++ -static-libgcc"` 静态链接。

#### 在 x86 上用 qemu 验证

```bash
sudo apt install -y qemu-user
export QEMU_LD_PREFIX=/opt/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc
./build-aarch64/bin/rtpmech_ut
```

> ⚠️ qemu 能验逻辑错误与依赖缺失，但验不出真实硬件的时序差异。最终仍要上板子。

### 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `RTPMECH_BUILD_TESTS` | ON | 单元测试（69 用例 / 344 断言） |
| `RTPMECH_BUILD_EXAMPLES` | ON | `rtp_loopback` / `rtp_recv` / `rtp_send` |
| `RTPMECH_WARNINGS_AS_ERRORS` | OFF | CI 用 |

---

## 目录结构

```
include/rtpmech/    公共头（6 个）
  span.hpp          轻量 span（C++17 没有 std::span）
  error.hpp         RtpErrc 错误码
  rtp_packet.hpp    RtpPacket（只读视图）+ RtpPacketInfo + 构造
  rtcp_packet.hpp   RtcpCompoundPacket + SR/RR/SDES/BYE/APP
  session.hpp       RtpSession / RtpHandler / Config / SendResult / Stats
  rtpmech.hpp       汇总头，业务代码只需要它

src/                实现（含内部头 source_tracker / rtcp_scheduler）
test/               69 用例 / 344 断言
example/            loopback / recv / send
docs/design/        概要设计与接口草案
```

---

## 平台支持

| 平台 | 编译器 | 状态 |
|---|---|---|
| Windows 10/11 x64 | MSVC 14.44 | ✅ 69 用例全绿 |
| Linux x86_64 | GCC 15（WSL2 Ubuntu） | ✅ 69 用例全绿 |
| Linux aarch64 | Arm GNU 10.2（交叉） | ✅ 交叉编译通过（运行时待板子验证） |

---

## 为什么不用现成的 RTP 库

jrtplib 是主要参考对象，但**只借鉴协议层，底层通信自己实现**：

| 维度 | jrtplib | 本项目 |
|---|---|---|
| 线程 | 每会话一个 poll thread | 固定 M 条 io 线程，N 路共享 |
| 驱动 | 轮询，固定 10 ms 一轮 | epoll / IOCP 事件驱动 |
| 加锁 | `rtpsession.cpp` 中 65 处 lock | 回调串行，零锁 |

200 路时，前者是 200 个线程 × 每秒 100 次空转唤醒 = 2 万次/秒的无效工作。
详见 [`docs/design/overview-design.md`](docs/design/overview-design.md) §1.5。

---

## 相关项目

- [netmech](../netmech) —— 底层网络库（TCP/UDP、io 线程、定时器）

---

## 致谢

本项目的实现与文档在 **DeepSeek-V4.1** 的协助下完成 —— 架构决策与验收由作者负责，
详见 [AUTHORS.md](AUTHORS.md)。

---

## License

MIT，见 [LICENSE](LICENSE)。
