// =============================================================================
//  h264.cpp —— H.264 的 RTP 分片实现（RFC 6184）
// =============================================================================
#include <rtpmech/h264.hpp>

#include <cstring>

namespace rtpmech {

namespace {

/// RTP 固定头长度。分片时要从包大小里扣掉。
constexpr std::size_t kRtpHeaderSize = 12;

/// H.264 NAL 类型（RFC 6184 §5.4 + RFC 6184 表 1）。
constexpr std::uint8_t kFuA        = 28;   ///< FU-A（分片单元 A）
constexpr std::uint8_t kStapA      = 24;   ///< STAP-A（聚合包）
constexpr std::uint8_t kNalTypeMax = 23;   ///< 1~23 是单个 NAL 单元

/// Annex-B 起始码长度（4 字节或 3 字节）。
std::size_t start_code_len(span<const std::uint8_t> b, std::size_t i) noexcept
{
    if (i + 4 <= b.size() && b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 1) {
        return 4;
    }
    if (i + 3 <= b.size() && b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
        return 3;
    }
    return 0;
}

} // namespace

H264Packetizer::H264Packetizer() : H264Packetizer(Config{}) {}

H264Packetizer::H264Packetizer(Config cfg) : m_cfg(cfg)
{
    if (m_cfg.max_packet_size <= kRtpHeaderSize + 2) {
        m_cfg.max_packet_size = kRtpHeaderSize + 3;   // 至少能装 3 字节（FU-A 头 + 1 数据）
    }
    m_buf.reserve(m_cfg.max_packet_size);
}

std::size_t H264Packetizer::max_payload() const noexcept
{
    return m_cfg.max_packet_size - kRtpHeaderSize;
}

void H264Packetizer::on_frame(span<const std::uint8_t> frame, const Sink& sink)
{
    if (frame.empty() || !sink) {
        return;
    }

    // ---- 1. 拆 Annex-B 起始码，收集所有 NAL 的范围 ----
    struct Range {
        std::size_t begin;
        std::size_t end;
    };
    std::vector<Range> nals;
    std::size_t nal_begin = 0;      // 当前 NAL 数据的起点（起始码之后）
    bool in_nal = false;

    std::size_t i = 0;
    while (i < frame.size()) {
        const std::size_t sc = start_code_len(frame, i);
        if (sc != 0) {
            if (in_nal && i > nal_begin) {
                nals.push_back({nal_begin, i});
            }
            i += sc;
            nal_begin = i;
            in_nal = true;
        } else {
            ++i;
        }
    }
    if (in_nal && frame.size() > nal_begin) {
        nals.push_back({nal_begin, frame.size()});
    }

    // 没有起始码：把整帧当成一个 NAL（有些设备直接给裸 NAL）
    if (nals.empty()) {
        if (frame.size() >= 1) {
            emit_nal(frame, /*marker=*/true, sink);
        }
        return;
    }

    // ---- 2. 逐个 NAL 发出，最后一个才置 marker ----
    const std::size_t last = nals.size() - 1;
    for (std::size_t k = 0; k < nals.size(); ++k) {
        emit_nal(frame.subspan(nals[k].begin, nals[k].end - nals[k].begin),
                 /*marker=*/k == last, sink);
    }
}

void H264Packetizer::emit_nal(span<const std::uint8_t> nal, bool marker, const Sink& sink)
{
    if (nal.empty()) {
        return;
    }
    const std::uint8_t type = nal[0] & 0x1F;

    // 参数集（SPS=7 / PPS=8）按配置决定是否带内发
    if ((type == 7 || type == 8) && !m_cfg.send_parameter_sets_inband) {
        return;
    }

    if (nal.size() <= max_payload()) {
        emit_single_nal(nal, marker, sink);
    } else {
        emit_fu_a(nal, marker, sink);
    }
}

void H264Packetizer::emit_single_nal(span<const std::uint8_t> nal, bool marker,
                                     const Sink& sink)
{
    // 单 NAL 单元包：payload 就是 NAL 本身（含 1 字节 NAL 头）
    sink(nal, marker);
}

void H264Packetizer::emit_fu_a(span<const std::uint8_t> nal, bool marker,
                               const Sink& sink)
{
    // FU-A（RFC 6184 §5.8）：
    //   +---------------+
    //   | FU indicator  |  F(1) NRI(2) Type(5)=28
    //   +---------------+
    //   | FU header     |  S(1) E(1) R(1) Type(5)=原 NAL type
    //   +---------------+
    //   | 分片数据 ...   |
    //   +---------------+
    const std::uint8_t nal_hdr   = nal[0];
    const std::uint8_t nri       = nal_hdr & 0x60;       // 保留优先级
    const std::uint8_t fal       = nal_hdr & 0x80;       // forbidden_zero_bit
    const std::uint8_t fu_ind    = static_cast<std::uint8_t>(fal | nri | kFuA);
    const std::uint8_t orig_type = nal_hdr & 0x1F;

    const std::size_t chunk_max = max_payload() - 2;      // 扣掉两个分片头
    if (chunk_max == 0) {
        return;
    }

    const std::uint8_t* data = nal.data() + 1;            // 跳过分片前的 NAL 头
    std::size_t remain = nal.size() - 1;
    bool first = true;

    m_buf.resize(max_payload());

    while (remain > 0) {
        const std::size_t n = (remain > chunk_max) ? chunk_max : remain;

        // 头两字节
        m_buf[0] = fu_ind;
        std::uint8_t fu_hdr = orig_type;
        if (first) {
            fu_hdr |= 0x80;                               // S = 1
        }
        const bool last = (remain == n);
        if (last) {
            fu_hdr |= 0x40;                               // E = 1
        }
        m_buf[1] = fu_hdr;

        std::memcpy(m_buf.data() + 2, data, n);
        data   += n;
        remain -= n;

        // marker 只置在**一整帧的最后一个包**上：这里是最后一片且调用方说这是最后一个 NAL
        const bool set_marker = marker && last;
        sink(span<const std::uint8_t>(m_buf.data(), 2 + n), set_marker);

        first = false;
    }
}

} // namespace rtpmech
