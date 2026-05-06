#include "probe/hardware_probe.h"

#include "media/codec_amf.h"
#include "media/codec_nvdec.h"
#include "media/codec_onevpl.h"
#include "pal/dxgi_caps_probe.h"
#include "pal/log.h"
#include "pal/metric.h"

namespace mcp::probe {

HardwareSnapshot run_hardware_probe(HWND hwnd) noexcept {
    pal::metric::ScopedTimer t{
        pal::metric::Registry::instance().timer("mc.probe.hardware_complete_ns")};

    HardwareSnapshot out;

    pal::DxgiCapsProbe probe;
    if (probe.probe() != MC_OK) {
        MCP_LOG_WARN("HardwareProbe: dxgi caps probe failed");
        return out;
    }

    const pal::AdapterCaps* caps = probe.find_by_hwnd(hwnd);
    if (!caps) {
        // 退化:用第一个非软件 adapter
        for (const auto& a : probe.adapters()) {
            if (!a.is_software) { caps = &a; break; }
        }
    }
    if (!caps) return out;

    out.vendor_id              = caps->vendor_id;
    out.is_software_adapter    = caps->is_software;
    out.h264_supported         = caps->h264_supported;
    out.hevc_main_supported    = caps->hevc_main_supported;
    out.hevc_main10_supported  = caps->hevc_main10_supported;
    out.av1_supported          = caps->av1_supported;
    out.dual_bind_supported    = caps->dual_bind_supported;
    out.tearing_supported      = caps->allow_tearing_supported;
    out.vendor_sdk_present     = caps->nvcuvid_dll_present || caps->onevpl_dll_present
                                  || caps->amf_dll_present;
    // Phase 8-B 实装后 hevc_extension_present 由 detector 填入。
    out.hevc_extension_present = false;
    out.complete               = true;

    pal::metric::Registry::instance().gauge("mc.probe.hardware_vendor_id")
        .set(static_cast<int64_t>(out.vendor_id));
    return out;
}

}  // namespace mcp::probe
