#include "preset/preset_apply.h"

#include <cstdio>

#include "pal/log.h"
#include "pal/metric.h"

namespace mcp::preset {

void emit_apply_partial(const char* subsystem, const char* target_tier,
                          const char* actual_tier) noexcept {
    char metric_name[160];
    std::snprintf(metric_name, sizeof(metric_name),
                  "mc.preset.apply_partial_count.%s.target.%s.actual.%s",
                  subsystem, target_tier, actual_tier);
    pal::metric::Registry::instance().counter(metric_name).inc();
    pal::metric::Registry::instance().counter("mc.preset.apply_partial_count").inc();
    MCP_LOGF(pal::LogLevel::info,
             "PresetApply graceful degrade subsystem=%s target=%s actual=%s",
             subsystem, target_tier, actual_tier);
}

mc_status_t apply_preset(const Preset& preset) noexcept {
    pal::metric::ScopedTimer t{
        pal::metric::Registry::instance().timer("mc.preset.apply_ns")};

    pal::metric::Registry::instance().gauge("mc.preset.active_id")
        .set(static_cast<int64_t>(preset.id));
    MCP_LOGF(pal::LogLevel::info,
             "PresetApply: applying %s (jitter=%d/%ums rtcp=%d present=%d render_profile=%d)",
             preset.name, static_cast<int>(preset.jitter), preset.jitter_target_delay_ms,
             static_cast<int>(preset.rtcp), static_cast<int>(preset.present),
             static_cast<int>(preset.render_profile));

    // Phase 9.0 主线 / 9.1-9.4 各子目标完成后,各子系统的 apply_preset() 接口
    // 在此调用并按 capability_probe §7.1 graceful degrade:
    //
    //   if (!decoder->apply_preset(preset)) emit_apply_partial("decoder", ...);
    //   if (!jitter ->apply_preset(preset)) emit_apply_partial("jitter", ...);   // 9.3
    //   if (!render ->apply_preset(preset)) emit_apply_partial("render", ...);   // 9.1
    //   if (!present->apply_preset(preset)) emit_apply_partial("present", ...);  // 9.4
    //   if (!rtcp   ->apply_preset(preset)) emit_apply_partial("rtcp", ...);     // 9.2
    //   if (!gate   ->apply_preset(preset)) emit_apply_partial("gate", ...);     // 9.0/9.1
    //
    // 当前 Phase 9.0 结构性骨架:仅落 active_id gauge 与日志;
    // 各子系统 apply 接口在 9.1/9.2/9.3/9.4 单独 wire。SDI_REPLACEMENT 命中
    // 实际效果按本表渐进 unlock(详 plan §9.x SDI 阶段图)。
    return MC_OK;
}

}  // namespace mcp::preset
