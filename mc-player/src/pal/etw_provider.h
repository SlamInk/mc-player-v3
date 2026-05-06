/*
 * ETW Provider — TraceLogging 包装（性能量度规范 §9.3 上报通道）。
 *
 * Provider GUID（自生成，与项目绑定）:
 *   `{F8E1A0C2-7A3B-4C1F-9D2E-6B5A4F8C1234}` (mcp.player.etw)
 *   logman query providers 应能看到 "mc-player"。
 *
 * 事件类别（keyword bit）:
 *   0x0001 Stage      — 9 段阶段 timer
 *   0x0002 Queue      — SPSC 队列水位 / drop
 *   0x0004 Gate       — Frame Validity Gate 六 bit drop / 污染态
 *   0x0008 Decoder    — 解码档位 / 降级
 *   0x0010 Render     — Present / DCOMP / PresentMon
 *   0x0020 Probe      — caps_probe 各档耗时 / skip reason
 *   0x0040 Preset     — Preset selector / reload / partial
 *   0x0080 Hdcm       — HDCM 组件状态 / 安装尝试
 *   0x0100 FirstFrame — 首帧分解
 *   0x0200 Resource   — CPU/GPU/Mem
 *   0x0400 Error      — device_lost / network / TDR
 *
 * Phase tag 通过 keyword 高位透传 (0x1_0000_0000 ColdStart / 0x2_0000_0000 WarmSteady /
 * 0x4_0000_0000 ErrorRecovery)；性能量度规范 §1.2 三场景区分。
 *
 * 实现：用 `<TraceLoggingProvider.h>` (Win10+ TraceLogging API)，
 * 不依赖 mc.man manifest 编译；运行期 register/unregister。
 */

#ifndef MC_PLAYER_PAL_ETW_PROVIDER_H_
#define MC_PLAYER_PAL_ETW_PROVIDER_H_

#include <cstdint>
#include <string_view>

#include "pal/metric.h"

namespace mcp::pal::etw {

/// Keyword bit（性能量度规范 §11.4 metric 域映射）。
enum class Keyword : uint64_t {
    Stage      = 0x0001,
    Queue      = 0x0002,
    Gate       = 0x0004,
    Decoder    = 0x0008,
    Render     = 0x0010,
    Probe      = 0x0020,
    Preset     = 0x0040,
    Hdcm       = 0x0080,
    FirstFrame = 0x0100,
    Resource   = 0x0200,
    Error      = 0x0400,
};

/// 进程启动期注册一次；进程退出期 unregister。线程安全。
void register_provider() noexcept;
void unregister_provider() noexcept;

/// 写入 Counter inc 事件（高频路径用，event level=Verbose）。
void emit_counter(std::string_view name, uint64_t delta, Keyword kw,
                  metric::Phase phase) noexcept;

/// 写入 Gauge set 事件（1Hz 聚合路径用，event level=Informational）。
void emit_gauge(std::string_view name, int64_t value, Keyword kw,
                metric::Phase phase) noexcept;

/// 写入 Histogram record 事件（高频，event level=Verbose；ETW 是源头数据）。
void emit_histogram(std::string_view name, int64_t value_ns, Keyword kw,
                    metric::Phase phase) noexcept;

/// Provider 是否已注册成功（init 期诊断用，logman 不可见时排查）。
[[nodiscard]] bool is_registered() noexcept;

}  // namespace mcp::pal::etw

#endif  // MC_PLAYER_PAL_ETW_PROVIDER_H_
