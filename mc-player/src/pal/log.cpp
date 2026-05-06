#include "pal/log.h"

#include <Windows.h>
#include <debugapi.h>
#include <TraceLoggingProvider.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "pal/clock.h"

// TraceLogging Provider GUID — mc-player.tracing v1
// 通过 PerfView/WPR 订阅 "mc-player.tracing" provider 即可拉取。
TRACELOGGING_DEFINE_PROVIDER(
    g_mcp_tracelog_provider,
    "mc-player.tracing",
    // {7c3f3e36-3b2c-4f63-9ad6-7f6c4f84a7d2}
    (0x7c3f3e36, 0x3b2c, 0x4f63, 0x9a, 0xd6, 0x7f, 0x6c, 0x4f, 0x84, 0xa7, 0xd2));

namespace mcp::pal {

namespace {

constexpr std::size_t kFormattedMessageMax = 1024;

struct LogState {
    std::mutex mu;
    LogConfig  cfg{};
    bool       initialized{false};
    FILE*      file_sink{nullptr};
};

LogState& state() noexcept {
    static LogState s;
    return s;
}

const char* basename_of(const char* path) noexcept {
    if (!path) return "?";
    const char* last = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

const char* level_to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::silent: return "SILENT";
        case LogLevel::trace:  return "TRACE";
        case LogLevel::debug:  return "DEBUG";
        case LogLevel::info:   return "INFO";
        case LogLevel::warn:   return "WARN";
        case LogLevel::error:  return "ERROR";
        case LogLevel::fatal:  return "FATAL";
    }
    return "?";
}

const wchar_t* level_to_wstring(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::trace: return L"TRACE";
        case LogLevel::debug: return L"DEBUG";
        case LogLevel::info:  return L"INFO";
        case LogLevel::warn:  return L"WARN";
        case LogLevel::error: return L"ERROR";
        case LogLevel::fatal: return L"FATAL";
    }
    return L"?";
}

void emit_to_debugger(LogLevel level, std::string_view msg, const std::source_location& loc) noexcept {
    wchar_t buf[kFormattedMessageMax];
    int n = std::swprintf(buf, kFormattedMessageMax,
                          L"[mc-player][%s][%hs:%u] %.*hs\n",
                          level_to_wstring(level),
                          loc.file_name(), static_cast<unsigned>(loc.line()),
                          static_cast<int>(msg.size()), msg.data());
    if (n > 0) {
        ::OutputDebugStringW(buf);
    }
}

}  // namespace

void log_init(const LogConfig& cfg) noexcept {
    auto& st = state();
    std::scoped_lock lk{st.mu};
    if (!st.initialized) {
        TraceLoggingRegister(g_mcp_tracelog_provider);
        st.initialized = true;
    }
    st.cfg = cfg;
}

void log_shutdown() noexcept {
    auto& st = state();
    std::scoped_lock lk{st.mu};
    if (st.initialized) {
        TraceLoggingUnregister(g_mcp_tracelog_provider);
        st.initialized = false;
    }
    st.file_sink = nullptr;
}

void log_attach_file(FILE* fp) noexcept {
    auto& st = state();
    std::scoped_lock lk{st.mu};
    st.file_sink = fp;
}

void log_set_level(LogLevel new_level) noexcept {
    auto& st = state();
    std::scoped_lock lk{st.mu};
    st.cfg.min_level = new_level;
}

LogLevel log_current_level() noexcept {
    auto& st = state();
    std::scoped_lock lk{st.mu};
    return st.cfg.min_level;
}

void log_write(LogLevel level,
               std::string_view msg,
               const std::source_location& loc) noexcept {
    auto& st = state();
    LogConfig cfg;
    {
        std::scoped_lock lk{st.mu};
        cfg = st.cfg;
    }

    if (static_cast<int>(level) < static_cast<int>(cfg.min_level)) {
        return;
    }

    if (cfg.enable_etw && st.initialized) {
        // ETW 字段保持简短；level 作为字符串字段写入（TraceLoggingLevel 必须是编译期常量，
        // 不能用 runtime level 参数；filter 端按 level 字段过滤即可）。
        TraceLoggingWrite(
            g_mcp_tracelog_provider,
            "Log",
            // 5 = WINEVENT_LEVEL_VERBOSE (avoid pulling evntcons.h)
            TraceLoggingLevel(5),
            TraceLoggingString(level_to_string(level), "level"),
            TraceLoggingString(loc.file_name(), "file"),
            TraceLoggingUInt32(static_cast<uint32_t>(loc.line()), "line"),
            TraceLoggingCountedString(msg.data(), static_cast<UINT16>(msg.size()), "msg"));
    }

    if (cfg.log_to_debugger && ::IsDebuggerPresent()) {
        emit_to_debugger(level, msg, loc);
    }

    if (cfg.log_to_stderr) {
        std::fprintf(stderr, "[mc-player][%s] %.*s\n",
                     level_to_string(level),
                     static_cast<int>(msg.size()), msg.data());
        std::fflush(stderr);
    }

    {
        std::scoped_lock lk{st.mu};
        if (st.file_sink) {
            std::fprintf(st.file_sink, "[mc-player][%s][%s:%u] %.*s\n",
                         level_to_string(level),
                         basename_of(loc.file_name()),
                         static_cast<unsigned>(loc.line()),
                         static_cast<int>(msg.size()), msg.data());
            std::fflush(st.file_sink);
        }
    }
}

void log_writef(LogLevel level,
                const std::source_location& loc,
                const char* fmt, ...) noexcept {
    char buf[kFormattedMessageMax];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (static_cast<std::size_t>(n) >= sizeof(buf)) {
        n = static_cast<int>(sizeof(buf) - 1);
    }
    log_write(level, std::string_view{buf, static_cast<std::size_t>(n)}, loc);
}

}  // namespace mcp::pal
