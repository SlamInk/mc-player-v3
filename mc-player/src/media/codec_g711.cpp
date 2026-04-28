#include "media/codec_g711.h"

#include <array>

namespace mcp::media {

namespace {

// ITU-T G.711 §2 A-law decode：8-bit codeword → 13-bit 线性 PCM（fits int16）。
constexpr int16_t alaw_to_pcm(uint8_t a) noexcept {
    a ^= 0x55;                              // A-law inverse mask
    int sign = (a & 0x80) ? -1 : 1;
    int seg  = (a >> 4) & 0x07;
    int mant = a & 0x0F;
    int linear;
    if (seg >= 1) {
        linear = ((mant << 4) + 0x108) << (seg - 1);
    } else {
        linear = (mant << 4) + 8;
    }
    return static_cast<int16_t>(sign * linear);
}

// ITU-T G.711 §3 μ-law decode：8-bit codeword → 14-bit 线性 PCM（fits int16）。
constexpr int16_t ulaw_to_pcm(uint8_t u) noexcept {
    u = static_cast<uint8_t>(~u);
    int sign = (u & 0x80) ? -1 : 1;
    int seg  = (u >> 4) & 0x07;
    int mant = u & 0x0F;
    int linear = ((mant << 3) + 0x84) << seg;
    return static_cast<int16_t>(sign * (linear - 0x84));
}

constexpr std::array<int16_t, 256> make_lut(int16_t (*f)(uint8_t)) noexcept {
    std::array<int16_t, 256> lut{};
    for (int i = 0; i < 256; ++i) {
        lut[i] = f(static_cast<uint8_t>(i));
    }
    return lut;
}

constexpr auto kAlawLut = make_lut(alaw_to_pcm);
constexpr auto kUlawLut = make_lut(ulaw_to_pcm);

constexpr float kInv32768 = 1.0f / 32768.0f;

void decode_with_lut(const std::array<int16_t, 256>& lut,
                      std::span<const uint8_t> raw,
                      std::vector<float>& out) noexcept {
    out.resize(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        out[i] = lut[raw[i]] * kInv32768;
    }
}

}  // namespace

void g711_alaw_decode(std::span<const uint8_t> raw, std::vector<float>& out_pcm) noexcept {
    decode_with_lut(kAlawLut, raw, out_pcm);
}

void g711_ulaw_decode(std::span<const uint8_t> raw, std::vector<float>& out_pcm) noexcept {
    decode_with_lut(kUlawLut, raw, out_pcm);
}

}  // namespace mcp::media
