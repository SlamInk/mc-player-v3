/*
 * Codec MFT Audio — Microsoft AAC Decoder MFT。
 *
 * SetOutputType 显式协商 MFAudioFormat_Float 32-bit interleaved，避免后置重采样（ADD §4.2 / §5.8）。
 */

#ifndef MC_PLAYER_MEDIA_CODEC_MFT_AUDIO_H_
#define MC_PLAYER_MEDIA_CODEC_MFT_AUDIO_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "media/frame.h"
#include "mc-player/mc_player_types.h"

namespace mcp::media {

class CodecMftAudio {
public:
    using EmitFn = std::function<void(AudioFrame&&)>;

    struct Config {
        mc_audio_codec_t codec        = MC_AUDIO_CODEC_AAC;
        uint32_t         sample_rate  = 0;
        uint32_t         channels     = 0;
        EmitFn           emit;
    };

    explicit CodecMftAudio(Config cfg);
    ~CodecMftAudio();

    mc_status_t start(std::span<const uint8_t> audio_specific_config) noexcept;
    void        submit(std::span<const uint8_t> raw, int64_t pts_us) noexcept;
    void        stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp::media

#endif  // MC_PLAYER_MEDIA_CODEC_MFT_AUDIO_H_
