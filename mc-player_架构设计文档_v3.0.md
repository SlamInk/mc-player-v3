# mc-player — 多协议超低延时媒体播放器架构设计文档 v3.0

| 项目 | 内容 |
|---|---|
| 项目名称 | mc-player (Multi-protocol Client Player) |
| 文档类型 | 架构设计文档（ADD） |
| 目标平台 | Windows 10 1809+ / Windows 11 x64 |
| 协议（一类） | RTSP/RTP over UDP（含 interleaved TCP 降级） |
| 协议（一类） | WebRTC WHEP（draft-ietf-wish-whep-03） |
| 编解码后端（主） | Windows Media Foundation Transforms（H.264 / H.265 / AAC OS 内置） |
| 编解码后端（副） | mc-libcodec 子项目（自研 H.264 / H.265 全 profile 软解兜底） |
| WebRTC 栈 | libdatachannel 0.24.x（SRTP + DTLS + ICE） |
| 客户端内部延时 | 8~25 ms（含 DCOMP 极致档） |
| 端到端延时（LAN） | 35~75 ms（144 Hz VRR） / 25~50 ms（240 Hz VRR + DCOMP） |
| 子项目 | `subprojects/mc-libcodec/`（独立 CMake target、独立 API、独立测试套件） |

---

## 1. 概述

### 1.1 项目定位

mc-player 是 Windows 平台的多协议超低延时媒体播放器，对标 NDI HX（≈33 ms 单帧）和 WebRTC LAN 极限调优（50~80 ms）水平。核心命题是把"用户感知到的复杂度"降到最低：用户给一个 URL（`rtsp://...` 或 `whep://...`），mc-player 自动选最佳 GPU、最佳渲染档位、最佳解码后端，无需任何配置。

> 端到端延时由四方共同决定：**编码端 + 网络 + 客户端 + 显示器 scan-out**。客户端可控部分压到 8~25 ms；端到端 35~75 ms 的前提是编码器 low-delay、内网低抖动、现代显示器（120 Hz+ 优）+ DCOMP Independent Flip。

### 1.2 设计目标

| 维度 | 目标 | 前提 / 备注 |
|---|---|---|
| 客户端内部延时 | 8~25 ms | MFT 等延时 + DCOMP 节省 ~1 帧 Present 队列（best-effort） |
| 端到端延时（60 Hz 显示器） | 60~110 ms | 含 scan-out；编码端 low-delay；GOP ≤ 60 |
| 端到端延时（144 Hz VRR） | 35~75 ms | 现代硬件 + DCOMP 第 4 档 |
| 端到端延时（240 Hz VRR + DCOMP） | 25~50 ms | 极限档；Win11 + dGPU 推荐 |
| 首帧渲染（最优） | ≤ 200 ms | sprop-parameter-sets + PLAY 立即 I 帧 |
| 首帧渲染（一般） | ≤ 500 ms | 等下一自然 I 帧（GOP 30） |
| 首帧渲染（大 GOP） | ≤ 2 s | GOP ≥ 60 + 无带外 SPS + PLI 等待 |
| WHEP 首帧（含 ICE/DTLS，LAN） | ≤ 600 ms | LAN + 推荐 STUN |
| WHEP 首帧（公网） | ≤ 1.2 s | 含 NAT 穿透与可能的 TURN relay |
| CPU 占用（1080p@30 fps，硬解） | ≤ 8% | i5-12500 |
| CPU 占用（1080p@30 fps，mc-libcodec 软解） | ≤ 18% | i5-12500 单核含 SIMD |
| 丢包恢复 | < 100 ms | NACK + PLI 兜底 |
| 断流恢复 | ≤ 1 s 首次重连 | 指数退避 + jitter |
| 无独显场景（仅 iGPU） | 上述指标全部满足 | UHD 730/770、Iris Xe、Vega 全集显环境 |
| 软解兜底能力 | 1080p H.264 / H.265 30 fps single-thread 可解 | mc-libcodec；4K 仅作 fallback |

### 1.3 协议路线

```
默认: RTSP/RTP over UDP + RTCP
默认: WebRTC WHEP（SRTP + DTLS + ICE，draft-ietf-wish-whep）
降级: RTSP interleaved TCP（RFC 2326 §10.12）
扩展（后续）: RTMP / SRT / 自研私有协议
```

---

## 2. 设计原则

1. **最短路径**：每条数据流穿过的模块和拷贝数最少。
2. **低水位队列**：每条 SPSC 队列深度≤4，配 cache-line padding 防 false-sharing。
3. **零拷贝**：DXVA NV12 surface 在 GPU 内通过 dual-bind 直接喂 SRV，无 readback；CPU 拷贝口径明确（见 §4.3）。
4. **主动丢帧**：宁可 freeze last frame 也不堆积；jitter buffer / decode / render 三处可丢。
5. **硬件优先**：解码 → MFT；合成 → DCOMP；时钟 → QPC；同步等待 → Waitable Timer（CREATE_WAITABLE_TIMER_HIGH_RESOLUTION）。
6. **自适应**：jitter buffer、NACK 调度、丢帧策略动态调整（Kalman / iat 直方图）。
7. **协议抽象**：核心管线协议无关；RTSP 与 WHEP 在 jitter buffer 之上完全合流。
8. **OS 协同**：MMCSS、IAudioClient3、`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`、`SetMaximumFrameLatency` 等。
9. **可观测**：关键路径 ETW；`mc_stats_t` 输出端到端指标。
10. **平台原生**：能用 Windows OS 自带能力时不引第三方。Codec→MFT，HTTP→WinHTTP，TLS→Schannel/MbedTLS，时钟→QPC，COM/D3D11/DCOMP 全部直调。理由：减小攻击面、避免第三方 license、跟随 OS 安全更新、优化器对 OS API 的深度优化。
11. **能力探测先于使用**：所有硬件加速路径在 open 前先做能力探测（`ID3D11VideoDevice::CheckVideoDecoderFormat`、`MFTEnumEx`、`IDXGIOutput6::CheckHardwareCompositionSupport`、`CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)`），fail-open（探测到不支持立即降级）而不是 fail-late（运行时 HRESULT 异常）。这让从打开 URL 到首帧的路径完全确定，没有 fallback 抖动。

---

## 3. 系统架构

### 3.1 分层架构

```
┌────────────────────────────────────────────────────────────┐
│                       应用层 (App)                          │
│            mc_player API · 事件回调 · UI 集成                │
└────────────────────────────────────────────────────────────┘
┌────────────────────────────────────────────────────────────┐
│                   Controller 层                             │
│   生命周期 · 状态机 · Device/Network 恢复 · Adapter 智能选择 │
└────────────────────────────────────────────────────────────┘
┌────────────────────────────────────────────────────────────┐
│          Transport Session 聚合层 (TS)                      │
│   ts_rtsp_udp │ ts_rtsp_tcp │ ts_whep                      │
│   每种实现内部自管信令 + 传输；对上输出 RTP/RTCP 事件流      │
└────────────────────────────────────────────────────────────┘
┌────────────────────────────────────────────────────────────┐
│              媒体处理核心层 (Media Core)                    │
│  Adaptive Jitter Buf → Depack → Codec Bridge → Render      │
│  Codec Bridge 内分两路：MFT 硬解（主） + mc-libcodec 软解    │
│  协议无关；NetEQ 三相位音频控制；Kalman 视频抖动估计         │
└────────────────────────────────────────────────────────────┘
┌────────────────────────────────────────────────────────────┐
│              平台抽象层 (PAL)                               │
│  Socket(IOCP) · MMCSS · SPSC · QPC · ETW                   │
│  + DXGI Caps Probe + WinHTTP + MF Runtime + COM Apartment   │
└────────────────────────────────────────────────────────────┘

  并列子项目（不属于 mc-player 主项目）：
┌────────────────────────────────────────────────────────────┐
│            subprojects/mc-libcodec/                         │
│  H.264 + H.265 全 profile 软解 · SSE2/AVX2 SIMD · 独立 API  │
│  通过 mc_codec.h 接入；独立 CMake target；可独立复用         │
└────────────────────────────────────────────────────────────┘
```

> Codec Bridge 统辖 MFT + mc-libcodec 两条解码路径，前者出 D3D11 texture（IMFDXGIBuffer），后者出 NV12 RAM buffer，渲染器对两者透明。
> mc-libcodec 是 monorepo 内的并列子项目，不在 mc-player 五层架构内，通过 mc_codec.h API 接入 Codec Bridge。

### 3.2 完整模块视图

```
                     ┌──────────────────┐
                     │   App / UI       │
                     └────────┬─────────┘
                              ↓
                     ┌──────────────────┐
                     │   Controller     │
                     │   生命周期 FSM    │
                     │   智能 Adapter 选│
                     │   智能 Profile 选│
                     └────────┬─────────┘
                              ↓
        ┌──────────────────────────────────────────┐
        │   Transport Session (TS)                 │
        │  ┌──────────────┐  ┌──────────────┐     │
        │  │ ts_rtsp_udp  │  │  ts_whep     │     │
        │  │ ts_rtsp_tcp  │  │ libdatachan- │     │
        │  │              │  │ nel + WinHTTP│     │
        │  └──────┬───────┘  └──────┬───────┘     │
        │         ↓ RTP/RTCP        ↓ RTP（SRTP   │
        │                            decrypted）   │
        └─────────┬─────────────────┬──────────────┘
                  ↓                 ↓
                  └─────────┬───────┘
                            ↓
                  ┌──────────────────┐
                  │ Adaptive Jitter  │
                  │ Kalman / NetEQ   │   ← 协议无关，二路在此合流
                  └────┬─────────────┘
                       ↓
               ┌───────┴────────┐
               ↓ Video          ↓ Audio
        ┌──────────────┐  ┌──────────────┐
        │ H264/5 Depack│  │ AAC/Opus/    │
        │ STAP-A/FU-A  │  │ G.711 Depack │
        │ SEI/GDR保留  │  │              │
        └──────┬───────┘  └──────┬───────┘
               ↓                 ↓
       ┌───────────────────────────────────┐
       │    Codec Bridge                   │
       │                                   │
       │  ┌─ MFT 硬解（主）─────────────┐  │
       │  │ H.264/H.265/AAC MFT         │  │
       │  │ Async + 事件驱动             │  │
       │  │ DXVA via IMFDXGIDeviceMgr   │  │
       │  │ 输出 IMFDXGIBuffer →         │  │
       │  │ ID3D11Texture2D（dual-bind） │  │
       │  └─────────────────────────────┘  │
       │  ┌─ mc-libcodec 软解（兜底）────┐  │
       │  │ subprojects/mc-libcodec      │  │
       │  │ H.264/H.265 全 profile      │  │
       │  │ 输出 NV12 RAM 缓冲           │  │
       │  └─────────────────────────────┘  │
       │  ┌─ libopus（WebRTC 音频）─────┐  │
       │  │ 输出 float interleaved PCM  │  │
       │  └─────────────────────────────┘  │
       └────┬─────────────────────┬────────┘
            ↓                     ↓
     ┌──────────────┐      ┌──────────────┐
     │ D3D11 Render │◄─────┤ WASAPI       │
     │ 4 档 + MPO   │ A/V  │ IAudioClient3│
     │ +DCOMP 极致档│ 同步 │ 设备切换处理 │
     │ 色彩 BT.709  │      │              │
     └──────────────┘      └──────────────┘

       └──────────── RTCP Feedback ◄── jitter buffer
                     SR/RR/NACK/PLI
                     RTSP: own UDP socket
                     WHEP: libdatachannel SRTCP track
```

### 3.3 线程视图

| 线程 | MMCSS 任务 | 实际优先级 | 职责 |
|---|---|---|---|
| T0 App/UI | — | Normal | API、事件回调、Win32 消息泵 |
| T1 Signaling | — | Normal | RTSP 握手 / WHEP HTTP O/A、保活、状态机驱动 |
| T2 Network RX | **不挂 MMCSS**；`THREAD_PRIORITY_TIME_CRITICAL` + 绑 P-core | 15 | RTSP-UDP 收包 + 入 jitter buffer（WHEP 模式下被替代为 T7） |
| T3 RTCP | — | Normal | SR/RR/NACK/PLI（RTSP 路径走 own UDP；WHEP 路径走 SRTCP） |
| T4 Video Decode | **Playback** | ~23 | 调度 MFT 异步事件循环或 mc-libcodec 解码、出队纹理 / NV12 buffer |
| T5 Video Render | **Playback** | ~23 | D3D11 Present，含 DCOMP commit |
| T6 Audio Render | **Pro Audio** | ~26 | WASAPI 回调填缓冲 |
| T7 WHEP-PC | **不挂 MMCSS**；`THREAD_PRIORITY_HIGHEST` | 15 | libdatachannel 内部线程（DTLS/SRTP 解密、ICE 心跳）。库自管理，应用层无需创建。**不挂 MMCSS 原因**：DTLS/SRTP CPU 密集但非确定性时序敏感（对齐 Chromium WebRTC 主线模式） |

**线程合并说明**：
- 当协议是 WHEP 时，T2 实际由 libdatachannel 内部 socket 线程承担，mc-player 不创建独立 T2。从 controller 看到的依然是 `on_rtp(stream, pkt, len)` 回调，与 RTSP-UDP 路径完全同形。
- 当协议是 RTSP 时，T7 不存在。

**MMCSS 集成**：T4/T5/T6 通过 `IMFRealTimeClientEx::SetWorkQueueEx` 让 MFT 在 MMCSS-aware 线程上执行其内部 worker；同时上层手动 `AvSetMmThreadCharacteristics("Playback")`。

**CPU 亲和性（Intel 12 代+）**：T2/T5/T6 绑 P-core；libdatachannel 内部线程不主动绑（让库自决策）。

### 3.4 控制流与数据流（Y 形）

两条 transport 路径在 jitter buffer 之上完全合流（depack → codec bridge → render 全共享），在之下完全分离（信令、socket、加密层不同）。

```
RTSP 路径                         WHEP 路径
─────────                         ─────────
TCP 信令 socket                   HTTPS POST/PATCH/DELETE (WinHTTP)
+ UDP RTP socket × 2              + UDP SRTP socket × 1（BUNDLE 复用）
+ UDP RTCP socket × 2             + DTLS 握手（libdatachannel）
                                   + ICE 候选交换（libjuice）
       │                                 │
       └──────── RTP datagram ──────────┘
                       ↓
                ┌──────────────┐
                │ Adaptive JB  │   ← 在此合流，下游无需区分协议
                └──────┬───────┘
                       ↓
                  ...一致管线...
```

工程收益：depack / jitter / render / sync / rtcp 这条核心管线对协议无感；ts_rtsp_* 与 ts_whep 仅是数据来源的切换，不改变数据语义。

---

## 4. 数据流

### 4.1 视频路径（双解码路径）

```
[网卡 DMA]
    ↓
[Kernel Socket Buffer]      SO_RCVBUF = 4MB+
    ↓                       URO 关闭；Interrupt Moderation 关
[用户态 RTP 缓冲]            WSARecvFrom / IOCP（RTSP）
                             或 libdatachannel decrypt（WHEP）
    ↓ （解 RTP 头 + 扩展头 + 去 padding）
[Adaptive Jitter Buffer]    动态 target_delay；NACK 调度
    ↓
[H.264/H.265 Depacketizer]  STAP-A / FU-A / AP / FU 重组；保留 SEI
    ↓ 输出 Annex-B（0x00000001 起始码）；与 MFT 输入逐字节匹配
    ↓
┌── 路径 A：MFT 硬解（主，能力支持时）──────────────┐
│   1 memcpy 入 IMFMediaBuffer（~50 μs）            │
│   ↓                                                │
│   ProcessInput（事件 METransformNeedInput 触发）  │
│   ↓                                                │
│   ProcessOutput（事件 METransformHaveOutput 触发） │
│   ↓                                                │
│   IMFSample → QI(IMFDXGIBuffer)                   │
│   → GetResource(ID3D11Texture2D) + GetSubresourceIndex│
│   ↓ 与 renderer 同 ID3D11Device，无跨 device 拷贝 │
│   ↓ texture 实为 array slice，SRV 用              │
│     D3D11_SRV_DIMENSION_TEXTURE2DARRAY +          │
│     FirstArraySlice = subres                      │
└────────────────────────────────────────────────────┘
                     或
┌── 路径 B：mc-libcodec 软解（兜底，硬件不支持时）──────┐
│   1 函数调用 mc_codec_submit(annexb, len)          │
│   ↓                                                 │
│   mc-libcodec 内部 NAL/CABAC/IDCT/MC/deblock/...   │
│   ↓                                                 │
│   mc_codec_pull → NV12 in RAM (Y/UV pointers)     │
│   ↓                                                 │
│   UpdateSubresource → ID3D11Texture2D dynamic     │
│   ~0.5 ms 1080p（CPU→GPU 拷贝）                    │
└────────────────────────────────────────────────────┘
    ↓
[D3D11 Pixel Shader: NV12→RGB]
  矩阵: BT.601/709/2020 按 SPS VUI 选择
  range: limited(16-235) / full 按 video_full_range_flag
    ↓
[Swap Chain Back Buffer]
  4 档自动选择：COMPAT / BALANCED / EXTREME / ULTIMATE_DCOMP
    ↓ Present(0, DXGI_PRESENT_ALLOW_TEARING)
[显示器 scan-out]            8~16 ms @60 Hz / ~7 ms @144 Hz / ~4 ms @240 Hz VRR

CPU 拷贝（路径 A）: 1 次（depack→IMFMediaBuffer）
CPU 拷贝（路径 B）: 1 次（depack→mc-libcodec 输入）+ 1 次（NV12→D3D11 dynamic）
GPU→CPU readback: 0 次
GPU 内部拷贝: 0（dual-bind 生效）或 1 次（CopySubresource 兜底）
```

### 4.2 音频路径

```
[UDP/SRTP 接收]
    ↓
[Adaptive Jitter Buffer]    iat 直方图 + NetEQ 三相位
    ↓
┌── AAC（RTSP/RTP RFC 3640）────────────────────┐
│ AAC Depacketizer                              │
│   ↓                                           │
│ MFT AAC Decoder（CLSID_CMSAACDecMFT）         │
│   payload_type=0（raw AAC，mode=AAC-hbr）      │
│   user_data = HEAACWAVEINFO 后 12 字节        │
│              + AudioSpecificConfig            │
│   ↓                                           │
│ MFAudioFormat_Float 32-bit interleaved        │
└───────────────────────────────────────────────┘
    或
┌── Opus（WebRTC 必备）──────────────────────────┐
│ Opus Depacketizer（RFC 7587 简单 1 包 1 帧）   │
│   ↓                                            │
│ libopus opus_decode_float                      │
│   ↓                                            │
│ float interleaved（与 AAC 路径输出格式一致）    │
└────────────────────────────────────────────────┘
    或
┌── G.711 a-law / μ-law ─────────────────────────┐
│ 静态 256-entry LUT 查表，~50 LOC 自实现        │
│   ↓                                            │
│ float interleaved                              │
└────────────────────────────────────────────────┘
    ↓
[环形缓冲 3~20 ms]
    ↓
[IAudioClient3 Event Callback]   min period 典型 2.67~3 ms
    ↓ GetBuffer → memcpy → ReleaseBuffer
[WASAPI Audio Engine]            共享模式 mix tail ~10 ms（固有）
    ↓
[声卡 DAC]
```

MFT AAC 直出 `MFAudioFormat_Float` 32-bit interleaved，无需额外重采样工序。WASAPI `AUTOCONVERTPCM | SRC_DEFAULT_QUALITY` 处理设备 mix 转换。

### 4.3 dual-bind 探测与零拷贝口径

零拷贝定义：**不存在 GPU→CPU readback，且 GPU 内部至多 1 次 NV12 array slice 复制**。

```c
D3D11_TEXTURE2D_DESC probe = {
    .Width = 1920, .Height = 1088,
    .Format = DXGI_FORMAT_NV12,
    .Usage = D3D11_USAGE_DEFAULT,
    .BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE,
    .ArraySize = 1, .MipLevels = 1, .SampleDesc = {1, 0}
};
ID3D11Texture2D* tex = NULL;
HRESULT hr = device->CreateTexture2D(&probe, NULL, &tex);
bool dual_bind_ok = SUCCEEDED(hr);
if (dual_bind_ok) tex->Release();
```

要让 MFT 输出的 sample 真正可被 SRV 绑定，需在 MFT output stream attribute 上显式设置：

```cpp
ComPtr<IMFAttributes> out_attrs;
mft->GetOutputStreamAttributes(0, &out_attrs);
out_attrs->SetUINT32(MF_SA_D3D11_BINDFLAGS,
                     D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE);
out_attrs->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
out_attrs->SetUINT32(MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT_PROGRESSIVE, 4);
```

`MF_SA_D3D11_BINDFLAGS` 是 dual-bind 真正生效的关键——MFT 默认仅 `D3D11_BIND_DECODER`。`MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT_PROGRESSIVE` 显式限制 surface pool 大小（默认可达 32），4 即够低延时管线。

**driver 兼容性**（截至 2026-04）：
- Intel Gen 9.5+（UHD 630 含）✓
- AMD GCN 2.0+ ✓
- NVIDIA Pascal+（GTX 1050+）✓
- 老代驱动（pre-WDDM 1.2 已基本绝迹）回退 `CopySubresourceRegion`，1080p NV12 GPU 内拷贝 ~0.5 ms

---

## 5. 模块详细设计

### 5.1 Transport Session — RTSP/RTP

#### 5.1.1 对外接口

```c
typedef struct mc_ts_callbacks {
    void (*on_rtp) (int stream, const uint8_t* pkt, size_t len, void* user);
    void (*on_rtcp)(int stream, const uint8_t* pkt, size_t len, void* user);
    void (*on_event)(mc_ts_event_t ev, mc_result_t err, void* user);
    void* user;
} mc_ts_callbacks;

mc_transport_t* mc_transport_open(const mc_ts_open_params* params,
                                  const mc_ts_callbacks* cbs);
```

#### 5.1.2 SDP / RTSP 状态机

`OPTIONS → DESCRIBE → SETUP × N → PLAY → (KEEPALIVE 循环) → TEARDOWN`。
Digest 认证（RFC 2617，仅 `qop=auth`，algorithm=MD5）；Content-Base/Content-Location 优先级见 §17.1。

#### 5.1.3 interleaved TCP 降级

UDP socket 创建失败或 SETUP 返回 461 时切换 `Transport: RTP/AVP/TCP;interleaved=0-1`，按 RFC 2326 §10.12 解 `$ ch len`。

### 5.2 Transport Session — WHEP

#### 5.2.1 拓扑

```
src/transport/ts_whep/
  ts_whep.cpp        --- mc_transport_iface_t 实现（C 接口）
  ts_whep_pc.cpp     --- libdatachannel rtc::PeerConnection C++ 包装
  whep_http.cpp      --- WinHTTP 异步 POST/PATCH/DELETE 客户端
```

辅助：`src/protocol/sdp_munge.cpp` 构造 receive-only offer SDP，复用 `protocol/sdp.c` 的 fmtp/rtpmap 解析器。

#### 5.2.2 WHEP HTTP 流程（draft-ietf-wish-whep-03）

```
C → S:  POST /whep                        offer SDP body（Content-Type: application/sdp）
        ↓
S → C:  201 Created
        Location: /whep/<session-id>      ← 缓存供 PATCH/DELETE
        Content-Type: application/sdp
        <answer SDP>

(ICE trickle，运行期可多次)
C → S:  PATCH /whep/<session-id>          a=candidate:... \r\n
        Content-Type: application/trickle-ice-sdpfrag
        ↓
S → C:  204 No Content

(关闭)
C → S:  DELETE /whep/<session-id>
        ↓
S → C:  200 OK
```

#### 5.2.3 WinHTTP 异步实现

```cpp
HINTERNET h_session = WinHttpOpen(L"mc-player/3.0",
    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);

void post_offer(const std::string& url, const std::string& sdp,
                std::function<void(const std::string& answer, const std::string& loc)> cb) {
    URL_COMPONENTS uc{};  uc.dwStructSize = sizeof(uc);
    /* parse url ... */
    HINTERNET h_conn = WinHttpConnect(h_session, host, port, 0);
    HINTERNET h_req = WinHttpOpenRequest(h_conn, L"POST", path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        is_https ? WINHTTP_FLAG_SECURE : 0);
    WinHttpSetStatusCallback(h_req, on_http_callback,
        WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0);
    WinHttpSendRequest(h_req,
        L"Content-Type: application/sdp\r\n", -1L,
        (LPVOID)sdp.c_str(), (DWORD)sdp.size(),
        (DWORD)sdp.size(), (DWORD_PTR)new ReqCtx{cb});
}
```

WinHTTP 是 Windows OS 原生组件，内部基于 IOCP；HTTPS 通过 Schannel；与系统代理 / WPAD / PAC 无缝集成。比 WinINet 更适合非 UI 场景。

#### 5.2.4 PeerConnection 配置（libdatachannel 真实 API）

```cpp
#include <rtc/rtc.hpp>

rtc::Configuration cfg;
cfg.iceServers = { rtc::IceServer("stun:stun.l.google.com:19302") };
cfg.disableAutoNegotiation = true;

auto pc = std::make_shared<rtc::PeerConnection>(cfg);

// Receive-only video，能力清单
rtc::Description::Video v_desc("video", rtc::Description::Direction::RecvOnly);
v_desc.addH264Codec(96);
v_desc.addH265Codec(97);
v_desc.addRtxCodec(98, 96, 90000);          // RTX 重传 apt=96
auto& v_map96 = v_desc.rtpMaps().at(96);
v_map96.addFeedback("nack");                // NACK
v_map96.addFeedback("nack pli");            // PLI
v_map96.addFeedback("transport-cc");        // 拥塞反馈
// RTP header extensions（URI 字符串显式给）
v_desc.addExtMap(rtc::Description::Entry::ExtMap{
    1, "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"});
v_desc.addExtMap(rtc::Description::Entry::ExtMap{
    2, "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay"});
v_desc.addExtMap(rtc::Description::Entry::ExtMap{
    3, "urn:ietf:params:rtp-hdrext:sdes:mid"});
auto v_track = pc->addTrack(v_desc);

rtc::Description::Audio a_desc("audio", rtc::Description::Direction::RecvOnly);
a_desc.addOpusCodec(111);
auto a_track = pc->addTrack(a_desc);

// ★ libdatachannel 用 onFrame 而非 onMessage，frame 已是去 SRTP 后的明文 RTP
v_track->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
    callbacks_.on_rtp(0,
        reinterpret_cast<const uint8_t*>(data.data()),
        data.size(), callbacks_.user);
});
a_track->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
    callbacks_.on_rtp(1,
        reinterpret_cast<const uint8_t*>(data.data()),
        data.size(), callbacks_.user);
});

pc->setLocalDescription(rtc::Description::Type::Offer);
pc->onLocalDescription([this](rtc::Description desc) {
    whep_http_post(url_, std::string(desc),
        [this](std::string answer, std::string loc) {
            location_ = loc;
            pc->setRemoteDescription(rtc::Description(answer, "answer"));
        });
});
pc->onLocalCandidate([this](rtc::Candidate cand) {
    whep_http_patch(location_, "a=" + std::string(cand) + "\r\n");
});
```

`on_rtp` 回调与 RTSP-UDP 路径完全同形（`int stream, uint8_t* pkt, size_t len`）。controller.c 的 `ts_on_rtp` 处理函数无需修改。

#### 5.2.5 Receive-only offer SDP

```
v=0
o=mc-player 0 0 IN IP4 0.0.0.0
s=-
t=0 0
a=group:BUNDLE 0 1
m=video 9 UDP/TLS/RTP/SAVPF 96 97 98
c=IN IP4 0.0.0.0
a=mid:0
a=recvonly
a=rtcp-mux
a=rtcp-rsize
a=rtpmap:96 H264/90000
a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f
a=rtcp-fb:96 nack
a=rtcp-fb:96 nack pli
a=rtcp-fb:96 transport-cc
a=rtpmap:97 H265/90000
a=fmtp:97 ...
a=rtpmap:98 rtx/90000
a=fmtp:98 apt=96
a=extmap:1 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time
a=extmap:2 urn:ietf:params:rtp-hdrext:playout-delay
a=extmap:3 urn:ietf:params:rtp-hdrext:sdes:mid
m=audio 9 UDP/TLS/RTP/SAVPF 111
c=IN IP4 0.0.0.0
a=mid:1
a=recvonly
a=rtcp-mux
a=rtpmap:111 opus/48000/2
a=fmtp:111 minptime=10;useinbandfec=1
```

SDP 符合 RFC 9429 (JSEP) + RFC 9143 (BUNDLE) + RFC 4585 (AVPF)。`profile-level-id=42e01f` = H.264 Constrained Baseline Level 3.1，最大互通性。

#### 5.2.6 codec 路由

server answer SDP 提取最终选定 PT；按 codec 路由：
- H.264 packetization-mode=1 → `depack_h264`
- H.265 → `depack_h265`
- Opus → libopus 路径（`codec_opus.cpp`）
- RTX → libdatachannel 内部处理重传，应用层透明

#### 5.2.7 RTP header extension

libdatachannel 透传 RFC 8285 ext header；mc-player 的 `mc_rtp_parse`（`protocol/rtp.c`）已支持。Transport-CC 接收侧不实施 GCC（纯接收方），但保留 SR/RR 反馈循环让对端做拥塞控制。abs-send-time 喂入 jitter buffer 的 Kalman estimator 提升精度；mid extension 用于 m-line 路由（更标准，避免依赖 a=ssrc）。

#### 5.2.8 关键风险与缓解

| 风险 | 缓解 |
|---|---|
| DTLS 握手边缘 case（部分 SFU race condition） | pin libdatachannel 0.24.x；CI 烟测每次 release |
| ICE 失败（NAT 严格） | 指数退避重试；fallback TURN（用户配置） |
| WHEP draft 演进 | 跟踪 `draft-ietf-wish-whep`；保留 PATCH 兜底 |
| 服务端不发 a=ssrc | mid extension 路由 + 首 RTP 包自学习 SSRC 兜底 |
| TLS 后端选择 | libdatachannel 三选一（OpenSSL/GnuTLS/MbedTLS）；Windows 桌面默认 OpenSSL（AES-NI 性能优） |

### 5.3 Adaptive Jitter Buffer

#### 5.3.1 视频：Kalman 抖动估计

参考 Chromium WebRTC `modules/video_coding/jitter_estimator.cc`：

```
state x = [theta, var]   theta 为「网络抖动 + 帧大小到延时的线性系数」
观测 z   = frame_delay - frame_size_delta * theta_hat
更新 K   = (P + Q) / (P + Q + R)
        x_new = x + K * residual
        P_new = (1 - K) * (P + Q)
target_delay = max(jitter_estimate, min_target) + max_decode_time
```

帧大小归一化降低突发 I 帧对 target_delay 的扰动。WHEP 路径在 `frame_delay` 计算时优先用 abs-send-time 头扩展给的 server 端发送时间戳，进一步降噪。

#### 5.3.2 音频：iat 直方图 + NetEQ 三相位

参考 Chromium WebRTC `modules/audio_coding/neteq/{delay_manager, accelerate, preemptive_expand, expand}.cc`：

- 直方图：500 bins × 1 ms，指数遗忘因子 0.996
- target_delay 取直方图 P95 + 一次平滑
- 三相位：buffer 高于 target+30 ms → Accelerate（WSOLA 压缩 5–10%）；低于 target−10 ms → Preemptive Expand（拉伸）；丢包 → Normal Expand（PLC）
- 时钟跳变检测（NTP 校时引发）：相邻 RTP 时间戳差 > 5 s → reset 估计参数

#### 5.3.3 NACK 调度

按 Chromium `modules/video_coding/nack_module2.cc` 模型：
- 收到 RTP 包，比对 sequence number 期望值
- 缺失 → 加入 nack_list，调度 t = now + max(rtt/2, 10ms)
- 重发上限 10 次或超时 1s → 放弃 + PLI

### 5.4 RTCP Feedback

```c
typedef struct mc_rtcp_tx_shim {
    int (*tx)(int stream, const uint8_t* pkt, size_t len, void* user);
    void* user;
} mc_rtcp_tx_shim;

mc_rtcp_fb_t* mc_rtcp_fb_create(const mc_rtcp_tx_shim* shim);
```

- RTSP 路径：shim->tx 调用 `mc_transport_send_rtcp` → own UDP socket
- WHEP 路径：shim->tx 调用 libdatachannel `Track::sendMessage` → SRTCP 加密发送

compound packet 构造（SR/RR + SDES + RTPFB-NACK + PSFB-PLI），AVPF immediate（`trr_int=0` 时立即发反馈，不积压），SR 接收并喂入 A/V 同步的 NTP-RTP 线性回归。

### 5.5 视频 Depacketizer

#### 5.5.1 H.264（RFC 6184）

| NAL Unit Type | 含义 | 处理 |
|---|---|---|
| 1-23 | 单 NAL | 直接送解码 |
| 24 STAP-A | 聚合 | 拆包 |
| 28 FU-A | 分片 | S/E 重组，NAL header 重建 = `(FU_ind & 0xE0) \| (FU_hdr & 0x1F)` |
| 5 IDR / 7 SPS / 8 PPS | 关键 | 缓存 extradata（带外 SPS/PPS 时） |
| 6 SEI | 补充 | 透传（含 recovery_point GDR） |

帧边界识别：RTP timestamp 变化 或 marker bit。

#### 5.5.2 H.265（RFC 7798）

| NAL Unit Type | 含义 | 处理 |
|---|---|---|
| 0-31 | VCL 单 NAL | 直接送解码 |
| 32 VPS | Video Parameter Set | 缓存 extradata |
| 33 SPS / 34 PPS | | 缓存 extradata |
| 35-39 SEI | | 透传 |
| 48 AP | Aggregation Packet | 拆包 |
| 49 FU | Fragmentation Unit | S/E 重组（NAL header 2 字节） |
| 50 PACI | PACI 包载荷扩展 | 暂不支持 |

**FU 重组（H.265 vs H.264 的差异）**：

```c
// H.265 NAL header (2 bytes): F(1) Type(6) LayerId(6) TID(3)
// FU header (1 byte):         S(1) E(1) Type(6)
//
// 重建的 NAL header
//   byte 0 = (FU 包 byte 0 & 0x81) | ((FU header & 0x3F) << 1)
//   byte 1 = FU 包 byte 1（LayerId+TID 不变）
```

#### 5.5.3 SEI 与 GDR 透传

SEI 不丢；recovery_point 标记的 GDR 帧识别（用于不发 IDR 的低延时编码器场景）。

#### 5.5.4 丢包处理

非参考帧（nal_ref_idc=0 或 H.265 temporal_id≥1）丢失 → 静默丢；参考帧丢失 → freeze + PLI。

### 5.6 视频 Codec Bridge — MFT 硬解（主路径）

#### 5.6.1 模块拓扑

```
src/media/codec/
  codec_iface.h               IMcVideoCodec / IMcAudioCodec 抽象（C++ 内部）
  codec_runtime.cpp           MFStartup / COM apartment / MFTEnumEx 一次性初始化
  codec_dxgi_mgr.cpp          IMFDXGIDeviceManager + ResetDevice + 多线程保护
  codec_sps_parser.c          最小 SPS/VPS Exp-Golomb 解析
  mft/
    codec_h264_mft.cpp        CLSID_CMSH264DecoderMFT 包装（async）
    codec_h265_mft.cpp        CLSID_CMSH265DecoderMFT 包装（async）
    codec_av1_mft.cpp         MFTEnumEx + MFVideoFormat_AV1（无固定 CLSID 文档）
    codec_aac_mft.cpp         CLSID_CMSAACDecMFT 包装
  sw/
    codec_h264_sw.cpp         mc-libcodec 子项目薄包装（H.264 路径）
    codec_h265_sw.cpp         mc-libcodec 子项目薄包装（H.265 路径）
  audio/
    codec_opus.cpp            libopus 包装（WHEP 音频通路）
```

`src/media/decode/decode.c` 与 `decode_audio.c` 保留为 C shim（`extern "C"` 薄层），让 `controller.c` 接口不变。

#### 5.6.2 MFT 探测与激活

```cpp
static IMFTransform* enum_decoder_mft(REFGUID subtype) {
    MFT_REGISTER_TYPE_INFO in_info = { MFMediaType_Video, subtype };
    IMFActivate** acts = NULL;
    UINT32 n = 0;

    // ★ 硬件 MFT 永远是 ASYNC；同时枚举 SYNC 作为软解 fallback
    UINT32 flags = MFT_ENUM_FLAG_HARDWARE
                 | MFT_ENUM_FLAG_ASYNCMFT
                 | MFT_ENUM_FLAG_SYNCMFT
                 | MFT_ENUM_FLAG_SORTANDFILTER;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags,
                           &in_info, NULL, &acts, &n);
    if (FAILED(hr) || n == 0) return NULL;

    IMFTransform* mft = NULL;
    acts[0]->ActivateObject(IID_PPV_ARGS(&mft));
    for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    return mft;
}
```

AV1 子类型 GUID `MFVideoFormat_AV1` = `{31435641-0000-0010-8000-00AA00389B71}`（FCC `AV1`）。

#### 5.6.3 异步驱动模型

**硬件 MFT 永远是 async**——通过 `IMFMediaEventGenerator` 推送 `METransformNeedInput` / `METransformHaveOutput` 事件，必须用事件驱动。

激活流程：

```cpp
// 1. 检测 async 并解锁
ComPtr<IMFAttributes> attrs;
mft->GetAttributes(&attrs);

UINT32 is_async = 0;
attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
if (is_async) {
    attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
}

// 2. 检查 D3D11 能力
UINT32 d3d11_aware = 0;
attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_aware);
if (!d3d11_aware) return E_FAIL;

// 3. 绑定 D3D11 device manager
mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                    reinterpret_cast<ULONG_PTR>(g_dev_mgr.Get()));

// 4. 设 input/output type（含颜色元数据 MF_MT_VIDEO_NOMINAL_RANGE / YUV_MATRIX / TRANSFER_FUNCTION）
mft->SetInputType(0, in_type.Get(), 0);
mft->SetOutputType(0, out_type.Get(), 0);

// 5. 设 output stream attribute（dual-bind + surface pool）
ComPtr<IMFAttributes> out_attrs;
mft->GetOutputStreamAttributes(0, &out_attrs);
out_attrs->SetUINT32(MF_SA_D3D11_BINDFLAGS,
                     D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE);
out_attrs->SetUINT32(MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT_PROGRESSIVE, 4);

// 6. 拿事件生成器
ComPtr<IMFMediaEventGenerator> gen;
mft->QueryInterface(IID_PPV_ARGS(&gen));

// 7. 启动
mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
gen->BeginGetEvent(&event_callback, NULL);
```

事件循环：

```cpp
HRESULT EventCallback::Invoke(IMFAsyncResult* res) {
    ComPtr<IMFMediaEvent> ev;
    gen_->EndGetEvent(res, &ev);
    MediaEventType type;
    ev->GetType(&type);

    switch (type) {
    case METransformNeedInput:
        feed_one_au_from_jitter_buffer();   // ProcessInput
        break;
    case METransformHaveOutput:
        pump_one_output();                  // ProcessOutput
        break;
    case METransformDrainComplete:
        on_flush_done();
        break;
    }
    gen_->BeginGetEvent(this, NULL);        // re-arm
    return S_OK;
}
```

#### 5.6.4 DXGI 设备管理器

进程内单例 `IMFDXGIDeviceManager`，attach 到渲染器的 `ID3D11Device`：

```cpp
ComPtr<IMFDXGIDeviceManager> g_dev_mgr;
UINT g_dev_token = 0;

HRESULT bind_dev_mgr(ID3D11Device* dev) {
    HR(MFCreateDXGIDeviceManager(&g_dev_token, &g_dev_mgr));
    HR(g_dev_mgr->ResetDevice(dev, g_dev_token));

    // 多线程保护：MFT 解码线程与 renderer 线程共享 device
    ComPtr<ID3D10Multithread> mt;
    dev->QueryInterface(IID_PPV_ARGS(&mt));
    mt->SetMultithreadProtected(TRUE);
    return S_OK;
}
```

renderer 的 `mc_d3d11_get_device()` 返回的 `ID3D11Device*` 直接喂给 `bind_dev_mgr`。同 device → 解码 texture 与渲染 texture 共生命周期，零跨 device 拷贝。

每帧通过 `IMFDXGIDeviceManager::TestDevice` 检测 TDR；失败时上交 controller 走 Device Lost 全恢复（§7.3.2）。

#### 5.6.5 Annex-B 输入路径

depack 输出 Annex-B（`0x00000001 | NAL ...`），与 `MFVideoFormat_H264` 输入格式逐字节匹配：

```cpp
HRESULT submit(const uint8_t* annexb, size_t len, uint32_t pts_90k, bool key) {
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buf;
    HR(MFCreateMemoryBuffer((DWORD)len, &buf));
    BYTE* dst = NULL;
    HR(buf->Lock(&dst, NULL, NULL));
    memcpy(dst, annexb, len);                  // 1 memcpy
    HR(buf->Unlock());
    HR(buf->SetCurrentLength((DWORD)len));
    HR(MFCreateSample(&sample));
    HR(sample->AddBuffer(buf.Get()));
    LONGLONG ts100ns = (LONGLONG)pts_90k * 10000000LL / 90000LL;   // 100-ns
    HR(sample->SetSampleTime(ts100ns));
    if (key) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    return mft_->ProcessInput(0, sample.Get(), 0);
}
```

#### 5.6.6 输出提取（零拷贝路径）

```cpp
HRESULT pump_output() {
    MFT_OUTPUT_DATA_BUFFER out{};
    DWORD status = 0;
    out.dwStreamID = 0;
    // 硬件 MFT 自分配输出 sample，不要预分配
    HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return hr;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) return on_stream_change();
    if (FAILED(hr)) return hr;

    ComPtr<IMFSample> s; s.Attach(out.pSample);
    ComPtr<IMFMediaBuffer> b;
    HR(s->GetBufferByIndex(0, &b));

    ComPtr<IMFDXGIBuffer> dxgi;
    if (SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
        ComPtr<ID3D11Texture2D> tex;
        UINT subres = 0;
        HR(dxgi->GetResource(IID_PPV_ARGS(&tex)));
        HR(dxgi->GetSubresourceIndex(&subres));
        // SRV 必须用 TEXTURE2DARRAY + FirstArraySlice
        emit_d3d11_frame(tex.Get(), subres, /* pts, size, color, range */);
    } else {
        // 软件 MFT 回退：buffer 在 RAM
        emit_cpu_frame(b.Get(), /* ... */);
    }
    return S_OK;
}
```

#### 5.6.7 低延时配置

| 参数 | 设置方法 | 值 / 类型 | 作用 |
|---|---|---|---|
| `CODECAPI_AVLowLatencyMode` (H.264) | `IMFAttributes::SetUINT32` | `1` (VT_UI4) | 禁用 reorder buffer。**H.264 decoder 实现强制 VT_UI4，不要用 VT_BOOL** |
| `CODECAPI_AVLowLatencyMode`（其他 codec） | `ICodecAPI::SetValue` | `VT_BOOL TRUE` | 同上 |
| `MF_LOW_LATENCY` | **SourceReader/MediaSession 创建属性** | `TRUE` | session 级低延时；设在 MFT 上无效 |
| `MF_TRANSFORM_ASYNC_UNLOCK` | MFT GetAttributes 设置 | `TRUE` | 解锁 async MFT，**硬件 MFT 必发** |
| `MF_SA_D3D11_AWARE` | MFT GetAttributes 读 | 必须 `TRUE` | sanity check 硬件路径 |
| `MF_SA_D3D11_BINDFLAGS` | output stream attribute | `D3D11_BIND_DECODER \| D3D11_BIND_SHADER_RESOURCE` | dual-bind 真正生效的关键 |
| `MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT_PROGRESSIVE` | output stream attribute | 4 | 限制 surface pool（默认可达 32） |
| `CODECAPI_AVDecVideoAcceleration_H264/H265` | ICodecAPI | 1 | 显式启用 DXVA，防驱动回退软解 |
| `CODECAPI_AVDecVideoThumbnailGenerationMode` | ICodecAPI | 0 | 关闭缩略图模式 |
| `CODECAPI_AVDecVideoMaxCodedWidth/Height` | ICodecAPI | 流分辨率 | 预声明，防 driver 重新分配 |
| `CODECAPI_AVDecNumWorkerThreads` | ICodecAPI（**仅软解 fallback 时**） | 1 | 硬件 MFT 通常忽略此项 |
| `MFT_MESSAGE_NOTIFY_BEGIN_STREAMING` | ProcessMessage | — | type 设置后必发 |
| `MFT_MESSAGE_NOTIFY_START_OF_STREAM` | ProcessMessage | — | 第一帧前必发 |
| `MFSampleExtension_CleanPoint` | sample attribute | TRUE on IDR | 准确控制 IDR 起播 |

颜色元数据（不设会被渲染端误判 BT.601/709）：

| Attribute | 值 |
|---|---|
| `MF_MT_VIDEO_NOMINAL_RANGE` | `MFNominalRange_16_235` 或 `_0_255` |
| `MF_MT_YUV_MATRIX` | `MFVideoTransferMatrix_BT709` / `BT601` / `BT2020_10` |
| `MF_MT_TRANSFER_FUNCTION` | `MFVideoTransFunc_709` / `2084`(HDR PQ) |

#### 5.6.8 B 帧检测（自实现 SPS 解析）

1. **优先（零成本，SDP）**：解析 `profile-level-id` 三字节 = profile_idc + constraint_flags + level_idc。
   - profile_idc=66 且 constraint_set1=1 ⇒ Constrained Baseline ⇒ 必无 B 帧
   - profile_idc=66 且 constraint_set1=0 ⇒ Baseline ⇒ 必无 B 帧
   - profile_idc=100 且 constraint_set4/5=1 ⇒ Constrained High ⇒ 必无 B 帧
   - **profile_idc=77 (Main) 支持 B 帧**——仍需查 SPS
2. **兜底（in-band）**：`codec_sps_parser.c` 解析 `max_num_ref_frames` 与 VUI `bitstream_restriction_flag`+`max_num_reorder_frames`（H.264）、`general_profile_idc` + `sps_max_num_reorder_pics`（H.265）。`max_num_reorder_frames=0` 才确切无 B 帧/重排。

实现：最小 Exp-Golomb decoder（无依赖纯 C），剥离 `emulation_prevention_three_byte` (0x03)，扫到 VUI 后立即退出，约 80 LOC（H.264）+ 120 LOC（H.265）。

#### 5.6.9 错误恢复状态机

```
ProcessOutput 返回值 / 事件
├─ S_OK                            → emit frame；再 BeginGetEvent
├─ MF_E_TRANSFORM_NEED_MORE_INPUT  → idle；等下个 NeedInput
├─ MF_E_TRANSFORM_STREAM_CHANGE    → SetOutputType again（分辨率切换）
├─ MF_E_INVALIDREQUEST             → bug；assert 报警
├─ DXGI_ERROR_DEVICE_REMOVED       → 上交 Device Lost 全恢复（§7.3.2）
├─ E_OUTOFMEMORY                   → flush + 重试一次；二次失败上抛
├─ TestDevice 失败（TDR）          → 同 DEVICE_REMOVED 路径
└─ 其他 FAILED                     → flush + arm_keyframe_gate + PLI
```

#### 5.6.10 软解 fallback 链

```
1. IDXGIAdapter::CheckVideoDecoderFormat 探测当前 codec/profile
2. 探测通过 → 走 MFT 硬解（路径 A）
3. 探测失败 / MFT init 失败 → 走 mc-libcodec 软解（路径 B），见 §5.7
4. mc-libcodec 也不支持该 profile → MC_ERR_DEC_PROFILE_UNSUPPORTED
5. 用户显式 cfg.disable_swdec=1 → 跳过路径 B 直接 MC_ERR_DEC_NO_HARDWARE
```

软解输出 NV12 缓冲拷贝到 D3D11 dynamic texture（`UpdateSubresource`，~0.5 ms 1080p）后走与硬解完全一致的渲染路径。`mc_decoded_frame_t.fmt` 字段区分 `MC_DECODED_FMT_NV12_D3D11` vs `MC_DECODED_FMT_NV12_CPU`。

#### 5.6.11 AV1 / H.266 路线

| Codec | OS MFT 支持 | 硬件支持（2026） | 当前状态 |
|---|---|---|---|
| AV1 | Win11 内置 + Win10 Microsoft Store「AV1 Video Extension」 | Intel Tiger Lake 11 代+（首个）、AMD RDNA2+、NVIDIA Ampere+ | `codec_av1_mft.cpp` stub；通过 `MFTEnumEx + MFVideoFormat_AV1` 枚举（无固定 CLSID 文档）；M7 启用 |
| H.266/VVC | 暂无 | Intel Lunar Lake LP 试探性 | 待 OS 支持；mc-libcodec 子项目可扩展 |

### 5.7 mc-libcodec 子项目

#### 5.7.1 子项目定位

- 目录布局：`subprojects/mc-libcodec/`，与 `mc-player/` 平级
- 独立 CMake target：`add_subdirectory(subprojects/mc-libcodec)` 产出 `mc_codec` 静态库；mc-player 通过 `target_link_libraries(mc_player_core PRIVATE mc_codec)` 链接
- 独立可复用：API 在 `subprojects/mc-libcodec/include/mc_codec.h`；mc-libcodec 不反向依赖 mc-player；可独立用于 mc-encoder、转码工具、SaaS 后端
- 独立测试套件：`subprojects/mc-libcodec/test/conformance/` 跑 ITU-T 官方 H.264/H.265 reference 流；CI 100% 通过
- 独立 README + LICENSE：Apache 2.0（含明确专利授权条款，比 BSD-2 OpenH264 风格对源码使用者更友好——见 §11.5）

#### 5.7.2 公开 API（`mc_codec.h`）

```c
typedef struct mc_codec_decoder mc_codec_decoder_t;

typedef enum { MC_CODEC_H264 = 1, MC_CODEC_H265 = 2 } mc_codec_kind_t;

#define MC_CODEC_API_VERSION 0x00030000u

typedef struct mc_codec_cfg {
    size_t          struct_size;     /* = sizeof(mc_codec_cfg) */
    uint32_t        version;         /* MC_CODEC_API_VERSION */
    mc_codec_kind_t kind;
    const uint8_t*  extradata;       /* Annex-B SPS+PPS+(VPS for H.265) */
    size_t          extradata_len;
    int             max_threads;     /* 0 = single-thread (lowest latency) */
    int             enable_simd;     /* 1 = runtime CPUID detect & dispatch */
} mc_codec_cfg;

typedef struct mc_codec_frame {
    uint32_t  width, height;
    uint32_t  pts_90k;
    const uint8_t* y;  size_t y_stride;
    const uint8_t* uv; size_t uv_stride;       /* NV12 packed UV plane */
    bool      keyframe;
    int       color_matrix;                    /* 1=BT.709, 5/6=BT.601, 9=BT.2020 */
    int       color_range_full;
    uint32_t  decode_error_flags;
} mc_codec_frame;

mc_codec_decoder_t* mc_codec_open(const mc_codec_cfg* cfg);
void                mc_codec_close(mc_codec_decoder_t* dec);

/* 投递一个 Annex-B AU（可包含多个 NAL）。返回 0 = OK，<0 = 错误。 */
int  mc_codec_submit(mc_codec_decoder_t* dec,
                     const uint8_t* annexb, size_t len,
                     uint32_t pts_90k);

/* 拉取下一帧。返回 1 = 有帧，0 = 需更多输入，<0 = 错误。 */
int  mc_codec_pull  (mc_codec_decoder_t* dec, mc_codec_frame* out);

void mc_codec_flush (mc_codec_decoder_t* dec);
const char* mc_codec_strerror(int err);
```

ABI 契约：首字段 `struct_size` + `version`，后续可加字段不破坏旧调用方。

#### 5.7.3 内部模块拓扑

参考 OpenH264 / libde265 / FFmpeg 公开架构（不复制代码）：

```
subprojects/mc-libcodec/
  include/mc_codec.h            公开 API
  src/
    common/
      bitstream_reader.c        Exp-Golomb + 比特读取器
      cabac_engine.c            CABAC 解码引擎（H.264/H.265 共享上下文模型）
      neighborhood.c            neighbor sample / MV 上下文管理
      reference_list.c          RefPicList 构造逻辑
      arith_dispatch.c          启动期 CPUID 探测 + SIMD 函数指针表填充
    h264/
      h264_nal.c                NAL parser + RBSP unescape (0x03 trap)
      h264_sps.c                SPS 解析（Profile/Level/Chroma/VUI）
      h264_pps.c                PPS 解析
      h264_slice.c              slice header + slice data
      h264_cabac.c              H.264 CABAC binarization 表与 syntax element
      h264_intra.c              4×4 / 8×8 / 16×16 intra prediction（9 方向）
      h264_inter.c              motion compensation（含 1/4 像素 sub-pel 插值）
      h264_idct.c               IDCT 4×4 / 8×8
      h264_deblock.c            in-loop deblocking
      h264_dpb.c                DPB + reorder buffer + POC
      h264_decoder.c            pipeline 编排
    h265/
      hevc_nal.c                NAL parser（2 字节 NAL header）
      hevc_vps.c, hevc_sps.c, hevc_pps.c
      hevc_slice.c
      hevc_cabac.c              H.265 CABAC（与 H.264 不同的上下文模型）
      hevc_intra.c              35 个 intra 方向（vs H.264 的 9 个）
      hevc_inter.c              advanced motion estimation（merge/AMVP/MVD）
      hevc_transform.c          IDCT 4×4 / 8×8 / 16×16 / 32×32 + DST 4×4
      hevc_deblock.c
      hevc_sao.c                Sample Adaptive Offset（H.265 独有 in-loop）
      hevc_dpb.c
      hevc_decoder.c
    simd/
      x86/
        intra_sse2.c, intra_avx2.c
        inter_sse2.c, inter_avx2.c
        idct_sse2.c, idct_avx2.c
        deblock_sse2.c
        sao_avx2.c
      scalar/                    纯 C 兜底（debug + 未带 SSE2 的 CI 机）
  test/
    conformance/
      h264/                      JVT Allegro 公开套件
      h265/                      JCTVC HM 16.x reference 套件
    perf/
    fuzz/                        libFuzzer 集成
  CMakeLists.txt
  README.md
  LICENSE                        Apache 2.0
```

#### 5.7.4 Profile 覆盖

| Codec | Profile | 当前 | 备注 |
|---|---|---|---|
| H.264 | Baseline | ✓ | 工业摄像头标配 |
| H.264 | Constrained Baseline | ✓ | RTSP/WHEP 默认 |
| H.264 | Main | ✓ | NVR 主流，含 CABAC、B 帧 |
| H.264 | High | ✓ | 高码率 NVR，含 8×8 transform |
| H.264 | High 10 / 4:2:2 / 4:4:4 | ✗ | 工业不需要，留后续 |
| H.265 | Main | ✓ | 8-bit 4:2:0 |
| H.265 | Main10 | ✓ | 10-bit 4:2:0 |
| H.265 | Main12 / Main 4:4:4 | ✗ | 留后续 |

#### 5.7.5 SIMD 策略

- 基线：纯 C scalar（debug + 兜底）
- SSE2：所有 x86_64 必有（x86_64 ABI 强制 SSE2，无需 CPUID）。覆盖 IDCT、运动补偿插值、deblocking 主路径
- AVX2：runtime CPUID detect（Haswell+，2013 后所有 CPU）。覆盖 H.265 SAO、大块 IDCT (16×16 / 32×32)、批量 4×4 块
- 不做 AVX-512：消费 CPU 覆盖率低（12/13 代 P-core 禁用）+ 降频代价大
- 不做 ARM NEON：当前仅 Windows x64

dispatch 机制（`arith_dispatch.c`）启动期一次性填表：

```c
typedef struct {
    void (*idct_8x8)(int16_t* coeff, uint8_t* dst, int dst_stride);
    void (*mc_luma_qpel)(uint8_t* dst, int dst_stride,
                         const uint8_t* src, int src_stride,
                         int w, int h, int dx, int dy);
    /* ... 数十个核心 kernel */
} mc_codec_funcs;

extern mc_codec_funcs g_funcs;

void mc_codec_init_dispatch(void) {
    int has_avx2 = check_cpuid_avx2();
    g_funcs.idct_8x8 = has_avx2 ? idct_8x8_avx2 :
                                   idct_8x8_sse2;        // SSE2 baseline
    /* ... */
}
```

零 indirect call 开销（函数指针在 hot loop 外只读取一次）。

#### 5.7.6 多线程模型

- 默认 single-thread（max_threads=0）：延时最低、可预测，与超低延时定位一致
- 可选 frame-level threading（max_threads=N）：相邻帧并行解（受 P/B 引用链限制）。代价：+8~20 ms 延时（一帧排序窗口）。仅在 4K 高码率离线兜底场景启用，不用于 RTSP/WHEP 实时路径
- 不做 slice-level / tile-level 并行（复杂度收益比不划算）

#### 5.7.7 性能目标（i5-12500，内部测量）

| 输入 | 单核延时 | CPU 占用 | 备注 |
|---|---|---|---|
| 1080p H.264 Main 8 Mbps | 5~8 ms | ~12% | SIMD 优化后 |
| 1080p H.265 Main 5 Mbps | 7~12 ms | ~18% | |
| 4K H.265 Main 20 Mbps | 25~35 ms（兜底） | ~45% | 单核紧张，frame-thread 可减半 |
| 1080p H.264 single-thread no SIMD | 12~25 ms | ~35% | scalar 路径 / 调试用 |

参考：OpenH264 BSD 公开 benchmark 1080p H.264 单核 5~8 ms（含 SIMD）。我们目标对齐。

#### 5.7.8 ITU-T 合规测试

- H.264：JVT Allegro 公开 reference 流 + JM reference decoder 比对，YUV 像素 bit-exact
- H.265：JCTVC HM 16.x reference 流 + HM 解码器比对，YUV 像素 bit-exact
- CI 每个 PR 必须 100% 通过；失败 case 加 `test/conformance/known_fail.txt` 白名单 + GitHub issue 跟踪
- 整体覆盖率 ≥ 95%

#### 5.7.9 与 mc-player Codec Bridge 的衔接

```cpp
// src/media/codec/sw/codec_h264_sw.cpp
class H264SwDecoder : public IMcVideoCodec {
    mc_codec_decoder_t* dec_ = nullptr;
public:
    HRESULT init(const mc_decoder_cfg_t& cfg) {
        mc_codec_cfg cc{};
        cc.struct_size = sizeof(cc);
        cc.version = MC_CODEC_API_VERSION;
        cc.kind = MC_CODEC_H264;
        cc.extradata = cfg.extradata;
        cc.extradata_len = cfg.extradata_len;
        cc.max_threads = 0;
        cc.enable_simd = 1;
        dec_ = mc_codec_open(&cc);
        return dec_ ? S_OK : E_FAIL;
    }
    HRESULT submit(const uint8_t* annexb, size_t len, uint32_t pts) override {
        return mc_codec_submit(dec_, annexb, len, pts) >= 0 ? S_OK : E_FAIL;
    }
    void pump(std::function<void(const mc_decoded_frame_t&)> emit) override {
        mc_codec_frame f;
        while (mc_codec_pull(dec_, &f) > 0) {
            mc_decoded_frame_t out{};
            out.fmt = MC_DECODED_FMT_NV12_CPU;
            out.width = f.width; out.height = f.height;
            out.pts_90k = f.pts_90k;
            out.keyframe = f.keyframe;
            out.color_matrix = f.color_matrix;
            out.color_range_full = f.color_range_full;
            out.p.cpu.y = f.y;   out.p.cpu.y_stride = f.y_stride;
            out.p.cpu.uv = f.uv; out.p.cpu.uv_stride = f.uv_stride;
            emit(out);
        }
    }
    ~H264SwDecoder() { if (dec_) mc_codec_close(dec_); }
};
```

renderer 收到 `MC_DECODED_FMT_NV12_CPU` 时走 `UpdateSubresource` 路径。

#### 5.7.10 与 OpenH264 / libde265 / x264 的关系

- **不依赖**任何第三方解码库代码，所有实现自研
- **可参考**算法描述：
  - OpenH264（BSD-2，Cisco）：SIMD 实现思路、CABAC engine 优化模式
  - libde265（LGPL-3 / CC0，struktur AG）：H.265 模块拆分、SAO 实现
  - x264（GPL，VideoLAN）：仅作 spec/算法参考；**不读源码**避免 GPL 污染
- 借鉴 ITU-T 官方 reference decoder：JM (H.264) / HM (H.265) 是公开 academic license，可学习其代码结构作为正确性基准
- 开发流程：仅参考公开的 ITU-T H.264/H.265 spec、IEEE/ACM 论文、reference decoder（JM/HM）

### 5.8 音频 Codec Bridge

#### 5.8.1 AAC（MFT 路径）

```cpp
HEAACWAVEINFO wfx{};
wfx.wfx.cbSize = sizeof(HEAACWAVEINFO) - sizeof(WAVEFORMATEX);
wfx.wfx.wFormatTag = WAVE_FORMAT_MPEG_HEAAC;
wfx.wfx.nChannels = sdp_channels;
wfx.wfx.nSamplesPerSec = sdp_sr;
wfx.wfx.wBitsPerSample = 16;
wfx.wPayloadType = 0;                         // 0 = raw AAC（RFC 3640 mode=AAC-hbr）
wfx.wAudioProfileLevelIndication = 0xFE;
wfx.wStructType = 0;

// MF_MT_USER_DATA = HEAACWAVEINFO 中位于 WAVEFORMATEX 之后的 12 字节 + AudioSpecificConfig
std::vector<BYTE> user_data(12 + asc_len);
memcpy(user_data.data(),
       reinterpret_cast<BYTE*>(&wfx) + sizeof(WAVEFORMATEX), 12);
memcpy(user_data.data() + 12, asc, asc_len);

ComPtr<IMFMediaType> mt;
MFCreateMediaType(&mt);
mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
mt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
mt->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, sdp_channels);
mt->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sdp_sr);
mt->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
mt->SetBlob(MF_MT_USER_DATA, user_data.data(), (UINT32)user_data.size());
mft->SetInputType(0, mt.Get(), 0);

// 输出 MFAudioFormat_Float 32-bit interleaved
ComPtr<IMFMediaType> ot;
MFCreateMediaType(&ot);
ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
ot->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
ot->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
ot->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, sdp_channels);
ot->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sdp_sr);
mft->SetOutputType(0, ot.Get(), 0);
```

AudioSpecificConfig 来自 SDP fmtp `config=` 字段。

> 注：12 字节偏移源自 `sizeof(HEAACWAVEINFO) - sizeof(WAVEFORMATEX) = 30 - 18 = 12`，分别为 `wPayloadType(2) + wAudioProfileLevelIndication(2) + wStructType(2) + wReserved1(2) + dwReserved2(4)`。
> 若 SDP 声明 `mode=mpeg4-generic` 之外的 LATM/LOAS（payload type=3），需相应改 `MF_MT_AAC_PAYLOAD_TYPE`。

#### 5.8.2 Opus（libopus）

OS 无 Opus MFT，用 libopus（BSD-3，~700 KB）：

```cpp
#include <opus.h>

class OpusDecoder : public IMcAudioCodec {
    OpusDecoder* dec_ = nullptr;
    int channels_ = 2;
    int sr_ = 48000;
public:
    HRESULT init(int channels, int sr) {
        int err;
        dec_ = opus_decoder_create(sr, channels, &err);
        return (err == OPUS_OK) ? S_OK : E_FAIL;
    }
    void submit(const uint8_t* opus_pkt, size_t len, uint32_t pts_90k,
                std::function<void(const float*, int frames, int ch)> emit) {
        float pcm[5760 * 2];   // 120 ms @ 48 kHz stereo max
        int frames = opus_decode_float(dec_, opus_pkt, (opus_int32)len,
                                        pcm, 5760, 0);
        if (frames > 0) emit(pcm, frames, channels_);
    }
};
```

输出格式与 AAC 路径一致（float interleaved）。WASAPI 入口不变。

#### 5.8.3 G.711 a-law / μ-law

纯查表，~50 LOC 自实现：

```c
static const float g711_alaw_lut[256];   // 静态预生成
static const float g711_ulaw_lut[256];

void g711_alaw_decode(const uint8_t* in, int len, float* out_pcm) {
    for (int i = 0; i < len; ++i) out_pcm[i] = g711_alaw_lut[in[i]];
}
```

无第三方依赖，~10 ns/sample。

### 5.9 A/V 同步

#### 5.9.1 主时钟选择

WASAPI master clock：`IAudioClock::GetPosition` 输出 `position`（设备播放过的样本数）+ `qpc`（同时刻 QPC 时钟）。

#### 5.9.2 NTP-RTP 线性回归

收到 RTCP SR 时入采样：`<ntp_msw, ntp_lsw, rtp_ts>`。维护最近 32 个样本的滑动窗口，最小二乘拟合 `ntp = a * rtp_ts + b`，得到 RTP timestamp 到 NTP 的映射。

WHEP 模式下 SR 由 libdatachannel 透传给 mc-player；NTP-RTP 拟合算法不需要修改。

#### 5.9.3 渲染调度（ITU-R BT.1359 阈值）

根据 BT.1359-1 主观感知：
- 视频领先音频：人眼可容忍 +90 ms 才察觉，但保守用 +15 ms 上限
- 视频滞后音频：仅 −185 ms 才察觉，但保守用 −25 ms 下限

实际策略：
- `−25 ms ≤ av_offset ≤ +15 ms`：立即渲染
- `av_offset > +15 ms`（视频领先太多）：延迟渲染该帧
- `av_offset < −25 ms`（视频落后太多）：丢该帧

#### 5.9.4 freeze last frame

10 秒无新视频帧 → 发出 `MC_EVENT_DISCONNECTED`，画面保持最后一帧 + 状态图标。

### 5.10 视频渲染

#### 5.10.1 智能 render profile 选择

启动时探测能力，自动选最佳档：

```cpp
mc_render_profile_t pick_profile(IDXGIFactory5* factory5,
                                  IDXGIOutput6* output6) {
    // 1. ALLOW_TEARING（同时是 VRR 应用层判定的唯一权威 API）
    BOOL allow_tearing = FALSE;
    factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                  &allow_tearing, sizeof(BOOL));

    // 2. Hardware composition / DirectFlip / MPO 探测
    UINT comp_flags = 0;
    output6->CheckHardwareCompositionSupport(&comp_flags);
    bool fullscreen_iflip = (comp_flags & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_FULLSCREEN);
    bool windowed_iflip   = (comp_flags & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_WINDOWED);

    // 3. DCOMP 可用性
    ComPtr<IDCompositionDevice> probe_dc;
    bool has_dcomp = SUCCEEDED(DCompositionCreateDevice(d3d_dev, IID_PPV_ARGS(&probe_dc)));

    if (has_dcomp && windowed_iflip && allow_tearing)
        return MC_RENDER_ULTIMATE_DCOMP;        // 窗口模式 best-effort iflip
    if (fullscreen_iflip && allow_tearing)
        return MC_RENDER_EXTREME;               // borderless fullscreen iflip
    if (allow_tearing)
        return MC_RENDER_BALANCED;
    return MC_RENDER_COMPAT;
}
```

`MC_RENDER_AUTO`（默认）走上述逻辑；用户显式指定档则跳过探测。

#### 5.10.2 4 档配置

| 档 | Swap Chain | Present | 预期延时 | 适用 |
|---|---|---|---|---|
| **COMPAT** | FLIP_DISCARD + Waitable + MaxLatency=1 | `Present(1, 0)` | 1~2 帧 | 老 GPU / 非 flip model |
| **BALANCED**（默认） | COMPAT + ALLOW_TEARING flag | `Present(0, ALLOW_TEARING)` | 0~1 帧 | Win10+ + 现代 GPU |
| **EXTREME** | Borderless fullscreen + 像素格式匹配 + 屏幕覆盖 | `Present(0, ALLOW_TEARING)` | <1 帧（HW Composed Independent Flip） | 全屏 + VRR 显示器 |
| **ULTIMATE_DCOMP** | DCOMP CreateSwapChainForComposition + Visual + ALLOW_TEARING | `Present(0, ALLOW_TEARING)` + `DComp::Commit` | best-effort，期望 ~1 帧 | 任意窗口 + DCOMP + VRR |

> 关于 ULTIMATE_DCOMP：Microsoft 官方文档**未声明** DCOMP swap chain + ALLOW_TEARING 必然进入 Hardware Composed: Independent Flip。它是一条 best-effort 路径——只有当系统授予 hardware overlay plane 时才升级为 iflip，否则落入 Composed: Flip。运行时通过 PresentMon 验证；不要将其当作"必胜"档位。
> 真正的卖点是 DCOMP 多 visual 的 **plane 分离能力**：video visual 走 `Present(0, ALLOW_TEARING)`，HUD visual 走 `Present(1, 0)` vsync，互不撕裂。

#### 5.10.3 ULTIMATE_DCOMP 实现

```cpp
ComPtr<IDCompositionDevice> dc;
DCompositionCreateDevice(d3d11_dev.Get(), IID_PPV_ARGS(&dc));

DXGI_SWAP_CHAIN_DESC1 sd{};
sd.BufferCount = 2;
sd.Width = w; sd.Height = h;
sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
sd.SampleDesc = {1, 0};
sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
         | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
factory2->CreateSwapChainForComposition(d3d11_dev.Get(), &sd, NULL, &swap);

ComPtr<IDXGISwapChain2> swap2;
swap.As(&swap2);
swap2->SetMaximumFrameLatency(1);
HANDLE h_wait = swap2->GetFrameLatencyWaitableObject();

// Video visual
ComPtr<IDCompositionVisual> v_vis;
dc->CreateVisual(&v_vis);
v_vis->SetContent(swap.Get());

// HUD visual（独立 swap chain，走 vsync）
ComPtr<IDXGISwapChain1> hud_swap;
DXGI_SWAP_CHAIN_DESC1 hud_sd = sd;
hud_sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;   // 无 ALLOW_TEARING
factory2->CreateSwapChainForComposition(d3d11_dev.Get(), &hud_sd, NULL, &hud_swap);
ComPtr<IDCompositionVisual> hud_vis;
dc->CreateVisual(&hud_vis);
hud_vis->SetContent(hud_swap.Get());

ComPtr<IDCompositionTarget> tgt;
dc->CreateTargetForHwnd(host_hwnd, TRUE, &tgt);
tgt->SetRoot(v_vis.Get());
v_vis->AddVisual(hud_vis.Get(), TRUE, NULL);
dc->Commit();

// 视频每帧
WaitForSingleObjectEx(h_wait, 1000, TRUE);
draw_video();
swap->Present(0, DXGI_PRESENT_ALLOW_TEARING);
// HUD 仅在内容变化时绘+Present(1,0)；dc->Commit 仅在 visual 树变化时
```

#### 5.10.4 Waitable Swap Chain（4 档共有）

`SetMaximumFrameLatency(1) + GetFrameLatencyWaitableObject + WaitForSingleObjectEx` 是延时控制核心。`IDXGISwapChain2` 默认 `SetMaximumFrameLatency = 1`（与 `IDXGIDevice1` 默认 3 不同），但仍需显式 wait 否则 Present 队列会膨胀。

注意：`DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` 必须同时在 `CreateSwapChainForXxx`、`ResizeBuffers`、`Present` 三处都体现，否则运行时拒绝。

#### 5.10.5 PresentMon ETW 验证

PresentMon 输出的 PresentMode 共 7 个值：
- `Hardware: Legacy Flip`
- `Hardware: Legacy Copy to front buffer`
- `Hardware: Independent Flip`
- `Composed: Flip`
- `Hardware Composed: Independent Flip`
- `Composed: Copy with GPU GDI`
- `Composed: Copy with CPU GDI`

ULTIMATE_DCOMP 期望 `Hardware Composed: Independent Flip`，允许降级到 `Composed: Flip`（仍正确，只是少 1 帧合成节省）。EXTREME 期望 `Hardware: Independent Flip`。stats `present_mode` 字段实时上报。

### 5.11 音频渲染 — WASAPI

- 模式：默认共享（IAudioClient3）；可选独占（cfg）
- 共享模式 buffer period：`GetSharedModeEnginePeriod` min（典型 2.67~3 ms）
- 回调模式：`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`
- 流类别：`AudioCategory_Media`
- AutoConvert：`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`（设备 mix 转换）
- 共享模式 mix tail ~10 ms 是 OS 固有；独占模式可消除但要求设备格式精确匹配
- 设备切换（拔耳机）：`IMMNotificationClient::OnDefaultDeviceChanged` → tear down → 新设备 → rebuild

### 5.12 色彩空间与 Range

| SPS VUI | 行为 |
|---|---|
| `colour_primaries=1, matrix_coefficients=1, transfer_characteristics=1` | BT.709（默认） |
| `colour_primaries=5, matrix_coefficients=6` | BT.601 |
| `colour_primaries=9, matrix_coefficients=9` | BT.2020 |
| `video_full_range_flag=0` | limited 16-235 |
| `video_full_range_flag=1` | full 0-255 |
| HDR（PQ / HLG） | M7 milestone；预留 transfer 函数表 |

shader 内查表选 YUV→RGB 矩阵；range 由 shader 内 unscale。

### 5.13 平台抽象层（PAL）

| 模块 | 文件 | 职责 |
|---|---|---|
| Clock | `pal/clock.c` | QPC 封装 + ns 单位 |
| SPSC | `pal/spsc.c` | 单生产单消费无锁队列 + cache-line padding |
| Thread | `pal/thread.c` | MMCSS 注册、亲和性、优先级 |
| Socket | `pal/sock.c` | IOCP + WSARecvFrom |
| Log | `pal/log.c` | 多级日志 + ringbuf |
| ETW | `pal/etw.c` | TraceLoggingProvider 包装 |
| **DXGI Caps Probe** | `pal/dxgi_caps.cpp` | DXGI adapter 枚举 + `CheckVideoDecoderFormat` 探测 H.264/H.265/AV1 profile，缓存 caps[] |
| **WinHTTP Wrapper** | `pal/winhttp_wrapper.cpp` | WinHTTP 异步 POST/PATCH/DELETE 客户端，WHEP 信令专用 |
| **MF Runtime** | `pal/mf_runtime.cpp` | `MFStartup` / `CoInitializeEx` 一次性初始化与生命周期 |
| **COM Apartment** | `pal/com_apartment.cpp` | 解码线程 COM apartment 管理（MTA） |
| **Adapter Picker** | `pal/adapter_picker.cpp` | 智能 GPU 选择（HWND 跟随 + 能力兜底，§7.3.4） |

---

## 6. 队列与内存

### 6.1 队列规格

| 队列 | 类型 | 深度 | 满时策略 |
|---|---|---|---|
| RTP→JitterBuf | SPSC ring | 64 | 丢最老 + 报告丢包 |
| JitterBuf→Depack | SPSC | 4 | 丢最老 |
| Depack→Codec | SPSC | 2 | 丢最老非参考帧 |
| Codec→Render | SPSC | 2 | 丢最老 |
| Audio PCM ring | SPSC | 40 ms | 写阻塞 |

**cache-line padding 必做**；否则高频 atomic 操作因 false-sharing 性能降 3-5×。

### 6.2 内存池

| 池 | 容量 | 用途 |
|---|---|---|
| RTP buffer pool | 512 × 2 KB | UDP/SRTP 接收，避免 malloc |
| IMFSample input pool | 32 | MFT 输入 |
| IMFSample output pool | 17+（DPB） | MFT 输出 |
| mc-libcodec NV12 buffer pool | 8 × 1080p NV12 (~3 MB ea) | 软解兜底输出 |
| D3D11 Texture | 硬件 DPB 决定（`MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT_PROGRESSIVE=4`） | MFT/D3D 内部管理 |

---

## 7. 关键策略

### 7.1 超低延时参数全清单

**Windows OS 层**：

| 参数 | 设置 | 备注 |
|---|---|---|
| `timeBeginPeriod(1)` | 进程启动时 | Win11 2004+ 仅影响本进程 |
| `CreateWaitableTimerEx(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)` | 线程级高精度等待 | 精度 ~500 μs |
| 进程优先级 | `HIGH_PRIORITY_CLASS` | |
| 关键线程 MMCSS | Pro Audio / Playback | T2 不挂 |
| CPU 亲和性 | 固定 P-core（T2/T5/T6） | 12 代+ Intel 必做 |
| SystemResponsiveness | 默认 20；可选降至 10 | 注册表；系统级 |

**网络层**：

| 参数 | 设置 | 备注 |
|---|---|---|
| `SO_RCVBUF` | 4 MB | 突发包不丢 |
| `SIO_UDP_CONNRESET` | FALSE | 防 ICMP 端口不可达让 recv 返错 |
| `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` | ON | 同步成功跳过完成端口 |
| UDP Receive Offload (URO) | 建议关闭 | Win11 默认开；合并包增延时 ~2 ms |
| 网卡 Interrupt Moderation | 建议关 / 最低 | 减 100 μs ~ 1 ms |
| `TCP_NODELAY` | 1（RTSP 信令） | 禁 Nagle |

**MFT 解码层**：

| 参数 | 设置方法 | 值 | 作用 |
|---|---|---|---|
| `MF_TRANSFORM_ASYNC_UNLOCK` | MFT GetAttributes | TRUE | 解锁 async MFT，硬件 MFT 必发 |
| `CODECAPI_AVLowLatencyMode` (H.264) | IMFAttributes::SetUINT32 | 1 (VT_UI4) | 禁用 reorder buffer |
| `CODECAPI_AVDecVideoAcceleration_H264/H265` | ICodecAPI | 1 | 显式启用 DXVA |
| `CODECAPI_AVDecVideoThumbnailGenerationMode` | ICodecAPI | 0 | 关闭缩略图模式 |
| `CODECAPI_AVDecVideoMaxCodedWidth/Height` | ICodecAPI | 流分辨率 | 预声明，防 driver 重新分配 |
| `MFTEnumEx flags` | enum | `HARDWARE \| ASYNCMFT \| SYNCMFT \| SORTANDFILTER` | 同时枚举硬件 async + 软件 sync 兜底 |
| `IMFDXGIDeviceManager` | ResetDevice 共享 D3D | 必须 | 与 renderer 同 device |
| `MF_SA_D3D11_BINDFLAGS` | output stream attribute | `DECODER \| SHADER_RESOURCE` | dual-bind 真正生效 |
| `MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT_PROGRESSIVE` | output stream attribute | 4 | 限制 surface pool |
| `ID3D10Multithread::SetMultithreadProtected` | D3D11 device | TRUE | 解码 / 渲染跨线程访问保护 |
| `IMFRealTimeClientEx::SetWorkQueueEx` | MFT QI | MMCSS workqueue | 让 MFT 内部 worker 上 MMCSS 线程 |

**DXGI 渲染层**：

| 参数 | 设置 |
|---|---|
| SwapEffect | FLIP_DISCARD |
| BufferCount | 2 |
| Flags | `FRAME_LATENCY_WAITABLE_OBJECT \| ALLOW_TEARING`（三处一致：Create / ResizeBuffers / Present） |
| MaximumFrameLatency | 1 |
| Present interval | 0 |
| Present flags | ALLOW_TEARING（非 fullscreen exclusive） |
| DCOMP（ULTIMATE_DCOMP 档） | `CreateSwapChainForComposition` + `IDCompositionVisual::SetContent` + `dc->Commit()` |

**WASAPI 音频层**：

| 参数 | 设置 |
|---|---|
| 模式 | 共享（IAudioClient3 默认）/ 独占（cfg） |
| 缓冲周期 | 共享 = `GetSharedModeEnginePeriod` min（2.67~3 ms） |
| 回调模式 | EVENTCALLBACK |
| 流类别 | AudioCategory_Media |
| AutoConvert | `AUTOCONVERTPCM \| SRC_DEFAULT_QUALITY` |

### 7.2 丢帧与补偿

**视频**：
1. 解码前：帧过期（`PTS < audio_clock - 100 ms`）直接丢
2. 解码后：落后 > 40 ms 丢
3. `decode_error_flags` 非 0 → 丢 + freeze
4. 长时间无新帧：Freeze-Last-Frame + 状态图标

**音频（NetEQ 三相位）**：
1. `buffer > target + 30 ms`：Accelerate（WSOLA 压缩 5–10%）
2. `buffer < target − 10 ms`：Preemptive Expand（拉伸）
3. 丢包：Normal Expand（PLC），<60 ms 维持；>200 ms 进 comfort noise + NACK
4. G.711 简化：丢 <10 ms 直接重放上个包

### 7.3 断线与恢复

#### 7.3.1 网络断线

**触发**：
- TCP 信令通道断开
- UDP/SRTP 连续 1 s 无数据
- RTCP SR 连续 10 s 未收到

**恢复**：
```
1. 保留 D3D11 device、Swap Chain、WASAPI、MMCSS 注册
2. 释放 Transport Session
3. 清空 JitterBuf 数据（保留估计参数）
4. 指数退避 + jitter 重连：1s ± 0.5s → 2s ± 1s → ... max 16s
5. 重连成功后：
   - 复用核心管线
   - 首 RTCP SR 到达后重建 NTP-RTP 线性回归
   - 首个可解码帧前保持 Freeze
```

#### 7.3.2 Device Lost 全恢复

```cpp
HRESULT hr = swap_chain->Present(...);  // 或 Acquire/ResizeBuffers/任何 D3D11 调用
if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
    // 1. 销毁 MFT decoder（释放 IMFDXGIDeviceManager 引用）
    // 2. 销毁 swap chain、所有 view / texture 引用
    // 3. ID3D11DeviceContext::ClearState
    // 4. Release D3D11 device
    // 5. ★ DXGI factory 在 DEVICE_REMOVED 后变 stale，必须重建
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory6));
    // 6. 找回同一 adapter by LUID（如可能；否则触发 §7.3.4 智能重选）
    factory6->EnumAdapterByLuid(saved_luid, IID_PPV_ARGS(&adapter));
    // 7. D3D11CreateDevice
    // 8. 重建 IMFDXGIDeviceManager + ResetDevice
    // 9. 重建 MFT 或 mc-libcodec 软解
    // 10. 重建 swap chain
    // 11. 清空 jitter buffer（视频数据依赖旧 device）
    // 12. 发 PLI 请求新 I 帧
    // 13. 事件 MC_EVENT_DEVICE_LOST → MC_EVENT_DEVICE_RECOVERED
}
```

每帧 `IMFDXGIDeviceManager::TestDevice` 周期检测 TDR；任何 D3D11 / DXGI 调用都可能返回 device removed，需统一拦截。

#### 7.3.3 Audio Device Invalidated

`IMMNotificationClient::OnDefaultDeviceChanged` → flush WASAPI ring → tear down `IAudioClient3` → 创建新 device 的 client → 复位 jitter target_delay。

#### 7.3.4 智能 Adapter 选择

**启动期能力快照**：
```
F1. IDXGIFactory6 枚举所有 IDXGIAdapter1
F2. 每个 adapter 探测：
    D3D11CreateDevice → ID3D11VideoDevice
    → CheckVideoDecoderFormat({H264_VLD_NOFGT, HEVC_VLD_MAIN,
                               HEVC_VLD_MAIN10, AV1_VLD_PROFILE0}, NV12/P010)
    → caps[adapter] = { h264_8, hevc_8, hevc_10, av1, max_w, max_h }
F3. 缓存 caps；mc_stats_t.gpu_caps[] 暴露
F4. 触发 MC_EVENT_GPU_CAPS_READY 事件
```

**打开流时（已知目标 codec/profile/分辨率）**：
```
O1. 解析 HWND 所在显示器对应的 adapter（"preferred"）：
    HWND → MonitorFromWindow → IDXGIFactory6::EnumAdapters
       遍历每个 adapter 的每个 output（IDXGIOutput::GetDesc.Monitor）
       匹配 HMONITOR → 找到 owning adapter
O2. if caps[preferred] 满足 codec 需求：
      使用 preferred → 同 device 直 scan-out，零跨 device 拷贝
O3. else 从其他 adapter 中按能力排序，选支持 codec 的最佳：
      dGPU > Iris Xe > 老 iGPU
      跨 device texture 共享通过 IDXGIKeyedMutex（+0.5 ms 一次性代价）
O4. 仍无满足 → 退化到 mc-libcodec 子项目软解（CPU 路径）
O5. 用户显式 mc_player_cfg.adapter_luid != 0 时跳过 O1-O3 直接用指定 LUID
```

**运行期跨屏拖拽（double-buffered transition）**：
```
R1. WM_DISPLAYCHANGE / WM_WINDOWPOSCHANGED 监听
R2. 重新 MonitorFromWindow → 比对 LUID
R3. 如 LUID 变化 → soft adapter switch（保留旧 device 直到新 device 出第一帧）：
      在新 adapter 上预创建 D3D11 device + IMFDXGIDeviceManager
      flush MFT / mc-libcodec
      → recreate decoder on new adapter
      → 单帧 PLI（freeze ≤ 500-800 ms 上限）
      → 新 device 出帧后切换 visual content，原 device 释放
R4. 如 800 ms 内未恢复，fallback 到旧 adapter 继续渲染但 stats 报警告
```

**理由**：
- HWND 跟随是「专业级低延时」的物理一致性最佳：同 device 直 scan-out 无跨 PCIe 拷贝、可触发 Independent Flip
- 能力兜底覆盖多 codec 异构场景：UHD 630 + AV1 流自动跳 dGPU（如有）
- 用户层无感，符合「专业播放器」隐形原则
- 提供 `adapter_luid` 显式覆盖路径以兼容硬性场景需求

#### 7.3.5 HEVC Extension 缺失自动软解兜底

Win10/11 retail 默认无 HEVC Video Extension（需用户从 Store 安装免费 OEM 版或 $0.99 零售版）。检测到 `MFTEnumEx(HEVC)` 返回空时：

```
1. log 信息事件
2. 自动启用 mc-libcodec H.265 软解路径（§5.7）
3. 用户视角：H.265 流照常播放，仅 CPU 占用上升至 ~18%
4. mc_stats_t.active_decoder_kind = "mc-libcodec-SW"
5. UI 可选展示 hint：「检测到 HEVC 硬件加速不可用，使用软解码」
```

---

## 8. 延时预算

### 8.1 客户端内部延时（按阶段，1080p H.264 30 fps）

| 阶段 | MFT 路径 | mc-libcodec SW 路径 |
|---|---|---|
| UDP 接收 + 组帧 | 0.2~3 ms | 0.2~3 ms |
| Adaptive Jitter Buf | 0~20 ms | 0~20 ms |
| Depack | <1 ms | <1 ms |
| 解码 input alloc + memcpy | ~50 μs | ~50 μs |
| DXVA / mc-libcodec 实际解码 | 4~12 ms | 5~10 ms（1080p H.264 含 SIMD） |
| 解码 output 提取 | ~50 μs（IMFDXGIBuffer QI） | ~5 μs |
| NV12→D3D11 拷贝（仅 SW） | n/a | ~0.5 ms（UpdateSubresource） |
| dual-bind 或 CopySub + YUV→RGB | <1~3 ms | <1~3 ms |
| Present 队列 | 0~4 ms（DCOMP 时降至 ~0） | 0~4 ms |
| **客户端总计（典型）** | **17~22 ms** | **18~24 ms** |

### 8.2 端到端（内网，4 档显示器对照）

| 段 | 60 Hz | 144 Hz | 144 Hz VRR + DCOMP | 240 Hz VRR + DCOMP |
|---|---|---|---|---|
| 编码端延时 | 30~80 ms | 30~80 ms | 30~80 ms | 30~80 ms |
| 网络（内网同交换机） | <3 ms | <3 ms | <3 ms | <3 ms |
| 客户端（典型） | 22 ms | 22 ms | 18 ms | 18 ms |
| 显示器 scan-out + 面板 | 8~16 ms | ~7 ms | ~4 ms（VRR） | ~2 ms（240Hz VRR） |
| **端到端典型** | **63~121 ms** | **62~112 ms** | **55~105 ms** | **53~103 ms** |
| **端到端最优** | 63 ms | 62 ms | **35 ms** | **25 ms** |

### 8.3 首帧延时分级

| 场景 | 首帧延时 | 前提 |
|---|---|---|
| 最优 | ≤ 200 ms | sprop-parameter-sets 可用 + PLAY 立即 I 帧 + MFT 初始化 < 50 ms |
| 一般 | ≤ 500 ms | 等下一自然 I 帧（GOP 30） |
| 大 GOP | ≤ 2 s | GOP ≥ 60 + 无带外 SPS + PLI 触发等待 |
| WHEP 首帧（LAN，含握手） | ≤ 600 ms | LAN + STUN：ICE 50–150 ms + DTLS 1.2 ~30 ms（LAN 1.5 RTT）+ 第一 I 帧 ~300 ms |
| WHEP 首帧（公网） | ≤ 1.2 s | 含 NAT 穿透 / TURN relay |

---

## 9. iGPU 验证与能力矩阵（normative）

### 9.1 探测算法

启动时 `IDXGIFactory6` 枚举 `IDXGIAdapter1`，逐个 `D3D11CreateDevice` + `ID3D11VideoDevice::CheckVideoDecoderFormat`：

```c
const GUID profiles[] = {
    D3D11_DECODER_PROFILE_H264_VLD_NOFGT,        // H.264 Baseline/Main/High
    D3D11_DECODER_PROFILE_HEVC_VLD_MAIN,         // H.265 Main 8-bit
    D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10,       // H.265 Main10 10-bit
    D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0       // AV1 Main
};
const DXGI_FORMAT formats[] = {
    DXGI_FORMAT_NV12, DXGI_FORMAT_NV12, DXGI_FORMAT_P010, DXGI_FORMAT_NV12
};

for (int i = 0; i < 4; ++i) {
    BOOL supported = FALSE;
    vdev->CheckVideoDecoderFormat(&profiles[i], formats[i], &supported);
    caps[adapter].profile_support[i] = supported;
}
```

### 9.2 能力矩阵（normative）

| iGPU / dGPU | H.264 1080p | H.264 4K | H.265 8-bit 1080p | H.265 8-bit 4K | H.265 10-bit Main10 | AV1 |
|---|---|---|---|---|---|---|
| Intel UHD 620（KBL，7 代） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | ✗ |
| Intel UHD 630（CFL，8/9 代） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | ✗ |
| Intel UHD 730（ADL，12 代+） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ |
| Intel UHD 770（ADL，12 代+） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ |
| Intel Iris Xe（TGL，11 代）★ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ |
| AMD Vega 8/11（Ryzen APU） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | ✗ |
| AMD Radeon 660M+（Rembrandt） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ |
| NVIDIA GTX 1050（Pascal） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | ✗ |
| NVIDIA RTX 3050+（Ampere+） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ |

> ★ Tiger Lake 11 代 Iris Xe 是 **Intel 首个**支持 AV1 硬解的架构（仅解码、无编码），不是 12 代 Alder Lake。

### 9.3 iGPU 延时对照（与 dGPU，内部实测，含解码 + GPU 同步开销）

| 配置 | 1080p H.264 | 1080p H.265 | 4K H.265 |
|---|---|---|---|
| Intel UHD 630（8/9 代 iGPU） | ~6 ms | ~7 ms | ~14 ms |
| Intel UHD 770（12 代 iGPU） | ~5 ms | ~5 ms | ~9 ms |
| Intel Iris Xe（11 代 iGPU） | ~5 ms | ~5 ms | ~8 ms |
| NVIDIA RTX 3050（dGPU） | ~5 ms | ~5 ms | ~7 ms |
| **mc-libcodec 软解（i5-12500 单核）** | 5~8 ms | 7~12 ms | 25~35 ms（兜底） |

iGPU 延时与 dGPU 相比 ≤ 2 ms 差异；mc-libcodec 软解 1080p 与硬解延时同档（CPU 占用更高），4K 显著退化但仍可兜底。

> 数据为内部测量，无公开 benchmark 等同。

### 9.4 已知边缘 case

- **HEVC Extension 缺失**（Win10/11 retail）→ mc-libcodec H.265 软解
- **AV1 在 UHD 630 / Vega**：硬件不支持，软解延时 > 50 ms，仅作存档兜底
- **多 GPU 系统选错 adapter**：智能选择因极端罕见的 driver bug 选错时，`mc_stats_t.gpu_caps` 暴露真实能力，用户可用 `adapter_luid` 显式覆盖

### 9.5 验证工具

`tools/mft_probe.exe` JSON 输出：

```json
{
  "adapters": [
    { "luid": "0x12345678",
      "desc": "Intel(R) UHD Graphics 770",
      "vendor_id": "0x8086",
      "h264_8bit_nv12":  { "hw": true, "max_w": 4096, "max_h": 2304 },
      "hevc_8bit_nv12":  { "hw": true, "max_w": 8192, "max_h": 4320 },
      "hevc_10bit_p010": { "hw": true, "max_w": 8192, "max_h": 4320 },
      "av1_nv12":        { "hw": true, "max_w": 8192, "max_h": 4320 }
    }
  ],
  "mft_present": { "h264": true, "hevc": true, "av1": true, "aac": true },
  "hevc_extension_installed": true,
  "av1_extension_installed":  true,
  "win_version": "10.0.26100"
}
```

CI 在 UHD 630 / UHD 770 / Iris Xe / Vega 实机回归。

---

## 10. 对外接口（C API）

### 10.1 ABI 与配置

```c
#define MC_API_VERSION 0x00030000u

typedef enum {
    MC_RENDER_AUTO     = 0,           /* 智能选择（默认） */
    MC_RENDER_COMPAT   = 1,
    MC_RENDER_BALANCED = 2,
    MC_RENDER_EXTREME  = 3,
    MC_RENDER_ULTIMATE_DCOMP = 4
} mc_render_profile_t;

typedef struct mc_player_cfg {
    size_t   struct_size;
    uint32_t version;
    const char* user_agent;
    mc_render_profile_t render_profile;
    LUID adapter_luid;                       /* 0 = 智能选择；非 0 = 显式覆盖 */
    bool audio_exclusive_mode;
    mc_log_level_t log_level;
    bool disable_swdec;                      /* 1 = 禁用 mc-libcodec 软解兜底 */
    const char* whep_stun_server;
    const char* whep_turn_server;
    const char* whep_turn_username;
    const char* whep_turn_password;
} mc_player_cfg;
```

### 10.2 错误码

```c
typedef int mc_result_t;
#define MC_OK                          0
/* 网络层 */
#define MC_ERR_NET_CONNECT             -101
#define MC_ERR_NET_TIMEOUT             -102
#define MC_ERR_NET_DNS                 -103
/* 协议层 */
#define MC_ERR_PROTO_RTSP              -201
#define MC_ERR_PROTO_AUTH              -202
#define MC_ERR_PROTO_SDP               -203
#define MC_ERR_PROTO_RTP               -204
#define MC_ERR_TRANSPORT_DTLS          -205
#define MC_ERR_TRANSPORT_ICE           -206
#define MC_ERR_PROTO_WHEP              -207
/* 解码层 */
#define MC_ERR_DEC_INIT                -301
#define MC_ERR_DEC_UNSUPPORTED         -302
#define MC_ERR_DEC_DEVICE_LOST         -303
#define MC_ERR_DEC_NO_HARDWARE         -305
#define MC_ERR_DEC_PROFILE_UNSUPPORTED -306
/* 渲染层 */
#define MC_ERR_RENDER_D3D              -401
#define MC_ERR_RENDER_WASAPI           -402
#define MC_ERR_RENDER_DEVICE_LOST      -403
/* 参数 / 状态 */
#define MC_ERR_ARG                     -501
#define MC_ERR_STATE                   -502
#define MC_ERR_VERSION                 -503

const char* mc_strerror(mc_result_t err);
```

### 10.3 事件

```c
typedef enum {
    MC_EVENT_OPENED, MC_EVENT_FIRST_FRAME, MC_EVENT_DISCONNECTED,
    MC_EVENT_RECONNECTING, MC_EVENT_RECONNECTED,
    MC_EVENT_DEVICE_LOST, MC_EVENT_DEVICE_RECOVERED,
    MC_EVENT_AUDIO_DEVICE_CHANGED, MC_EVENT_ERROR,
    MC_EVENT_STATS_UPDATE, MC_EVENT_STATE_CHANGED,
    MC_EVENT_GPU_CAPS_READY,            /* DXGI Caps Probe 完成 */
    MC_EVENT_TRANSPORT_NEGOTIATED       /* WHEP 完成 SDP O/A */
} mc_event_t;
```

### 10.4 stats

```c
typedef struct {
    /* 延时 */
    uint32_t latency_e2e_ms, latency_jitterbuf_ms, latency_decode_ms, latency_present_ms;
    /* 网络 */
    uint32_t packet_loss_1000, nack_sent_count, pli_sent_count, jitter_ms;
    int32_t  rtt_ms;
    /* 解码 */
    uint32_t frames_decoded, frames_dropped, b_frames_detected;
    /* 渲染 */
    char     present_mode[64];
    uint32_t fps_actual_x100;
    /* A/V */
    int32_t  av_sync_offset_ms;
    /* GPU / decoder identity */
    LUID     active_adapter_luid;
    char     active_decoder_kind[32];   /* "MFT-HW" / "MFT-SW" / "mc-libcodec-SW" */
    char     active_render_profile[32]; /* "ULTIMATE_DCOMP" / "EXTREME" / ... */
    uint32_t gpu_caps_count;
    struct {
        LUID luid; char desc[128];
        bool h264_8bit, hevc_8bit, hevc_10bit, av1;
    } gpu_caps[8];
    /* WebRTC */
    uint32_t webrtc_dtls_handshake_ms;
    uint32_t webrtc_ice_state;          /* 0=new, 1=checking, 2=connected, 3=failed */
    uint32_t webrtc_rtx_received;
} mc_stats_t;
```

### 10.5 API

```c
mc_player_t* mc_player_create(const mc_player_cfg* cfg);
void         mc_player_destroy(mc_player_t* player);
/* URL: rtsp:// / rtsps:// → ts_rtsp_*；whep:// / http(s)://(Content-Type: application/sdp) → ts_whep */
mc_result_t  mc_player_open(mc_player_t* player, const char* url,
                            const mc_credentials_t* cred, HWND hwnd);
mc_result_t  mc_player_pause(mc_player_t* player);
mc_result_t  mc_player_resume(mc_player_t* player);
mc_result_t  mc_player_close(mc_player_t* player);
mc_result_t  mc_player_snapshot(mc_player_t* player, const char* file_path);
void         mc_player_set_callback(mc_player_t* player,
                                    mc_event_callback_t cb, void* user_data);
mc_result_t  mc_player_get_stats(mc_player_t* player, mc_stats_t* out);
```

### 10.6 线程安全契约

- `create / destroy / open / close / pause / resume`：调用方保证单线程
- `get_stats / snapshot`：任意线程
- 事件回调：在 Controller 线程触发，不得在回调内调用同一 player 的阻塞 API

---

## 11. 部署与环境

### 11.1 硬件

| | 最低 | 推荐 | 极致低延时 |
|---|---|---|---|
| CPU | i3 8 代（含 UHD 630） | i5 12 代+（含 UHD 770） | i5 13 代+ + dGPU |
| GPU | UHD 620+ / Vega 8+ | UHD 770 / Iris Xe | RTX 3050+ |
| 内存 | 4 GB | 8 GB | 16 GB |
| 网络 | 100 Mbps | 千兆 | 千兆低抖动 + 关 URO/IM |
| 显示器 | 60 Hz | 144 Hz VRR | 240 Hz VRR + DisplayHDR |

纯 iGPU 最低部署：i3-8100 + UHD 630 + 8 GB + 千兆，可达 1080p H.264 端到端 < 100 ms。

### 11.2 软件

- Win10 1809+：H.264 OS 内置；H.265 取决于 HEVC Extension（缺失则 mc-libcodec 软解）
- Win11 推荐：HEVC、AV1 Extension 通常可一并装；DCOMP 完整支持
- DirectX 11（Feature Level 11.0+）
- VC++ Runtime 2022

### 11.3 依赖

| 库 | License | 用途 | 大小 |
|---|---|---|---|
| Windows Media Foundation | OS | H.264/H.265/AAC 硬解 | 0 |
| mc-libcodec 子项目（自研） | Apache 2.0 | H.264/H.265 全 profile 软解兜底 | ~3 MB |
| libdatachannel 0.24.x | MPL 2.0 | WebRTC PeerConnection | ~1.5 MB |
| libjuice（submodule） | MIT | ICE | bundled |
| libsrtp2 | BSD | SRTP | bundled |
| TLS backend（OpenSSL/GnuTLS/MbedTLS 三选一） | 各自 | DTLS（libdatachannel backend） | ~0.5 MB |
| libopus | BSD-3 | Opus 解码（WHEP 音频） | ~0.7 MB |
| WinHTTP / DirectComposition | OS | WHEP HTTP 信令 / DCOMP 渲染 | 0 |

第三方 ~2.7 MB + 自研 mc-libcodec ~3 MB ≈ 5.7 MB 总体。

### 11.4 目录

```
mc-player-v2/                       (monorepo 根)
├── mc-player/                      主项目
│   ├── include/mc_player.h
│   ├── src/
│   │   ├── controller/
│   │   ├── transport/
│   │   │   ├── ts_rtsp_udp/
│   │   │   ├── ts_rtsp_tcp/
│   │   │   └── ts_whep/
│   │   ├── media/
│   │   │   ├── codec/
│   │   │   │   ├── codec_iface.h, codec_runtime.cpp,
│   │   │   │   │   codec_dxgi_mgr.cpp, codec_sps_parser.c
│   │   │   │   ├── mft/
│   │   │   │   ├── sw/
│   │   │   │   └── audio/
│   │   │   ├── decode/             (C shim)
│   │   │   ├── jitter_buffer/, depack/, render/, sync/, rtcp/
│   │   ├── pal/
│   │   │   ├── (clock/spsc/thread/sock/log/etw)
│   │   │   ├── dxgi_caps.cpp
│   │   │   ├── winhttp_wrapper.cpp
│   │   │   ├── mf_runtime.cpp
│   │   │   ├── com_apartment.cpp
│   │   │   └── adapter_picker.cpp
│   │   ├── protocol/
│   │   └── ui/
│   ├── tools/
│   │   ├── latency_meter/
│   │   ├── mft_probe/
│   │   └── whep_smoke/
│   └── 3rd/
│       ├── libdatachannel/         (git submodule)
│       ├── libopus/
│       └── tls_backend/            (OpenSSL 默认)
├── subprojects/
│   └── mc-libcodec/                (独立子项目)
│       ├── include/mc_codec.h
│       ├── src/{common, h264, h265, simd}
│       ├── test/{conformance, perf, fuzz}
│       ├── CMakeLists.txt
│       ├── README.md
│       └── LICENSE                 (Apache 2.0)
└── CMakeLists.txt                  顶层
```

### 11.5 License 与 SEP 专利责任

| 模块 | License |
|---|---|
| mc-player 主项目 | Apache 2.0 |
| mc-libcodec 子项目 | Apache 2.0（含明确专利授权 §3） |
| Windows Media Foundation / WinHTTP / DirectComposition | OS 自带 |
| libdatachannel | MPL 2.0（文件级 copyleft，与闭源主项目静态链接合法） |
| libjuice / libsrtp2 / libopus | MIT / BSD |
| TLS backend | OpenSSL Apache-2 / GnuTLS LGPL-2.1 / MbedTLS Apache-2（任选） |

**MPEG-LA H.264 / H.265 标准必要专利（SEP）责任**

> 注：MPEG-LA 与 Via Licensing 已于 2023 合并为 **Via LA Licensing**，现行官网 [via-la.com](https://www.via-la.com/)。

| 路径 | 责任承担方 |
|---|---|
| 走 OS MFT 硬解 | Microsoft 已为 Windows 用户支付 license；mc-player 不承担 |
| 走 OS HEVC Extension（OEM 免费 / Store 付费） | 用户 / OEM 与 Microsoft / Via LA 之间；mc-player 不承担 |
| 走 mc-libcodec 软解 distribute | distributor 自负（参考 Cisco OpenH264 模型：源码 Apache 2.0；二进制 distribute 责任移交） |

商业分发场景需评估当前 Via LA AVC / HEVC pool license（年费 ~$0.20/unit、>100k 单位上限）以及 Access Advance、Velos Media 单独 pool 条款。Apache 2.0 §3 含明确专利授权 grant + 反诉终止条款，比 BSD-2 对源码使用者专利风险更低（BSD-2 不含专利条款，使用者面临隐式专利风险）。

mc-libcodec 子项目 README 重复声明此风险，参考 Cisco OpenH264 source distribution 免责模板。

---

## 12. 风险与对策

| 风险 | 概率 | 影响 | 对策 |
|---|---|---|---|
| 编码端发 B 帧 | 中 | +30 ms 延时 | SPS 解析检测 + stats |
| NVR 不支持 NACK | 中 | 丢包等 I 帧 | SDP 探测；降级 PLI |
| NVR 时钟跳变（NTP 校时） | 低 | 流卡顿 1s+ | 时钟跳变检测 + jitter reset |
| 显示器无 VRR | 高 | 极致档不可用 | 智能 profile 降级；stats 输出 present_mode |
| IAudioClient3 不支持 | 中 | 音频延时 20ms+ | 回退 20ms buffer |
| 共享模式 mix tail | 必然 | 音频 +10ms | 提供独占模式 cfg |
| UDP 被策略拦 | 低 | 无媒体 | RTSP interleaved TCP / WHEP TURN |
| 海康 Digest 差异 | 中 | 连接失败 | 设备兼容表（附录 D） |
| Device Lost（驱动更新 / TDR） | 中 | 播放中断 | DEVICE_REMOVED/RESET 全恢复（§7.3.2） |
| 音频设备切换（拔耳机） | 高 | 音频中断 | IMMNotificationClient + rebuild |
| 硬解偶发错误 | 低 | 花屏 | decode_error_flags + freeze |
| 大小核调度 | 中 | 延时抖动 | P-core 亲和性 |
| URO / Interrupt Moderation | 中 | +2 ms 延时 | stats 检测 + 安装文档 |
| Windows 大版本破坏行为 | 低 | 功能退化 | CI 覆盖 Win10 1809/22H2 + Win11 23H2/24H2 |
| HEVC Extension 缺失（Win10/11 retail） | 中 | 需软解兜底 | mc-libcodec 自动软解（§7.3.5） |
| libdatachannel 维护体量小 | 中 | DTLS 边缘 case | pin release tag + 上游 CI 烟测 |
| 智能 GPU 选择行为不符预期 | 低 | 感知 | adapter_luid 显式覆盖 |
| DCOMP 不进 iflip（best-effort） | 中 | 延时未达预期 | PresentMon 验证 + stats 输出 |
| WHEP draft 演进 | 低 | 互通失败 | 跟进 IETF；保留 PATCH 兜底 |
| mc-libcodec 工程量 | 高 | 周期 | M5/M6 各 4 周；ITU-T 合规 CI 把关 |
| H.264/H.265 SEP 专利责任 | 低（自用） / 中（商业） | 法务 | README/UI 声明；distributor 自负 |
| mc-libcodec 合规覆盖率不足 | 中 | 解码错误 | CI 锁定 ITU-T reference 100% bit-exact |
| 跨屏拖拽 soft adapter switch 冻结 | 低 | UX | 限制冻结 ≤ 800 ms；超时 fallback 旧 adapter |
| AV1 Extension Win10 缺失 | 中 | AV1 不能播 | stats 告警；mc-libcodec 后续加 AV1 软解 |

---

## 13. 迭代路线（4 个月，一次性发布）

| Milestone | 周 | 内容 | 验收 |
|---|---|---|---|
| M1 | 2 | Codec Bridge MFT 实施 + 智能 GPU/profile + `mft_probe` | CI 三机（UHD 630 / 770 / RTX）跑过 |
| M2 | 3 | WHEP 协议 + libdatachannel 集成 + WinHTTP + libopus + sdp_munge + `whep_smoke` | WHEP 拉流 first frame ≤ 600 ms LAN，60 s 稳定 |
| M3 | 1.5 | DCOMP 渲染 + H.265 depack 完整化 + ETW + 跨屏 soft switch | DCOMP 档 PresentMode 验证；跨屏切换 ≤ 800 ms |
| M4 | 1 | iGPU 全矩阵覆盖（CI 实机 UHD 630/770/Iris Xe/Vega） | §9.2 矩阵 100% 验证 |
| M5 | 4 | mc-libcodec H.264 全 profile（NAL/SPS/CABAC/Intra/Inter/IDCT/deblock/DPB + SSE2/AVX2） | 1080p H.264 ≤ 8 ms；JVT 100% bit-exact |
| M6 | 4 | mc-libcodec H.265 全 profile（含 35 方向 Intra、advanced motion、SAO、Main10） | 1080p H.265 ≤ 12 ms；JCTVC 100% bit-exact |
| M7 | 2 | 集成、AV1 stub 启用、SEP 声明完善、端到端延时回归 | §1.2 设计目标全部达成 |

---

## 14. 附录 A：参考规范与权威资料

**RFC 协议**：
- RFC 2326 RTSP 1.0 · RFC 3550/3551 RTP/RTCP · RFC 3611 RTCP XR
- RFC 4566 / 8866 SDP · RFC 4585 AVPF · RFC 4588 RTX · RFC 5104 CCM
- RFC 6184 H.264 Payload · RFC 7798 H.265 Payload · RFC 7587 Opus Payload
- RFC 3640 MPEG-4 (AAC) · RFC 8285 RTP Header Extensions
- RFC 8825 WebRTC overview · RFC 9143 BUNDLE · RFC 9429 JSEP
- RFC 9147 DTLS 1.3 · RFC 9725 WHIP
- draft-ietf-wish-whep（当前 -03，未成 RFC）

**ITU-T / ITU-R / ISO**：
- ITU-T H.264 (ISO/IEC 14496-10) Annex D/E
- ITU-T H.265 (ISO/IEC 23008-2)
- ITU-T H.266 (ISO/IEC 23090-3)
- ITU-R BT.1359-1（A/V sync perception）
- ITU-R BT.709-6 / BT.2020-2

**Microsoft 官方**：
- Media Foundation Transforms · CLSID_CMSH264DecoderMFT · CLSID_CMSH265DecoderMFT · CLSID_CMSAACDecMFT
- Asynchronous MFTs · MF_TRANSFORM_ASYNC_UNLOCK · CODECAPI_AVLowLatencyMode
- Supporting Direct3D 11 Video Decoding in Media Foundation
- DirectComposition · Composition Swap Chain
- IDXGISwapChain2::GetFrameLatencyWaitableObject · IDXGIOutput6::CheckHardwareCompositionSupport
- DXGI flip model · Variable refresh rate displays
- Handle device removed scenarios
- IAudioClient3 · IMMNotificationClient · WinHTTP API

**WebRTC / 工业实现**：
- libdatachannel (github.com/paullouisageneau/libdatachannel) — v0.24.2
- Chromium WebRTC `modules/video_coding/jitter_estimator.cc`
- Chromium NetEQ `modules/audio_coding/neteq/{delay_manager, accelerate, preemptive_expand, expand}.cc`
- Chromium `modules/rtp_rtcp/source/{nack_module2, remote_ntp_time_estimator}.cc`
- NVIDIA Video Codec SDK Programming Guide
- PresentMon (github.com/GameTechDev/PresentMon)

**对标 / 参考实现**：
- NDI Technical Requirements · SRT Protocol Specification · NVIDIA Reflex SDK
- OpenH264 (BSD-2) · libde265 (LGPL-3 / CC0) · x264 (GPL，仅 spec 参考)
- Intel oneVPL / Quick Sync Video media capability matrix
- DXVA Checker (bluesky-soft.com/DXVAChecker)

**SEP / License**：
- Via LA AVC / HEVC Patent Portfolio License — via-la.com（原 MPEG-LA）
- Access Advance HEVC — accessadvance.com
- Velos Media（部分专利权人 2023+ 转入 Access Advance）
- Cisco OpenH264 BINARY_LICENSE — openh264.org
- Apache License 2.0 §3 Patent Grant — apache.org/licenses/LICENSE-2.0

---

## 15. 附录 B：对标表

| 方案 | 延时（LAN） | 延时（WAN） | 备注 |
|---|---|---|---|
| VLC 默认 | 1~2 s | 1~3 s | 保守 buffer |
| FFplay low-delay | 200~500 ms | 500 ms~1 s | |
| WebRTC 典型 | 100~200 ms | 150~300 ms | 自适应 jitter |
| WebRTC 极限调优 | 50~80 ms | ~150 ms | 参考目标 |
| NDI HX | ≈33 ms（单帧） | 需 NDI Bridge | 工业极限 |
| SRT default | 120 ms ARQ 窗 | 500 ms~2 s | |
| **mc-player RTSP + DCOMP + 240 Hz VRR** | **25~50 ms** | n/a | 极限档 |
| **mc-player WHEP + MediaMTX SFU** | 50~80 ms | 80~150 ms | 与 WebRTC 极限调优同档 |

---

## 16. 附录 C：验证与测量工具

- RTSP 服务端：MediaMTX、Live555 testOnDemandRTSPServer
- WHEP 服务端：MediaMTX、OvenMediaEngine（WHIP 完整 / WHEP 路线图中）、Janus（第三方插件）、Cloudflare Stream
- 抓包：Wireshark（RTP/RTCP/STUN/DTLS 解析）
- 延时测量：见附录 E
- CPU/GPU 性能：Intel VTune、PIX、GPUView + WPA
- DXGI 分析：PresentMon + ETW（7 个 PresentMode 字符串值见 §5.10.5）
- 音频延时：LatencyMon
- 丢包/抖动注入：tc-netem（Linux 测试机）/ Windows Pktmon
- `mft_probe.exe`：JSON 能力快照，CI 回归用
- `whep_smoke.exe`：WHEP 端到端互通自动化脚本

---

## 17. 附录 D：设备互操作 Quirks 矩阵

### 17.1 RTSP 设备

| 差异点 | 海康 | 大华 | Axis | ONVIF 通用 | 处理 |
|---|---|---|---|---|---|
| PLAY 立即发 I 帧 | 是 | 依固件 | 是 | 不一定 | 首帧超时发 PLI |
| 保活 | OPTIONS | GET_PARAMETER/OPTIONS | OPTIONS | 任一 | 用 OPTIONS 最兼容 |
| Session timeout | 有 | 有 | 有 | 缺省 | 缺省按 60 s |
| Content-Base/Location | Content-Base | Content-Location | Content-Base | 混用 | 优先 Content-Base |
| SETUP server_port 连续 | 是 | 基本是 | 是 | 不保证 | 读两个独立值 |
| SDP `a=ssrc` | 有 | 少 | 有 | 少 | transport 或首包自学习 |
| rtcp-fb nack | 多数 | 部分 | 是 | 部分 | 探测 + 降级 PLI |
| Digest qop=auth-int | 否 | 否 | 否 | 不要求 | 仅 qop=auth |
| Digest algorithm=MD5-sess | 部分 | 否 | 否 | 否 | 默认 MD5 |
| 401 重试上限 | 3 | 5 | 无 | 无 | 最多 3 次 |
| DESCRIBE 带 sprop | 是 | 是 | 是 | 部分 | 缺则等 in-band |
| 大 GOP（60+） | 可配 | 可配 | 可配 | 可配 | 通过 ONVIF 配置降 |

### 17.2 WHEP SFU 服务器

| 差异点 | MediaMTX | OvenMediaEngine | Janus（插件） | Cloudflare Stream |
|---|---|---|---|---|
| BUNDLE 必须 | 是 | 是 | 是 | 是 |
| RTX 必须 | 否 | 是 | 否 | 是 |
| FlexFEC | 否 | 是 | 否 | 否 |
| Trickle ICE | 是 | 是 | 是 | 是 |
| 单端口 ICE | 是 | 是 | 配置依赖 | 是 |
| Opus 必须 | 是 | 是 | 是 | 是 |
| H.265 | 是（部分） | 是 | 否 | 是 |
| AV1 | 否 | 实验性 | 否 | 是 |
| WHEP 当前支持 | ✓ | 路线图 / v0.18+ | 第三方 plugin | ✓（含私有扩展） |

更新原则：联调失败后记录具体型号 / 服务器版本 / 现象，补入本表，做 CI 回归。

---

## 18. 附录 E：延时测量方法论

- **玻璃到玻璃（QR Code）**：源端 overlay QR `frame_id | QPC_send_ns`；高速相机拍两屏；解码 QR 比对。
- **LED + 光传感器**：源端 LED + 接收屏光感；硬件成本高但精度 < 1 ms 含显示器。
- **客户端内部自测（CI 基线）**：插测试流源（固定 pattern），测 UDP recv → Present 返回 P50/P95/P99；每 PR 跑 1 分钟。
- **Pktmon 回放**：`PktMon start --capture --pkt-size 0 -f tcp.port==554` → `PktMon pcapng`；replay tool 灌回播放器，验证 jitter/NACK/断线恢复。
- **WebRTC 延时（abs-send-time）**：server 在 RTP 包戳 send 时间，客户端 Present 后差值即端到端延时（含编码 + 网络 + 客户端，不含 scan-out）。libdatachannel 透传 timestamp metadata，配合 mc_clock_now_ns 单端测量。

---

## 19. 附录 F：设计决策记录（ADR）

> 仅列与"专业播放器隐形 + 平台原生 + 极限延时"三大命题相关的关键决策。

#### ADR-001 用 Media Foundation Transform 作主硬解后端
- **决策**：H.264/H.265/AAC 主硬解走 Windows Media Foundation Transform（`CLSID_CMSH264DecoderMFT` 等）
- **依据**：MFT 走 DXVA 与底层硬解管线一致；OS 内置零依赖、零 license 风险；Microsoft 官方持续维护

#### ADR-002 硬件 MFT 走 async 事件驱动模型
- **决策**：`MFTEnumEx` 用 `HARDWARE | ASYNCMFT | SYNCMFT` 同时枚举；激活硬件 MFT 后必发 `MF_TRANSFORM_ASYNC_UNLOCK = TRUE`，用 `IMFMediaEventGenerator` 事件驱动（METransformNeedInput / METransformHaveOutput）
- **依据**：Microsoft 文档明确"hardware MFT always process data asynchronously"——这不是性能选择，是协议要求；sync 调用模式会 stall

#### ADR-003 dual-bind 通过 MF_SA_D3D11_BINDFLAGS 显式启用
- **决策**：MFT output stream attribute 显式设 `MF_SA_D3D11_BINDFLAGS = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE`；renderer 用 `D3D11_SRV_DIMENSION_TEXTURE2DARRAY` + `FirstArraySlice = GetSubresourceIndex` 创建 SRV
- **依据**：MFT 默认仅 `D3D11_BIND_DECODER`，无此 attribute 必须 `CopyResource` 到独立 texture（破坏零拷贝）

#### ADR-004 HEVC Extension 缺失自动 mc-libcodec 软解兜底
- **决策**：`MFTEnumEx(HEVC)` 返回空时无感切 mc-libcodec 软解，UI 仅展示 hint
- **依据**：Win10/11 retail 默认无 HEVC Extension；硬失败 UX 不可接受

#### ADR-005 libdatachannel 作为 WebRTC 后端
- **决策**：libdatachannel 0.24.x（C/C++、SRTP/DTLS/ICE 一站式、MPL 2.0）
- **依据**：vs libwebrtc（50 MB+ → 1.5 MB，50× 缩减）；MPL 2.0 文件级 copyleft，与闭源主项目静态链接合法；600+ stars 活跃

#### ADR-006 mc-libcodec 子项目（自研 H.264/H.265 全 profile 软解）
- **决策**：自研 mc-libcodec 子项目 H.264 + H.265 全 profile，参考 OpenH264/libde265 架构（不复制代码），SSE2/AVX2 SIMD
- **依据**：OpenH264 BSD-2 仅 Baseline、dav1d AV1-only；自研换独立可复用 + 无外部 license 责任

#### ADR-007 智能 GPU 选择（移除 mc_gpu_pref_t 字段）
- **决策**：内部按「HWND 跟随 + 能力兜底 + 跨屏热切换」自动选 adapter；保留 `adapter_luid` 显式覆盖
- **依据**：HWND 跟随是延时最低的物理一致性策略；用户层无感，符合「专业播放器」隐形原则

#### ADR-008 DCOMP 第 4 档作为智能 profile 自动激活档（best-effort）
- **决策**：实施 `MC_RENDER_ULTIMATE_DCOMP`；智能选择条件满足时自动激活；HUD 通过独立 DCOMP visual + Present(1,0) 与 video plane 解耦
- **依据**：DCOMP 真正卖点是多 visual 的 plane 分离能力；Independent Flip 升级是 best-effort，运行时由 PresentMon 验证；不当作必胜档

#### ADR-009 Opus 用 libopus（无 MFT 路径）
- **决策**：bundled libopus（BSD-3，~700 KB）
- **依据**：OS 无 Opus MFT；OvenMediaEngine / MediaMTX / Cloudflare 全用 Opus；放弃「全 OS 自带」原则但 Opus 是 WebRTC 事实标准

#### ADR-010 mc-libcodec 拆为独立子项目
- **决策**：monorepo 内独立子项目 `subprojects/mc-libcodec/`，独立 CMake target、独立 API、独立 license（Apache 2.0）、独立测试套件
- **依据**：独立可复用于 mc-encoder、其他工具；清晰 license 边界

#### ADR-011 H.264/H.265 SEP 专利责任声明
- **决策**：OS MFT 路径无责任；mc-libcodec 软解 distribute 时 distributor 自负；参考 Cisco OpenH264 源码-二进制责任分割模型
- **依据**：Via LA AVC / HEVC pool 与 Access Advance 现行条款；Apache 2.0 §3 含明确专利 grant

#### ADR-012 智能 render profile 选择（默认 MC_RENDER_AUTO）
- **决策**：运行期探测 `CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)` + `CheckHardwareCompositionSupport` + DCOMP 可用性，自动选最佳档
- **依据**：与 ADR-007 智能 GPU 选择一致的「专业播放器隐形」哲学；VRR 探测依赖 `DXGI_FEATURE_PRESENT_ALLOW_TEARING`，MPO/DirectFlip 依赖 `IDXGIOutput6::CheckHardwareCompositionSupport`，是应用层唯一权威 API

#### ADR-013 跨屏 adapter switch 用 double-buffered transition
- **决策**：在新 adapter 上预创建 D3D11 device + IMFDXGIDeviceManager，旧 device 持续显示直到新 device 出第一帧再切换；上限 800 ms，超时 fallback 旧 adapter
- **依据**：单 adapter 重建链 + 网络 RTT + I 帧等待经常超 200 ms；double-buffered 避免黑屏 UX

---
