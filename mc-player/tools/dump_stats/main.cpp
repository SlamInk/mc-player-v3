/*
 * mc-player metric stats dump 工具 — plan Phase 0 §0.1 验收"stats JSON dump 工具可输出"。
 *
 * 用途：
 *   1. 验证 ETW Provider "mc-player" 已注册可见（logman query providers）。
 *   2. 验证 metric Registry 能录入 Counter/Gauge/Histogram 三类。
 *   3. 输出符合性能量度规范 §9.1 命名规范的 JSON 快照（CI 闸消费）。
 *
 * 用法：
 *   dump_stats.exe                          # 录几条样本 metric + 立即 snapshot 输出 JSON
 *   dump_stats.exe --duration_ms 60000      # 录样本后 sleep 60s 再 snapshot（Phase 0 §0.4 commit 模板要求）
 *   dump_stats.exe --self_test              # 自检 + 验证字段排序稳定性（CI 用）
 *
 * Phase 0 hot path 埋点尚未完成（Phase 1+ 落地）；本工具自录的 metric 字段为 placeholder。
 */

#include "mc-player/mc_player.h"

#include "pal/etw_provider.h"
#include "pal/metric.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

void emit_placeholder_samples() {
    using namespace mcp::pal::metric;
    // 性能量度规范 §2.2 9 段 timer placeholder（Phase 1+ 由 controller hot path 真实 record）。
    Registry::instance().timer("mc.stage.udp_rx_ns").record(2'500'000);          // 2.5 ms
    Registry::instance().timer("mc.stage.jitter_dwell_ns").record(8'000'000);    // 8 ms
    Registry::instance().timer("mc.stage.depack_ns").record(500'000);            // 0.5 ms
    Registry::instance().timer("mc.stage.decode_alloc_ns").record(50'000);       // 50 us
    Registry::instance().timer("mc.stage.decode_actual_ns").record(6'000'000);   // 6 ms
    Registry::instance().timer("mc.stage.decode_output_ns").record(5'000);       // 5 us
    Registry::instance().timer("mc.stage.upload_ns").record(0);
    Registry::instance().timer("mc.stage.yuv2rgb_ns").record(2'000'000);         // 2 ms
    Registry::instance().timer("mc.stage.present_queue_ns").record(2'000'000);   // 2 ms

    Registry::instance().counter("mc.gate.drop_refs_resolved_count").inc(0);
    Registry::instance().counter("mc.gate.drop_params_present_count").inc(0);
    Registry::instance().counter("mc.gate.drop_recovery_complete_count").inc(0);
    Registry::instance().counter("mc.gate.drop_color_meta_known_count").inc(0);
    Registry::instance().counter("mc.gate.drop_reorder_resolved_count").inc(0);
    Registry::instance().counter("mc.gate.drop_gpu_fence_signaled_count").inc(0);

    Registry::instance().gauge("mc.decoder.kind").set(0);  // MC_DECODER_NONE placeholder
    Registry::instance().gauge("mc.preset.active_id").set(2);  // REALTIME_LAN placeholder

    // Phase 3 Present Epoch + Watchdog (plan §3.0 / 性能量度规范 §7.1) placeholder。
    // 真实值由 T5 渲染线程在 hot path 写入 (PresentEpoch / DcompRoot::commit)。
    Registry::instance().counter("mc.render.epoch_pair_skew_count").inc(0);   // 必达 = 0 永久
    Registry::instance().counter("mc.render.dcomp_commit_count").inc(0);
    Registry::instance().counter("mc.render.watchdog_redraw_count").inc(0);
    Registry::instance().counter("mc.render.resize_buffers_count").inc(0);
    Registry::instance().counter("mc.render.resize_clearstate_violation_count").inc(0);
    Registry::instance().gauge("mc.render.present_epoch_id").set(0);
    Registry::instance().gauge("mc.render.profile_target").set(0);   // MC_RENDER_PROFILE_AUTO
    Registry::instance().gauge("mc.render.profile_actual").set(0);
    Registry::instance().gauge("mc.render.dcomp_on").set(0);
    Registry::instance().gauge("mc.render.frame_latency_waitable_on").set(0);
    Registry::instance().gauge("mc.render.allow_tearing_on").set(0);

    // ETW emit 一次,验证 Provider 在线 (运维侧 tracelog 抓取可见)。
    using mcp::pal::etw::Keyword;
    using mcp::pal::metric::Phase;
    mcp::pal::etw::emit_counter("mc.dump_stats.placeholder", 1, Keyword::Stage, Phase::WarmSteady);
    mcp::pal::etw::emit_gauge("mc.dump_stats.placeholder_g", 0, Keyword::Stage, Phase::WarmSteady);
    mcp::pal::etw::emit_histogram("mc.dump_stats.placeholder_h", 1'000, Keyword::Stage, Phase::WarmSteady);
}

void escape_json(std::string& out, std::string_view s) {
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
}

void dump_json(const mcp::pal::metric::Registry::Snapshot& snap) {
    std::string out;
    out.reserve(8 * 1024);
    out += "{\n";

    out += "  \"counters\": [\n";
    for (size_t i = 0; i < snap.counters.size(); ++i) {
        const auto& [n, v] = snap.counters[i];
        out += "    {\"name\": \"";
        escape_json(out, n);
        out += "\", \"value\": " + std::to_string(v) + "}";
        if (i + 1 < snap.counters.size()) out += ",";
        out += "\n";
    }
    out += "  ],\n";

    out += "  \"gauges\": [\n";
    for (size_t i = 0; i < snap.gauges.size(); ++i) {
        const auto& [n, v] = snap.gauges[i];
        out += "    {\"name\": \"";
        escape_json(out, n);
        out += "\", \"value\": " + std::to_string(v) + "}";
        if (i + 1 < snap.gauges.size()) out += ",";
        out += "\n";
    }
    out += "  ],\n";

    out += "  \"histograms\": [\n";
    for (size_t i = 0; i < snap.histograms.size(); ++i) {
        const auto& [n, count, mn, mx, p50, p95, p99] = snap.histograms[i];
        out += "    {\"name\": \"";
        escape_json(out, n);
        out += "\", \"count\": " + std::to_string(count);
        out += ", \"min_ns\": " + std::to_string(mn);
        out += ", \"max_ns\": " + std::to_string(mx);
        out += ", \"p50_ns\": " + std::to_string(p50);
        out += ", \"p95_ns\": " + std::to_string(p95);
        out += ", \"p99_ns\": " + std::to_string(p99);
        out += "}";
        if (i + 1 < snap.histograms.size()) out += ",";
        out += "\n";
    }
    out += "  ],\n";

    out += "  \"etw_provider_registered\": ";
    out += (mcp::pal::etw::is_registered() ? "true" : "false");
    out += "\n";

    out += "}\n";

    std::fwrite(out.data(), 1, out.size(), stdout);
}

}  // namespace

int main(int argc, char** argv) {
    int duration_ms = 0;
    bool self_test  = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--duration_ms") == 0 && i + 1 < argc) {
            duration_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--self_test") == 0) {
            self_test = true;
        }
    }

    mc_init_options_t init{};
    init.struct_size        = sizeof(init);
    init.struct_version     = 2;
    init.enable_etw_tracing = 1;
    if (mc_global_init(&init) != MC_OK) {
        std::fprintf(stderr, "mc_global_init failed\n");
        return 1;
    }

    emit_placeholder_samples();

    if (duration_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    }

    auto snap = mcp::pal::metric::Registry::instance().snapshot();
    dump_json(snap);

    if (self_test) {
        // CI 校验：必有 9 段 timer + 6 bit gate counter + ETW Provider 注册。
        bool ok = mcp::pal::etw::is_registered() && snap.histograms.size() >= 9 &&
                  snap.counters.size() >= 6;
        if (!ok) {
            std::fprintf(stderr, "self_test FAILED: etw=%d hist=%zu cnt=%zu\n",
                         mcp::pal::etw::is_registered() ? 1 : 0,
                         snap.histograms.size(),
                         snap.counters.size());
            mc_global_shutdown();
            return 2;
        }
    }

    mc_global_shutdown();
    return 0;
}
