/*
 * H.265 软解 — Main / Main10。Main12 / Main 4:4:4 留后续。
 *
 * v1 关键场景：HEVC Extension 缺失（Win10 / 11 retail 默认）— 自动兜底，UX 无感。
 *
 * 模块拓扑（ADD §5.7.3）：
 *   2-byte NAL header · VPS/SPS/PPS · slice · CABAC（与 H.264 不同上下文模型）·
 *   35 intra 方向 · advanced motion (merge / AMVP / MVD) · IDCT 4×4–32×32 + DST 4×4 ·
 *   deblocking · SAO · DPB
 */

#ifndef MC_LIBCODEC_DECODER_H265_H_
#define MC_LIBCODEC_DECODER_H265_H_

#include <cstdint>
#include <memory>

#include "mc-libcodec/mc_libcodec.h"

namespace mclc {

class DecoderH265 {
public:
    DecoderH265();
    ~DecoderH265();

    DecoderH265(const DecoderH265&)            = delete;
    DecoderH265& operator=(const DecoderH265&) = delete;

    mclc_status_t submit(const uint8_t* annexb, size_t bytes, int64_t pts_us) noexcept;
    mclc_status_t pull(mclc_nv12_frame_t* out_frame) noexcept;
    mclc_status_t flush() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mclc

#endif  // MC_LIBCODEC_DECODER_H265_H_
