#include "media/codec_mft_video.h"

#include <codecapi.h>
#include <d3d11_4.h>
#include <evr.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "pal/cache_line.h"
#include "pal/error.h"
#include "pal/log.h"
#include "pal/spsc_queue.h"
#include "pal/thread.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

using Microsoft::WRL::ComPtr;

namespace mcp::media {

namespace {

constexpr DWORD kFirstStreamId = 0;     // MFT 单 stream 解码器：input/output 都是 0

GUID codec_to_subtype(mc_video_codec_t c) noexcept {
    switch (c) {
        case MC_VIDEO_CODEC_H264: return MFVideoFormat_H264;
        case MC_VIDEO_CODEC_H265: return MFVideoFormat_HEVC;
        default:                  return GUID_NULL;
    }
}

const char* codec_name(mc_video_codec_t c) noexcept {
    switch (c) {
        case MC_VIDEO_CODEC_H264: return "H264";
        case MC_VIDEO_CODEC_H265: return "HEVC";
        default:                  return "UNKNOWN";
    }
}

struct PendingAu {
    std::vector<uint8_t> bytes;
    int64_t              pts_us         = 0;
    int64_t              arrival_qpc_ns = 0;     // 端到端延时探针起点（first-packet RX 戳）
    bool                 keyframe       = false;
};

// AU 队列容量（pal::SpscQueue 要求 power-of-two）。低延时模式下 host 端 AU 缓存只是
// 「让 codec 立刻拉到下一个」的接力槽。MFT 自身有内部 input buffering（async event
// 驱动），host 多缓 ≠ 多吞吐，反而把网络拥塞转成解码侧滞后。
// 4 = 一个 anchor + 2-3 P，对应 ~130 ms@30fps 上限（≈ P0-3 的 3 +1，satisfy 2^n）。
// SPSC 不变量约束 producer 不能动 tail，"丢最老"由 consumer 在 try_pop 前调
// try_drop_oldest 到只剩 1 个实现（"最新优先"等价于 producer 端"丢最老"）。
// producer 满时直接丢自己（drop newest，极少触发，4 远大于 MFT 内部 buffering 需求）。
constexpr std::size_t kAuQueueCap = 4;

// 自定义 IMFSample attribute — 透传 arrival_qpc_ns。MFT 文档未明示
// input→output 自定义 attribute 透传，实测 driver 行为不一；失败时回退
// PTS-keyed ring map（见 Impl::arrival_ring_）。
// {6E5C4A5B-2F7E-4A0B-9D3C-1A2B3C4D5E6F}
const GUID kArrivalQpcNsAttr =
    { 0x6E5C4A5B, 0x2F7E, 0x4A0B, { 0x9D, 0x3C, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F } };

}  // namespace

struct CodecMftVideo::Impl {
    Config                          cfg;
    ComPtr<IMFTransform>            transform;
    ComPtr<IMFMediaEventGenerator>  event_gen;
    ComPtr<IMFDXGIDeviceManager>    device_manager;
    UINT                            dxgi_reset_token = 0;

    ComPtr<ID3D11DeviceContext>     ctx;
    ComPtr<ID3D11Texture2D>         out_pool;          // 私有 NV12 texture array
    UINT                            out_pool_size    = 0;
    UINT                            out_pool_next    = 0;
    UINT                            out_width        = 0;
    UINT                            out_height       = 0;

    std::thread                     event_thread;
    std::atomic<bool>               stop_requested{false};
    std::atomic<bool>               started{false};

    // SPSC ring + cache-line padding（CLAUDE.md / ADD §6.1 硬约束）。
    // au_queue 是真正的 lock-free SPSC（producer = controller 线程，consumer = T4
    // event/sync worker），mutex+cv 仅用于 wake/sleep 同步。三个字段独占 cache line
    // 防 false-sharing。kAuQueueCap = 4。
    alignas(pal::kCacheLineSize) std::mutex                 au_mu;
    alignas(pal::kCacheLineSize) std::condition_variable    au_cv;
    alignas(pal::kCacheLineSize) pal::SpscQueue<PendingAu>  au_queue{kAuQueueCap};
    bool                            need_input_pending = false;

    // PTS-keyed arrival 戳兜底 ring（仅 T4 单线程访问，无锁）。MFT 不保证
    // 自定义 attribute 透传；GetUINT64 失败时按 SampleTime (us) 回查 ring。
    struct ArrivalEntry { int64_t pts_us; int64_t arrival_qpc_ns; };
    static constexpr std::size_t   kArrivalRingSize = 16;
    std::array<ArrivalEntry, kArrivalRingSize> arrival_ring_{};
    std::size_t                    arrival_ring_head_ = 0;

    void record_arrival(int64_t pts_us, int64_t arrival_qpc_ns) noexcept {
        if (arrival_qpc_ns == 0) return;
        arrival_ring_[arrival_ring_head_ % kArrivalRingSize] = {pts_us, arrival_qpc_ns};
        ++arrival_ring_head_;
    }
    int64_t lookup_arrival(int64_t pts_us) const noexcept {
        for (const auto& e : arrival_ring_) {
            if (e.pts_us == pts_us && e.arrival_qpc_ns != 0) return e.arrival_qpc_ns;
        }
        return 0;
    }

    bool                            sps_seen = false;
    bool                            is_sync_mft = false;             // Microsoft HEVC Extension 是 sync software MFT
    bool                            mft_provides_samples = true;     // 软解 MFT 通常需要 host 分配 output sample
    DWORD                           output_sample_size = 0;
    DWORD                           output_sample_align = 0;
    bool                            sync_streaming_started = false;

    // P0-1 真零拷贝（fast path）：MFT sample 直接喂 render，省去 SR-only 私有 pool 的
    // CopySubresourceRegion blit (~0.1-0.2 ms/帧)。runtime 决定是否启用：
    //   1) startup hint   — SetUINT32(MF_SA_D3D11_BINDFLAGS, BIND_DECODER|BIND_SHADER_RESOURCE)
    //                        告诉 MFT 内部 sample pool 创建带 SR bind 的 texture（driver 可忽略）
    //   2) runtime probe  — 第一次 ProcessOutput 出 sample 时检查 src_tex.BindFlags 是否带 SR；
    //                        + 烟测创建 R8 plane SRV（防 driver 接受 BindFlags 但 SRV 创建失败的陷阱）
    //   3) 失败回退       — fast_path_enabled = false，沿用 v1 路径（自管 SR-only pool + CopySub + mt 锁）
    // 决策只做一次（fast_path_decided），之后所有帧走同一分支。
    bool                            fast_path_decided = false;
    bool                            fast_path_enabled = false;

    // sync 路径用：RAM NV12 → dynamic texture
    ComPtr<ID3D11Texture2D>         upload_tex;
    UINT                            upload_w = 0;
    UINT                            upload_h = 0;
    bool ensure_upload_texture(UINT w, UINT h) noexcept {
        if (upload_tex && upload_w == w && upload_h == h) return true;
        upload_tex.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width            = w;
        desc.Height           = h;
        desc.MipLevels        = 1;
        desc.ArraySize        = 1;
        desc.Format           = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_DYNAMIC;
        desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(cfg.device->CreateTexture2D(&desc, nullptr, &upload_tex))) return false;
        upload_w = w;
        upload_h = h;
        return true;
    }

    // ─── helpers ───────────────────────────────────────────────────
    HRESULT enable_async() noexcept {
        ComPtr<IMFAttributes> attrs;
        HRESULT hr = transform->GetAttributes(&attrs);
        if (FAILED(hr)) return hr;
        UINT32 is_async = 0;
        if (FAILED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) || !is_async) {
            return MF_E_TRANSFORM_ASYNC_LOCKED;
        }
        return attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    }

    HRESULT set_input_type(mc_video_codec_t codec, UINT32 hint_w = 0, UINT32 hint_h = 0) noexcept {
        ComPtr<IMFMediaType> type;
        HRESULT hr = ::MFCreateMediaType(&type);
        if (FAILED(hr)) return hr;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, codec_to_subtype(codec));
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        // 默认假设 30 fps；MFT 不强制要求精确帧率，仅用于带宽估算。
        ::MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 30, 1);
        ::MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (hint_w && hint_h) {
            ::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, hint_w, hint_h);
        }
        return transform->SetInputType(kFirstStreamId, type.Get(), 0);
    }

    HRESULT set_output_type_nv12(UINT32 hint_w = 0, UINT32 hint_h = 0) noexcept {
        // 找 NV12 候选（如果 hint 给定，构造完整 type；否则让 driver 决定）。
        if (hint_w && hint_h) {
            // sync MFT 路径：driver 还没看 SPS，列表里 NV12 type frame_size=0。
            // 完全自构 NV12 type 反而更可控。
            ComPtr<IMFMediaType> type;
            if (FAILED(::MFCreateMediaType(&type))) return E_FAIL;
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            ::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, hint_w, hint_h);
            ::MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            ::MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 30, 1);
            type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE);
            type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
            type->SetUINT32(MF_MT_SAMPLE_SIZE, hint_w * hint_h * 3 / 2);
            type->SetUINT32(MF_MT_DEFAULT_STRIDE, hint_w);
            return transform->SetOutputType(kFirstStreamId, type.Get(), 0);
        }
        for (DWORD i = 0; ; ++i) {
            ComPtr<IMFMediaType> type;
            HRESULT hr = transform->GetOutputAvailableType(kFirstStreamId, i, &type);
            if (hr == MF_E_NO_MORE_TYPES) break;
            if (FAILED(hr)) return hr;
            GUID sub{};
            type->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12) {
                return transform->SetOutputType(kFirstStreamId, type.Get(), 0);
            }
        }
        return MF_E_NO_MORE_TYPES;
    }

    HRESULT update_output_dims() noexcept {
        ComPtr<IMFMediaType> type;
        HRESULT hr = transform->GetOutputCurrentType(kFirstStreamId, &type);
        if (FAILED(hr)) return hr;
        UINT32 w = 0, h = 0;
        ::MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        out_width  = w;
        out_height = h;
        return S_OK;
    }

    HRESULT ensure_out_pool() noexcept {
        if (out_pool && out_pool_size == cfg.surface_pool_max) return S_OK;
        if (!out_width || !out_height) return E_NOT_VALID_STATE;
        out_pool.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width            = out_width;
        desc.Height           = out_height;
        desc.MipLevels        = 1;
        desc.ArraySize        = cfg.surface_pool_max ? cfg.surface_pool_max : 4;
        desc.Format           = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_DEFAULT;
        desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags   = 0;
        HRESULT hr = cfg.device->CreateTexture2D(&desc, nullptr, &out_pool);
        if (FAILED(hr)) return hr;
        out_pool_size = desc.ArraySize;
        out_pool_next = 0;
        return S_OK;
    }

    // P1-6: MFT input IMFMediaBuffer pool —— 每次 NeedInput / submit_sync 不再
    // ::MFCreateMemoryBuffer 新分配 + 销毁，从双桶 pool 取空闲 buffer 复用。
    // 桶设计：小 ≤ 64KB（P/B AU），大 ≤ 2MB（IDR/IRAP），> 2MB 直接新建不缓存。
    // idle 探测：buf->AddRef() → buf->Release() 返回值 == 1 即 pool 单持引用 → 可复用。
    // COM Release 在并发下保守可靠：MFT 持 N>0 ref 时返回 N+1>1，绝不误判 busy 为
    // idle（误判反向无害——保守判 busy 触发新建，多 1 次 alloc 而已）。
    // 单线程访问：async 仅 event_loop / sync 仅 sync_worker_loop（is_sync_mft 互斥），
    // pool 无锁可靠；stop() 在 transform.Reset() 之后清空 pool（MFT 已释放飞行 sample）。
    // 字段名避开 `small` —— Win32 <rpcndr.h> 有 `#define small char` 污染。
    struct InputBufferPool {
        static constexpr DWORD       kSmallCap = 64u * 1024u;
        static constexpr DWORD       kBigCap   = 2u  * 1024u * 1024u;
        static constexpr std::size_t kSmallMax = 4;
        static constexpr std::size_t kBigMax   = 4;
        std::vector<ComPtr<IMFMediaBuffer>> bucket_small;
        std::vector<ComPtr<IMFMediaBuffer>> bucket_big;
        uint64_t hit_small = 0, miss_small = 0;
        uint64_t hit_big   = 0, miss_big   = 0;
        uint64_t alloc_oversize = 0;

        static bool is_idle(IMFMediaBuffer* b) noexcept {
            b->AddRef();
            return b->Release() == 1;
        }

        HRESULT acquire(DWORD req, ComPtr<IMFMediaBuffer>* out) noexcept {
            if (req <= kSmallCap)
                return acquire_bucket(bucket_small, kSmallCap, kSmallMax,
                                       hit_small, miss_small, out);
            if (req <= kBigCap)
                return acquire_bucket(bucket_big,   kBigCap,   kBigMax,
                                       hit_big,   miss_big,   out);
            ++alloc_oversize;
            return ::MFCreateMemoryBuffer(req, out->ReleaseAndGetAddressOf());
        }

        HRESULT acquire_bucket(std::vector<ComPtr<IMFMediaBuffer>>& bucket,
                                DWORD cap, std::size_t max,
                                uint64_t& hit, uint64_t& miss,
                                ComPtr<IMFMediaBuffer>* out) noexcept {
            for (auto& b : bucket) {
                if (is_idle(b.Get())) {
                    b->SetCurrentLength(0);
                    *out = b;
                    ++hit;
                    return S_OK;
                }
            }
            ComPtr<IMFMediaBuffer> nb;
            HRESULT hr = ::MFCreateMemoryBuffer(cap, &nb);
            if (FAILED(hr)) return hr;
            if (bucket.size() < max) bucket.push_back(nb);
            *out = nb;
            ++miss;
            return S_OK;
        }

        void clear() noexcept {
            bucket_small.clear();
            bucket_big.clear();
        }
    };
    InputBufferPool input_buf_pool;

    void event_loop() noexcept;
    void sync_worker_loop() noexcept;
    HRESULT on_need_input() noexcept;
    HRESULT on_have_output() noexcept;
    HRESULT process_have_output(IMFSample* host_sample) noexcept;
    HRESULT submit_sync(const PendingAu& au) noexcept;
    HRESULT drain_sync_output() noexcept;
    HRESULT emit_from_sample(ComPtr<IMFSample> sample) noexcept;
};

void CodecMftVideo::Impl::event_loop() noexcept {
    // T4 Video Decode (ADD §3.3)：MMCSS Playback (~优先级 23)。host 线程仅驱动事件循环；
    // MFT 内部 worker queue 由驱动管理，不在此处注册。
    pal::ThreadRegistration reg;
    pal::ThreadOptions opt;
    opt.name        = "mc-player T4 MFT-Decode";
    opt.mmcss_task  = pal::MmcssTask::playback;
    reg.apply(opt);

    while (!stop_requested.load(std::memory_order_acquire)) {
        ComPtr<IMFMediaEvent> ev;
        HRESULT hr = event_gen->GetEvent(0, &ev);
        if (FAILED(hr)) {
            if (hr != MF_E_SHUTDOWN) {
                MCP_LOGF(pal::LogLevel::warn, "CodecMftVideo: GetEvent hr=0x%08lX", hr);
            }
            break;
        }
        MediaEventType met = MEUnknown;
        ev->GetType(&met);
        if (met == METransformNeedInput) {
            on_need_input();
        } else if (met == METransformHaveOutput) {
            on_have_output();
        } else if (met == METransformDrainComplete) {
            // 仅日志；上层 stop 时主动等线程退出。
        } else if (met == MEError) {
            HRESULT err_hr = S_OK;
            ev->GetStatus(&err_hr);
            MCP_LOGF(pal::LogLevel::error, "CodecMftVideo: MEError hr=0x%08lX", err_hr);
        }
    }
}

void CodecMftVideo::Impl::sync_worker_loop() noexcept {
    // sync MFT (Microsoft HEVC Video Extension 等 software MFT) 没有事件机制；
    // 这里阻塞等 au_queue，每次 pop 一个 AU 同步跑 submit_sync (ProcessInput + drain)。
    // 收益：把原本在 RX (T2 / TIME_CRITICAL) 上同步的软解搬到独立 MMCSS Playback 线程。
    pal::ThreadRegistration reg;
    pal::ThreadOptions opt;
    opt.name        = "mc-player T4 MFT-Sync-Decode";
    opt.mmcss_task  = pal::MmcssTask::playback;
    reg.apply(opt);

    while (!stop_requested.load(std::memory_order_acquire)) {
        PendingAu au;
        bool got = false;
        {
            std::unique_lock lk{au_mu};
            au_cv.wait(lk, [&]{
                return stop_requested.load(std::memory_order_acquire) ||
                        au_queue.approx_size() > 0;
            });
            if (stop_requested.load(std::memory_order_acquire)) break;
            // SPSC 不变量：consumer 端的 try_drop_oldest / try_pop 必须串行；与
            // flush()（也是 consumer 角色）持同一把 mu 互斥。producer try_push 仍 lock-free。
            // "最新优先"：drop 到只剩 1 再 pop（兑现 CLAUDE.md "满时丢最老" 语义）。
            while (au_queue.approx_size() > 1) (void)au_queue.try_drop_oldest();
            got = au_queue.try_pop(au);
        }
        if (!got) continue;     // race / spurious wake，回 wait
        HRESULT hr = submit_sync(au);
        if (FAILED(hr)) {
            MCP_LOGF(pal::LogLevel::warn,
                     "CodecMftVideo: sync submit hr=0x%08lX", hr);
        }
    }
}

HRESULT CodecMftVideo::Impl::on_need_input() noexcept {
    PendingAu au;
    bool got = false;
    {
        std::unique_lock lk{au_mu};
        au_cv.wait(lk, [&]{
            return stop_requested.load(std::memory_order_acquire) ||
                    au_queue.approx_size() > 0;
        });
        if (stop_requested.load(std::memory_order_acquire)) return S_OK;
        // SPSC consumer 操作必须持 mu（与 flush() 互斥）；producer try_push 仍 lock-free。
        // "最新优先"：drop 到只剩 1 再 pop（兑现 CLAUDE.md "满时丢最老" 语义）。
        while (au_queue.approx_size() > 1) (void)au_queue.try_drop_oldest();
        got = au_queue.try_pop(au);
    }
    if (!got) return S_OK;     // race / spurious wake，回事件循环再等

    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = input_buf_pool.acquire(static_cast<DWORD>(au.bytes.size()), &buf);
    if (FAILED(hr)) return hr;

    BYTE* dst = nullptr;
    DWORD max_len = 0;
    hr = buf->Lock(&dst, &max_len, nullptr);
    if (FAILED(hr)) return hr;
    std::memcpy(dst, au.bytes.data(), au.bytes.size());
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(au.bytes.size()));

    ComPtr<IMFSample> sample;
    hr = ::MFCreateSample(&sample);
    if (FAILED(hr)) return hr;
    sample->AddBuffer(buf.Get());
    // PTS in 100ns units (MF time)
    sample->SetSampleTime(au.pts_us * 10);
    if (au.keyframe) {
        sample->SetUINT32(MFSampleExtension_CleanPoint, 1);
    }
    // 端到端延时探针：双路径透传（attribute + PTS ring 兜底）。
    record_arrival(au.pts_us, au.arrival_qpc_ns);
    if (au.arrival_qpc_ns != 0) {
        sample->SetUINT64(kArrivalQpcNsAttr, static_cast<UINT64>(au.arrival_qpc_ns));
    }
    return transform->ProcessInput(kFirstStreamId, sample.Get(), 0);
}

HRESULT CodecMftVideo::Impl::on_have_output() noexcept {
    MFT_OUTPUT_STREAM_INFO info{};
    HRESULT hr = transform->GetOutputStreamInfo(kFirstStreamId, &info);
    if (FAILED(hr)) return hr;

    // hardware MFT 自管 sample。
    MFT_OUTPUT_DATA_BUFFER out{};
    out.dwStreamID = kFirstStreamId;
    DWORD status   = 0;
    hr = transform->ProcessOutput(0, 1, &out, &status);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        update_output_dims();
        // STREAM_CHANGE 时 fast_path 还未决定（需要看真 sample 的 BindFlags），保留
        // ensure_out_pool 让 slow path 可立即工作；fast path 启用后该 pool 不被使用。
        ensure_out_pool();
        return S_OK;
    }
    if (FAILED(hr)) return hr;
    if (!out.pSample) return S_OK;

    ComPtr<IMFSample> sample;
    sample.Attach(out.pSample);
    if (out.pEvents) out.pEvents->Release();

    if (out_width == 0 || out_height == 0) {
        update_output_dims();
    }

    // 取 D3D 纹理 + slice。
    DWORD nbufs = 0;
    sample->GetBufferCount(&nbufs);
    if (nbufs == 0) return S_OK;

    ComPtr<IMFMediaBuffer> mbuf;
    sample->GetBufferByIndex(0, &mbuf);
    ComPtr<IMFDXGIBuffer> dbuf;
    if (FAILED(mbuf.As(&dbuf))) return S_OK;

    ComPtr<ID3D11Texture2D> src_tex;
    if (FAILED(dbuf->GetResource(IID_PPV_ARGS(&src_tex)))) return S_OK;
    UINT src_slice = 0;
    dbuf->GetSubresourceIndex(&src_slice);

    // P0-1 fast-path 决策（一次性，第一帧）：检查 MFT 自分配 texture 是否带 SR bind +
    // 烟测 R8 plane SRV 创建（防 driver 接受 BindFlags hint 但实际 SRV 创建失败的陷阱）。
    if (!fast_path_decided) {
        fast_path_decided = true;
        D3D11_TEXTURE2D_DESC td{};
        src_tex->GetDesc(&td);
        const bool has_sr_bind = (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
        bool srv_ok = false;
        if (has_sr_bind) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R8_UNORM;
            if (td.ArraySize > 1) {
                sd.ViewDimension                  = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                sd.Texture2DArray.MipLevels       = 1;
                sd.Texture2DArray.FirstArraySlice = src_slice;
                sd.Texture2DArray.ArraySize       = 1;
            } else {
                sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
            }
            ComPtr<ID3D11ShaderResourceView> probe_srv;
            const HRESULT probe_hr =
                cfg.device->CreateShaderResourceView(src_tex.Get(), &sd, &probe_srv);
            srv_ok = SUCCEEDED(probe_hr) && probe_srv;
            if (!srv_ok) {
                MCP_LOGF(pal::LogLevel::warn,
                         "CodecMftVideo: fast-path SRV smoke fail bind=0x%X arr=%u hr=0x%08lX",
                         td.BindFlags, td.ArraySize, probe_hr);
            }
        }
        fast_path_enabled = has_sr_bind && srv_ok;
        MCP_LOGF(pal::LogLevel::info,
                 "CodecMftVideo: P0-1 fast_path=%d (BindFlags=0x%X has_sr=%d srv_ok=%d arr=%u %ux%u)",
                 fast_path_enabled, td.BindFlags, has_sr_bind, srv_ok,
                 td.ArraySize, td.Width, td.Height);
    }

    LONGLONG t100 = 0;
    sample->GetSampleTime(&t100);

    // MFSampleExtension_CleanPoint：MFT decoder 在输出 IDR/IRAP 时置 1。
    // 这是 Frame Validity Gate 污染传播退出（anchor 解 freeze）的唯一可靠信号。
    UINT32 clean_point = 0;
    (void)sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point);

    VideoFrame f;
    f.pts_us              = t100 / 10;
    f.width               = out_width;
    f.height              = out_height;
    f.source              = FrameSource::mft_dxva;
    f.is_keyframe         = (clean_point != 0);
    f.color_primaries     = cfg.color.primaries;
    f.color_range         = cfg.color.range;
    f.color_matrix        = cfg.color.matrix;
    f.validity_mask       = kValidityAll;

    if (fast_path_enabled) {
        // 真零拷贝：直接透出 MFT 自分配 sample 的 texture + slice。frame 持 IMFSample
        // 引用阻止 MFT internal pool recycle 该 slice；frame 析构（render 用完）后
        // ComPtr 减引用 → MFT 可 recycle。MFT 已保证 ProcessOutput 出来的 sample
        // 是 GPU-ready，host 端无需额外 fence wait（同 immediate context 串行）。
        f.dxva_texture     = src_tex;
        f.dxva_array_slice = src_slice;
        sample.As(&f.mft_sample);
    } else {
        // slow path（v1 路径）：CopySub 到自管 SR-only pool，避免 MFT 直接 SR 采样不可用。
        if (!out_pool) {
            if (FAILED(ensure_out_pool())) return E_FAIL;
        }
        const UINT dst_slice = out_pool_next;
        out_pool_next        = (out_pool_next + 1) % out_pool_size;
        {
            // 多线程保护：MFT 在 worker 线程做内部 GPU 提交，与 ctx 共享 device。
            ComPtr<ID3D10Multithread> mt;
            if (SUCCEEDED(cfg.device.As(&mt))) mt->Enter();
            ctx->CopySubresourceRegion(out_pool.Get(), dst_slice, 0, 0, 0,
                                        src_tex.Get(), src_slice, nullptr);
            if (mt) mt->Leave();
        }
        f.dxva_texture     = out_pool;
        f.dxva_array_slice = dst_slice;
    }

    // 端到端延时探针：先尝试 sample attribute（快路径），失败回退 PTS ring 查询。
    {
        UINT64 arr = 0;
        if (SUCCEEDED(sample->GetUINT64(kArrivalQpcNsAttr, &arr)) && arr != 0) {
            f.arrival_qpc_ns = static_cast<int64_t>(arr);
        } else {
            f.arrival_qpc_ns = lookup_arrival(f.pts_us);
        }
    }

    if (cfg.emit) cfg.emit(std::move(f));
    return S_OK;
}

// sync 路径 — Microsoft HEVC Video Extension 等 software MFT。
HRESULT CodecMftVideo::Impl::submit_sync(const PendingAu& au) noexcept {
    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = input_buf_pool.acquire(static_cast<DWORD>(au.bytes.size()), &buf);
    if (FAILED(hr)) return hr;
    BYTE* dst = nullptr;
    DWORD max_len = 0;
    hr = buf->Lock(&dst, &max_len, nullptr);
    if (FAILED(hr)) return hr;
    std::memcpy(dst, au.bytes.data(), au.bytes.size());
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(au.bytes.size()));

    ComPtr<IMFSample> sample;
    hr = ::MFCreateSample(&sample);
    if (FAILED(hr)) return hr;
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(au.pts_us * 10);
    if (au.keyframe) sample->SetUINT32(MFSampleExtension_CleanPoint, 1);
    // 端到端延时探针：双路径透传（attribute + PTS ring 兜底）。
    record_arrival(au.pts_us, au.arrival_qpc_ns);
    if (au.arrival_qpc_ns != 0) {
        sample->SetUINT64(kArrivalQpcNsAttr, static_cast<UINT64>(au.arrival_qpc_ns));
    }

    hr = transform->ProcessInput(kFirstStreamId, sample.Get(), 0);
    static int submit_n = 0;
    ++submit_n;
    if (submit_n <= 3) {
        MCP_LOGF(pal::LogLevel::info,
                 "CodecMftVideo: sync ProcessInput[#%d] hr=0x%08lX bytes=%zu pts=%lld",
                 submit_n, hr, au.bytes.size(), au.pts_us);
    }
    if (FAILED(hr)) return hr;

    return drain_sync_output();
}

HRESULT CodecMftVideo::Impl::drain_sync_output() noexcept {
    int loops = 0;
    while (true) {
        ++loops;
        ComPtr<IMFSample> host_sample;
        if (!mft_provides_samples) {
            // host 分配 NV12 RAM sample
            HRESULT hr = ::MFCreateSample(&host_sample);
            if (FAILED(hr)) return hr;
            const DWORD sz = output_sample_size ? output_sample_size : (out_width * out_height * 3 / 2);
            ComPtr<IMFMediaBuffer> obuf;
            hr = ::MFCreateAlignedMemoryBuffer(sz,
                output_sample_align ? output_sample_align - 1 : 0, &obuf);
            if (FAILED(hr)) return hr;
            host_sample->AddBuffer(obuf.Get());
        }

        MFT_OUTPUT_DATA_BUFFER out{};
        out.dwStreamID = kFirstStreamId;
        out.pSample    = host_sample.Get();
        DWORD status   = 0;
        HRESULT hr = transform->ProcessOutput(0, 1, &out, &status);
        if (loops <= 3) {
            MCP_LOGF(pal::LogLevel::info,
                     "CodecMftVideo: sync ProcessOutput[loop=%d] hr=0x%08lX status=0x%lX provides=%d",
                     loops, hr, status, mft_provides_samples);
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // 选 NV12 作为 output（必须在 update_output_dims 之前；driver 设新 type 后才有 frame size）
            for (DWORD i = 0; ; ++i) {
                ComPtr<IMFMediaType> type;
                HRESULT h2 = transform->GetOutputAvailableType(kFirstStreamId, i, &type);
                if (h2 == MF_E_NO_MORE_TYPES) break;
                if (FAILED(h2)) break;
                GUID sub{};
                type->GetGUID(MF_MT_SUBTYPE, &sub);
                if (sub == MFVideoFormat_NV12) {
                    transform->SetOutputType(kFirstStreamId, type.Get(), 0);
                    break;
                }
            }
            update_output_dims();
            // 刷新 stream info — SetOutputType 后才有 cbSize
            MFT_OUTPUT_STREAM_INFO osi{};
            if (SUCCEEDED(transform->GetOutputStreamInfo(kFirstStreamId, &osi))) {
                mft_provides_samples =
                    (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                     MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
                output_sample_size  = osi.cbSize;
                output_sample_align = osi.cbAlignment;
            }
            // 推迟启动的 BEGIN_STREAMING + START_OF_STREAM
            if (!sync_streaming_started) {
                transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
                transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
                sync_streaming_started = true;
                MCP_LOGF(pal::LogLevel::info,
                         "CodecMftVideo: sync STREAM_CHANGE → %ux%u provides=%d osize=%lu, BEGIN_STREAMING",
                         out_width, out_height, mft_provides_samples, output_sample_size);
            }
            continue;
        }
        if (FAILED(hr)) return hr;
        if (!out.pSample) return S_OK;
        ComPtr<IMFSample> result;
        if (mft_provides_samples) {
            // driver 自分配，需要 release ref
            result.Attach(out.pSample);
        } else {
            // host 已通过 host_sample 持有 ref，仅别名引用，不抢 ownership
            result = host_sample;
        }
        if (out.pEvents) out.pEvents->Release();

        // RAM buffer → NV12 texture upload + emit
        ComPtr<IMFMediaBuffer> mbuf;
        HRESULT hcc = result->ConvertToContiguousBuffer(&mbuf);
        static int emit_n = 0;
        ++emit_n;
        if (emit_n <= 3) {
            MCP_LOGF(pal::LogLevel::info,
                     "CodecMftVideo: sync emit#%d ConvertToContiguousBuffer hr=0x%08lX",
                     emit_n, hcc);
        }
        if (FAILED(hcc)) continue;
        BYTE* base = nullptr;
        DWORD cur_len = 0;
        HRESULT hlk = mbuf->Lock(&base, nullptr, &cur_len);
        if (emit_n <= 3) {
            MCP_LOGF(pal::LogLevel::info,
                     "CodecMftVideo: sync emit#%d Lock hr=0x%08lX cur_len=%lu w=%u h=%u",
                     emit_n, hlk, cur_len, out_width, out_height);
        }
        if (FAILED(hlk)) continue;

        if (out_width == 0 || out_height == 0) update_output_dims();
        if (out_width == 0 || out_height == 0) { mbuf->Unlock(); continue; }
        if (!ensure_upload_texture(out_width, out_height)) { mbuf->Unlock(); continue; }

        // NV12 单 subresource Map：driver 给 Y plane 起始；UV plane 在 base+RowPitch*Height。
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hmap = ctx->Map(upload_tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        if (emit_n <= 3) {
            MCP_LOGF(pal::LogLevel::info,
                     "CodecMftVideo: sync emit#%d Map hr=0x%08lX rowpitch=%u",
                     emit_n, hmap, static_cast<unsigned>(m.RowPitch));
        }
        if (FAILED(hmap)) { mbuf->Unlock(); continue; }
        // 拷 Y plane（RAM 是 packed 即 stride=width，texture map 的 RowPitch 通常 >= width）
        auto* dst = static_cast<uint8_t*>(m.pData);
        for (UINT row = 0; row < out_height; ++row) {
            std::memcpy(dst + row * m.RowPitch, base + row * out_width, out_width);
        }
        // 拷 UV plane（NV12 UV plane 在 RAM 紧跟 Y 之后；texture 中也在 m.pData + RowPitch * height）
        const uint8_t* src_uv = base + out_width * out_height;
        uint8_t* dst_uv = dst + m.RowPitch * out_height;
        const UINT uv_rows = out_height / 2;
        for (UINT row = 0; row < uv_rows; ++row) {
            std::memcpy(dst_uv + row * m.RowPitch, src_uv + row * out_width, out_width);
        }
        ctx->Unmap(upload_tex.Get(), 0);

        mbuf->Unlock();

        LONGLONG t100 = 0;
        result->GetSampleTime(&t100);

        UINT32 clean_point_sync = 0;
        (void)result->GetUINT32(MFSampleExtension_CleanPoint, &clean_point_sync);

        VideoFrame f;
        f.pts_us              = t100 / 10;
        f.width               = out_width;
        f.height              = out_height;
        f.source              = FrameSource::libcodec_software;
        f.dxva_texture        = upload_tex;
        f.dxva_array_slice    = 0;
        f.is_keyframe         = (clean_point_sync != 0);
        f.color_primaries     = cfg.color.primaries;
        f.color_range         = cfg.color.range;
        f.color_matrix        = cfg.color.matrix;
        f.validity_mask       = kValidityAll;
        // 端到端延时探针：sample attribute → PTS ring 兜底。
        {
            UINT64 arr = 0;
            if (SUCCEEDED(result->GetUINT64(kArrivalQpcNsAttr, &arr)) && arr != 0) {
                f.arrival_qpc_ns = static_cast<int64_t>(arr);
            } else {
                f.arrival_qpc_ns = lookup_arrival(f.pts_us);
            }
        }
        if (cfg.emit) cfg.emit(std::move(f));
    }
}

CodecMftVideo::CodecMftVideo(Config cfg) : impl_{std::make_unique<Impl>()} {
    impl_->cfg = std::move(cfg);
}

CodecMftVideo::~CodecMftVideo() { stop(); }

mc_status_t CodecMftVideo::start(std::span<const uint8_t> /*extradata*/) noexcept {
    if (!impl_->cfg.device) return MC_ERR_INVALID_ARG;
    if (impl_->started.load(std::memory_order_acquire)) return MC_OK;

    impl_->cfg.device->GetImmediateContext(&impl_->ctx);

    // 多线程保护：MFT 内部线程会触碰 device。
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(impl_->cfg.device.As(&mt))) {
        mt->SetMultithreadProtected(TRUE);
    }

    // 1. MFTEnumEx — 优先硬件 async sorted。
    MFT_REGISTER_TYPE_INFO in_info{};
    in_info.guidMajorType = MFMediaType_Video;
    in_info.guidSubtype   = codec_to_subtype(impl_->cfg.codec);
    if (in_info.guidSubtype == GUID_NULL) return MC_ERR_UNSUPPORTED;

    // 诊断：先 enum 所有候选 MFT（不限 hw/sw/async/sync），看 OS 实际注册了什么。
    {
        IMFActivate** all = nullptr;
        UINT32 n_all = 0;
        ::MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_ALL,
                    &in_info, nullptr, &all, &n_all);
        MCP_LOGF(pal::LogLevel::info,
                 "CodecMftVideo: MFTEnumEx(ALL) for %s found %u",
                 codec_name(impl_->cfg.codec), n_all);
        for (UINT32 i = 0; i < n_all; ++i) {
            WCHAR namew[256] = {};
            UINT32 cch = 0;
            all[i]->GetString(MFT_FRIENDLY_NAME_Attribute, namew, _countof(namew), &cch);
            char namea[256] = {};
            ::WideCharToMultiByte(CP_UTF8, 0, namew, -1, namea, sizeof(namea) - 1,
                                   nullptr, nullptr);
            UINT32 is_hw = 0, is_async = 0;
            all[i]->GetUINT32(MFT_ENUM_HARDWARE_URL_Attribute, &is_hw);  // presence implies HW
            all[i]->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
            MCP_LOGF(pal::LogLevel::info,
                     "  [%u] '%s' hw_url=%u async=%u",
                     i, namea, is_hw, is_async);
            all[i]->Release();
        }
        if (all) ::CoTaskMemFree(all);
    }

    // ADR-015 / ADR-002 + plan Phase 1: 本档（ADR-015 档 3 MFT hardware async）仅接受
     // hw_url=1 && async=1 的 MFT。sync software MFT（如 Microsoft H264/H265 Video Decoder MFT，
     // hw_url=0 && async=0）在 SPS reorder>0 流上强制缓 ≥3 帧（实测 +100ms vs VLC，本仓库
     // commit 历史已记录），不当作硬解；codec_mft_video::start 在仅 sync software 可用时
     // 直接返回 MC_ERR_NO_HARDWARE，由 controller 路由到 ADR-015 档 4 mc-libcodec 软解。
     // 退路 1（software async）+ 退路 2（sync software）已在 Phase 1 删除（ADR-015 + ADR-002 范围澄清）。
    UINT32      flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT |
                        MFT_ENUM_FLAG_SORTANDFILTER;
    IMFActivate** activates = nullptr;
    UINT32       n_activates = 0;
    HRESULT      hr = ::MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags,
                                   &in_info, nullptr, &activates, &n_activates);
    if (FAILED(hr) || n_activates == 0) {
        if (activates) ::CoTaskMemFree(activates);
        MCP_LOGF(pal::LogLevel::warn,
                 "CodecMftVideo: no hardware async MFT for %s "
                 "(sync software MFT excluded by ADR-015); will fall through to libcodec",
                 codec_name(impl_->cfg.codec));
        return MC_ERR_NO_HARDWARE;
    }

    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&impl_->transform));
    for (UINT32 i = 0; i < n_activates; ++i) activates[i]->Release();
    ::CoTaskMemFree(activates);
    if (FAILED(hr)) {
        MCP_LOGF(pal::LogLevel::error,
                 "CodecMftVideo: ActivateObject hr=0x%08lX", hr);
        return pal::status_from_hresult(hr);
    }
    MCP_LOGF(pal::LogLevel::info,
             "CodecMftVideo: ActivateObject ok, sync=%d", impl_->is_sync_mft);

    // 2. async 路径需要 unlock async；sync MFT 跳过。
    if (!impl_->is_sync_mft) {
        if (HRESULT a = impl_->enable_async(); FAILED(a)) {
            MCP_LOGF(pal::LogLevel::error,
                     "CodecMftVideo: enable_async failed hr=0x%08lX", a);
            return pal::status_from_hresult(a);
        }
    }

    // 3. SET_D3D_MANAGER — 仅 async（GPU）路径需要；sync software MFT 强制走 RAM 路径。
    //    实测 MS HEVC Video Extension sync MFT 收到 D3D manager 后 BEGIN_STREAMING hang。
    if (!impl_->is_sync_mft) {
        hr = ::MFCreateDXGIDeviceManager(&impl_->dxgi_reset_token, &impl_->device_manager);
        if (FAILED(hr)) return pal::status_from_hresult(hr);
        hr = impl_->device_manager->ResetDevice(impl_->cfg.device.Get(), impl_->dxgi_reset_token);
        if (FAILED(hr)) return pal::status_from_hresult(hr);
        hr = impl_->transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(impl_->device_manager.Get()));
        if (FAILED(hr)) {
            MCP_LOGF(pal::LogLevel::warn,
                     "CodecMftVideo: SET_D3D_MANAGER failed hr=0x%08lX", hr);
            return pal::status_from_hresult(hr);
        }

        // P0-1 fast-path hint：要求 MFT 内部 sample pool 的 NV12 texture 带 SR bind，
        // 让 render 端直接 SRV 采样、不再 CopySub 到自管 pool。driver 可忽略此提示
        // （Intel 较新驱动响应良好；AMD/NV 行为不一），runtime probe 在 on_have_output
        // 第一次时按 src_tex.BindFlags 决定 fast vs slow path。设置失败不阻断启动。
        ComPtr<IMFAttributes> mft_attrs;
        if (SUCCEEDED(impl_->transform->GetAttributes(&mft_attrs)) && mft_attrs) {
            const UINT32 bind_hint = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
            HRESULT hh = mft_attrs->SetUINT32(MF_SA_D3D11_BINDFLAGS, bind_hint);
            MCP_LOGF(SUCCEEDED(hh) ? pal::LogLevel::info : pal::LogLevel::warn,
                     "CodecMftVideo: MF_SA_D3D11_BINDFLAGS hint=0x%X hr=0x%08lX",
                     bind_hint, hh);
        }
    }

    MCP_LOG_INFO("CodecMftVideo: D3D manager done, setting input type");
    // 4. set types。sync MFT 必须给 frame size 才能后续 SetOutputType。
    hr = impl_->set_input_type(impl_->cfg.codec,
                                impl_->is_sync_mft ? 1920u : 0u,
                                impl_->is_sync_mft ? 1088u : 0u);
    if (FAILED(hr)) {
        MCP_LOGF(pal::LogLevel::error,
                 "CodecMftVideo: SetInputType failed hr=0x%08lX", hr);
        return pal::status_from_hresult(hr);
    }
    MCP_LOG_INFO("CodecMftVideo: SetInputType ok");
    if (impl_->is_sync_mft) {
        // sync software MFT 没看过 SPS 时 NV12 type 的 MF_MT_FRAME_SIZE 是 0；必须 host 填一个 size 才接受。
        // 用 1920×1088 作为初值（最常见摄像机分辨率，CTB-aligned）；真实尺寸不一致时 driver 发 STREAM_CHANGE 重协商。
        hr = impl_->set_output_type_nv12(1920, 1088);
        if (FAILED(hr)) {
            MCP_LOGF(pal::LogLevel::warn,
                     "CodecMftVideo: sync SetOutputType(1920x1088) hr=0x%08lX, 退到延迟设",
                     hr);
        } else {
            impl_->update_output_dims();
            MCP_LOGF(pal::LogLevel::info,
                     "CodecMftVideo: sync SetOutputType ok, out=%ux%u",
                     impl_->out_width, impl_->out_height);
        }
    } else {
        hr = impl_->set_output_type_nv12();
        if (FAILED(hr)) {
            MCP_LOGF(pal::LogLevel::error,
                     "CodecMftVideo: SetOutputType(NV12) failed hr=0x%08lX", hr);
            return pal::status_from_hresult(hr);
        }
        impl_->update_output_dims();
    }

    // ADD §5.6.3 / ADR-014：CODECAPI 低延时配置 —— 一次拿 ICodecAPI 后做完所有 hint。
    //
    // (1) AVLowLatencyMode：禁 reorder buffer（默认开缓 1~3 帧 = 33~100ms）。
    //     类型陷阱：H.264 VT_UI4=1（MS Learn 明示），其它 VT_BOOL→VT_UI4 fallback。
    //     B-Frame Policy（ADD §5.6.4）：caller 检出 reorder>0 传 prefer_low_latency=false
    //     避开 LowLatency-vs-B 冲突花屏。运行期 PTS 反序检测兜底（Step 1c）。
    // (2) DropPicWithMissingRef = TRUE：ref 缺失立即丢，丢包场景额外 33~66ms 收益。
    // (3) NumWorkerThreads = 1：1080p@30fps driver 多 worker 反引入 sync overhead。
    // (4) AVDecVideoSWPowerLevel = 100：SW 路径 performance 优先（HW 路径忽略，无害）。
    //
    // 任一项失败仅记日志不阻断启动。
    {
        ComPtr<ICodecAPI> codec_api;
        if (SUCCEEDED(impl_->transform.As(&codec_api)) && codec_api) {
            // (1) AVLowLatencyMode
            if (impl_->cfg.prefer_low_latency) {
                const bool is_h264 = impl_->cfg.codec == MC_VIDEO_CODEC_H264;
                VARIANT v{};
                HRESULT hl = E_FAIL;
                if (is_h264) {
                    v.vt = VT_UI4;  v.ulVal = 1;
                    hl = codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
                } else {
                    v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
                    hl = codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
                    if (FAILED(hl)) {
                        v = {};
                        v.vt = VT_UI4; v.ulVal = 1;
                        hl = codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
                    }
                }
                MCP_LOGF(SUCCEEDED(hl) ? pal::LogLevel::info : pal::LogLevel::warn,
                         "CodecMftVideo: AVLowLatencyMode set hr=0x%08lX (codec=%s type=%s)",
                         hl, codec_name(impl_->cfg.codec), is_h264 ? "VT_UI4" : "VT_BOOL/UI4");
            } else {
                MCP_LOG_INFO("CodecMftVideo: LowLatencyMode disabled by caller (B-frame stream)");
            }

            // (2) DropPicWithMissingRef = TRUE
            {
                VARIANT vd{}; vd.vt = VT_BOOL; vd.boolVal = VARIANT_TRUE;
                HRESULT hd = codec_api->SetValue(&CODECAPI_AVDecVideoDropPicWithMissingRef, &vd);
                MCP_LOGF(SUCCEEDED(hd) ? pal::LogLevel::info : pal::LogLevel::warn,
                         "CodecMftVideo: DropPicWithMissingRef=TRUE hr=0x%08lX", hd);
            }

            // (3) NumWorkerThreads = 1
            {
                VARIANT vw{}; vw.vt = VT_UI4; vw.ulVal = 1;
                HRESULT hw = codec_api->SetValue(&CODECAPI_AVDecNumWorkerThreads, &vw);
                MCP_LOGF(SUCCEEDED(hw) ? pal::LogLevel::info : pal::LogLevel::warn,
                         "CodecMftVideo: NumWorkerThreads=1 hr=0x%08lX", hw);
            }

            // (4) AVDecVideoSWPowerLevel = 100（max performance）
            {
                VARIANT vp{}; vp.vt = VT_UI4; vp.ulVal = 100;
                HRESULT hp = codec_api->SetValue(&CODECAPI_AVDecVideoSWPowerLevel, &vp);
                MCP_LOGF(SUCCEEDED(hp) ? pal::LogLevel::info : pal::LogLevel::warn,
                         "CodecMftVideo: SWPowerLevel=100 hr=0x%08lX", hp);
            }
        } else {
            MCP_LOG_WARN("CodecMftVideo: ICodecAPI not exposed; low-latency hints skipped");
        }
    }

    // 5. event generator（仅 async）；sync 路径无事件队列。
    if (!impl_->is_sync_mft) {
        hr = impl_->transform.As(&impl_->event_gen);
        if (FAILED(hr)) return pal::status_from_hresult(hr);
    }

    // 6. 查询 output stream info — async MFT 此时 OutputType 已设可查；sync MFT 推迟到 STREAM_CHANGE 后。
    if (!impl_->is_sync_mft) {
        MFT_OUTPUT_STREAM_INFO osi{};
        if (SUCCEEDED(impl_->transform->GetOutputStreamInfo(kFirstStreamId, &osi))) {
            impl_->mft_provides_samples =
                (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                 MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            impl_->output_sample_size  = osi.cbSize;
            impl_->output_sample_align = osi.cbAlignment;
        }
    } else {
        // 默认 sync MFT 不自分配 sample；STREAM_CHANGE 事件处理里会刷新。
        impl_->mft_provides_samples = false;
    }

    // 7. begin streaming
    HRESULT hb1 = impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    HRESULT hb2 = impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    MCP_LOGF(pal::LogLevel::info,
             "CodecMftVideo: BEGIN_STREAMING hr=0x%08lX START_OF_STREAM hr=0x%08lX",
             hb1, hb2);
    impl_->sync_streaming_started = true;

    impl_->started.store(true, std::memory_order_release);
    // async MFT：driver 通过 IMFMediaEventGenerator 发 NeedInput/HaveOutput；event_loop 驱动。
    // sync MFT：无事件机制，sync_worker_loop 阻塞等 au_queue + 直接 ProcessInput。
    if (!impl_->is_sync_mft) {
        impl_->event_thread = std::thread([this]{ impl_->event_loop(); });
    } else {
        impl_->event_thread = std::thread([this]{ impl_->sync_worker_loop(); });
    }

    MCP_LOGF(pal::LogLevel::info,
             "CodecMftVideo: started codec=%s out=%ux%u sync=%d provides_sample=%d osize=%lu",
             codec_name(impl_->cfg.codec), impl_->out_width, impl_->out_height,
             impl_->is_sync_mft, impl_->mft_provides_samples,
             impl_->output_sample_size);
    return MC_OK;
}

void CodecMftVideo::submit(std::vector<uint8_t> au_bytes, int64_t pts_us,
                             int64_t arrival_qpc_ns) noexcept {
    if (!impl_->started.load(std::memory_order_acquire)) return;
    PendingAu p;
    p.bytes          = std::move(au_bytes);  // ownership 移交，无 memcpy
    p.pts_us         = pts_us;
    p.arrival_qpc_ns = arrival_qpc_ns;

    // SPSC try_push lock-free。满时 producer 不能动 tail（违反 SPSC 不变量），
    // 直接丢自己（drop-newest）；consumer 端在 try_pop 前会做 drop_to_last 维持
    // "最新优先"语义，所以即便偶尔丢新 AU，下个 AU 仍能被 consumer 拿到（旧的被
    // consumer 自己 drop）。容量 4 远大于 MFT 内部 buffering 需求，触发极少。
    if (!impl_->au_queue.try_push(std::move(p))) {
        static std::atomic<uint64_t> dropped{0};
        const auto n = dropped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || (n & (n - 1)) == 0) {  // 1, 2, 4, 8, 16, ...
            MCP_LOGF(pal::LogLevel::warn,
                     "CodecMftVideo: au_queue full (cap=%zu), drop newest AU (#%llu)",
                     kAuQueueCap, static_cast<unsigned long long>(n));
        }
        return;
    }
    // 标准 cv 模式：modify state 后短暂持锁作 happens-before barrier，再 notify。
    // 保证 consumer 要么在 wait predicate 检查时已看到 push，要么被 notify 唤醒。
    {
        std::scoped_lock lk{impl_->au_mu};
    }
    impl_->au_cv.notify_one();
}

void CodecMftVideo::flush() noexcept {
    if (!impl_->transform) return;
    impl_->transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    // SPSC 不变量：try_drop_oldest 是 consumer 操作，与 worker 端的 try_pop 串行。
    // 持 au_mu 排他即可（worker 也在该锁内做 SPSC pop）。
    std::scoped_lock lk{impl_->au_mu};
    while (impl_->au_queue.try_drop_oldest()) { /* drain */ }
}

void CodecMftVideo::stop() noexcept {
    if (!impl_->started.exchange(false, std::memory_order_acq_rel)) return;
    impl_->stop_requested.store(true, std::memory_order_release);
    impl_->au_cv.notify_all();

    if (impl_->is_sync_mft) {
        // sync 路径：先唤醒 worker 让它退出，再独占 transform 调 DRAIN + drain_sync_output。
        // worker 与 stop 并发访问 transform 会撕裂 ProcessOutput 状态机。
        if (impl_->event_thread.joinable()) impl_->event_thread.join();
        if (impl_->transform) {
            impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            impl_->transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            impl_->drain_sync_output();
        }
    } else {
        // async 路径：driver 收到 END_OF_STREAM + DRAIN 后会发 METransformDrainComplete
        // 事件，event_loop 内部继续抽完 NeedInput/HaveOutput 直到 driver 关闭队列。
        if (impl_->transform) {
            impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            impl_->transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        }
        if (impl_->event_thread.joinable()) impl_->event_thread.join();
    }

    if (impl_->transform) {
        impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        impl_->transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        impl_->transform.Reset();
    }
    impl_->event_gen.Reset();
    impl_->device_manager.Reset();
    impl_->out_pool.Reset();
    impl_->upload_tex.Reset();
    impl_->ctx.Reset();

    // P1-6: MFT 已 Reset → 所有飞行 sample 释放 → buffer ref 归 pool 自有 → 安全清空。
    {
        const auto& p = impl_->input_buf_pool;
        const uint64_t total_s = p.hit_small + p.miss_small;
        const uint64_t total_b = p.hit_big   + p.miss_big;
        MCP_LOGF(pal::LogLevel::info,
                 "CodecMftVideo: input_buf_pool small hit/miss=%llu/%llu (%.1f%%) "
                 "big hit/miss=%llu/%llu (%.1f%%) oversize=%llu",
                 static_cast<unsigned long long>(p.hit_small),
                 static_cast<unsigned long long>(p.miss_small),
                 total_s ? 100.0 * static_cast<double>(p.hit_small) /
                          static_cast<double>(total_s) : 0.0,
                 static_cast<unsigned long long>(p.hit_big),
                 static_cast<unsigned long long>(p.miss_big),
                 total_b ? 100.0 * static_cast<double>(p.hit_big) /
                          static_cast<double>(total_b) : 0.0,
                 static_cast<unsigned long long>(p.alloc_oversize));
    }
    impl_->input_buf_pool.clear();
}

}  // namespace mcp::media
