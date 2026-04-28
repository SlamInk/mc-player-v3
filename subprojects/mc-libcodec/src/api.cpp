#include "mc-libcodec/mc_libcodec.h"

#include <new>

#include "decoder_h264.h"
#include "decoder_h265.h"

struct mclc_decoder_s {
    mclc_codec_t                          codec;
    std::unique_ptr<mclc::DecoderH264>    h264;
    std::unique_ptr<mclc::DecoderH265>    h265;
};

extern "C" {

mclc_status_t mclc_create(const mclc_create_options_t* options, mclc_decoder_t* out) {
    if (!options || !out) return MCLC_ERR_INVALID_ARG;
    if (options->struct_size < sizeof(mclc_create_options_t)) return MCLC_ERR_INVALID_ARG;

    auto* d = new (std::nothrow) mclc_decoder_s{};
    if (!d) return MCLC_ERR_OUT_OF_MEMORY;
    d->codec = options->codec;
    if (options->codec == MCLC_CODEC_H264) {
        d->h264 = std::make_unique<mclc::DecoderH264>();
    } else if (options->codec == MCLC_CODEC_H265) {
        d->h265 = std::make_unique<mclc::DecoderH265>();
    } else {
        delete d;
        return MCLC_ERR_UNSUPPORTED;
    }
    *out = d;
    return MCLC_OK;
}

mclc_status_t mclc_destroy(mclc_decoder_t handle) {
    delete handle;
    return MCLC_OK;
}

mclc_status_t mclc_submit(mclc_decoder_t handle,
                           const uint8_t* annexb, size_t bytes,
                           int64_t pts_us) {
    if (!handle || !annexb) return MCLC_ERR_INVALID_ARG;
    if (handle->h264) return handle->h264->submit(annexb, bytes, pts_us);
    if (handle->h265) return handle->h265->submit(annexb, bytes, pts_us);
    return MCLC_ERR_UNSUPPORTED;
}

mclc_status_t mclc_pull(mclc_decoder_t handle, mclc_nv12_frame_t* out_frame) {
    if (!handle || !out_frame) return MCLC_ERR_INVALID_ARG;
    if (out_frame->struct_size < sizeof(mclc_nv12_frame_t)) return MCLC_ERR_INVALID_ARG;
    if (handle->h264) return handle->h264->pull(out_frame);
    if (handle->h265) return handle->h265->pull(out_frame);
    return MCLC_ERR_UNSUPPORTED;
}

mclc_status_t mclc_flush(mclc_decoder_t handle) {
    if (!handle) return MCLC_ERR_INVALID_ARG;
    if (handle->h264) return handle->h264->flush();
    if (handle->h265) return handle->h265->flush();
    return MCLC_OK;
}

}  // extern "C"
