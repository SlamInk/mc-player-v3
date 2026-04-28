#include "media/codec_libcodec.h"

#include "mc-libcodec/mc_libcodec.h"
#include "pal/error.h"
#include "pal/log.h"

using Microsoft::WRL::ComPtr;

namespace mcp::media {

namespace {

mclc_codec_t map_codec(mc_video_codec_t c) noexcept {
    switch (c) {
        case MC_VIDEO_CODEC_H264: return MCLC_CODEC_H264;
        case MC_VIDEO_CODEC_H265: return MCLC_CODEC_H265;
        default:                  return MCLC_CODEC_H265;
    }
}

}  // namespace

struct CodecLibcodecVideo::Impl {
    Config                          cfg;
    mclc_decoder_t                  decoder = nullptr;
    ComPtr<ID3D11DeviceContext>     ctx;

    // RAM→GPU 上传纹理（NV12 dynamic）。Phase 2 真实像素就绪后接通。
    ComPtr<ID3D11Texture2D>         upload_tex;
    uint32_t                        upload_w = 0;
    uint32_t                        upload_h = 0;

    bool ensure_upload_texture(uint32_t w, uint32_t h) noexcept {
        if (upload_tex && upload_w == w && upload_h == h) return true;
        upload_tex.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width            = w;
        desc.Height           = h;
        desc.MipLevels        = 1;
        desc.ArraySize        = 1;
        desc.Format           = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_DEFAULT;
        desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags   = 0;
        if (FAILED(cfg.device->CreateTexture2D(&desc, nullptr, &upload_tex))) return false;
        upload_w = w;
        upload_h = h;
        return true;
    }

    void upload_and_emit(const mclc_nv12_frame_t& f) noexcept {
        if (!ensure_upload_texture(f.width, f.height)) return;

        // Y + UV 平面通过 UpdateSubresource 上传（DEFAULT NV12 仅支持 UpdateSubresource，不支持 Map）。
        // NV12 整张作为 subresource 0：UpdateSubresource1 with box 切两次（Y 平面 + UV 平面）。
        D3D11_BOX box_y{};
        box_y.left   = 0;            box_y.right  = f.width;
        box_y.top    = 0;            box_y.bottom = f.height;
        box_y.front  = 0;            box_y.back   = 1;
        ctx->UpdateSubresource(upload_tex.Get(), 0, &box_y,
                                f.y_plane, f.y_stride, 0);

        D3D11_BOX box_uv{};
        box_uv.left   = 0;           box_uv.right  = f.width;
        box_uv.top    = f.height;    box_uv.bottom = f.height + f.height / 2;
        box_uv.front  = 0;           box_uv.back   = 1;
        ctx->UpdateSubresource(upload_tex.Get(), 0, &box_uv,
                                f.uv_plane, f.uv_stride, 0);

        VideoFrame vf;
        vf.pts_us           = f.pts_us;
        vf.width            = f.width;
        vf.height           = f.height;
        vf.source           = FrameSource::libcodec_software;
        vf.dxva_texture     = upload_tex;
        vf.dxva_array_slice = 0;
        vf.is_keyframe      = f.is_keyframe != 0;
        vf.decode_error     = f.decode_error != 0;
        // VUI → mc_color_*：仅当 SPS 明示有 colour_description 才填，否则 AUTO。
        if (f.vui_colour_primaries != 2)
            vf.color_primaries = static_cast<mc_color_primaries_t>(f.vui_colour_primaries);
        if (f.vui_matrix_coefficients != 2)
            vf.color_matrix    = static_cast<mc_color_matrix_t>(f.vui_matrix_coefficients);
        vf.color_range = f.vui_video_full_range_flag ? MC_COLOR_RANGE_FULL : MC_COLOR_RANGE_LIMITED;

        vf.validity_mask = vf.decode_error ? 0u : kValidityAll;

        if (cfg.emit) cfg.emit(std::move(vf));
    }
};

CodecLibcodecVideo::CodecLibcodecVideo(Config cfg) : impl_{std::make_unique<Impl>()} {
    impl_->cfg = std::move(cfg);
}
CodecLibcodecVideo::~CodecLibcodecVideo() { stop(); }

mc_status_t CodecLibcodecVideo::start() noexcept {
    if (!impl_->cfg.device) return MC_ERR_INVALID_ARG;
    impl_->cfg.device->GetImmediateContext(&impl_->ctx);

    mclc_create_options_t o{};
    o.struct_size    = sizeof(o);
    o.struct_version = MCLC_CREATE_OPTIONS_VERSION;
    o.codec          = map_codec(impl_->cfg.codec);
    o.single_thread  = 1;
    o.enable_avx2    = 1;
    if (mclc_create(&o, &impl_->decoder) != MCLC_OK) {
        MCP_LOGF(pal::LogLevel::error, "CodecLibcodecVideo: mclc_create failed");
        return MC_ERR_INTERNAL;
    }
    MCP_LOGF(pal::LogLevel::info,
             "CodecLibcodecVideo: started codec=%d (libcodec software fallback; "
             "Phase 1 only — emits decode_error placeholders pending Phase 2-4)",
             static_cast<int>(impl_->cfg.codec));
    return MC_OK;
}

void CodecLibcodecVideo::submit(std::span<const uint8_t> au, int64_t pts_us) noexcept {
    if (!impl_->decoder) return;
    mclc_submit(impl_->decoder, au.data(), au.size(), pts_us);

    // 同步 drain：libcodec 是 single-threaded，在 submit 内即可 pull。
    while (true) {
        mclc_nv12_frame_t f{};
        f.struct_size    = sizeof(f);
        f.struct_version = MCLC_NV12_FRAME_VERSION;
        const mclc_status_t s = mclc_pull(impl_->decoder, &f);
        if (s == MCLC_ERR_NEED_MORE_INPUT) break;
        if (s != MCLC_OK) break;
        impl_->upload_and_emit(f);
    }
}

void CodecLibcodecVideo::flush() noexcept {
    if (impl_->decoder) mclc_flush(impl_->decoder);
}

void CodecLibcodecVideo::stop() noexcept {
    if (!impl_) return;
    if (impl_->decoder) {
        mclc_destroy(impl_->decoder);
        impl_->decoder = nullptr;
    }
    impl_->upload_tex.Reset();
    impl_->ctx.Reset();
}

}  // namespace mcp::media
