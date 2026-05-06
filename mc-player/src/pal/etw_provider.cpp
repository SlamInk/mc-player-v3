/*
 * ETW TraceLogging Provider 实现 — 性能量度规范 §9.3。
 *
 * 设计：
 *   - 单文件 self-describing TraceLogging Provider（`TRACELOGGING_DEFINE_PROVIDER`）；
 *   - 进程启动期 `register_provider`，退出期 `unregister_provider`；
 *   - 三类 emit_* 函数对位 Counter/Gauge/Histogram 的 record；
 *   - keyword bit + phase tag 通过 keyword field 透传，运维侧用 `tracelog -enableex` 抓指定 keyword。
 *
 * Provider name: "mc-player"（注册后 `logman query providers mc-player` 可见）。
 * Provider GUID: F8E1A0C2-7A3B-4C1F-9D2E-6B5A4F8C1234（自生成 v4 UUID）。
 */

#include "pal/etw_provider.h"

#include <Windows.h>

// WINEVENT_LEVEL_* 在 <winmeta.h>（与 manifest-based ETW 同源；TraceLogging 不自动 include）。
#include <winmeta.h>

// TraceLogging 公开 API（Win10+，全自描述事件）。
#include <TraceLoggingProvider.h>

#include <atomic>
#include <cstring>
#include <string>

namespace mcp::pal::etw {

namespace {

// Provider 描述符 — 由 TraceLoggingRegister 拥有。GUID 用 GUID literal 形式。
TRACELOGGING_DEFINE_PROVIDER(
    g_provider,
    "mc-player",
    // {F8E1A0C2-7A3B-4C1F-9D2E-6B5A4F8C1234}
    (0xF8E1A0C2, 0x7A3B, 0x4C1F, 0x9D, 0x2E, 0x6B, 0x5A, 0x4F, 0x8C, 0x12, 0x34));

std::atomic<bool> g_registered{false};

constexpr uint64_t phase_bits(metric::Phase p) noexcept {
    switch (p) {
        case metric::Phase::ColdStart:     return 0x1'0000'0000ULL;
        case metric::Phase::WarmSteady:    return 0x2'0000'0000ULL;
        case metric::Phase::ErrorRecovery: return 0x4'0000'0000ULL;
    }
    return 0x2'0000'0000ULL;  // 默认 WarmSteady
}

}  // namespace

void register_provider() noexcept {
    if (g_registered.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    HRESULT hr = TraceLoggingRegister(g_provider);
    if (FAILED(hr)) {
        // 注册失败不阻塞启动 — Phase 0 验收会在 logman query 阶段发现。
        g_registered.store(false, std::memory_order_release);
    }
}

void unregister_provider() noexcept {
    if (!g_registered.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    TraceLoggingUnregister(g_provider);
}

bool is_registered() noexcept {
    return g_registered.load(std::memory_order_acquire);
}

// TraceLoggingKeyword macro 要求编译期常量;keyword bit + phase bit 通过 event field
// 透传(运维侧 post-filter,而不是 ETW kernel-side filter)。这是"灵活性 < 性能"的工程让步:
// hot path 的 ETW write 在 keyword=0x0 时 ETW kernel 不走 filter 路径，仍然 inline。
void emit_counter(std::string_view name, uint64_t delta, Keyword kw,
                  metric::Phase phase) noexcept {
    if (!g_registered.load(std::memory_order_acquire)) return;
    // TraceLogging 要求 null-terminated string；string_view 不保证 NUL，需复制到 stack。
    char buf[128];
    const auto n = (name.size() < sizeof(buf) - 1) ? name.size() : sizeof(buf) - 1;
    std::memcpy(buf, name.data(), n);
    buf[n] = '\0';

    const uint64_t kwbits   = static_cast<uint64_t>(kw);
    const uint64_t phasebit = phase_bits(phase);

    TraceLoggingWrite(
        g_provider,
        "Counter",
        TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
        TraceLoggingString(buf, "name"),
        TraceLoggingHexUInt64(kwbits,   "keyword"),
        TraceLoggingHexUInt64(phasebit, "phase_bit"),
        TraceLoggingUInt64(delta, "delta"),
        TraceLoggingUInt8(static_cast<uint8_t>(phase), "phase"));
}

void emit_gauge(std::string_view name, int64_t value, Keyword kw,
                metric::Phase phase) noexcept {
    if (!g_registered.load(std::memory_order_acquire)) return;
    char buf[128];
    const auto n = (name.size() < sizeof(buf) - 1) ? name.size() : sizeof(buf) - 1;
    std::memcpy(buf, name.data(), n);
    buf[n] = '\0';

    const uint64_t kwbits   = static_cast<uint64_t>(kw);
    const uint64_t phasebit = phase_bits(phase);

    TraceLoggingWrite(
        g_provider,
        "Gauge",
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingString(buf, "name"),
        TraceLoggingHexUInt64(kwbits,   "keyword"),
        TraceLoggingHexUInt64(phasebit, "phase_bit"),
        TraceLoggingInt64(value, "value"),
        TraceLoggingUInt8(static_cast<uint8_t>(phase), "phase"));
}

void emit_histogram(std::string_view name, int64_t value_ns, Keyword kw,
                    metric::Phase phase) noexcept {
    if (!g_registered.load(std::memory_order_acquire)) return;
    char buf[128];
    const auto n = (name.size() < sizeof(buf) - 1) ? name.size() : sizeof(buf) - 1;
    std::memcpy(buf, name.data(), n);
    buf[n] = '\0';

    const uint64_t kwbits   = static_cast<uint64_t>(kw);
    const uint64_t phasebit = phase_bits(phase);

    TraceLoggingWrite(
        g_provider,
        "Histogram",
        TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
        TraceLoggingString(buf, "name"),
        TraceLoggingHexUInt64(kwbits,   "keyword"),
        TraceLoggingHexUInt64(phasebit, "phase_bit"),
        TraceLoggingInt64(value_ns, "value_ns"),
        TraceLoggingUInt8(static_cast<uint8_t>(phase), "phase"));
}

}  // namespace mcp::pal::etw
