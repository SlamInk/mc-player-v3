/*
 * DPB（Decoded Picture Buffer）+ 引用追踪（ADD §5.7.3）。
 *
 * 软解必备 — MFT 路径由驱动黑盒处理；mc-libcodec 必须自己保证：
 *   - 显式维护 {frame_num, poc, refs_used_by, mark}
 *   - 解码每个 slice 前校验所需 ref 全部 present
 *   - FrameNum gap > 0 或 POC 不连续 → 判定参考帧丢失，丢该 slice，标后续非 anchor 帧 invalid
 *
 * 该不变量是软解路径区别于 MFT 黑盒最关键的一点。
 */

#ifndef MC_LIBCODEC_DPB_H_
#define MC_LIBCODEC_DPB_H_

#include <cstdint>
#include <vector>

namespace mclc {

struct DpbEntry {
    int32_t frame_num    = 0;
    int32_t poc          = 0;
    bool    is_reference = false;
    bool    is_long_term = false;
};

class Dpb {
public:
    void clear() noexcept;
    bool can_decode_slice(int32_t expected_frame_num, int32_t expected_poc) const noexcept;
    void insert(const DpbEntry& e) noexcept;
    void mark_unused(int32_t frame_num) noexcept;

private:
    std::vector<DpbEntry> entries_;
};

}  // namespace mclc

#endif  // MC_LIBCODEC_DPB_H_
