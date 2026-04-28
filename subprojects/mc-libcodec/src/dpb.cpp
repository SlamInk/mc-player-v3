#include "dpb.h"

#include <algorithm>

namespace mclc {

void Dpb::clear() noexcept {
    entries_.clear();
}

bool Dpb::can_decode_slice(int32_t expected_frame_num, int32_t /*expected_poc*/) const noexcept {
    // 简化判定：只校验 frame_num 连续；POC 校验留实现层（与 ref-list 构造耦合）。
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [expected_frame_num](const DpbEntry& e) {
                               return e.is_reference && e.frame_num == expected_frame_num;
                           });
    return it != entries_.end() || expected_frame_num == 0;
}

void Dpb::insert(const DpbEntry& e) noexcept {
    entries_.push_back(e);
}

void Dpb::mark_unused(int32_t frame_num) noexcept {
    for (auto& e : entries_) {
        if (e.frame_num == frame_num) e.is_reference = false;
    }
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                   [](const DpbEntry& e) { return !e.is_reference; }),
                   entries_.end());
}

}  // namespace mclc
