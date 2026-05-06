#include "pal/dxgi_caps_probe.h"

#include <d3d11.h>
#include <d3d11_4.h>
#include <initguid.h>
#include <mfapi.h>

#include "pal/error.h"
#include "pal/log.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;

namespace mcp::pal {

namespace {

// DXVA decoder profile GUIDs (Windows DDK / mfapi.h)。
// 将 GUID 内联以减少头文件依赖；与 mfapi.h 中宏值同。
// {1B81BE68-A0C7-11D3-B984-00C04F2E73C5} D3D11_DECODER_PROFILE_H264_VLD_NOFGT — H.264 Main/High.
DEFINE_GUID(MCP_D3D11_DECODER_PROFILE_H264_VLD_NOFGT,
    0x1b81be68, 0xa0c7, 0x11d3, 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5);

// {5b11d51b-2f4c-4452-bcc3-09f2a1160cc0} HEVC Main
DEFINE_GUID(MCP_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN,
    0x5b11d51b, 0x2f4c, 0x4452, 0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0);

// {107af0e0-ef1a-4d19-aba8-67a163073d13} HEVC Main10
DEFINE_GUID(MCP_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10,
    0x107af0e0, 0xef1a, 0x4d19, 0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13);

// {b8be4ccb-cf53-46ba-8d59-d6b8a6da5d2a} AV1 Profile 0
DEFINE_GUID(MCP_D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0,
    0xb8be4ccb, 0xcf53, 0x46ba, 0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a);

constexpr UINT kDualBindProbeWidth   = 64;
constexpr UINT kDualBindProbeHeight  = 64;

bool check_decoder_format(ID3D11VideoDevice* vd, REFGUID profile, DXGI_FORMAT format) noexcept {
    if (!vd) return false;
    BOOL supported = FALSE;
    HRESULT hr = vd->CheckVideoDecoderFormat(&profile, format, &supported);
    return SUCCEEDED(hr) && supported == TRUE;
}

bool check_dual_bind(ID3D11Device* device) noexcept {
    if (!device) return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width            = kDualBindProbeWidth;
    desc.Height           = kDualBindProbeHeight;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags   = 0;

    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &tex);
    return SUCCEEDED(hr);
}

bool check_allow_tearing(IDXGIFactory6* factory) noexcept {
    if (!factory) return false;
    BOOL supported = FALSE;
    HRESULT hr = factory->CheckFeatureSupport(
        DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported));
    return SUCCEEDED(hr) && supported == TRUE;
}

void enumerate_outputs(IDXGIAdapter1* adapter, std::vector<HMONITOR>& out) noexcept {
    UINT i = 0;
    ComPtr<IDXGIOutput> output;
    while (adapter->EnumOutputs(i++, &output) != DXGI_ERROR_NOT_FOUND) {
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor != nullptr) {
            out.push_back(desc.Monitor);
        }
        output.Reset();
    }
}

// vendor SDK DLL 轻量探测：LoadLibraryW 试打开 → FreeLibrary 关闭。
// 仅判存在性 + 版本兼容性 (driver 加载需求)；实际 decode 能力由具体 codec impl 验。
bool dll_loadable(const wchar_t* name) noexcept {
    if (!name) return false;
    HMODULE h = ::LoadLibraryW(name);
    if (!h) return false;
    ::FreeLibrary(h);
    return true;
}

}  // namespace

mc_status_t DxgiCapsProbe::probe() noexcept {
    factory_.Reset();
    adapters_.clear();

    HRESULT hr = ::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) {
        return status_from_hresult(hr);
    }

    const bool tearing_global = check_allow_tearing(factory_.Get());

    UINT idx = 0;
    ComPtr<IDXGIAdapter1> adapter;
    while (factory_->EnumAdapters1(idx++, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);

        AdapterCaps caps;
        caps.luid                    = desc.AdapterLuid;
        caps.description             = desc.Description;
        caps.dedicated_video_memory  = desc.DedicatedVideoMemory;
        caps.dedicated_system_memory = desc.DedicatedSystemMemory;
        caps.shared_system_memory    = desc.SharedSystemMemory;
        caps.is_software             = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        caps.allow_tearing_supported = tearing_global;
        caps.vendor_id               = desc.VendorId;

        enumerate_outputs(adapter.Get(), caps.monitors);

        // 只对硬件 adapter 探测 codec / dual-bind。WARP 软适配器不算硬解。
        if (!caps.is_software) {
            ComPtr<ID3D11Device>        device;
            ComPtr<ID3D11DeviceContext> context;
            const D3D_FEATURE_LEVEL fls[] = {
                D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            };
            D3D_FEATURE_LEVEL got{};
            HRESULT hr_dev = ::D3D11CreateDevice(
                adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                fls, _countof(fls), D3D11_SDK_VERSION,
                &device, &got, &context);
            if (SUCCEEDED(hr_dev)) {
                ComPtr<ID3D11VideoDevice> video_device;
                if (SUCCEEDED(device.As(&video_device))) {
                    caps.h264_supported = check_decoder_format(
                        video_device.Get(), MCP_D3D11_DECODER_PROFILE_H264_VLD_NOFGT,
                        DXGI_FORMAT_NV12);
                    caps.hevc_main_supported = check_decoder_format(
                        video_device.Get(), MCP_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN,
                        DXGI_FORMAT_NV12);
                    caps.hevc_main10_supported = check_decoder_format(
                        video_device.Get(), MCP_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10,
                        DXGI_FORMAT_P010);
                    caps.av1_supported = check_decoder_format(
                        video_device.Get(), MCP_D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0,
                        DXGI_FORMAT_NV12);

                    // 4K H.264：粗略以 max texture dim 推断；CheckVideoDecoderFormat 不直接给最大分辨率。
                    caps.h264_4k_supported = caps.h264_supported;
                }
                caps.dual_bind_supported = check_dual_bind(device.Get());
            }
        }

        adapters_.push_back(std::move(caps));
        adapter.Reset();
    }

    if (adapters_.empty()) {
        MCP_LOG_WARN("DxgiCapsProbe: no DXGI adapters enumerated");
    }

    // Vendor SDK 探测进程级单次执行（DLL 名是机器属性,与 adapter 无关）；
    // 结果灌到每个 adapter 的 *_dll_present 便于 caps 表统一查询。
    const bool nvcuvid = dll_loadable(L"nvcuvid.dll");
    const bool onevpl  = dll_loadable(L"vpl.dll") || dll_loadable(L"libmfx.dll");
    const bool amf     = dll_loadable(L"amfrt64.dll");
    for (auto& a : adapters_) {
        a.nvcuvid_dll_present = nvcuvid;
        a.onevpl_dll_present  = onevpl;
        a.amf_dll_present     = amf;
    }

    for (const auto& a : adapters_) {
        // 转 description 到 ASCII（MCP_LOGF 取 char*）。
        char desc[128]{};
        ::WideCharToMultiByte(CP_UTF8, 0, a.description.c_str(), -1,
                              desc, sizeof(desc) - 1, nullptr, nullptr);
        MCP_LOGF(LogLevel::info,
                 "DxgiCapsProbe: adapter='%s' vid=0x%04X sw=%d h264=%d hevc_main=%d hevc_main10=%d "
                 "av1=%d dual_bind=%d tearing=%d sdk{nv=%d vpl=%d amf=%d}",
                 desc, a.vendor_id, a.is_software, a.h264_supported, a.hevc_main_supported,
                 a.hevc_main10_supported, a.av1_supported,
                 a.dual_bind_supported, a.allow_tearing_supported,
                 a.nvcuvid_dll_present, a.onevpl_dll_present, a.amf_dll_present);
    }
    return MC_OK;
}

const AdapterCaps* DxgiCapsProbe::find_by_hwnd(HWND hwnd) const noexcept {
    if (!hwnd) return nullptr;
    HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!mon) return nullptr;
    for (const auto& a : adapters_) {
        for (HMONITOR m : a.monitors) {
            if (m == mon) return &a;
        }
    }
    return nullptr;
}

const AdapterCaps* DxgiCapsProbe::find_by_luid(LUID luid) const noexcept {
    for (const auto& a : adapters_) {
        if (a.luid.LowPart == luid.LowPart && a.luid.HighPart == luid.HighPart) {
            return &a;
        }
    }
    return nullptr;
}

}  // namespace mcp::pal
