#include "media/depack_aac.h"

#include <cctype>
#include <cstring>

namespace mcp::media {

namespace {
int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

DepackAac::DepackAac(EmitFn emit) noexcept : emit_{std::move(emit)} {}

void DepackAac::set_audio_specific_config_hex(std::string_view hex) noexcept {
    audio_specific_config_.clear();
    audio_specific_config_.reserve(hex.size() / 2);
    int high = -1;
    for (char c : hex) {
        const int v = hex_value(c);
        if (v < 0) continue;
        if (high < 0) high = v;
        else {
            audio_specific_config_.push_back(static_cast<uint8_t>((high << 4) | v));
            high = -1;
        }
    }
}

void DepackAac::set_au_header_lengths(uint32_t size_length, uint32_t index_length) noexcept {
    size_length_  = size_length;
    index_length_ = index_length;
}

void DepackAac::on_rtp(int64_t pts_us, std::span<const uint8_t> payload) noexcept {
    if (payload.size() < 2) return;
    const uint16_t headers_len_bits = static_cast<uint16_t>(
        (static_cast<uint16_t>(payload[0]) << 8) | payload[1]);
    const std::size_t headers_bytes = (headers_len_bits + 7u) / 8u;
    if (payload.size() < 2 + headers_bytes) return;

    // 简化：仅处理 size_length=13, index_length=3 的现网通用形态（每 AU 头 2 字节）。
    // 多 AU 在一个 RTP 时按头解析逐个长度切；下面单 AU 路径是常见情况。
    const std::size_t data_off = 2 + headers_bytes;
    if (data_off >= payload.size()) return;

    AacAccessUnit au;
    au.pts_us = pts_us;
    au.raw_aac.assign(payload.begin() + data_off, payload.end());
    if (emit_) emit_(std::move(au));
}

void DepackAac::reset() noexcept { /* AAC 解包无重组态 */ }

}  // namespace mcp::media
