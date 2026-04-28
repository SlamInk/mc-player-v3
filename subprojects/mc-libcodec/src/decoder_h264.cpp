#include "decoder_h264.h"

#include "cpu_features.h"
#include "dpb.h"

namespace mclc {

struct DecoderH264::Impl {
    CpuFeatures cpu = detect_cpu_features();
    Dpb         dpb;
    // TODO: SPS/PPS / slice / CABAC / IDCT / MC / deblocking 全套；
    // 参考 OpenH264 (BSD-2)、libde265 (LGPL-3) 的 *算法描述*，不复制代码。
    // ITU-T JM reference (BSD-3) 作 100% bit-exact CI 闸。
};

DecoderH264::DecoderH264()  : impl_{std::make_unique<Impl>()} {}
DecoderH264::~DecoderH264() = default;

mclc_status_t DecoderH264::submit(const uint8_t*, size_t, int64_t) noexcept {
    return MCLC_ERR_UNSUPPORTED;     // v1 骨架；MFT 主路径优先，HEVC Extension 缺失才会路由到 H.265 软解
}
mclc_status_t DecoderH264::pull(mclc_nv12_frame_t*) noexcept { return MCLC_ERR_NEED_MORE_INPUT; }
mclc_status_t DecoderH264::flush() noexcept { return MCLC_OK; }

}  // namespace mclc
