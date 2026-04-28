#include "cpu_features.h"

#include <intrin.h>

namespace mclc {

CpuFeatures detect_cpu_features() noexcept {
    CpuFeatures f{};
    int regs[4] = {0};

    __cpuid(regs, 0);
    const int max_leaf = regs[0];

    if (max_leaf >= 7) {
        __cpuidex(regs, 7, 0);
        // EBX bit 5 = AVX2
        f.has_avx2 = (regs[1] & (1 << 5)) != 0;
    }
    return f;
}

}  // namespace mclc
