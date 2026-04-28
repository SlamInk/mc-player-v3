/*
 * Cache-line 常量 — 用于 SPSC 队列 padding，避免 false sharing。
 *
 * ADD §6.1：高频 atomic 操作因 false-sharing 性能降 3-5×。
 * x86_64 主流 CPU cache line = 64B；Apple M-series & 某些服务器 CPU 用 128B；
 * 本项目仅 Windows x64，按 64B 对齐已足够，但保守按 std::hardware_destructive_interference_size 暴露。
 */

#ifndef MC_PLAYER_PAL_CACHE_LINE_H_
#define MC_PLAYER_PAL_CACHE_LINE_H_

#include <cstddef>
#include <new>

namespace mcp::pal {

#if defined(__cpp_lib_hardware_interference_size) && __cpp_lib_hardware_interference_size >= 201703L
inline constexpr std::size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kCacheLineSize = 64;
#endif

}  // namespace mcp::pal

#endif  // MC_PLAYER_PAL_CACHE_LINE_H_
