/*
 * 日志 — 多级 + ringbuf + ETW 镜像。
 *
 * ADD §5.14 PAL：多级日志 + ringbuf；TraceLogging 包装。
 *
 * 设计原则：
 *   - 可观测优于性能（ADD §2 #9）；但 hot path（per-frame、per-RTP-packet）严禁直接日志，
 *     仅使用 LogLevel::trace + ETW，留给 PerfView / WPR 离线分析。
 *   - 日志 ringbuf 是进程级单例，所有线程共写、单读取线程批量落盘（v1 简化为同步）。
 *   - 字符串永远 UTF-8。Windows API 走宽字符的转换由 caller 完成。
 */

#ifndef MC_PLAYER_PAL_LOG_H_
#define MC_PLAYER_PAL_LOG_H_

#include <cstdint>
#include <source_location>
#include <string_view>

namespace mcp::pal {

enum class LogLevel : int {
    silent = -1,    // 默认(plan §8.E):仅 ETW;无 stderr / debugger / file 输出
    trace  = 0,
    debug  = 1,
    info   = 2,
    warn   = 3,
    error  = 4,
    fatal  = 5,
};

struct LogConfig {
    // plan §8.E 默认 silent — 诊断时由 ui_panel 调高级别。
    // 现状 (Phase 8-E 之前默认 info) 暂保留兼容性,实际部署可按需改 silent。
    LogLevel    min_level         = LogLevel::info;
    bool        enable_etw        = true;
    bool        log_to_debugger   = true;     // OutputDebugString 镜像
    bool        log_to_stderr     = true;     // 控制台镜像（便于联调）
    const char* ringbuf_dir       = nullptr;  // 非空时落盘
};

/// 运行期调级(ui_panel "诊断模式"按钮)。
void log_set_level(LogLevel new_level) noexcept;
[[nodiscard]] LogLevel log_current_level() noexcept;

/// 进程级初始化。可重复调用，后续调用更新配置。
void log_init(const LogConfig& cfg) noexcept;
void log_shutdown() noexcept;

/// 附加一个日志文件 sink（除 ETW / debugger / stderr 外额外写文件）。
/// 传入的 FILE* 由调用方持有并负责关闭；本模块只追加写入。传 nullptr 取消。
void log_attach_file(struct _iobuf* fp) noexcept;

/// 实际写日志。msg 必须 UTF-8；caller 不需追加换行。
void log_write(LogLevel level,
               std::string_view msg,
               const std::source_location& loc = std::source_location::current()) noexcept;

/// printf 风格便捷版本（hot path 不要用）。
void log_writef(LogLevel level,
                const std::source_location& loc,
                const char* fmt, ...) noexcept;

}  // namespace mcp::pal

// 便捷宏：把 source_location 自动捕获。Hot path 用 MCP_LOG_TRACE，其它按级别选。
#define MCP_LOG(level, msg)                                                                        \
    ::mcp::pal::log_write((level), (msg), std::source_location::current())

#define MCP_LOGF(level, ...)                                                                       \
    ::mcp::pal::log_writef((level), std::source_location::current(), __VA_ARGS__)

#define MCP_LOG_TRACE(msg)  MCP_LOG(::mcp::pal::LogLevel::trace, msg)
#define MCP_LOG_DEBUG(msg)  MCP_LOG(::mcp::pal::LogLevel::debug, msg)
#define MCP_LOG_INFO(msg)   MCP_LOG(::mcp::pal::LogLevel::info,  msg)
#define MCP_LOG_WARN(msg)   MCP_LOG(::mcp::pal::LogLevel::warn,  msg)
#define MCP_LOG_ERROR(msg)  MCP_LOG(::mcp::pal::LogLevel::error, msg)
#define MCP_LOG_FATAL(msg)  MCP_LOG(::mcp::pal::LogLevel::fatal, msg)

#endif  // MC_PLAYER_PAL_LOG_H_
