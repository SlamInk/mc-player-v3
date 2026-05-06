/*
 * QPC 封装 — ns 单位时钟。
 *
 * ADD §2 #5：时钟 → QPC；同步等待 → 高分辨率 Waitable Timer。
 * 全项目禁止 std::chrono::steady_clock 直接采样：QPC 才是 Windows 上 ns 级单调时钟，
 * 且与 ETW / IAudioClock::GetPosition 时间基对齐。
 */

#ifndef MC_PLAYER_PAL_CLOCK_H_
#define MC_PLAYER_PAL_CLOCK_H_

#include <cstdint>

namespace mcp::pal {

class Clock {
public:
    /// 初始化（首次调用读取 QPF）。线程安全；可重复调用。
    static void init() noexcept;

    /// 当前 QPC 值（原始 ticks）。
    static int64_t qpc_now() noexcept;

    /// 当前时间戳，单位 100 ns（与 NTP / RTCP 兼容的精度）。
    static int64_t now_100ns() noexcept;

    /// 当前时间戳，单位 ns。
    static int64_t now_ns() noexcept;

    /// `now_ns()` 的别名（性能量度规范 §9.5 / plan Phase 0 §0.1）。
    /// 命名对位 ETW + metric 调用习惯；与 `now_ns` 完全等价。
    static int64_t qpc_now_ns() noexcept { return now_ns(); }

    /// 当前时间戳，单位 us。
    static int64_t now_us() noexcept;

    /// 当前时间戳，单位 ms。
    static int64_t now_ms() noexcept;

    /// QPC ticks → ns。
    static int64_t ticks_to_ns(int64_t ticks) noexcept;

    /// ns → QPC ticks（向下取整）。
    static int64_t ns_to_ticks(int64_t ns) noexcept;
};

/// 高分辨率单次 sleep（µs 精度）。底层用 CreateWaitableTimerExW + HIGH_RESOLUTION。
/// 主线程可用，但优先级敏感线程（T4/T5/T6）应用 WaitForSingleObject(swap_chain_waitable, …) 替代。
void high_res_sleep_us(int64_t us) noexcept;

}  // namespace mcp::pal

#endif  // MC_PLAYER_PAL_CLOCK_H_
