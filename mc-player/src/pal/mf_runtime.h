/*
 * Media Foundation runtime — 进程级 MFStartup / MFShutdown 引用计数。
 *
 * MFStartup 必须在任何 MF API 之前调用；MFShutdown 配对释放。多 session 共用同一份。
 */

#ifndef MC_PLAYER_PAL_MF_RUNTIME_H_
#define MC_PLAYER_PAL_MF_RUNTIME_H_

#include "mc-player/mc_player_types.h"

namespace mcp::pal {

/// 引用计数式启动；线程安全；未启动会调 MFStartup(MF_VERSION, MFSTARTUP_FULL)。
mc_status_t mf_runtime_acquire() noexcept;

/// 引用计数 -1；归零时调 MFShutdown。
void mf_runtime_release() noexcept;

class MfRuntimeRef {
public:
    MfRuntimeRef() noexcept : status_{mf_runtime_acquire()} {}
    ~MfRuntimeRef() noexcept {
        if (status_ == MC_OK) mf_runtime_release();
    }
    MfRuntimeRef(const MfRuntimeRef&)            = delete;
    MfRuntimeRef& operator=(const MfRuntimeRef&) = delete;

    [[nodiscard]] mc_status_t status() const noexcept { return status_; }

private:
    mc_status_t status_;
};

}  // namespace mcp::pal

#endif  // MC_PLAYER_PAL_MF_RUNTIME_H_
