// =============================================================================
//  rtpmech - H.264 的 RTP 分片（RFC 6184）
// =============================================================================
//  文件    : rtpmech/h264.hpp
//  用途    : 把一帧 H.264（Annex-B，可能含多个 NAL）切成 RTP payload。
//
//  支持的打包方式：
//    * **单 NAL 单元包**：NAL 能塞进一个 RTP 包时，原样作为一个包（RFC 6184 §5.6）
//    * **FU-A 分片**：NAL 太大时切成多片（RFC 6184 §5.8）
//
//  暂不支持：
//    * STAP-A 聚合（多个小 NAL 合成一个包）—— 收益是省头部开销，不影响互通
//    * FU-B（§5.9，带 DON，用于交错模式）
//    * MTAP（§5.7，多时间层）
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <rtpmech/packetizer.hpp>

namespace rtpmech {

/// H.264 分片器。
///
/// 无状态（每次都从 frame 里重新拆 NAL），因此可以给多路复用 —— 但要保证
/// 同一路不会并发调 `on_frame`。
class H264Packetizer final : public Packetizer {
public:
    struct Config {
        /// **整个 RTP 包**的上限（含 RTP 头 12 字节）。
        ///
        /// 默认 1400：以太网 MTU 1500 − IP 20 − UDP 8 = 1472，留 VPN/PPPoE 余量。
        /// 实际单包 payload 上限 = `max_packet_size - 12`。
        std::size_t max_packet_size = 1400;

        /// 是否把 SPS/PPS 也当普通 NAL 发出去（true）。
        ///
        /// H.264 的 SPS/PPS 通常通过 SDP 的 `sprop-parameter-sets` 带外传递；
        /// 但很多设备也会在关键帧前重复发一遍（带内）。这里默认**照发**——
        /// 对端忽略也无害，对不支持带外的播放器反而更兼容。
        bool send_parameter_sets_inband = true;
    };

    /// 用默认配置构造。
    ///
    /// @note 刻意不写成 `H264Packetizer(Config cfg = Config{})`：
    ///       `Config` 是嵌套类型，它的默认成员初始化器在外部类定义完成前
    ///       不可用，GCC 会直接报错（MSVC 放过了）。拆成两个重载两边都合法。
    H264Packetizer();

    /// 用指定配置构造。
    explicit H264Packetizer(Config cfg);

    /// @copydoc Packetizer::on_frame
    void on_frame(span<const std::uint8_t> frame, const Sink& sink) override;

    /// 单包 payload 上限（`max_packet_size - 12`）。
    [[nodiscard]] std::size_t max_payload() const noexcept;

    [[nodiscard]] const Config& config() const noexcept { return m_cfg; }

private:
    /// 发一个 NAL：能塞下就单包，否则 FU-A 分片。
    void emit_nal(span<const std::uint8_t> nal, bool marker, const Sink& sink);

    void emit_single_nal(span<const std::uint8_t> nal, bool marker, const Sink& sink);
    void emit_fu_a(span<const std::uint8_t> nal, bool marker, const Sink& sink);

    Config                 m_cfg;
    std::vector<std::uint8_t> m_buf;   ///< 复用的输出缓冲
};

} // namespace rtpmech
