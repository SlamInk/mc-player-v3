/*
 * Error mapping — HRESULT / WSA error / errno → mc_status_t。
 *
 * 公开 ABI 只暴露 mc_status_t（int32_t）；内部传递 HRESULT 时通过本模块归一化。
 * 任何路径失败应同时把 ETW 日志打齐，由 caller 决定是否经事件回调上报。
 */

#ifndef MC_PLAYER_PAL_ERROR_H_
#define MC_PLAYER_PAL_ERROR_H_

#include <Windows.h>

#include "mc-player/mc_player_types.h"

namespace mcp::pal {

mc_status_t status_from_hresult(HRESULT hr) noexcept;
mc_status_t status_from_wsa(int wsa_err) noexcept;

const char* status_to_string(mc_status_t status) noexcept;

// 仅用于诊断日志：把 HRESULT 翻译成静态字符串（库内 buffer，调用方不释放）。
const char* hresult_to_string(HRESULT hr) noexcept;

}  // namespace mcp::pal

#endif  // MC_PLAYER_PAL_ERROR_H_
