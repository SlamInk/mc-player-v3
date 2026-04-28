/*
 * H.264 软解 — Baseline / Constrained Baseline / Main（含 CABAC、B 帧）/ High（含 8×8 transform）。
 *
 * 高级 profile（High 10 / 4:2:2 / 4:4:4）留后续。
 *
 * 模块拓扑（ADD §5.7.3）：
 *   bitstream reader (Exp-Golomb) · CABAC engine · neighborhood / RefPicList ·
 *   SIMD dispatch · DPB + reorder + POC · pipeline 编排 ·
 *   NAL parser + RBSP unescape · SPS/PPS · slice header & data ·
 *   4×4 / 8×8 / 16×16 intra (9 方向) · motion compensation (1/4 像素 sub-pel 插值) ·
 *   IDCT · in-loop deblocking
 */

#ifndef MC_LIBCODEC_DECODER_H264_H_
#define MC_LIBCODEC_DECODER_H264_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "mc-libcodec/mc_libcodec.h"

namespace mclc {

class DecoderH264 {
public:
    DecoderH264();
    ~DecoderH264();

    DecoderH264(const DecoderH264&)            = delete;
    DecoderH264& operator=(const DecoderH264&) = delete;

    mclc_status_t submit(const uint8_t* annexb, size_t bytes, int64_t pts_us) noexcept;
    mclc_status_t pull(mclc_nv12_frame_t* out_frame) noexcept;
    mclc_status_t flush() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mclc

#endif  // MC_LIBCODEC_DECODER_H264_H_
