/*
 * Codec MFT Audio — Microsoft AAC Decoder MFT (CLSID_CMSAACDecMFT)。
 *
 * 流程（同步模式；音频 MFT 不要求 async unlock — 仅 video 硬解 MFT 强制 async，ADD §5.6.1）：
 *   1. CoCreateInstance(CLSID_CMSAACDecMFT)
 *   2. Input type: MFAudioFormat_AAC + AAC_PAYLOAD_TYPE=0 (Raw) + MF_MT_USER_DATA
 *      = 12 字节 HEAACWAVEINFO 头（wPayloadType, wAudioProfileLevelIndication, wStructType, reserved×6）
 *      + AudioSpecificConfig（来自 SDP fmtp config=）
 *   3. Output type: MFAudioFormat_Float, 32-bit interleaved（ADD §4.2 / §5.8）
 *   4. submit(raw, pts): MFCreateMemoryBuffer + IMFSample, ProcessInput → 循环 ProcessOutput → emit AudioFrame
 *
 * 不实装 LATM/LOAS（RFC 6416）— 仅常见 AAC-hbr 路径。
 */

#include "media/codec_mft_audio.h"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mmreg.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

#include "pal/log.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

using Microsoft::WRL::ComPtr;

namespace mcp::media {

namespace {

// HEAACWAVEINFO 中 wfx 之后的 12 字节头（MF_MT_USER_DATA 起始）。
struct AacUserDataHeader {
    WORD  wPayloadType;
    WORD  wAudioProfileLevelIndication;
    WORD  wStructType;
    WORD  wReserved1;
    DWORD wReserved2;
};
static_assert(sizeof(AacUserDataHeader) == 12, "AAC user data header must be 12 bytes");

bool set_input_type(IMFTransform* mft,
                    uint32_t channels, uint32_t sample_rate,
                    std::span<const uint8_t> asc) noexcept {
    ComPtr<IMFMediaType> t;
    if (FAILED(::MFCreateMediaType(&t))) return false;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_AAC);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,         channels);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,   sample_rate);
    t->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE,           0);     // 0 = Raw AAC
    t->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,      16);    // 占位（解码器决定）

    std::vector<uint8_t> user_data(sizeof(AacUserDataHeader) + asc.size(), 0);
    auto* hdr = reinterpret_cast<AacUserDataHeader*>(user_data.data());
    hdr->wPayloadType                = 0;        // Raw AAC
    hdr->wAudioProfileLevelIndication = 0xFE;    // unknown
    hdr->wStructType                 = 0;
    if (!asc.empty()) {
        std::memcpy(user_data.data() + sizeof(AacUserDataHeader), asc.data(), asc.size());
    }
    t->SetBlob(MF_MT_USER_DATA, user_data.data(), static_cast<UINT32>(user_data.size()));

    HRESULT hr = mft->SetInputType(0, t.Get(), 0);
    if (FAILED(hr)) {
        MCP_LOGF(pal::LogLevel::error,
                 "CodecMftAudio: SetInputType hr=0x%08lX", hr);
        return false;
    }
    return true;
}

// 选择 float-32 interleaved output；sr/ch 直接采用 MFT 枚举出的实际值，
// 不用 SDP rtpmap 覆盖（HE-AAC SBR 流的合法 float 输出是 SBR 后参数，
// 强行写 16000/1 会触发 MF_E_INVALIDMEDIATYPE）。
bool set_output_type_float(IMFTransform* mft,
                            uint32_t& out_channels, uint32_t& out_sample_rate) noexcept {
    for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> t;
        HRESULT hr = mft->GetOutputAvailableType(0, i, &t);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr)) break;

        GUID sub{};
        UINT32 bps = 0;
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bps);
        if (sub != MFAudioFormat_Float || bps != 32) continue;

        UINT32 ch = 0, sr = 0;
        t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,        &ch);
        t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,  &sr);
        // BLOCK_ALIGN/AVG_BYTES 仍按枚举给的 ch/sr 写完整，避免 MFT 拒绝。
        t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,     ch * 4);
        t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, ch * 4 * sr);
        HRESULT shr = mft->SetOutputType(0, t.Get(), 0);
        if (SUCCEEDED(shr)) {
            out_channels    = ch;
            out_sample_rate = sr;
            MCP_LOGF(pal::LogLevel::info,
                     "CodecMftAudio: output type negotiated -> %u Hz / %u ch float-32",
                     sr, ch);
            return true;
        }
        MCP_LOGF(pal::LogLevel::warn,
                 "CodecMftAudio: SetOutputType float (%u/%u) hr=0x%08lX", sr, ch, shr);
    }
    MCP_LOG_ERROR("CodecMftAudio: no acceptable float-32 output type available");
    return false;
}

}  // namespace

struct CodecMftAudio::Impl {
    Config              cfg;
    ComPtr<IMFTransform> mft;
    bool                started = false;
    uint32_t            out_channels    = 0;
    uint32_t            out_sample_rate = 0;
    std::atomic<uint64_t> input_count{0};
    std::atomic<uint64_t> output_count{0};
    std::atomic<uint64_t> process_output_count{0};

    // STREAM_CHANGE 之后枚举 output types 找 float-32（HE-AAC SBR 流必经路径）。
    // sr/ch 直接采用 MFT 给的值（流实际参数，可能与 SDP rtpmap 不同，例如 SBR 上变两倍）。
    bool reselect_output_float() noexcept {
        for (DWORD i = 0; ; ++i) {
            ComPtr<IMFMediaType> t;
            HRESULT hr = mft->GetOutputAvailableType(0, i, &t);
            if (hr == MF_E_NO_MORE_TYPES) break;
            if (FAILED(hr)) break;

            GUID sub{}; UINT32 bps = 0;
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bps);
            if (sub != MFAudioFormat_Float || bps != 32) continue;

            UINT32 ch = 0, sr = 0;
            t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,        &ch);
            t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,  &sr);
            t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,     ch * 4);
            t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, ch * 4 * sr);
            HRESULT shr = mft->SetOutputType(0, t.Get(), 0);
            if (SUCCEEDED(shr)) {
                MCP_LOGF(pal::LogLevel::info,
                         "CodecMftAudio: output type (re)negotiated -> %u Hz / %u ch float-32",
                         sr, ch);
                out_sample_rate = sr;
                out_channels    = ch;
                return true;
            }
        }
        return false;
    }

    void emit_pending() noexcept {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{};
            mft->GetOutputStreamInfo(0, &info);

            ComPtr<IMFSample>     out_sample;
            ComPtr<IMFMediaBuffer> out_buf;
            const bool provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                                    MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            if (!provides) {
                ::MFCreateSample(&out_sample);
                ::MFCreateMemoryBuffer(info.cbSize > 0 ? info.cbSize : 16384, &out_buf);
                out_sample->AddBuffer(out_buf.Get());
            }

            MFT_OUTPUT_DATA_BUFFER ob{};
            ob.dwStreamID = 0;
            ob.pSample    = provides ? nullptr : out_sample.Get();
            DWORD status = 0;
            HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);

            // 前 8 次 ProcessOutput 全打 hr 用于诊断（HE-AAC SBR 启动期）。
            const auto po_n = ++process_output_count;
            if (po_n <= 8 || (po_n % 1000) == 0) {
                MCP_LOGF(pal::LogLevel::info,
                         "CodecMftAudio: ProcessOutput[#%llu] hr=0x%08lX status=0x%lX provides=%d",
                         static_cast<unsigned long long>(po_n),
                         hr, status, provides ? 1 : 0);
            }

            // MFT spec: STREAM_CHANGE / FAIL / NEED_MORE_INPUT 路径 caller 仍负责
            // release driver-allocated pSample(provides=true 时)与 pEvents。同 video MFT。
            auto release_ob = [&]() {
                if (provides && ob.pSample) { ob.pSample->Release(); ob.pSample = nullptr; }
                if (ob.pEvents) { ob.pEvents->Release(); ob.pEvents = nullptr; }
            };

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { release_ob(); break; }

            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                release_ob();
                if (reselect_output_float()) continue;
                MCP_LOG_WARN("CodecMftAudio: STREAM_CHANGE 后未找到 float-32 output type");
                break;
            }

            if (FAILED(hr)) {
                release_ob();
                MCP_LOGF(pal::LogLevel::warn,
                         "CodecMftAudio: ProcessOutput hr=0x%08lX", hr);
                break;
            }
            // 成功路径下也释放 pEvents(pSample 由后续 Attach/raw 路径处理)。
            if (ob.pEvents) { ob.pEvents->Release(); ob.pEvents = nullptr; }

            // 引用计数语义（MSDN）：
            //   - provides=true → MFT 把它 AddRef 过的 sample 写到 ob.pSample，caller 必须 Release。
            //                     用 ComPtr::Attach 接管，避免 double-free。
            //   - provides=false → ob.pSample 仍是 caller 的 raw 指针，MFT 不 AddRef，caller 不 Release。
            ComPtr<IMFSample> got;
            if (provides) {
                got.Attach(ob.pSample);    // takeover; do NOT add-ref again
                ob.pSample = nullptr;
            } else {
                got = out_sample;
            }
            if (!got) continue;

            LONGLONG pts_100ns = 0;
            got->GetSampleTime(&pts_100ns);
            ComPtr<IMFMediaBuffer> mb;
            if (SUCCEEDED(got->ConvertToContiguousBuffer(&mb)) && mb) {
                BYTE* p = nullptr;
                DWORD cb = 0;
                if (SUCCEEDED(mb->Lock(&p, nullptr, &cb)) && cb > 0) {
                    AudioFrame af;
                    af.pts_us         = pts_100ns / 10;
                    af.sample_rate_hz = out_sample_rate;
                    af.channels       = out_channels;
                    const std::size_t n = cb / sizeof(float);
                    af.pcm_interleaved.assign(reinterpret_cast<const float*>(p),
                                                reinterpret_cast<const float*>(p) + n);
                    mb->Unlock();
                    const auto idx = output_count.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (idx == 1) {
                        MCP_LOGF(pal::LogLevel::info,
                                 "CodecMftAudio: first PCM frame emit (sr=%u ch=%u samples=%zu pts=%lld)",
                                 out_sample_rate, out_channels, n,
                                 static_cast<long long>(af.pts_us));
                    }
                    if (cfg.emit) cfg.emit(std::move(af));
                } else if (p) {
                    mb->Unlock();
                }
            }
        }
    }
};

CodecMftAudio::CodecMftAudio(Config cfg) : impl_{std::make_unique<Impl>()} {
    impl_->cfg = std::move(cfg);
}
CodecMftAudio::~CodecMftAudio() { stop(); }

mc_status_t CodecMftAudio::start(std::span<const uint8_t> asc) noexcept {
    if (impl_->cfg.codec != MC_AUDIO_CODEC_AAC) {
        MCP_LOG_WARN("CodecMftAudio: only AAC implemented in v1");
        return MC_ERR_UNSUPPORTED;
    }
    if (impl_->cfg.channels == 0 || impl_->cfg.sample_rate == 0) {
        return MC_ERR_INVALID_ARG;
    }

    HRESULT hr = ::CoCreateInstance(CLSID_CMSAACDecMFT, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&impl_->mft));
    if (FAILED(hr)) {
        MCP_LOGF(pal::LogLevel::error,
                 "CodecMftAudio: CoCreateInstance CMSAACDecMFT hr=0x%08lX", hr);
        return MC_ERR_INTERNAL;
    }

    if (!set_input_type(impl_->mft.Get(), impl_->cfg.channels, impl_->cfg.sample_rate, asc)) {
        return MC_ERR_INTERNAL;
    }
    if (!set_output_type_float(impl_->mft.Get(), impl_->out_channels, impl_->out_sample_rate)) {
        return MC_ERR_INTERNAL;
    }

    impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    impl_->started = true;
    MCP_LOGF(pal::LogLevel::info,
             "CodecMftAudio: started AAC decoder, %u Hz / %u ch, asc %zu bytes",
             impl_->cfg.sample_rate, impl_->cfg.channels, asc.size());
    return MC_OK;
}

void CodecMftAudio::submit(std::span<const uint8_t> raw, int64_t pts_us) noexcept {
    if (!impl_->started || !impl_->mft || raw.empty()) return;

    const auto idx = impl_->input_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (idx == 1) {
        MCP_LOGF(pal::LogLevel::info,
                 "CodecMftAudio: first AAC AU submit (size=%zu, pts=%lld)",
                 raw.size(), static_cast<long long>(pts_us));
    }

    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(::MFCreateMemoryBuffer(static_cast<DWORD>(raw.size()), &buf))) return;
    BYTE* p = nullptr;
    DWORD max_len = 0;
    if (FAILED(buf->Lock(&p, &max_len, nullptr))) return;
    std::memcpy(p, raw.data(), raw.size());
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(raw.size()));

    ComPtr<IMFSample> sample;
    if (FAILED(::MFCreateSample(&sample))) return;
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(pts_us * 10);          // 100ns ticks
    sample->SetSampleDuration(0);

    HRESULT hr = impl_->mft->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        impl_->emit_pending();
        hr = impl_->mft->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        MCP_LOGF(pal::LogLevel::warn,
                 "CodecMftAudio: ProcessInput hr=0x%08lX", hr);
        return;
    }
    impl_->emit_pending();
}

void CodecMftAudio::stop() noexcept {
    if (impl_->mft) {
        impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        impl_->mft.Reset();
    }
    impl_->started = false;
}

}  // namespace mcp::media
