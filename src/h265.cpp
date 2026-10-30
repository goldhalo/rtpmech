// =============================================================================
//  h265.cpp —— H.265 的 RTP 分片实现（RFC 7798）
// =============================================================================
#include <rtpmech/h265.hpp>

#include <cstring>

namespace rtpmech {

namespace {

constexpr std::size_t kRtpHeaderSize = 12;
constexpr std::uint8_t kFu  = 49;   ///< FU（分片单元）
constexpr std::uint8_t kAp  = 48;   ///< AP（聚合包）

/// H.265 参数集类型（RFC 7798 §1.1.4）
constexpr std::uint8_t kVps = 32;
constexpr std::uint8_t kSps = 33;
constexpr std::uint8_t kPps = 34;

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

/// H.265 NAL 头（2 字节）：
///   +---------------+---------------+
///   |F|   Type(6)   |  LayerId(6)   | TID(3) |
///   +---------------+---------------+
///   byte0 = F(1) | Type(6) | LayerId 高 1 位
///   byte1 = LayerId 低 5 位 | TID(3)
struct NalHeader {
    std::uint8_t forbidden = 0;   ///< F 位
    std::uint8_t type      = 0;   ///< 6 位
    std::uint8_t layer     = 0;   ///< 6 位
    std::uint8_t tid       = 0;   ///< 3 位
};

NalHeader parse_nal_header(span<const std::uint8_t> nal) noexcept
{
    NalHeader h;
    if (nal.size() < 2) {
        return h;
    }
    h.forbidden = static_cast<std::uint8_t>((nal[0] >> 7) & 0x01);
    h.type      = static_cast<std::uint8_t>((nal[0] >> 1) & 0x3F);
    h.layer     = static_cast<std::uint8_t>(((nal[0] & 0x01) << 5) | ((nal[1] >> 3) & 0x1F));
    h.tid       = static_cast<std::uint8_t>(nal[1] & 0x07);
    return h;
}

} // namespace

H265Packetizer::H265Packetizer() : H265Packetizer(Config{}) {}

H265Packetizer::H265Packetizer(Config cfg) : m_cfg(cfg)
{
    // 至少能装 FU 头 3 字节 + 1 字节数据
    if (m_cfg.max_packet_size <= kRtpHeaderSize + 3) {
        m_cfg.max_packet_size = kRtpHeaderSize + 4;
    }
    m_buf.reserve(m_cfg.max_packet_size);
}

std::size_t H265Packetizer::max_payload() const noexcept
{
    return m_cfg.max_packet_size - kRtpHeaderSize;
}

void H265Packetizer::on_frame(span<const std::uint8_t> frame, const Sink& sink)
{
    if (frame.empty() || !sink) {
        return;
    }

    struct Range {
        std::size_t begin;
        std::size_t end;
    };
    std::vector<Range> nals;
    std::size_t nal_begin = 0;
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

    if (nals.empty()) {
        if (frame.size() >= 2) {
            emit_nal(frame, /*marker=*/true, sink);
        }
        return;
    }

    const std::size_t last = nals.size() - 1;
    for (std::size_t k = 0; k < nals.size(); ++k) {
        emit_nal(frame.subspan(nals[k].begin, nals[k].end - nals[k].begin),
                 /*marker=*/k == last, sink);
    }
}

void H265Packetizer::emit_nal(span<const std::uint8_t> nal, bool marker, const Sink& sink)
{
    if (nal.size() < 2) {
        return;   // H265 的 NAL 头就 2 字节，更短的非法
    }
    const NalHeader h = parse_nal_header(nal);

    const bool is_param = (h.type == kVps || h.type == kSps || h.type == kPps);
    if (is_param && !m_cfg.send_parameter_sets_inband) {
        return;
    }

    if (nal.size() <= max_payload()) {
        emit_single_nal(nal, marker, sink);
    } else {
        emit_fu(nal, marker, sink);
    }
}

void H265Packetizer::emit_single_nal(span<const std::uint8_t> nal, bool marker,
                                     const Sink& sink)
{
    sink(nal, marker);
}

void H265Packetizer::emit_fu(span<const std::uint8_t> nal, bool marker, const Sink& sink)
{
    // FU（RFC 7798 §4.4.3）：
    //   +---------------+---------------+
    //   | PayloadHdr (2 字节)            |  F(1) | Type=49(6) | LayerId(6) | TID(3)
    //   +---------------+---------------+
    //   | FU header (1 字节)             |  S(1) | E(1) | FuType(6)
    //   +---------------+---------------+
    //   | 分片数据 ...                    |  = 原 NAL 去掉 2 字节头的部分
    //   +---------------+---------------+
    const NalHeader h = parse_nal_header(nal);

    const std::uint8_t payload_hdr0 = static_cast<std::uint8_t>(
        (h.forbidden << 7) | static_cast<std::uint8_t>((kFu & 0x3F) << 1) | ((h.layer >> 5) & 0x01));
    const std::uint8_t payload_hdr1 = static_cast<std::uint8_t>(
        ((h.layer & 0x1F) << 3) | (h.tid & 0x07));

    const std::size_t chunk_max = max_payload() - 3;   // 扣 3 字节头
    if (chunk_max == 0) {
        return;
    }

    const std::uint8_t* data = nal.data() + 2;         // 跳过 2 字节 NAL 头
    std::size_t remain = nal.size() - 2;
    bool first = true;

    m_buf.resize(max_payload());

    while (remain > 0) {
        const std::size_t n = (remain > chunk_max) ? chunk_max : remain;

        m_buf[0] = payload_hdr0;
        m_buf[1] = payload_hdr1;

        std::uint8_t fu_hdr = static_cast<std::uint8_t>(h.type & 0x3F);
        if (first) {
            fu_hdr |= 0x80;      // S = 1
        }
        const bool last = (remain == n);
        if (last) {
            fu_hdr |= 0x40;      // E = 1
        }
        m_buf[2] = fu_hdr;

        std::memcpy(m_buf.data() + 3, data, n);
        data   += n;
        remain -= n;

        sink(span<const std::uint8_t>(m_buf.data(), 3 + n), marker && last);

        first = false;
    }
}

} // namespace rtpmech
