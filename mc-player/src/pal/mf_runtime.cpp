#include "pal/mf_runtime.h"

#include <Windows.h>
#include <mfapi.h>

#include <atomic>
#include <mutex>

#include "pal/error.h"

namespace mcp::pal {

namespace {

struct State {
    std::mutex mu;
    int        refcount{0};
};

State& state() noexcept {
    static State s;
    return s;
}

}  // namespace

mc_status_t mf_runtime_acquire() noexcept {
    auto& st = state();
    std::scoped_lock lk{st.mu};
    if (st.refcount == 0) {
        HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) {
            return status_from_hresult(hr);
        }
    }
    ++st.refcount;
    return MC_OK;
}

void mf_runtime_release() noexcept {
    auto& st = state();
    std::scoped_lock lk{st.mu};
    if (st.refcount == 0) return;
    if (--st.refcount == 0) {
        ::MFShutdown();
    }
}

}  // namespace mcp::pal
