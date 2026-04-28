/*
 * 通用 RAII 工具 — 全项目禁止裸 new/delete / CloseHandle / Release。
 *
 * - HandleGuard:   持有 HANDLE，析构调用 CloseHandle；忽略 INVALID_HANDLE_VALUE / NULL。
 * - SocketGuard:   持有 SOCKET，析构 closesocket。
 * - 函数式 ScopeExit: 任何离域释放（取消 task、unlock unique_lock 之外的清理）。
 *
 * 公开 ABI 不暴露这些类型；仅库内部使用。
 */

#ifndef MC_PLAYER_PAL_RAII_H_
#define MC_PLAYER_PAL_RAII_H_

#include <Windows.h>
#include <winsock2.h>

#include <utility>

namespace mcp::pal {

class HandleGuard {
public:
    HandleGuard() noexcept = default;
    explicit HandleGuard(HANDLE h) noexcept : h_{h} {}
    HandleGuard(const HandleGuard&)            = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;

    HandleGuard(HandleGuard&& other) noexcept : h_{other.h_} {
        other.h_ = nullptr;
    }
    HandleGuard& operator=(HandleGuard&& other) noexcept {
        if (this != &other) {
            reset(other.h_);
            other.h_ = nullptr;
        }
        return *this;
    }
    ~HandleGuard() noexcept { reset(); }

    HANDLE get() const noexcept { return h_; }
    [[nodiscard]] bool valid() const noexcept {
        return h_ != nullptr && h_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() noexcept {
        HANDLE tmp = h_;
        h_ = nullptr;
        return tmp;
    }
    void reset(HANDLE h = nullptr) noexcept {
        if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h_);
        }
        h_ = h;
    }
    HANDLE* put() noexcept {
        reset();
        return &h_;
    }

private:
    HANDLE h_{nullptr};
};

class SocketGuard {
public:
    SocketGuard() noexcept = default;
    explicit SocketGuard(SOCKET s) noexcept : s_{s} {}
    SocketGuard(const SocketGuard&)            = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

    SocketGuard(SocketGuard&& other) noexcept : s_{other.s_} {
        other.s_ = INVALID_SOCKET;
    }
    SocketGuard& operator=(SocketGuard&& other) noexcept {
        if (this != &other) {
            reset(other.s_);
            other.s_ = INVALID_SOCKET;
        }
        return *this;
    }
    ~SocketGuard() noexcept { reset(); }

    SOCKET get() const noexcept { return s_; }
    [[nodiscard]] bool valid() const noexcept { return s_ != INVALID_SOCKET; }
    SOCKET release() noexcept {
        SOCKET tmp = s_;
        s_ = INVALID_SOCKET;
        return tmp;
    }
    void reset(SOCKET s = INVALID_SOCKET) noexcept {
        if (s_ != INVALID_SOCKET) {
            ::closesocket(s_);
        }
        s_ = s;
    }

private:
    SOCKET s_{INVALID_SOCKET};
};

template <typename Fn>
class ScopeExit {
public:
    explicit ScopeExit(Fn&& fn) noexcept : fn_{std::move(fn)} {}
    ScopeExit(const ScopeExit&)            = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() noexcept {
        if (active_) {
            fn_();
        }
    }
    void dismiss() noexcept { active_ = false; }

private:
    Fn   fn_;
    bool active_{true};
};

template <typename Fn>
[[nodiscard]] auto on_scope_exit(Fn&& fn) noexcept {
    return ScopeExit<Fn>{std::forward<Fn>(fn)};
}

}  // namespace mcp::pal

#endif  // MC_PLAYER_PAL_RAII_H_
