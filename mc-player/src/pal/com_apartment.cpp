#include "pal/com_apartment.h"

#include <Windows.h>
#include <Objbase.h>

namespace mcp::pal {

ComApartment::ComApartment(Model m) noexcept {
    const DWORD flags =
        (m == Model::mta ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED) | COINIT_DISABLE_OLE1DDE;
    HRESULT hr = ::CoInitializeEx(nullptr, flags);
    // S_OK = 此次 init 拥有；S_FALSE = 已经在同一线程被相同模式 init 过；RPC_E_CHANGED_MODE = 模式冲突。
    owns_init_ = (hr == S_OK || hr == S_FALSE);
}

ComApartment::~ComApartment() noexcept {
    if (owns_init_) {
        ::CoUninitialize();
    }
}

}  // namespace mcp::pal
