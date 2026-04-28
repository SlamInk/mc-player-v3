#include "pal/clock.h"

#include <Windows.h>
#include <profileapi.h>
#include <synchapi.h>

#include <atomic>

#include "pal/raii.h"

namespace mcp::pal {

namespace {

constexpr int64_t kNsPerSec  = 1'000'000'000LL;
constexpr int64_t kHnsPerSec = 10'000'000LL;       // 100 ns ticks
constexpr int64_t kUsPerSec  = 1'000'000LL;
constexpr int64_t kMsPerSec  = 1'000LL;

std::atomic<int64_t> g_qpf_hz{0};

int64_t qpf_hz() noexcept {
    int64_t cached = g_qpf_hz.load(std::memory_order_acquire);
    if (cached != 0) return cached;

    LARGE_INTEGER li{};
    ::QueryPerformanceFrequency(&li);
    g_qpf_hz.store(li.QuadPart, std::memory_order_release);
    return li.QuadPart;
}

}  // namespace

void Clock::init() noexcept {
    (void)qpf_hz();
}

int64_t Clock::qpc_now() noexcept {
    LARGE_INTEGER li{};
    ::QueryPerformanceCounter(&li);
    return li.QuadPart;
}

int64_t Clock::ticks_to_ns(int64_t ticks) noexcept {
    const int64_t hz = qpf_hz();
    if (hz <= 0) return 0;
    // 防溢出：先除后乘的精度损失可忽略（ticks 数量级远小于 ns）。
    // 对于 hz=10MHz、ticks~1e15（>3 年）：ticks * 1e9 仍在 int64 范围。
    return (ticks / hz) * kNsPerSec + ((ticks % hz) * kNsPerSec) / hz;
}

int64_t Clock::ns_to_ticks(int64_t ns) noexcept {
    const int64_t hz = qpf_hz();
    if (hz <= 0) return 0;
    return (ns / kNsPerSec) * hz + ((ns % kNsPerSec) * hz) / kNsPerSec;
}

int64_t Clock::now_ns() noexcept {
    return ticks_to_ns(qpc_now());
}

int64_t Clock::now_100ns() noexcept {
    return now_ns() / 100;
}

int64_t Clock::now_us() noexcept {
    return now_ns() / 1000;
}

int64_t Clock::now_ms() noexcept {
    return now_ns() / 1'000'000;
}

void high_res_sleep_us(int64_t us) noexcept {
    if (us <= 0) return;

    HandleGuard timer{::CreateWaitableTimerExW(
        nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS)};

    // HIGH_RESOLUTION timer 自 Win10 1803 起可用；老 OS 路径用普通 timer 兜底。
    if (!timer.valid()) {
        timer.reset(::CreateWaitableTimerW(nullptr, FALSE, nullptr));
        if (!timer.valid()) {
            ::Sleep(static_cast<DWORD>((us + 999) / 1000));
            return;
        }
    }

    LARGE_INTEGER due{};
    // SetWaitableTimer 接受 100 ns 单位，负值 = 相对当前时间。
    due.QuadPart = -static_cast<LONGLONG>(us * 10);

    if (::SetWaitableTimer(timer.get(), &due, 0, nullptr, nullptr, FALSE)) {
        ::WaitForSingleObject(timer.get(), INFINITE);
    } else {
        ::Sleep(static_cast<DWORD>((us + 999) / 1000));
    }
}

}  // namespace mcp::pal
