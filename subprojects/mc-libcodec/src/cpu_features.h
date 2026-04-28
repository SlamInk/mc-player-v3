/*
 * CPU feature detection — 启动期 CPUID 探测填函数指针表（ADD §5.7.5）。
 * SSE2 是 x86_64 ABI 强制，无需 CPUID；AVX2 需 runtime detect。
 */

#ifndef MC_LIBCODEC_CPU_FEATURES_H_
#define MC_LIBCODEC_CPU_FEATURES_H_

namespace mclc {

struct CpuFeatures {
    bool has_sse2 = true;       // x86_64 ABI 保证
    bool has_avx2 = false;
};

CpuFeatures detect_cpu_features() noexcept;

}  // namespace mclc

#endif  // MC_LIBCODEC_CPU_FEATURES_H_
