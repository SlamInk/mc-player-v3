#include "media/codec_bridge.h"

#include "pal/log.h"

namespace mcp::media {

struct CodecBridge::Impl {
    mc_video_codec_t                            codec;
    Microsoft::WRL::ComPtr<ID3D11Device>        device;
    bool                                        allow_software_fallback;
    EmitFn                                      emit;
    mc_decoder_kind_t                           active_kind = MC_DECODER_NONE;
    // TODO: codec_mft_video instance / mc-libcodec instance；按能力探测激活其一。
};

CodecBridge::CodecBridge(mc_video_codec_t codec,
                         Microsoft::WRL::ComPtr<ID3D11Device> device,
                         bool allow_software_fallback,
                         EmitFn emit)
    : impl_{std::make_unique<Impl>()} {
    impl_->codec                   = codec;
    impl_->device                  = std::move(device);
    impl_->allow_software_fallback = allow_software_fallback;
    impl_->emit                    = std::move(emit);
}

CodecBridge::~CodecBridge() = default;

void CodecBridge::submit_h264(H264AccessUnit&& au) noexcept {
    // TODO: 把 AU 喂给 codec_mft_video（async event loop）；emit VideoFrame 经 dual-bind fence wait。
    (void)au;
}

void CodecBridge::submit_h265(H265AccessUnit&& au) noexcept {
    (void)au;
}

void CodecBridge::flush() noexcept {
    impl_->active_kind = MC_DECODER_NONE;
}

mc_decoder_kind_t CodecBridge::active_kind() const noexcept {
    return impl_->active_kind;
}

}  // namespace mcp::media
