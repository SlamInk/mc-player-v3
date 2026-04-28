#include "pal/thread.h"

#include <Windows.h>
#include <avrt.h>
#include <processthreadsapi.h>

#include <atomic>
#include <vector>

#include "pal/log.h"

namespace mcp::pal {

namespace {

const wchar_t* mmcss_task_name(MmcssTask t) noexcept {
    switch (t) {
        case MmcssTask::playback:  return L"Playback";
        case MmcssTask::pro_audio: return L"Pro Audio";
        case MmcssTask::capture:   return L"Capture";
        case MmcssTask::none:      return nullptr;
    }
    return nullptr;
}

DWORD_PTR query_pcore_mask() noexcept {
    // 通过 GetLogicalProcessorInformationEx(RelationProcessorCore + EfficiencyClass) 区分。
    // EfficiencyClass: 大值 = 高性能（P-core），0 = 能效（E-core）。
    DWORD len = 0;
    ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0) return 0;

    std::vector<unsigned char> buf(len);
    if (!::GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data()),
            &len)) {
        return 0;
    }

    BYTE max_eff = 0;
    DWORD_PTR pcore_mask = 0;
    DWORD_PTR all_mask   = 0;

    auto* p   = buf.data();
    auto* end = p + len;
    while (p < end) {
        auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(p);
        if (info->Relationship == RelationProcessorCore) {
            const BYTE eff = info->Processor.EfficiencyClass;
            DWORD_PTR mask = 0;
            for (WORD i = 0; i < info->Processor.GroupCount; ++i) {
                mask |= info->Processor.GroupMask[i].Mask;
            }
            all_mask |= mask;
            if (eff > max_eff) {
                max_eff    = eff;
                pcore_mask = mask;
            } else if (eff == max_eff) {
                pcore_mask |= mask;
            }
        }
        p += info->Size;
    }

    // 没有异构 (max_eff==0 全机器)：返回 0 让 caller 不绑亲和性。
    if (max_eff == 0 || pcore_mask == all_mask) {
        return 0;
    }
    return pcore_mask;
}

}  // namespace

ThreadRegistration::ThreadRegistration(ThreadRegistration&& other) noexcept
    : mmcss_handle_{other.mmcss_handle_},
      mmcss_task_index_{other.mmcss_task_index_},
      priority_set_{other.priority_set_} {
    other.mmcss_handle_     = nullptr;
    other.mmcss_task_index_ = 0;
    other.priority_set_     = false;
}

ThreadRegistration& ThreadRegistration::operator=(ThreadRegistration&& other) noexcept {
    if (this != &other) {
        release();
        mmcss_handle_     = other.mmcss_handle_;
        mmcss_task_index_ = other.mmcss_task_index_;
        priority_set_     = other.priority_set_;
        other.mmcss_handle_     = nullptr;
        other.mmcss_task_index_ = 0;
        other.priority_set_     = false;
    }
    return *this;
}

ThreadRegistration::~ThreadRegistration() noexcept {
    release();
}

void ThreadRegistration::release() noexcept {
    if (mmcss_handle_ != nullptr) {
        ::AvRevertMmThreadCharacteristics(mmcss_handle_);
        mmcss_handle_ = nullptr;
    }
    if (priority_set_) {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        priority_set_ = false;
    }
}

mc_status_t ThreadRegistration::apply(const ThreadOptions& options) noexcept {
    if (!options.name.empty()) {
        set_current_thread_name(options.name);
    }

    if (const wchar_t* task = mmcss_task_name(options.mmcss_task)) {
        DWORD index = 0;
        HANDLE h = ::AvSetMmThreadCharacteristicsW(task, &index);
        if (h == nullptr) {
            MCP_LOG_WARN("AvSetMmThreadCharacteristicsW failed");
            // 不致命：失败时退化为普通优先级。
        } else {
            mmcss_handle_     = h;
            mmcss_task_index_ = index;
        }
    }

    if (options.base_priority != THREAD_PRIORITY_NORMAL) {
        if (::SetThreadPriority(::GetCurrentThread(), options.base_priority)) {
            priority_set_ = true;
        }
    }

    if (options.affinity == ThreadAffinityHint::pcore_only) {
        const DWORD_PTR mask = detect_pcore_affinity_mask();
        if (mask != 0) {
            ::SetThreadAffinityMask(::GetCurrentThread(), mask);
        }
    }

    return MC_OK;
}

DWORD_PTR detect_pcore_affinity_mask() noexcept {
    // 单次探测后缓存（机器拓扑运行期不变）。
    static std::atomic<DWORD_PTR> cached{static_cast<DWORD_PTR>(-1)};
    DWORD_PTR v = cached.load(std::memory_order_acquire);
    if (v == static_cast<DWORD_PTR>(-1)) {
        v = query_pcore_mask();
        cached.store(v, std::memory_order_release);
    }
    return v;
}

void set_current_thread_name(const std::string& utf8_name) noexcept {
    if (utf8_name.empty()) return;
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, utf8_name.c_str(),
                                     static_cast<int>(utf8_name.size()),
                                     nullptr, 0);
    if (wlen <= 0) return;
    std::wstring wname(static_cast<std::size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8_name.c_str(),
                          static_cast<int>(utf8_name.size()),
                          wname.data(), wlen);
    // SetThreadDescription 自 Win10 1607+ 可用，VS / WinDbg / ETW 都能取到。
    ::SetThreadDescription(::GetCurrentThread(), wname.c_str());
}

}  // namespace mcp::pal
