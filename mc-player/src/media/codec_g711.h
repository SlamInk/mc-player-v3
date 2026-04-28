/*
 * G.711 A-law / μ-law 解码器（ITU-T G.711 Appendix）。
 *
 * 设计（CLAUDE.md：256-entry LUT 自实现）：
 *   - 不走 MFT（OS 不提供专用 G.711 MFT）；不引第三方。
 *   - 8-bit codeword → 13/14-bit 线性 PCM，在加载时编译期生成 LUT。
 *   - RTP payload 字节流（每字节 1 个 8 kHz mono sample）直接喂解码即可，
 *     无 packetization 重组（RFC 3551 §4.5.14 / §4.5.15）。
 */

#ifndef MC_PLAYER_MEDIA_CODEC_G711_H_
#define MC_PLAYER_MEDIA_CODEC_G711_H_

#include <cstdint>
#include <span>
#include <vector>

namespace mcp::media {

/// A-law: 把 RTP G.711 PCMA payload（每字节 1 sample）解到 [-1, 1] float interleaved（mono）。
/// out_pcm 会被 resize 到 raw.size()。
void g711_alaw_decode(std::span<const uint8_t> raw, std::vector<float>& out_pcm) noexcept;

/// μ-law: 同上，G.711 PCMU。
void g711_ulaw_decode(std::span<const uint8_t> raw, std::vector<float>& out_pcm) noexcept;

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_CODEC_G711_H_
