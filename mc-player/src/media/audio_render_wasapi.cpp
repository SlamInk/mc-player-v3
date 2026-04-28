/*
 * WASAPI 共享模式音频渲染（ADD §5.11）。
 *
 * v1 实装：
 *   - IMMDeviceEnumerator → eRender / eConsole 默认设备 → IAudioClient3
 *   - 取 mix format（GetMixFormat）；目标格式与 mix format 不一致时由 WASAPI AutoConvert + SRC 完成
 *   - 事件驱动；buffer 周期 = GetSharedModeEnginePeriod 的 default
 *   - 后台线程 wait event → 取 padding → fill buffer
 *   - submit(AudioFrame) 把 PCM float interleaved 写入 ring buffer
 *   - master_clock_us 用 IAudioClock::GetPosition 折算
 *
 * 设备切换、IAudioClient3 低延时模式（GetSharedModeEnginePeriod 的 driver-min）留 v2。
 */

#include "media/audio_render_wasapi.h"

#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "pal/log.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "avrt.lib")

extern "C" {
HANDLE WINAPI AvSetMmThreadCharacteristicsW(LPCWSTR TaskName, LPDWORD TaskIndex);
BOOL   WINAPI AvRevertMmThreadCharacteristics(HANDLE AvrtHandle);
}

using Microsoft::WRL::ComPtr;

namespace mcp::media {

namespace {

constexpr REFERENCE_TIME kHns100NsPerSec = 10'000'000ll;

// CLSID/IID — 通过 mmdevapi.lib 解析；这里直接用宏 INITGUID 在第一个 cpp 里需小心，
// 简化做法：手动声明常量，避免污染其它 TU。
const CLSID kClsidMMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C,
    { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
const IID   kIidIMMDeviceEnumerator  = { 0xA95664D2, 0x9614, 0x4F35,
    { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
const IID   kIidIAudioClient         = { 0x1CB9AD4C, 0xDBFA, 0x4C32,
    { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
const IID   kIidIAudioRenderClient   = { 0xF294ACFC, 0x3146, 0x4483,
    { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
const IID   kIidIAudioClock          = { 0xCD63314F, 0x3FBA, 0x4A1B,
    { 0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7 } };

}  // namespace

struct AudioRenderWasapi::Impl {
    ComPtr<IMMDeviceEnumerator>  enumerator;
    ComPtr<IMMDevice>            device;
    ComPtr<IAudioClient>         client;
    ComPtr<IAudioRenderClient>   render_client;
    ComPtr<IAudioClock>          audio_clock;

    HANDLE                       evt          = nullptr;
    HANDLE                       evt_stop     = nullptr;
    UINT32                       buffer_frames = 0;
    UINT32                       device_sample_rate = 0;
    UINT32                       device_channels    = 0;
    GUID                         device_subformat   = {};
    UINT32                       device_bits_per_sample = 0;

    UINT32                       src_sample_rate = 0;
    UINT32                       src_channels    = 0;

    std::thread                  worker;
    std::atomic<bool>            running{false};

    // Ring：std::deque<float> + mutex；音频拷贝量小，临界区短，足够。
    std::mutex                   ring_mu;
    std::deque<float>            ring;          // 设备声道数 × interleaved
    static constexpr std::size_t kMaxRingSamples = 48000 * 2 * 2;  // ~2s @ 48k stereo

    UINT64                       clock_freq = 0;
    std::atomic<int64_t>         master_us{0};

    bool init_device() noexcept {
        if (FAILED(::CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                       kIidIMMDeviceEnumerator, &enumerator))) {
            MCP_LOG_ERROR("AudioRenderWasapi: CoCreateInstance MMDeviceEnumerator failed");
            return false;
        }
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
            MCP_LOG_ERROR("AudioRenderWasapi: GetDefaultAudioEndpoint failed");
            return false;
        }
        if (FAILED(device->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr, &client))) {
            MCP_LOG_ERROR("AudioRenderWasapi: device->Activate IAudioClient failed");
            return false;
        }
        return true;
    }

    bool init_format() noexcept {
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client->GetMixFormat(&mix)) || !mix) {
            MCP_LOG_ERROR("AudioRenderWasapi: GetMixFormat failed");
            return false;
        }
        device_sample_rate     = mix->nSamplesPerSec;
        device_channels        = mix->nChannels;
        device_bits_per_sample = mix->wBitsPerSample;
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
            device_subformat = ext->SubFormat;
        } else {
            device_subformat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
                                ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                                : KSDATAFORMAT_SUBTYPE_PCM;
        }

        REFERENCE_TIME def_period = 0, min_period = 0;
        client->GetDevicePeriod(&def_period, &min_period);

        // AutoConvert PCM + SRC：caller 不必匹配 mix format。
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                          | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                          | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, def_period, 0, mix, nullptr);
        ::CoTaskMemFree(mix);
        if (FAILED(hr)) {
            MCP_LOGF(pal::LogLevel::error,
                     "AudioRenderWasapi: IAudioClient::Initialize hr=0x%08lX", hr);
            return false;
        }

        if (FAILED(client->GetBufferSize(&buffer_frames))) {
            MCP_LOG_ERROR("AudioRenderWasapi: GetBufferSize failed");
            return false;
        }
        evt = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        evt_stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!evt || !evt_stop) return false;
        if (FAILED(client->SetEventHandle(evt))) {
            MCP_LOG_ERROR("AudioRenderWasapi: SetEventHandle failed");
            return false;
        }
        if (FAILED(client->GetService(kIidIAudioRenderClient, &render_client))) {
            MCP_LOG_ERROR("AudioRenderWasapi: GetService IAudioRenderClient failed");
            return false;
        }
        if (FAILED(client->GetService(kIidIAudioClock, &audio_clock))) {
            MCP_LOG_WARN("AudioRenderWasapi: GetService IAudioClock failed; master clock disabled");
        } else {
            audio_clock->GetFrequency(&clock_freq);
        }

        MCP_LOGF(pal::LogLevel::info,
                 "AudioRenderWasapi: device %u Hz / %u ch / %u bps; buffer=%u frames",
                 device_sample_rate, device_channels, device_bits_per_sample, buffer_frames);
        return true;
    }

    bool prefill_silence() noexcept {
        BYTE* data = nullptr;
        if (SUCCEEDED(render_client->GetBuffer(buffer_frames, &data))) {
            const UINT32 bytes = buffer_frames * device_channels * (device_bits_per_sample / 8);
            std::memset(data, 0, bytes);
            render_client->ReleaseBuffer(buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
            return true;
        }
        return false;
    }

    // 把 ring 中的 device-format 样本写到 WASAPI buffer。
    // ring 已经按 device 格式 interleaved（resample/重映射在 submit 阶段做）。
    // 输出 bytes 由 device_subformat + bits 决定：float32 直接 memcpy；int16 转换。
    void fill_buffer(BYTE* dst, UINT32 frames) noexcept {
        const std::size_t need_samples = static_cast<std::size_t>(frames) * device_channels;
        std::vector<float> tmp;
        tmp.reserve(need_samples);
        {
            std::lock_guard<std::mutex> lk{ring_mu};
            const std::size_t take = std::min(need_samples, ring.size());
            for (std::size_t i = 0; i < take; ++i) tmp.push_back(ring.front()), ring.pop_front();
        }
        if (tmp.size() < need_samples) tmp.resize(need_samples, 0.0f);

        if (device_subformat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && device_bits_per_sample == 32) {
            std::memcpy(dst, tmp.data(), need_samples * sizeof(float));
        } else if (device_subformat == KSDATAFORMAT_SUBTYPE_PCM && device_bits_per_sample == 16) {
            auto* d16 = reinterpret_cast<int16_t*>(dst);
            for (std::size_t i = 0; i < need_samples; ++i) {
                float s = tmp[i];
                if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
                d16[i] = static_cast<int16_t>(s * 32767.0f);
            }
        } else {
            // 未知格式 → 静音兜底
            std::memset(dst, 0, frames * device_channels * (device_bits_per_sample / 8));
        }
    }

    void worker_loop() noexcept {
        DWORD task_idx = 0;
        HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx);

        prefill_silence();
        client->Start();

        HANDLE handles[2] = { evt, evt_stop };
        while (running.load(std::memory_order_acquire)) {
            const DWORD wr = ::WaitForMultipleObjects(2, handles, FALSE, 200);
            if (wr == WAIT_OBJECT_0 + 1) break;       // stop signaled
            if (wr != WAIT_OBJECT_0) continue;        // timeout / error

            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) continue;
            const UINT32 free_frames = buffer_frames - padding;
            if (free_frames == 0) continue;

            BYTE* data = nullptr;
            if (FAILED(render_client->GetBuffer(free_frames, &data)) || !data) continue;
            fill_buffer(data, free_frames);
            render_client->ReleaseBuffer(free_frames, 0);

            if (audio_clock && clock_freq) {
                UINT64 pos = 0, qpc = 0;
                if (SUCCEEDED(audio_clock->GetPosition(&pos, &qpc))) {
                    master_us.store(static_cast<int64_t>(pos * 1'000'000ull / clock_freq),
                                     std::memory_order_release);
                }
            }
        }

        client->Stop();
        if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
    }
};

AudioRenderWasapi::AudioRenderWasapi()  : impl_{std::make_unique<Impl>()} {}
AudioRenderWasapi::~AudioRenderWasapi() { stop(); }

mc_status_t AudioRenderWasapi::start(uint32_t sample_rate_hz, uint32_t channels) noexcept {
    if (impl_->running.load()) return MC_OK;
    impl_->src_sample_rate = sample_rate_hz;
    impl_->src_channels    = channels;
    if (!impl_->init_device()) return MC_ERR_INTERNAL;
    if (!impl_->init_format()) return MC_ERR_INTERNAL;
    impl_->running.store(true, std::memory_order_release);
    impl_->worker = std::thread([this] { impl_->worker_loop(); });
    return MC_OK;
}

void AudioRenderWasapi::stop() noexcept {
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        if (impl_->evt)      { ::CloseHandle(impl_->evt);      impl_->evt = nullptr; }
        if (impl_->evt_stop) { ::CloseHandle(impl_->evt_stop); impl_->evt_stop = nullptr; }
        return;
    }
    if (impl_->evt_stop) ::SetEvent(impl_->evt_stop);
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->render_client.Reset();
    impl_->audio_clock.Reset();
    impl_->client.Reset();
    impl_->device.Reset();
    impl_->enumerator.Reset();
    if (impl_->evt)      { ::CloseHandle(impl_->evt);      impl_->evt = nullptr; }
    if (impl_->evt_stop) { ::CloseHandle(impl_->evt_stop); impl_->evt_stop = nullptr; }
    {
        std::lock_guard<std::mutex> lk{impl_->ring_mu};
        impl_->ring.clear();
    }
}

void AudioRenderWasapi::submit(AudioFrame&& frame) noexcept {
    if (!impl_->running.load(std::memory_order_acquire)) return;
    if (frame.pcm_interleaved.empty()) return;
    if (frame.channels == 0 || frame.sample_rate_hz == 0) return;

    {
        static std::atomic<uint64_t> n{0};
        if (n.fetch_add(1, std::memory_order_relaxed) == 0) {
            MCP_LOGF(pal::LogLevel::info,
                     "AudioRenderWasapi: first PCM submit (src=%u Hz/%u ch device=%u Hz/%u ch samples=%zu)",
                     frame.sample_rate_hz, frame.channels,
                     impl_->device_sample_rate, impl_->device_channels,
                     frame.pcm_interleaved.size());
        }
    }

    // 1) 声道映射到 device_channels（mono→stereo 复制；多余声道丢；小于补零）。
    const UINT32 dev_ch = impl_->device_channels;
    const UINT32 src_ch = frame.channels;
    const std::size_t src_frames = frame.pcm_interleaved.size() / src_ch;

    std::vector<float> mapped;
    mapped.reserve(src_frames * dev_ch);
    for (std::size_t i = 0; i < src_frames; ++i) {
        const float* src = frame.pcm_interleaved.data() + i * src_ch;
        if (src_ch == dev_ch) {
            mapped.insert(mapped.end(), src, src + src_ch);
        } else if (src_ch == 1) {
            for (UINT32 c = 0; c < dev_ch; ++c) mapped.push_back(src[0]);
        } else if (src_ch == 2 && dev_ch > 2) {
            mapped.push_back(src[0]);
            mapped.push_back(src[1]);
            for (UINT32 c = 2; c < dev_ch; ++c) mapped.push_back(0.0f);
        } else {
            // 多→少：截取前 dev_ch
            for (UINT32 c = 0; c < dev_ch; ++c) {
                mapped.push_back(c < src_ch ? src[c] : 0.0f);
            }
        }
    }

    // 2) 采样率不一致时让 WASAPI AutoConvertPCM/SRC 处理 — 我们只能按 device rate 写入，
    //    所以这里要把 src_rate → device_rate 做线性重采样（最简实现）。
    //    实际上 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 是针对 Initialize 阶段格式协商的，
    //    我们仍然按 device format 写。
    if (frame.sample_rate_hz != impl_->device_sample_rate) {
        const double ratio = static_cast<double>(impl_->device_sample_rate) / frame.sample_rate_hz;
        const std::size_t out_frames = static_cast<std::size_t>(src_frames * ratio + 0.5);
        std::vector<float> rs;
        rs.reserve(out_frames * dev_ch);
        for (std::size_t i = 0; i < out_frames; ++i) {
            const double sx = i / ratio;
            const std::size_t i0 = static_cast<std::size_t>(sx);
            const std::size_t i1 = std::min(i0 + 1, src_frames - 1);
            const float t = static_cast<float>(sx - i0);
            for (UINT32 c = 0; c < dev_ch; ++c) {
                const float a = mapped[i0 * dev_ch + c];
                const float b = mapped[i1 * dev_ch + c];
                rs.push_back(a + (b - a) * t);
            }
        }
        mapped = std::move(rs);
    }

    {
        std::lock_guard<std::mutex> lk{impl_->ring_mu};
        // 上限保护：超过 ~2s 就丢老的（防失控）。
        if (impl_->ring.size() + mapped.size() > Impl::kMaxRingSamples) {
            const std::size_t drop = impl_->ring.size() + mapped.size() - Impl::kMaxRingSamples;
            for (std::size_t i = 0; i < drop && !impl_->ring.empty(); ++i) impl_->ring.pop_front();
        }
        impl_->ring.insert(impl_->ring.end(), mapped.begin(), mapped.end());
    }
}

int64_t AudioRenderWasapi::master_clock_us() const noexcept {
    return impl_->master_us.load(std::memory_order_acquire);
}

}  // namespace mcp::media
