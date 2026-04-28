#include "pal/error.h"

#include <cstdio>
#include <winerror.h>
#include <winsock2.h>
#include <mferror.h>
#include <dxgi.h>

namespace mcp::pal {

mc_status_t status_from_hresult(HRESULT hr) noexcept {
    if (SUCCEEDED(hr)) {
        return MC_OK;
    }
    switch (hr) {
        case E_INVALIDARG:                  return MC_ERR_INVALID_ARG;
        case E_POINTER:                     return MC_ERR_INVALID_ARG;
        case E_OUTOFMEMORY:                 return MC_ERR_OUT_OF_MEMORY;
        case E_NOTIMPL:                     return MC_ERR_UNSUPPORTED;
        case E_ACCESSDENIED:                return MC_ERR_AUTH;
        case HRESULT_FROM_WIN32(ERROR_TIMEOUT):
        case HRESULT_FROM_WIN32(WAIT_TIMEOUT):
            return MC_ERR_TIMEOUT;

        case DXGI_ERROR_DEVICE_REMOVED:
        case DXGI_ERROR_DEVICE_RESET:
        case DXGI_ERROR_DEVICE_HUNG:
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
            return MC_ERR_DEVICE_LOST;

        case MF_E_TRANSFORM_TYPE_NOT_SET:
        case MF_E_INVALIDMEDIATYPE:
        case MF_E_UNSUPPORTED_D3D_TYPE:
            return MC_ERR_UNSUPPORTED;

        default:
            return MC_ERR_INTERNAL;
    }
}

mc_status_t status_from_wsa(int wsa_err) noexcept {
    if (wsa_err == 0) {
        return MC_OK;
    }
    switch (wsa_err) {
        case WSAEACCES:                     return MC_ERR_AUTH;
        case WSAEADDRINUSE:
        case WSAEADDRNOTAVAIL:
        case WSAENETDOWN:
        case WSAENETUNREACH:
        case WSAEHOSTUNREACH:
        case WSAECONNREFUSED:
        case WSAECONNRESET:
        case WSAECONNABORTED:
        case WSAESHUTDOWN:
            return MC_ERR_IO;
        case WSAETIMEDOUT:                  return MC_ERR_TIMEOUT;
        case WSA_NOT_ENOUGH_MEMORY:         return MC_ERR_OUT_OF_MEMORY;
        case WSAEINVAL:                     return MC_ERR_INVALID_ARG;
        default:                            return MC_ERR_IO;
    }
}

const char* status_to_string(mc_status_t status) noexcept {
    switch (status) {
        case MC_OK:                  return "MC_OK";
        case MC_ERR_INVALID_ARG:     return "MC_ERR_INVALID_ARG";
        case MC_ERR_NULL_HANDLE:     return "MC_ERR_NULL_HANDLE";
        case MC_ERR_OUT_OF_MEMORY:   return "MC_ERR_OUT_OF_MEMORY";
        case MC_ERR_INVALID_STATE:   return "MC_ERR_INVALID_STATE";
        case MC_ERR_UNSUPPORTED:     return "MC_ERR_UNSUPPORTED";
        case MC_ERR_TIMEOUT:         return "MC_ERR_TIMEOUT";
        case MC_ERR_IO:              return "MC_ERR_IO";
        case MC_ERR_PROTOCOL:        return "MC_ERR_PROTOCOL";
        case MC_ERR_AUTH:            return "MC_ERR_AUTH";
        case MC_ERR_DEVICE_LOST:     return "MC_ERR_DEVICE_LOST";
        case MC_ERR_NO_HARDWARE:     return "MC_ERR_NO_HARDWARE";
        case MC_ERR_INTERNAL:        return "MC_ERR_INTERNAL";
        default:                     return "MC_ERR_UNKNOWN";
    }
}

const char* hresult_to_string(HRESULT hr) noexcept {
    // 静态返回常见 HRESULT 名称；其它返回 "HRESULT_<hex>"。线程局部 buffer 保证 reentrancy。
    thread_local char buffer[24];
    switch (hr) {
        case S_OK:                              return "S_OK";
        case S_FALSE:                           return "S_FALSE";
        case E_FAIL:                            return "E_FAIL";
        case E_INVALIDARG:                      return "E_INVALIDARG";
        case E_OUTOFMEMORY:                     return "E_OUTOFMEMORY";
        case E_POINTER:                         return "E_POINTER";
        case E_NOTIMPL:                         return "E_NOTIMPL";
        case E_ACCESSDENIED:                    return "E_ACCESSDENIED";
        case DXGI_ERROR_DEVICE_REMOVED:         return "DXGI_ERROR_DEVICE_REMOVED";
        case DXGI_ERROR_DEVICE_RESET:           return "DXGI_ERROR_DEVICE_RESET";
        case DXGI_ERROR_DEVICE_HUNG:            return "DXGI_ERROR_DEVICE_HUNG";
        case DXGI_ERROR_INVALID_CALL:           return "DXGI_ERROR_INVALID_CALL";
        case MF_E_TRANSFORM_NEED_MORE_INPUT:    return "MF_E_TRANSFORM_NEED_MORE_INPUT";
        case MF_E_TRANSFORM_STREAM_CHANGE:      return "MF_E_TRANSFORM_STREAM_CHANGE";
        case MF_E_INVALIDREQUEST:               return "MF_E_INVALIDREQUEST";
        default:
            std::snprintf(buffer, sizeof(buffer), "HRESULT_%08lX", static_cast<unsigned long>(hr));
            return buffer;
    }
}

}  // namespace mcp::pal
