/*
 * 线程管理 — MMCSS 注册、亲和性、优先级（ADD §3.3）。
 *
 * 强约束：
 *   - 视频 Decode/Render 线程：MMCSS task = "Playback"，优先级实测 ~23
 *   - 音频 Render 线程：MMCSS task = "Pro Audio"，优先级实测 ~26
 *   - 网络 RX：不挂 MMCSS；TIME_CRITICAL + 绑 P-core
 *   - libdatachannel 内部线程不主动绑（对齐 Chromium 主线）
 *
 * MMCSS 自 Win10 1607 起对每条注册线程自动维持优先级；不需 RT-aware loop。
 *
 * Intel 12 代+ E-core 调度问题：T2/T5/T6 应绑 P-core。本模块提供 P-core 探测；
 * 具体绑哪个核由 caller 决定（我们绑 mask = "all P-cores"，让 OS 在 P-core 间挑）。
 */

#ifndef MC_PLAYER_PAL_THREAD_H_
#define MC_PLAYER_PAL_THREAD_H_

#include <Windows.h>

#include <cstdint>
#include <string>

#include "mc-player/mc_player_types.h"
#include "pal/raii.h"

namespace mcp::pal {

enum class MmcssTask {
    none,           // 不注册，用普通优先级
    playback,       // 视频 / 通用回放
    pro_audio,      // 音频引擎
    capture,
};

enum class ThreadAffinityHint {
    any,            // OS 自由调度
    pcore_only,     // 优先 P-core（Intel 12 代+ 才区分；其它机器 == any）
};

struct ThreadOptions {
    std::string         name;
    MmcssTask           mmcss_task         = MmcssTask::none;
    int                 base_priority      = THREAD_PRIORITY_NORMAL;
    ThreadAffinityHint  affinity           = ThreadAffinityHint::any;
};

/// 给当前线程注册 MMCSS task / 设置优先级 / 设置亲和性 / 命名。
/// 返回的 handle 在线程退出前必须保持存活；caller 持有该结构生命周期。
class ThreadRegistration {
public:
    ThreadRegistration() noexcept = default;
    ThreadRegistration(const ThreadRegistration&)            = delete;
    ThreadRegistration& operator=(const ThreadRegistration&) = delete;
    ThreadRegistration(ThreadRegistration&& other) noexcept;
    ThreadRegistration& operator=(ThreadRegistration&& other) noexcept;
    ~ThreadRegistration() noexcept;

    /// 在当前线程内调用一次。
    mc_status_t apply(const ThreadOptions& options) noexcept;

    /// 显式释放（析构时也会释放）。
    void release() noexcept;

private:
    HANDLE  mmcss_handle_{nullptr};   // AvSetMmThreadCharacteristicsW 返回值
    DWORD   mmcss_task_index_{0};
    bool    priority_set_{false};
};

/// 探测 P-core mask（Intel 12 代+ 区分 P/E-core 时返回非零；否则返回 0 = "无信息")。
DWORD_PTR detect_pcore_affinity_mask() noexcept;

/// 给当前线程命名（VS / WinDbg / ETW 显示）。仅诊断用。
void set_current_thread_name(const std::string& utf8_name) noexcept;

}  // namespace mcp::pal

#endif  // MC_PLAYER_PAL_THREAD_H_
