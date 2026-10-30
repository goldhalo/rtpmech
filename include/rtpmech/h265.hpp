// =============================================================================
//  rtpmech - H.265 的 RTP 分片（RFC 7798）
// =============================================================================
//  文件    : rtpmech/h265.hpp
//  用途    : 把一帧 H.265（Annex-B）切成 RTP payload。
//
//  ★ 与 H.264 的差异（不能共用代码）
//  ---------------------------------------------------------------------------
//    | 项            | H.264（RFC 6184）        | H.265（RFC 7798）           |
//    |---------------|--------------------------|-----------------------------|
//    | NAL 头        | 1 字节（type 占 5 位）    | **2 字节**（type 占 6 位）   |
//    | 分片类型      | FU-A（28）                | **FU（49）**                 |
//    | 分片头        | 1 字节指示 + 1 字节 NAL 头 | **2 字节 PayloadHdr + 1 字节 FU header** |
//    | 聚合类型      | STAP-A（24）              | **AP（48）**                 |
//    | 参数集        | SPS(7) / PPS(8)           | **VPS(32) / SPS(33) / PPS(34)** |
//  ---------------------------------------------------------------------------
//  所以这里是独立实现，不是 H264Packetizer 的特化。
//
//  支持的打包方式：
//    * 单 NAL 单元包（NAL 能塞进一个包）
//    * FU 分片（NAL 太大时）
//
//  暂不支持：AP 聚合、PACI、交织模式。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <rtpmech/packetizer.hpp>

namespace rtpmech {

/// H.265 分片器。无状态，同上。
class H265Packetizer final : public Packetizer {
public:
    struct Config {
        /// 整个 RTP 包的上限（含 12 字节 RTP 头）。默认 1400。
        std::size_t max_packet_size = 1400;

        /// 是否把 VPS/SPS/PPS 带内发送。默认 true（理由同 H264）。
        bool send_parameter_sets_inband = true;
    };

    /// 用默认配置构造。
    ///
    /// @note 同样拆成两个重载：嵌套 Config 的默认成员初始化器在外部类
    ///       定义完成前不可用（GCC 报错、MSVC 放过）。
    H265Packetizer();

    explicit H265Packetizer(Config cfg);

    /// @copydoc Packetizer::on_frame
    void on_frame(span<const std::uint8_t> frame, const Sink& sink) override;

    [[nodiscard]] std::size_t max_payload() const noexcept;

    [[nodiscard]] const Config& config() const noexcept { return m_cfg; }

private:
    void emit_nal(span<const std::uint8_t> nal, bool marker, const Sink& sink);
    void emit_single_nal(span<const std::uint8_t> nal, bool marker, const Sink& sink);
    void emit_fu(span<const std::uint8_t> nal, bool marker, const Sink& sink);

    Config                    m_cfg;
    std::vector<std::uint8_t> m_buf;
};

} // namespace rtpmech
