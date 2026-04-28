#include "decoder_h265.h"

#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

#include "cpu_features.h"
#include "dpb.h"
#include "hevc_parser.h"
#include "hevc_syntax.h"
#include "nal.h"

namespace mclc {

namespace {

/// Phase 1 占位：尚未实现实际像素重建。生成中间灰 NV12 帧并标 decode_error，
/// 让上游 ValidityGate 丢帧。Phase 2 补充真实重建后该分支删除。
struct PlaceholderFrame {
    std::vector<uint8_t> y;
    std::vector<uint8_t> uv;
    uint32_t             width  = 0;
    uint32_t             height = 0;
    int64_t              pts_us = 0;
    bool                 is_keyframe = false;
    int8_t               vui_primaries = 2;
    int8_t               vui_matrix    = 2;
    int8_t               vui_transfer  = 2;
    int8_t               vui_full_range = 0;
};

}  // namespace

struct DecoderH265::Impl {
    CpuFeatures cpu = detect_cpu_features();

    std::unordered_map<uint8_t, HevcVps> vps_map;
    std::unordered_map<uint8_t, HevcSps> sps_map;
    std::unordered_map<uint8_t, HevcPps> pps_map;

    std::deque<PlaceholderFrame> output_queue;
    std::vector<uint8_t>         scratch_rbsp;

    void on_nal(const HevcNalHeader& hdr, std::span<const uint8_t> rbsp, int64_t pts_us) noexcept {
        switch (hdr.nal_unit_type) {
            case kNalVpsNut: {
                HevcVps vps{};
                if (parse_vps(rbsp, vps)) vps_map[vps.vps_id] = std::move(vps);
                break;
            }
            case kNalSpsNut: {
                HevcSps sps{};
                if (parse_sps(rbsp, sps)) sps_map[sps.sps_id] = std::move(sps);
                break;
            }
            case kNalPpsNut: {
                HevcPps pps{};
                // 找对应 SPS（简化：取唯一 SPS 或 sps_id 0）。
                const HevcSps* sps_ptr = nullptr;
                if (!sps_map.empty()) sps_ptr = &sps_map.begin()->second;
                if (!sps_ptr) break;
                if (parse_pps(rbsp, pps, *sps_ptr)) pps_map[pps.pps_id] = std::move(pps);
                break;
            }
            default: {
                if (!is_slice(hdr.nal_unit_type)) break;
                if (sps_map.empty() || pps_map.empty()) break;
                const HevcPps& pps = pps_map.begin()->second;
                auto sps_it = sps_map.find(pps.sps_id);
                if (sps_it == sps_map.end()) break;
                const HevcSps& sps = sps_it->second;

                HevcSliceHeader sh{};
                if (!parse_slice_header(rbsp, hdr.nal_unit_type, sps, pps, sh)) break;

                // Phase 1 占位：仅 IDR 切片产生输出帧（标 decode_error）。
                if (is_idr(hdr.nal_unit_type) && sh.first_slice_segment_in_pic_flag) {
                    PlaceholderFrame f;
                    f.width  = sps.pic_width_in_luma_samples;
                    f.height = sps.pic_height_in_luma_samples;
                    f.y.assign(static_cast<std::size_t>(f.width) * f.height, 128);
                    f.uv.assign(static_cast<std::size_t>(f.width) * f.height / 2, 128);
                    f.pts_us = pts_us;
                    f.is_keyframe = true;
                    f.vui_primaries = sps.colour_description_present_flag ?
                                        sps.colour_primaries : 2;
                    f.vui_matrix    = sps.colour_description_present_flag ?
                                        sps.matrix_coeffs : 2;
                    f.vui_transfer  = sps.colour_description_present_flag ?
                                        sps.transfer_characteristics : 2;
                    f.vui_full_range = sps.video_full_range_flag;
                    output_queue.push_back(std::move(f));
                }
                break;
            }
        }
    }
};

DecoderH265::DecoderH265()  : impl_{std::make_unique<Impl>()} {}
DecoderH265::~DecoderH265() = default;

mclc_status_t DecoderH265::submit(const uint8_t* annexb, size_t bytes, int64_t pts_us) noexcept {
    if (!annexb || bytes == 0) return MCLC_ERR_INVALID_ARG;
    hevc_split_nals(std::span<const uint8_t>(annexb, bytes),
                     [&](const HevcNalHeader& h, std::span<const uint8_t> rbsp) {
                         impl_->on_nal(h, rbsp, pts_us);
                     },
                     impl_->scratch_rbsp);
    return MCLC_OK;
}

mclc_status_t DecoderH265::pull(mclc_nv12_frame_t* out_frame) noexcept {
    if (impl_->output_queue.empty()) return MCLC_ERR_NEED_MORE_INPUT;

    // pull 取队首；指针指向 impl 内 buffer，下一次 submit/pull 前有效。
    static thread_local PlaceholderFrame s_held;
    s_held = std::move(impl_->output_queue.front());
    impl_->output_queue.pop_front();

    out_frame->width    = s_held.width;
    out_frame->height   = s_held.height;
    out_frame->pts_us   = s_held.pts_us;
    out_frame->y_plane  = s_held.y.data();
    out_frame->y_stride = s_held.width;
    out_frame->uv_plane = s_held.uv.data();
    out_frame->uv_stride= s_held.width;
    out_frame->is_keyframe = s_held.is_keyframe ? 1 : 0;
    // Phase 1 占位：实际像素未重建，标 decode_error 让 ValidityGate 丢。
    out_frame->decode_error = 1;

    out_frame->vui_colour_primaries        = s_held.vui_primaries;
    out_frame->vui_matrix_coefficients     = s_held.vui_matrix;
    out_frame->vui_transfer_characteristics = s_held.vui_transfer;
    out_frame->vui_video_full_range_flag   = s_held.vui_full_range;
    return MCLC_OK;
}

mclc_status_t DecoderH265::flush() noexcept {
    impl_->output_queue.clear();
    return MCLC_OK;
}

}  // namespace mclc
