# mc-player — 多协议超低延时媒体播放器架构设计文档 v3.0

| 项目 | 内容 |
|---|---|
| 项目名称 | mc-player (Multi-protocol Client Player) |
| 文档类型 | 架构设计文档（ADD）—— 原理 / 架构 / 可行性 |
| 目标平台 | Windows 10 1809+ / Windows 11 x64 |
| 协议（一） | RTSP/RTP over UDP（含 interleaved TCP 降级） |
| 协议（二） | WebRTC WHEP（draft-ietf-wish-whep-03；该 draft 已 expired & archived，WG 标注 "Revised I-D Needed"，技术内容稳定，按 -03 实施） |
| 编解码后端（主） | Windows Media Foundation Transforms（H.264 / H.265 / AAC） |
| 编解码后端（副） | mc-libcodec 子项目（自研 H.264 / H.265 主流 profile 软解兜底） |
| WebRTC 栈 | libdatachannel 0.24.x（自 v0.18 起切换为 MPL 2.0；当前最新 0.24.2 / 2026-03；SRTP + DTLS + ICE） |
| 客户端内部延时 | 8~25 ms（含 DCOMP 极致档） |
| 端到端延时（LAN） | 35~75 ms（144 Hz VRR） / 25~50 ms（240 Hz VRR + DCOMP） |
| 子项目 | `subprojects/mc-libcodec/`（独立 target、独立 API、独立测试套件） |

> 本文聚焦**原理与架构**的设计陈述与**可行性**论证：分层、模块拓扑、数据流、关键算法的形式化原理、延时预算、硬件能力矩阵、依赖与 License、风险与对策。具体实施（API 头文件、参数清单、代码骨架、目录结构、迭代排期、测量工具命令）不在本文范围。

---

## 1. 概述

### 1.1 项目定位

mc-player 是 Windows 平台的多协议超低延时媒体播放器，对标 NDI full bandwidth（≈16-50 ms 单帧）与 WebRTC LAN 极限调优（50~80 ms）水平；NDI HX（H.264 压缩流）实际典型 80-200 ms，本播放器目标优于该量级。核心命题是把"用户感知到的复杂度"降到最低：用户给一个 URL（`rtsp://...` 或 `whep://...`），mc-player 自动选最佳 GPU、最佳渲染档位、最佳解码后端，无需任何配置。

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
默认: WebRTC WHEP（SRTP + DTLS + ICE）
降级: RTSP interleaved TCP（RFC 2326 §10.12）
扩展（后续）: RTMP / SRT / 自研私有协议
```

---

## 2. 设计原则

1. **最短路径**：每条数据流穿过的模块和拷贝数最少。
2. **低水位队列**：每条 SPSC 队列深度极低，配 cache-line padding 防 false-sharing。
3. **零拷贝**：DXVA NV12 surface 在 GPU 内通过 dual-bind 直接喂 SRV，无 readback。
4. **主动丢帧**：宁可 freeze last frame 也不堆积；jitter buffer / decode / render 三处可丢。
5. **硬件优先**：解码 → MFT；合成 → DCOMP；时钟 → QPC；同步等待 → 高分辨率 Waitable Timer。
6. **自适应**：jitter buffer、NACK 调度、丢帧策略动态调整（Kalman 滤波 / iat 直方图）。
7. **协议抽象**：核心管线协议无关；RTSP 与 WHEP 在 jitter buffer 之上完全合流。
8. **OS 协同**：MMCSS、共享音频引擎低周期、高精度等待原语。
9. **可观测**：关键路径 ETW；end-to-end stats 暴露端到端指标。
10. **平台原生**：能用 Windows OS 自带能力时不引第三方。Codec → MFT，HTTP → WinHTTP，TLS → Schannel（libdatachannel 内部走 OpenSSL / GnuTLS / Mbed TLS 三选一作 DTLS 后端），时钟 → QPC，COM/D3D11/DCOMP 全部直调。理由：减小攻击面、避免第三方 license、跟随 OS 安全更新、优化器对 OS API 的深度优化。
11. **能力探测先于使用**：所有硬件加速路径在 open 前先做能力探测，fail-open（探测到不支持立即降级）而不是 fail-late（运行时异常）。这让从打开 URL 到首帧的路径完全确定，没有 fallback 抖动。
12. **正确性先于延时**：花屏与陈旧区域未刷新比延时多 1 帧更不可接受。Frame Validity Gate (§5.13) 与 Present Epoch (§5.10.5) 是该原则的两个执行点；refresh anchor、B-Frame Policy、color VUI 三级兜底、DPB 引用追踪四类机制收口于此。

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
│  H.264 + H.265 主流 profile 软解 · SSE2/AVX2 SIMD · 独立 API  │
└────────────────────────────────────────────────────────────┘
```

> Codec Bridge 统辖 MFT + mc-libcodec 两条解码路径，前者出 D3D11 texture，后者出 NV12 RAM buffer，渲染器对两者透明。
> mc-libcodec 是 monorepo 内的并列子项目，不在 mc-player 五层架构内，通过公开 API 接入 Codec Bridge。

### 3.2 模块视图

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
       │  ┌─ MFT 硬解（主）─────────────┐  │
       │  │ H.264/H.265/AAC MFT         │  │
       │  │ Async + 事件驱动             │  │
       │  │ DXVA via 共享 D3D11 device   │  │
       │  │ 输出 NV12 D3D11 texture     │  │
       │  └─────────────────────────────┘  │
       │  ┌─ mc-libcodec 软解（兜底）────┐  │
       │  │ H.264/H.265 主流 profile      │  │
       │  │ 输出 NV12 RAM 缓冲           │  │
       │  └─────────────────────────────┘  │
       │  ┌─ libopus（WebRTC 音频）─────┐  │
       │  │ 输出 float interleaved PCM  │  │
       │  └─────────────────────────────┘  │
       └────┬─────────────────────┬────────┘
            ↓ video frame           ↓ PCM
     ┌──────────────────┐           │
     │ Frame Validity   │           │   ← §5.13 协议无关输出门控
     │ Gate（输出门控） │           │      花屏防御 / DPB / VUI
     │ all bits set→emit│           │      / GDR anchor / fence
     └──────┬───────────┘           │
            ↓                       ↓
     ┌──────────────┐      ┌──────────────┐
     │ D3D11 Render │◄─────┤ WASAPI       │
     │ 4 档 + MPO   │ A/V  │ IAudioClient3│
     │ +DCOMP 极致档│ 同步 │ 设备切换处理 │
     │ Present Epoch│      │              │   ← §5.10.5 单点 commit
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
| T2 Network RX | **不挂 MMCSS**；TIME_CRITICAL + 绑 P-core | 15 | RTSP-UDP 收包 + 入 jitter buffer（WHEP 模式下被替代为 T7） |
| T3 RTCP | — | Normal | SR/RR/NACK/PLI（RTSP 路径走 own UDP；WHEP 路径走 SRTCP） |
| T4 Video Decode | **Playback** | ~23 | 调度 MFT 异步事件循环或 mc-libcodec 解码、出队纹理 / NV12 buffer |
| T5 Video Render | **Playback** | ~23 | D3D11 Present，含 DCOMP commit |
| T6 Audio Render | **Pro Audio** | ~26 | WASAPI 回调填缓冲 |
| T7 WHEP-PC | **不挂 MMCSS**；HIGHEST | 10 | libdatachannel 内部线程（DTLS/SRTP 解密、ICE 心跳） |

> 优先级备注：在 `NORMAL_PRIORITY_CLASS` 下 `THREAD_PRIORITY_TIME_CRITICAL=15`、`THREAD_PRIORITY_HIGHEST=10`；T4/T5 走 MMCSS Playback（base ≈ 23）、T6 走 MMCSS Pro Audio（base ≈ 26）。MMCSS 实际优先级以系统注册返回值为准。

**线程合并说明**：

- 当协议是 WHEP 时，T2 实际由 libdatachannel 内部 socket 线程承担，应用层无需创建独立 T2。从 controller 看到的依然是 RTP 回调，与 RTSP-UDP 路径完全同形。
- 当协议是 RTSP 时，T7 不存在。

**T7 不挂 MMCSS 原因**：DTLS/SRTP CPU 密集但非确定性时序敏感（对齐 Chromium WebRTC 主线模式）。

**MMCSS 集成原则**：仅指定 work queue id 不会自动让 MFT worker 跑在 MMCSS 线程上——需先创建 MMCSS-aware 共享 work queue，再让平台层 work queue 整体注册 MMCSS，最后把 MFT 绑定到该 queue。T5/T6 上层手动注册 Playback / Pro Audio 任务。

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
[Kernel Socket Buffer]      大 SO_RCVBUF；URO 关闭；Interrupt Moderation 关
    ↓
[用户态 RTP 缓冲]            WSARecvFrom / IOCP（RTSP）
                             或 libdatachannel decrypt（WHEP）
    ↓ （解 RTP 头 + 扩展头 + 去 padding）
[Adaptive Jitter Buffer]    动态 target_delay；NACK 调度
    ↓
[H.264/H.265 Depacketizer]  STAP-A / FU-A / AP / FU 重组；保留 SEI
    ↓ 输出 Annex-B；与 MFT 输入逐字节匹配
    ↓
┌── 路径 A：MFT 硬解（主，能力支持时）──────────────┐
│   1 次 CPU memcpy 入 MF buffer（~50 μs）          │
│   ↓                                                │
│   异步事件驱动 ProcessInput / ProcessOutput        │
│   ↓                                                │
│   输出 D3D11 NV12 array slice                      │
│   ↓ 与 renderer 同 ID3D11Device，无跨 device 拷贝 │
│   ↓ dual-bind 直绑 SRV                             │
└────────────────────────────────────────────────────┘
                     或
┌── 路径 B：mc-libcodec 软解（兜底，硬件不支持时）──────┐
│   1 次函数调用提交 Annex-B AU                       │
│   ↓                                                 │
│   内部 NAL/CABAC/IDCT/MC/deblock/...               │
│   ↓                                                 │
│   输出 NV12 in RAM (Y/UV pointers)                 │
│   ↓                                                 │
│   CPU→GPU 上传 D3D11 dynamic texture（~0.5 ms）    │
└────────────────────────────────────────────────────┘
    ↓
[D3D11 Pixel Shader: NV12→RGB]
  矩阵: BT.601/709/2020 按 SPS VUI 选择
  range: limited(16-235) / full 按 video_full_range_flag
    ↓
[Swap Chain Back Buffer]
  4 档自动选择：COMPAT / BALANCED / EXTREME / ULTIMATE_DCOMP
    ↓ Present + ALLOW_TEARING
[显示器 scan-out]            8~16 ms @60 Hz / ~7 ms @144 Hz / ~4 ms @240 Hz VRR

CPU 拷贝（路径 A）: 1 次（depack→MF buffer）
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
│ AAC Depacketizer → MFT AAC Decoder            │
│ 输入 Raw AAC + AudioSpecificConfig             │
│ 输出 MFAudioFormat_Float 32-bit interleaved   │
└───────────────────────────────────────────────┘
    或
┌── Opus（WebRTC 必备，RFC 7587）────────────────┐
│ libopus opus_decode_float                      │
│ 输出 float interleaved（与 AAC 路径一致）       │
└────────────────────────────────────────────────┘
    或
┌── G.711 a-law / μ-law（256-entry LUT）─────────┐
│ float interleaved                              │
└────────────────────────────────────────────────┘
    ↓
[环形缓冲 3~20 ms]
    ↓
[IAudioClient3 Event Callback]   driver-defined min period（典型 2.67~3 ms）
    ↓
[WASAPI Audio Engine]            共享模式 mix tail ~10 ms（固有）
    ↓
[声卡 DAC]
```

MFT AAC Decoder 默认输出 16-bit PCM；本设计在 SetOutputType 阶段显式协商 `MFAudioFormat_Float` 32-bit interleaved，避免后置重采样；WASAPI 在共享模式下完成设备 mix 转换。

### 4.3 零拷贝原理（dual-bind）

零拷贝定义：**不存在 GPU→CPU readback，且 GPU 内部至多 1 次 NV12 array slice 复制**。

实现要点：

- **探测**：在目标 D3D11 device 上尝试创建一张 `BIND_DECODER | BIND_SHADER_RESOURCE` 的 NV12 texture，成功即说明 driver 支持 dual-bind。
- **启用**：在 MFT 输出流的属性集上显式请求该 BindFlags 组合（这是 hint，driver 可忽略；忽略时降级为单次 GPU 内拷贝路径）。
- **共享**：MFT 与 renderer 共享同一 ID3D11Device，并启用多线程保护，从而解码 texture 与渲染 texture 共生命周期、零跨 device 拷贝。
- **SRV 绑定**：texture 实为 array slice，SRV 用 `TEXTURE2DARRAY` 维度 + `FirstArraySlice` 指向真实子资源。
- **生命周期与 fence**：每个待显示帧持有 `IMFSample` 强引用直到 Present 完成；array slice 在 DPB 被 evict 前用 `ID3D11Fence`（D3D11.3+）等待 SRV 读完，避免 "slice 已回收但 shader 仍在采样" 的花屏。该 fence 信号同时是 §5.13 Frame Validity Gate 的 `gpu_fence_signaled` bit 来源。

driver 兼容性（截至 2026-04）：Intel Gen 9.5+（含 UHD 630）、AMD GCN 2.0+、NVIDIA Pascal+ 全部支持；老代驱动（pre-WDDM 1.2 已基本绝迹）回退到单次 GPU 内拷贝，1080p NV12 ≈ 0.5 ms。

---

## 5. 模块原理

> 本章只阐述每个模块"为什么这样做"与"做什么"，不展开 API、字段名、参数清单等实施细节。

### 5.1 Transport Session — RTSP/RTP

**状态机骨架**：`OPTIONS → DESCRIBE → SETUP × N → PLAY → (KEEPALIVE 循环) → TEARDOWN`。

**Digest 认证现网兼容路径**：以 RFC 2617（仅 `qop=auth`、algorithm=MD5）为主——海康/大华/Axis 等老 IPC 都仅支持 MD5。RTSP 1.0 (RFC 2326 §12.5) 引用 RFC 2069；RTSP 2.0 (RFC 7826) 才正式引用 RFC 7616（SHA-256），实施层可选支持 SHA-256 升级路径。

**interleaved TCP 降级**：UDP socket 创建失败或 SETUP 返回 461 时切换 `Transport: RTP/AVP/TCP;interleaved=0-1`，按 RFC 2326 §10.12 解析 `$ ch len` 帧化。

### 5.2 Transport Session — WHEP

**HTTP 信令流程**（draft-ietf-wish-whep-03）：

```
C → S:  POST  /whep                  Content-Type: application/sdp（offer）
S → C:  201 Created + Location + answer SDP

(运行期 ICE trickle，可多次)
C → S:  PATCH /whep/<id>             Content-Type: application/trickle-ice-sdpfrag
                                     If-Match: <ETag>（可选；ICE restart 必带）
S → C:  204 No Content              ← 单纯 trickle 候选累加
        或 200 OK                    ← ICE restart 路径，body 含新 ufrag/pwd

(关闭) DELETE /whep/<id>
```

**HTTP 客户端选型**：使用 WinHTTP 异步模式。WinHTTP 是 Windows OS 原生组件，HTTPS 通过 Schannel，与系统代理 / WPAD / PAC 无缝集成；Microsoft 官方推荐 server applications and non-UI HTTP clients 使用 WinHTTP 而非 WinINet。

**WebRTC 栈**：libdatachannel 提供 PeerConnection + DTLS-SRTP + ICE 一站式实现。SDP 构造为 receive-only offer（含 H.264 / H.265 / Opus / RTX 能力清单 + NACK/PLI/transport-cc 反馈 + abs-send-time / playout-delay / mid 头扩展），符合 RFC 9429 (JSEP) + RFC 9143 (BUNDLE) + RFC 4585 (AVPF)。

**关键设计选择**：libdatachannel 提供 `onFrame`（库内部完成 depacketization）和 `onMessage`（单包明文 RTP）两种接收回调。本设计选 **onMessage 路径**——保留 SEI / GDR / 字节级控制权，并让 RTSP 与 WHEP 在 jitter buffer + depack 层完全合流。

**codec 路由**：从 server answer SDP 中提取最终选定的 PT，按 codec 分别走 H.264 / H.265 / Opus / RTX 路径。RTX 重传由 libdatachannel 内部处理，应用层透明。

**RTP header extension**：libdatachannel 透传 RFC 8285 ext header；abs-send-time 喂入 jitter buffer 的 Kalman estimator 提升精度；mid extension 用于 m-line 路由（更标准，避免依赖 a=ssrc）。

**关键风险**：

| 风险 | 缓解原理 |
|---|---|
| DTLS 握手边缘 case | pin libdatachannel release tag；CI 烟测每次 release |
| ICE 失败（NAT 严格） | 指数退避重试 ICE；TURN 不可达时上抛 transport 错误，由 App 决定是否切协议（mc-player 不在 WHEP/RTSP 间自动切换） |
| 服务端不发 a=ssrc | mid extension 路由 + 首 RTP 包自学习 SSRC 兜底 |
| TLS 后端选择 | libdatachannel 三选一（OpenSSL / GnuTLS / Mbed TLS），均支持 AES-NI |
| DTLS 协商版本 | DTLS-SRTP（RFC 5764）；现网主流 DTLS 1.2，DTLS 1.3 渐进引入 |

### 5.3 Adaptive Jitter Buffer

#### 5.3.1 视频：Kalman 抖动估计

参考 Chromium WebRTC `frame_delay_variation_kalman_filter.cc`（主线已从早期 `jitter_estimator.cc` 拆出独立 KF 模块）。原理形式化：

```
状态 x = [1 / channel_rate, queuing_delay_offset]ᵀ        （二维）
观测 H = [delta_frame_bytes, 1]                           （行向量）
观测 z = observed_frame_delay_variation                   （标量）
预测 P = P + Q,  Q = diag(2.5e-10, 1e-10)                 （过程噪声）
增益 K = P · Hᵀ · (H · P · Hᵀ + R)⁻¹                       （列向量）
更新 x ← x + K · (z − H · x)
更新 P ← (I − K · H) · P
target_delay = max(jitter_estimate, min_target) + max_decode_time
```

二维状态使估计同时包含信道带宽倒数与排队延时偏移，比早期版本的标量近似 `K = (P+Q)/(P+Q+R)` 更稳健。`R` 通常做"突发 vs 稳态"自适应。WHEP 路径在 `frame_delay_variation` 计算时优先用 abs-send-time 头扩展给的 server 端发送时间戳，进一步降噪。

#### 5.3.2 音频：iat 直方图 + NetEQ 三相位

参考 Chromium WebRTC `modules/audio_coding/neteq` 主线模块：

- **直方图**：相邻包到达间隔（iat）按桶累计，配指数遗忘因子。
- **target_delay**：取直方图高分位数（典型 0.97）+ 一次平滑。
- **三相位**：buffer 高于 target+30 ms → Accelerate（WSOLA 压缩 5–10%）；低于 target−10 ms → Preemptive Expand（拉伸）；丢包 → Normal Expand（PLC）。
- **时钟跳变检测**（NTP 校时引发）：相邻 RTP 时间戳差超过阈值 → reset 估计参数。

#### 5.3.3 NACK 调度

按 Chromium `nack_module2.cc` 主线模型：

- 收到 RTP 包，比对 sequence number 期望值，缺失 → 加入 nack_list。
- 首次调度立即触发；后续重传调度按 `rtt × backoff_factor` 指数退避。
- 重发上限（10 次或 ~1 s）→ 放弃 + 升级 PLI。

### 5.4 RTCP Feedback

RTCP 收发与协议解耦：通过一个发送 shim 抽象 transport 出口。

- **RTSP 路径**：shim 走 own UDP socket。
- **WHEP 路径**：shim 走 libdatachannel SRTCP 加密发送。

复合包构造：SR/RR + SDES + RTPFB-NACK + PSFB-PLI。AVPF 反馈策略采用 RFC 4585 的 **Immediate Feedback mode**（按组规模与 FB 阈值切换）配合 SDP `a=rtcp-fb:* trr-int 0`（默认）——前者保证 FB 不积压、后者不强制 Regular RTCP 最小间隔，二者是相互独立的两个机制，需同时满足才能近实时反馈。SR 接收并喂入 A/V 同步的 NTP-RTP 线性回归。

### 5.5 视频 Depacketizer

#### 5.5.1 H.264（RFC 6184）

| NAL Unit Type | 含义 | 处理 |
|---|---|---|
| 1-23 | 单 NAL | 直接送解码 |
| 24 STAP-A | 聚合（无 DON） | 拆包 |
| 25 STAP-B | 聚合（含 DON） | 仅 packetization-mode=2 启用；本设计不支持 |
| 26 MTAP16 | 多时聚合（16-bit TS offset） | 仅 interleaved；本设计不支持 |
| 27 MTAP24 | 多时聚合（24-bit TS offset） | 仅 interleaved；本设计不支持 |
| 28 FU-A | 分片（无 DON） | S/E 重组，NAL header 重建 |
| 29 FU-B | 分片（含 DON） | 仅 interleaved；本设计不支持 |
| 5 IDR / 7 SPS / 8 PPS | 关键 | 缓存 extradata（带外 SPS/PPS 时） |
| 6 SEI | 补充 | 透传（含 recovery_point GDR） |

> 仅启用 `packetization-mode=1`（non-interleaved）：STAP-A + FU-A 即可覆盖现网 IPC / NVR 与 WebRTC SFU 的全部部署形态；25/26/27/29 仅在 SDP 协商出 `packetization-mode=2` 时才会出现，本设计在 SDP answer 阶段拒绝该模式以保证去 DON 复杂度。

帧边界识别：RTP timestamp 变化 或 marker bit。

#### 5.5.2 H.265（RFC 7798）

| NAL Unit Type | 含义 | 处理 |
|---|---|---|
| 0-31 | VCL 单 NAL | 直接送解码 |
| 32 VPS / 33 SPS / 34 PPS | 非 VCL 参数集 | 缓存 extradata |
| 35 AUD_NUT | 分隔符 | 透传或丢弃 |
| 36 / 37 EOS_NUT / EOB_NUT | 流结束信号 | 上报流结束事件，触发清理 |
| 38 FD_NUT | Filler Data | 丢弃 |
| 39 / 40 Prefix / Suffix SEI | 补充 | 透传（含 recovery_point GDR） |
| 48 AP | 聚合 | 拆包 |
| 49 FU | 分片 | S/E 重组（NAL header 2 字节） |
| 50 PACI | 载荷扩展 | 暂不支持 |

依据：ITU-T H.265 Table 7-1（值 0–47）+ RFC 7798 §1.1.4 占用 48–50。值 48/49/50 在 H.265 规范本体的 Table 7-1 中标为 unspecified，由 RFC 7798 在 RTP 层占用，不属于 H.265 视频规范本体。注意 SEI 是 39（Prefix）和 40（Suffix）两个独立 NAL Type。

H.265 NAL header 为 **2 字节**：F(1) + Type(6) + LayerId(6) + TID(3) = 16 bits。FU 重组与 H.264 的差异在于：FU 携带的是 1 字节 FU header 重写后再前置 2 字节重建的 NAL header，重建时需保留 LayerId/TID 不变、按 FU header 内的 type 字段重组 Type 域。

#### 5.5.3 SEI 与 Refresh Anchor

SEI 不丢。识别下列帧并打上 **refresh anchor** 标记，作为错误恢复后唯一允许解 freeze 的入口（送至 §5.13 Frame Validity Gate 的 `recovery_complete` bit）：

- IDR（H.264 NAL=5；H.265 IRAP：BLA_W_LP / BLA_W_RADL / BLA_N_LP / IDR_W_RADL / IDR_N_LP / CRA_NUT）。
- recovery_point SEI 且 `recovery_frame_cnt == 0`（GDR 完成点）。
- 不发 IDR、仅发 GDR 的低延时编码器场景：必须等到 `recovery_complete` 帧出现才解 freeze；中间任何 P/B 帧仍带 invalid 标记下沉，避免花屏蔓延。

把 IDR 与 GDR 等价处理（"参考帧丢就 freeze 一帧再继续"）是花屏的常见根因——GDR 流的"下一帧"几乎肯定还在 refresh window 内。

#### 5.5.4 丢包处理

- 非参考帧（H.264 `nal_ref_idc=0` 或 H.265 `temporal_id≥1`）丢失 → 静默丢，不影响下游。
- 参考帧丢失 → 立即 PLI + 标记**所有后续帧 invalid 直至下一 refresh anchor**（§5.5.3）；中间帧由 §5.13 Frame Validity Gate 统一丢弃。
- 不在收到 anchor 前解除 invalid 状态，是预防花屏蔓延的关键不变量。

### 5.6 视频 Codec Bridge — MFT 硬解（主路径）

#### 5.6.1 模块原理

Codec Bridge 是协议无关的解码抽象层，统辖 MFT 硬解与 mc-libcodec 软解两条路径。决策点在 open 流时：先做能力探测，硬件支持目标 codec/profile 即走 MFT，否则降级到软解。运行期任一路径出错（device removed、TDR、profile unsupported）按状态机返回 controller 走全恢复。

#### 5.6.2 MFT 探测与激活原理

- **同时枚举硬件 async + 软件 sync MFT**，作为兜底回退候选。
- **硬件 MFT 永远是 async**（Microsoft 文档明确）：不是性能选择，是协议要求；sync 调用模式会 stall。激活时必须通过属性显式 unlock async；用事件生成器驱动 NeedInput / HaveOutput 循环。
- **D3D11 共享设备**：通过 IMFDXGIDeviceManager 把渲染器的 ID3D11Device 注入 MFT，确保解码 texture 与渲染 texture 同 device。

#### 5.6.3 异步事件驱动模型

事件循环只处理三类事件：

- `METransformNeedInput` → 从 jitter buffer 取一个 AU 喂入。
- `METransformHaveOutput` → 拉取一个解码完成的 sample 并发射给 renderer。
- `METransformDrainComplete` → 完成 flush。

每次处理后 re-arm 下一次事件。

#### 5.6.4 零拷贝输出路径

输出 sample 的 D3D11 buffer 通过 IMFDXGIBuffer 接口拿到原生 texture + array slice index，即说明走零拷贝硬解路径，否则 fallback 到 RAM buffer（SW MFT 或 driver 不支持 dual-bind）。

#### 5.6.5 低延时配置原则

- 显式启用 codec 级低延时模式（禁 reorder buffer）：通过 `CODECAPI_AVLowLatencyMode` 设置。**Microsoft H.264 decoder 该属性使用 `VT_UI4`（UINT32），其它（H.264 encoder、H.265 decoder/encoder）使用 `VT_BOOL`**——这是 Media Foundation 文档明示的实现差异（见 Microsoft Learn AVLowLatencyMode 页"Warning"块），SetValue 时必须按 codec 区分类型。
- 显式启用 DXVA 加速，防 driver 在某些条件下静默回退软解。
- 限定 surface pool 上限（典型 6-8）以兼顾 reorder 较深的 Main/High Profile。
- 颜色元数据（nominal range / YUV matrix / transfer function）必须显式设置，否则 renderer 会被迫按默认值（BT.601/709）误判。

#### 5.6.6 B 帧检测与 Policy

低延时管线必须在 open 前确认有无 B 帧重排序：

1. **优先（零成本，SDP 层）**：解析 `profile-level-id`。Constrained Baseline / Baseline / Constrained High 必无 B 帧，可直接判定。
2. **兜底（in-band SPS）**：用最小 Exp-Golomb 解码器扫到 VUI 后立即退出，读 `max_num_reorder_frames`（H.264）或 `sps_max_num_reorder_pics`（H.265），等于 0 才确切无 B 帧 / 重排。

**B-Frame Policy（正确性优先）**：检出 reorder > 0 时强制采用以下三条，否则与 §5.6.5 的 low-latency mode 直接冲突 → **必然花屏**。

- 取消 `CODECAPI_AVLowLatencyMode`（不再 disable reorder buffer）。
- surface pool 上限抬到 `max_num_reorder_frames + 2`（H.264）或 `sps_max_num_reorder_pics + 2`（H.265）。
- stats 上报 `reorder_cost_ms`（典型 +30~60 ms）；不在 codec 层悄悄做权衡，由用户感知该流不适合极致延时档。

§5.13 Frame Validity Gate 的 `reorder_resolved` bit 由 codec bridge 在 reorder buffer 排序完成时置位。

#### 5.6.7 错误恢复状态机

```
ProcessOutput 返回值 / 事件
├─ S_OK                            → emit frame；继续事件循环
├─ MF_E_TRANSFORM_NEED_MORE_INPUT  → idle；等下个 NeedInput
├─ MF_E_TRANSFORM_STREAM_CHANGE    → 重设 OutputType（分辨率切换）
├─ MF_E_INVALIDREQUEST             → 编程错误；assert 报警
├─ DXGI_ERROR_DEVICE_REMOVED       → 上交 Device Lost 全恢复（§7.2）
├─ E_OUTOFMEMORY                   → flush + 重试一次；二次失败上抛
├─ TestDevice 失败（TDR）          → 同 DEVICE_REMOVED 路径
└─ 其他 FAILED                     → flush + 重置 keyframe gate + 发 PLI
```

任意 FAILED 或输出 sample 携带 decode error flag → 标记本帧及所有 outstanding 待显示帧 invalid，由 §5.13 Frame Validity Gate 统一丢弃；解码器侧不再尝试 "输出但加 freeze" 的兜底路径，避免错误帧因竞态被 Present。

#### 5.6.8 软解 fallback 链原理

```
1. CheckVideoDecoderFormat 探测当前 codec/profile
2. 探测通过 → 走 MFT 硬解
3. 探测失败 / MFT init 失败 → 走 mc-libcodec 软解
4. mc-libcodec 也不支持该 profile → 上报 profile unsupported
5. 用户显式禁用软解 → 跳过软解直接上报 no hardware
```

软解输出 NV12 RAM buffer，渲染端通过通用 dynamic texture 上传走与硬解完全一致的渲染路径，对 renderer 透明。

#### 5.6.9 AV1 / H.266 路线

| Codec | OS MFT 支持 | 硬件支持（2026） | 状态 |
|---|---|---|---|
| AV1 | Win10/11 均需从 Microsoft Store 安装免费 AV1 Video Extension | Intel Tiger Lake 11 代+（首个）、AMD RDNA2+、NVIDIA Ampere+ | 通过 MFTEnumEx + AV1 子类型枚举（无固定 CLSID 文档） |
| H.266/VVC | 暂无 | Intel Lunar Lake LP 试探性 | 待 OS 支持；mc-libcodec 子项目可扩展 |

### 5.7 mc-libcodec 子项目

#### 5.7.1 子项目定位

- 目录布局：`subprojects/mc-libcodec/`，与 `mc-player/` 平级。
- 独立 target、独立 API、独立测试套件、独立 README + LICENSE（Apache 2.0，含明确专利授权）。
- 不反向依赖 mc-player，可独立用于 mc-encoder、转码工具、SaaS 后端。

#### 5.7.2 ABI 契约原理

公开 API 头采用首字段 `struct_size + version` 的演进式 ABI：未来字段追加不破坏旧调用方。语义为"一次提交一个 Annex-B AU、一次拉取一个 NV12 帧"，与 MFT 异步模型对位。

#### 5.7.3 内部模块拓扑

参考 OpenH264 / libde265 / FFmpeg /VLC播放器 公开架构（不复制代码）：

- **公共层**：bitstream reader（Exp-Golomb）、CABAC engine、neighborhood / RefPicList、SIMD dispatch（启动期 CPUID 探测填充函数指针表）、**显式 DPB 与引用追踪**（见下）。
- **H.264 层**：NAL parser + RBSP unescape、SPS/PPS、slice header & data、CABAC、4×4 / 8×8 / 16×16 intra（9 方向）、motion compensation（含 1/4 像素 sub-pel 插值）、IDCT、in-loop deblocking、DPB + reorder + POC、pipeline 编排。
- **H.265 层**：2 字节 NAL header、VPS/SPS/PPS、slice、CABAC（与 H.264 不同上下文模型）、35 个 intra 方向、advanced motion（merge / AMVP / MVD）、IDCT 4×4–32×32 + DST 4×4、deblocking、SAO、DPB。

**引用追踪与花屏防御（软解必备，MFT 路径由驱动黑盒处理）**：

- DPB 表显式维护 `{frame_num, poc, refs_used_by, mark}`；解码每个 slice 前校验所需 ref 全部 present。
- FrameNum gap > 0 或 POC 不连续 → 判定参考帧丢失，丢该 slice，标记后续非 anchor 帧为 invalid（送 §5.13 Frame Validity Gate）+ 上抛 PLI 请求。
- 该不变量是软解路径区别于 MFT 黑盒最关键的一点——MFT 由驱动内部处理；mc-libcodec 必须自己保证，否则在丢包恢复期持续输出脏帧。

#### 5.7.4 Profile 覆盖

| Codec | Profile | 当前 | 备注 |
|---|---|---|---|
| H.264 | Baseline / Constrained Baseline | ✓ | RTSP/WHEP 默认 |
| H.264 | Main | ✓ | NVR 主流，含 CABAC、B 帧 |
| H.264 | High | ✓ | 高码率 NVR，含 8×8 transform |
| H.264 | High 10 / 4:2:2 / 4:4:4 | ✗ | 工业不需要，留后续 |
| H.265 | Main | ✓ | 8-bit 4:2:0 |
| H.265 | Main10 | ✓ | 10-bit 4:2:0 |
| H.265 | Main12 / Main 4:4:4 | ✗ | 留后续 |

#### 5.7.5 SIMD 策略

- **基线**：纯 C scalar（debug + 兜底）。
- **SSE2**：所有 x86_64 必有（ABI 强制，无需 CPUID）。覆盖 IDCT、运动补偿插值、deblocking 主路径。
- **AVX2**：runtime CPUID detect（Haswell+，2013 后所有 CPU）。覆盖 H.265 SAO、大块 IDCT、批量小块。
- **不做 AVX-512**：消费 CPU 覆盖率低 + 降频代价大。
- **不做 ARM NEON**：当前仅 Windows x64。

dispatch 在启动期一次性填表，hot loop 内零 indirect call 开销。

#### 5.7.6 多线程模型

- **默认 single-thread**：延时最低、可预测，与超低延时定位一致。
- **可选 frame-level threading**：相邻帧并行解（受 P/B 引用链限制），代价是 +8~20 ms 延时（一帧排序窗口）。仅 4K 高码率离线兜底场景启用，不用于实时路径。
- 不做 slice-level / tile-level 并行（复杂度收益比不划算）。

#### 5.7.7 性能目标（i5-12500，内部测量）

| 输入 | 单核延时 | CPU 占用 | 备注 |
|---|---|---|---|
| 1080p H.264 Main 8 Mbps | 5~8 ms | ~12% | SIMD 优化后 |
| 1080p H.265 Main 5 Mbps | 7~12 ms | ~18% | |
| 4K H.265 Main 20 Mbps | 25~35 ms（兜底） | ~45% | 单核紧张，frame-thread 可减半 |
| 1080p H.264 single-thread no SIMD | 12~25 ms | ~35% | scalar 路径 / 调试用 |

参考：OpenH264 BSD 公开 benchmark 1080p H.264 单核 5~8 ms（含 SIMD），目标对齐。

#### 5.7.8 ITU-T 合规测试

- **H.264**：JVT Allegro 公开 reference 流 + JM reference decoder 比对，YUV 像素 bit-exact。
- **H.265**：JCTVC HM 16.x reference 流 + HM 解码器比对，YUV 像素 bit-exact。
- CI 100% 通过；失败 case 加白名单 + issue 跟踪。覆盖率 ≥ 95%。

#### 5.7.9 与第三方实现的关系

- 不依赖任何第三方解码库代码。
- 可参考算法描述：OpenH264（BSD-2，SIMD 思路、CABAC engine 优化模式）；libde265（LGPL-3，H.265 模块拆分、SAO 实现）。
- x264：仅作 spec/算法参考；不读源码避免 GPL 污染。
- 借鉴 ITU-T 官方 reference decoder（JM / HM，BSD-3-Clause）作为正确性基准。

### 5.8 音频解码

| 路径 | 实现方式 | 输入 | 输出 |
|---|---|---|---|
| AAC（RTSP/RTP） | Microsoft AAC MFT；Raw AAC payload；AudioSpecificConfig 来自 SDP fmtp `config=` | mpeg4-generic/AAC-hbr | float 32-bit interleaved |
| Opus（WebRTC） | bundled libopus（无 Opus MFT） | RFC 7587，1 RTP 包 = 1 Opus packet | float 32-bit interleaved |
| G.711 a-law / μ-law | 静态 256-entry LUT 自实现 | 8-bit | float 32-bit interleaved |

> 分层澄清：SDP 层 RFC 3640 的 `mode=AAC-hbr / AAC-lbr / generic / CELP-...` 是字符串值；MFT 层的 payload 类型是整数（`0=Raw / 1=ADTS / 2=ADIF / 3=LATM-LOAS`），两者不同语义。典型对应：`mode=AAC-hbr` ⇒ Raw AAC frame in RTP ⇒ MFT payload type = 0；若服务端走 LATM/LOAS over RTP（RFC 6416 `MP4A-LATM`，与 RFC 3640 不同 RTP payload format），则改用 LATM。

### 5.9 A/V 同步

#### 5.9.1 主时钟

WASAPI master clock：通过 `IAudioClock::GetPosition` 同时取得"已播放样本数"与"对应 QPC 时刻"。

#### 5.9.2 NTP-RTP 线性回归

收到 RTCP SR 时入采样 `<ntp, rtp_ts>`。维护最近 N 个样本的滑动窗口，最小二乘拟合 `ntp = a · rtp_ts + b`，得到 RTP timestamp 到 NTP 的映射。WHEP 路径下 SR 由 libdatachannel 透传，算法不变。

#### 5.9.3 渲染调度（ITU-R BT.1359 阈值）

**符号约定**（沿用 ITU-R BT.1359-1）：`av_offset = audio_time − video_time`，**正值表示音频领先视频**，负值表示音频滞后视频。

| 阈值类型 | 音频领先视频 | 音频滞后视频 |
|---|---|---|
| 察觉阈值（detectability） | +45 ms | −125 ms |
| 系统可接受阈值（tolerance） | +90 ms | −185 ms |

实际策略（保守取察觉阈值再收紧到 ±15/−25）：

- `−25 ms ≤ av_offset ≤ +15 ms`：立即渲染。
- `av_offset > +15 ms`（音频领先视频太多 ⇒ 视频帧已迟于音频主时钟）：**丢该帧**。
- `av_offset < −25 ms`（音频滞后视频太多 ⇒ 视频帧 PTS 早于音频主时钟）：**延迟渲染**该帧。

#### 5.9.4 freeze last frame

10 秒无新视频帧 → 发出断流事件，画面保持最后一帧 + 状态图标。

### 5.10 视频渲染

#### 5.10.1 智能 render profile 选择

启动时三步探测：

1. **ALLOW_TEARING**（`IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, ...)` 是应用层判定 tearing / VRR 兼容呈现支持的权威 API；它检测的是 driver/OS 是否允许撕裂呈现，而非物理显示器是否真为 VRR——本设计将其作为 VRR 路径的必要前提）。
2. **Hardware composition / DirectFlip / MPO**（`IDXGIOutput6::CheckHardwareCompositionSupport`，区分 fullscreen / windowed）。
3. **DCOMP 可用性**（尝试创建 IDCompositionDevice）。

按能力降序自动选档；用户显式指定档则跳过探测。

#### 5.10.2 4 档配置

| 档 | Swap Chain 模型 | Present 策略 | 预期延时 | 适用 |
|---|---|---|---|---|
| **COMPAT** | Flip Discard + Waitable + MaxLatency=1 | vsync interval=1 | 1~2 帧 | 老 GPU / 非 flip model |
| **BALANCED**（默认） | COMPAT + ALLOW_TEARING | tearing 允许 | 0~1 帧 | Win10+ + 现代 GPU |
| **EXTREME** | Borderless fullscreen + 像素格式匹配 + 屏幕覆盖 | tearing 允许 | <1 帧（HW Composed Independent Flip） | 全屏 + VRR 显示器 |
| **ULTIMATE_DCOMP** | DCOMP `CreateSwapChainForComposition` + Visual + ALLOW_TEARING | tearing 允许 + DCOMP commit | best-effort，期望 ~1 帧 | 任意窗口 + DCOMP + VRR |

> 关于 ULTIMATE_DCOMP：Microsoft 官方文档**未声明** DCOMP swap chain + ALLOW_TEARING 必然进入 Hardware Composed: Independent Flip。它是一条 best-effort 路径——只有当系统授予 hardware overlay plane 时才升级为 iflip，否则落入 Composed: Flip。运行时通过 PresentMon 验证；不要将其当作"必胜"档位。
>
> 真正的卖点是 DCOMP 多 visual 的 **plane 分离能力**：video visual 走 ALLOW_TEARING，HUD visual 走 vsync，互不撕裂。多 visual 是否每个拿到独立硬件 overlay plane 由 DWM + 显卡驱动综合判定——典型显卡 1-4 个 hardware plane（部分集显仅 1 个），且受 Z-order、像素格式、缩放比例、是否含 alpha 等约束。常见结果：video visual 拿独立 plane 升级 iflip；HUD 因 alpha / 非 1:1 缩放被合成 fallback。这与"双 swap chain 各自独立 Present"的应用层期望并不矛盾，但应通过 PresentMon 实测确认。

#### 5.10.3 Waitable Swap Chain（4 档共有）

`SetMaximumFrameLatency(1) + GetFrameLatencyWaitableObject + WaitForSingleObjectEx` 是延时控制核心。两条不变量：

- **ALLOW_TEARING 一致性**：标志必须在 Create / ResizeBuffers / Present 三处一致出现，否则运行时拒绝。
- **ResizeBuffers 前清场**：ResizeBuffers / device-lost 全恢复前必须先 `ImmediateContext::ClearState` 并显式释放所有引用 back buffer 的 SRV/RTV/UAV，否则 ResizeBuffers 返回 `DXGI_ERROR_INVALID_CALL`，常表现为窗口 resize / 跨屏拖拽后**陈旧内容残留**或黑屏。该路径由 §5.10.5 Present Epoch watchdog 兜底强制 redraw。

#### 5.10.4 PresentMon 验证

PresentMon 输出 7 个 PresentMode 字符串值之一：

```
Hardware: Legacy Flip
Hardware: Legacy Copy to front buffer
Hardware: Independent Flip
Composed: Flip
Hardware Composed: Independent Flip
Composed: Copy with GPU GDI
Composed: Copy with CPU GDI
```

ULTIMATE_DCOMP 期望 `Hardware Composed: Independent Flip`，允许降级到 `Composed: Flip`（仍正确，只是少 1 帧合成节省）。EXTREME 期望 `Hardware: Independent Flip`。运行期通过 stats 字段实时上报。

#### 5.10.5 Present Epoch 与刷新权威（陈旧区域防御）

DCOMP 多 visual + ALLOW_TEARING + freeze last frame + 设备/显示器切换共同导致**陈旧区域未刷新**：HUD visual 已 commit 新内容，video plane 因独立 Present 还在播旧帧；或 freeze 期间 status icon 上层 alpha 残留；或 ResizeBuffers / soft adapter switch 过渡期 DWM 缓存的旧 visual 未失效。

**单点刷新权威**：

- T5（render thread）是**唯一** DCOMP `IDCompositionDevice::Commit` 的调用方。video / hud / status 三个 visual 的内容更新必须经 T5 队列汇总。
- 引入 **Present Epoch ID**（单调递增，每帧 +1）：视频 swap chain Present 与 DCOMP commit 同 epoch 配对，每个 epoch 只 commit 一次，避免"video 已 Present、HUD 未 commit"的撕裂半态。

**陈旧区域 Watchdog**：

- T5 周期检查"最近一次 Present 时刻 vs 当前 epoch"。超过 `N × frame_period`（典型 N=3）未推进 → 强制 redraw last-good 并重 commit；处理 WM_PAINT / DPI 变更 / 显示模式切换 / 设备切换期间的 DWM 缓存旧 visual。
- ResizeBuffers / soft adapter switch（§7.3.3）的过渡完成时必触发一次显式 redraw + commit；不依赖下一自然帧到达。
- 长时间 freeze last frame（§5.9.4）路径下，每秒至少一次 redraw + commit，刷新 status icon 与告警状态层。

### 5.11 音频渲染 — WASAPI

- **模式**：默认共享（IAudioClient3）；可选独占。
- **共享模式 buffer period**：调用 `IAudioClient3::GetSharedModeEnginePeriod` 取 `pMinPeriodInFrames` 并按采样率换算到毫秒；inbox HDAudio 驱动实测下限典型为 128 frames @ 48 kHz ≈ **2.66 ms**（Microsoft "Low Latency Audio" 文档参考值），最大可达 480 frames ≈ 10 ms；具体下限由 driver 声明，本设计取 driver-reported min。共享模式 default engine period 通常约 10 ms（Win10 之前固定如此，Win10+ AudioGraph SystemDefault 仍为该量级）；独占模式可消除该开销但要求设备格式精确匹配。
- **回调模式**：事件驱动。
- **AutoConvert**：开启 PCM auto-convert + default-quality SRC，由 WASAPI 完成设备 mix 转换。
- **设备切换（拔耳机）**：通过 `IMMNotificationClient::OnDefaultDeviceChanged` → tear down → 新设备 → rebuild。

### 5.12 色彩空间与 Range

现网 IPC 大量出现 SPS VUI 缺失或填错（全 0 / 全 1，或声明 BT.709 但实际是 BT.601）的情况，单纯按 VUI 取值会出现明显色偏。本设计采用**三级兜底**，最终生效值由 stats 暴露便于诊断。

**第一级 — SPS VUI（自洽时取用）**

| SPS VUI | 行为 |
|---|---|
| `colour_primaries=1, matrix_coefficients=1, transfer_characteristics=1` | BT.709 |
| `colour_primaries=5, matrix_coefficients=6` | BT.601 |
| `colour_primaries=9, matrix_coefficients=9` | BT.2020 |
| `video_full_range_flag=0 / 1` | limited 16-235 / full 0-255 |
| `transfer_characteristics=16 / 18` | HDR PQ / HLG（后续 milestone） |

自洽性判定：三字段未全 0、未全 1、且组合命中表内行；否则视为 VUI 不可信走第二级。

**第二级 — 启发式（VUI 不可信时）**

| 条件 | 默认 |
|---|---|
| 高度 ≥ 720 且未声明 HDR transfer | BT.709 limited |
| 高度 < 720（SD） | BT.601 limited |
| 高度 ≥ 2160 且 transfer ∈ {16, 18} | BT.2020 + PQ / HLG |

**第三级 — 用户显式覆盖**：API 字段 + UI 设置，按 stream URL 持久化。

shader 内查表选 YUV→RGB 矩阵；range 由 shader 内 unscale。`color_meta_known` bit（§5.13）在三级中任一级锁定后置位，否则该帧由 Frame Validity Gate 丢弃。

### 5.13 Frame Validity Gate（输出门控）

Codec 与 Render 之间的协议无关闸门。所有花屏预防机制（refs / GDR anchor / VUI / B 帧 reorder / dual-bind fence）的执行点收口于此，避免散落在多个模块各自 freeze 各自兜底导致竞态。

**Validity Mask（每帧一组 bit，所有为 1 才允许 emit）**

| Bit | 含义 | 由谁置位 / 清零 |
|---|---|---|
| `refs_resolved` | DPB 无 FrameNum / POC gap | depack（参考帧丢失时清零） / mc-libcodec 软解 / MFT decode error flag 强制清零 |
| `params_present` | SPS/PPS（H.265 含 VPS）已缓存且与帧内 PPS_id / SPS_id 匹配 | depack |
| `recovery_complete` | 错误恢复后已遇到 refresh anchor（§5.5.3） | depack |
| `color_meta_known` | VUI 自洽 或 启发式 / 用户覆盖 已锁定 | §5.12 |
| `reorder_resolved` | 若有 B 帧，所需依赖已到（按 POC 排序就绪） | codec bridge / B-Frame Policy |
| `gpu_fence_signaled` | dual-bind array slice 写完成（仅硬解路径） | renderer 取帧前等 fence |

**判定**：所有 bit set → emit 给 §5.10 renderer；任一 bit 0 → 丢帧 + 保留 last-good，不推进视频 PTS（音频时钟独立前进）。

**与延时的折中**：错误恢复 / 首帧 / VUI 缺失场景下会多 freeze 1~3 帧。这是文档级明文权衡——**正确性优先于延时**（§2 设计原则 #12），延时让位于"画面不出错"。

**统计**：每个 bit 累计 drop 计数 + 最近一次 drop 的帧 PTS，由 stats 暴露，用于诊断"为什么这一帧没显示"。

### 5.14 平台抽象层（PAL）

| 模块 | 职责 |
|---|---|
| Clock | QPC 封装 + ns 单位 |
| SPSC | 单生产单消费无锁队列 + cache-line padding |
| Thread | MMCSS 注册、亲和性、优先级 |
| Socket | IOCP + WSARecvFrom |
| Log / ETW | 多级日志 + ringbuf；TraceLogging 包装 |
| DXGI Caps Probe | adapter 枚举 + 各 codec/profile 探测，缓存能力快照 |
| WinHTTP Wrapper | 异步 POST/PATCH/DELETE 客户端，WHEP 信令专用 |
| MF Runtime | MFStartup + COM apartment 一次性初始化与生命周期 |
| Adapter Picker | 智能 GPU 选择（HWND 跟随 + 能力兜底） |

---

## 6. 队列与内存

### 6.1 队列规格（原理性数量级）

| 队列 | 类型 | 深度量级 | 满时策略 |
|---|---|---|---|
| RTP→JitterBuf | SPSC ring | 大（O(64)） | 丢最老 + 报告丢包 |
| JitterBuf→Depack | SPSC | 极低 | 丢最老 |
| Depack→Codec | SPSC | 极低 | 丢最老非参考帧 |
| Codec→Render | SPSC | 极低 | 丢最老 |
| Audio PCM ring | SPSC | 数十 ms | 写阻塞 |

**cache-line padding 必做**：否则高频 atomic 操作因 false-sharing 性能降 3-5×。

### 6.2 内存池

| 池 | 用途 | 容量原则 |
|---|---|---|
| RTP buffer pool | UDP/SRTP 接收 | 避免 malloc，固定数量 |
| MF 输入 sample 池 | MFT 输入 | 与 jitter buffer 深度同量级 |
| MFT 输出 surface 池 | hardware DPB + 输出缓冲 | 由 codec profile 决定，建议 6-8 |
| mc-libcodec NV12 buffer pool | 软解兜底输出 | 1080p NV12 ~3 MB / 帧，少量帧 |
| D3D11 Texture | MFT/D3D 内部管理 | 由 surface pool 上限驱动 |

---

## 7. 关键策略

### 7.1 丢帧与补偿

**视频**：

1. 解码前：帧过期（PTS 显著早于 audio_clock）直接丢。
2. 解码后：落后超过帧周期一定倍数即丢。
3. 解码 error flags 非 0 → 丢 + freeze。
4. 长时间无新帧：Freeze-Last-Frame + 状态图标。

**音频（NetEQ 三相位）**：

1. `buffer > target + 30 ms`：Accelerate（WSOLA 压缩 5–10%）。
2. `buffer < target − 10 ms`：Preemptive Expand（拉伸）。
3. 丢包：Normal Expand（PLC），<60 ms 维持；>200 ms 进 comfort noise + NACK。
4. G.711 简化：丢 <10 ms 直接重放上个包。

### 7.2 断线与恢复

#### 7.2.1 网络断线

**触发条件**：

- TCP 信令通道断开。
- UDP/SRTP 连续 1 s 无数据。
- RTCP SR 连续 10 s 未收到。

**恢复流程**：

1. 保留 D3D11 device、Swap Chain、WASAPI、MMCSS 注册。
2. 释放 Transport Session。
3. 清空 JitterBuf 数据（保留估计参数）。
4. 指数退避 + jitter 重连：1s ± 0.5s → 2s ± 1s → ... max 16s。
5. 重连成功后复用核心管线；首 RTCP SR 到达后重建 NTP-RTP 线性回归；首个可解码帧前保持 Freeze。

#### 7.2.2 Device Lost 全恢复

任一 D3D11 / DXGI 调用返回 `DEVICE_REMOVED` 或 `DEVICE_RESET`：

1. 销毁 MFT decoder（释放 IMFDXGIDeviceManager 引用）。
2. 销毁 swap chain、所有 view / texture 引用。
3. ClearState；释放 D3D11 device。
4. **DXGI factory 在 DEVICE_REMOVED 后变 stale，必须重建**。
5. 找回同一 adapter by LUID（如可能；否则触发智能重选 §7.3）。
6. 重建 D3D11 device + IMFDXGIDeviceManager。
7. 重建 MFT 或 mc-libcodec 软解。
8. 重建 swap chain。
9. 清空 jitter buffer（视频数据依赖旧 device）。
10. 发 PLI 请求新 I 帧。
11. 触发 device-lost / device-recovered 事件序列。

每帧通过 `IMFDXGIDeviceManager::TestDevice` 周期检测 TDR；任何 D3D11 / DXGI 调用都可能返回 device removed，需统一拦截。

#### 7.2.3 Audio Device Invalidated

`OnDefaultDeviceChanged` → flush WASAPI ring → tear down → 新 device 重建 → 复位 jitter target_delay。

### 7.3 智能 Adapter 选择

#### 7.3.1 启动期能力快照

枚举所有 IDXGIAdapter1，对每个 adapter 探测 H.264 / H.265-8 / H.265-10 / AV1 等关键 profile 的硬解支持与最大分辨率，缓存为能力快照并通过事件暴露。

#### 7.3.2 打开流时（已知目标 codec/profile/分辨率）

```
O1. 解析 HWND 所在显示器对应的 adapter（"preferred"）：
    HWND → MonitorFromWindow → 遍历每个 adapter 的每个 output → 匹配 HMONITOR → owning adapter
O2. if caps[preferred] 满足 codec 需求：使用 preferred → 同 device 直 scan-out，零跨 device 拷贝
O3. else 从其他 adapter 中按能力排序，选支持 codec 的最佳：
    dGPU > Iris Xe > 老 iGPU
    跨 device texture 共享通过 KeyedMutex（+0.5 ms 一次性代价）
O4. 仍无满足 → 退化到 mc-libcodec 子项目软解（CPU 路径）
O5. 用户显式覆盖 adapter LUID 时跳过 O1-O3 直接用指定 LUID
```

#### 7.3.3 运行期跨屏拖拽（double-buffered transition）

监听 `WM_DISPLAYCHANGE / WM_WINDOWPOSCHANGED` → 重新解析 owning adapter → 比对 LUID。LUID 变化时执行 soft adapter switch：在新 adapter 上预创建 D3D11 device + IMFDXGIDeviceManager，flush 旧解码器，新 device 出第一帧后切换 visual content，原 device 释放。冻结上限 800 ms，超时 fallback 旧 adapter 继续渲染并以 stats 报警告。

#### 7.3.4 设计理由

- HWND 跟随是「专业级低延时」的物理一致性最佳：同 device 直 scan-out 无跨 PCIe 拷贝、可触发 Independent Flip。
- 能力兜底覆盖多 codec 异构场景：UHD 630 + AV1 流自动跳 dGPU（如有）。
- 用户层无感，符合「专业播放器」隐形原则。
- 提供 LUID 显式覆盖路径以兼容硬性场景需求。

### 7.4 HEVC Extension 缺失自动软解兜底

Win10/11 retail 默认无 HEVC Video Extension（需用户从 Store 安装免费 OEM 版或 $0.99 零售版）。检测到 HEVC MFT 枚举为空时：

1. log 信息事件。
2. 自动启用 mc-libcodec H.265 软解路径（§5.7）。
3. 用户视角：H.265 流照常播放，仅 CPU 占用上升至 ~18%。
4. stats 暴露真实 active decoder kind。
5. UI 可选展示 hint：「检测到 HEVC 硬件加速不可用，使用软解码」。

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
| 解码 output 提取 | ~50 μs | ~5 μs |
| NV12→D3D11 拷贝（仅 SW） | n/a | ~0.5 ms |
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

### 9.1 探测原理

启动时枚举 IDXGIAdapter1，逐个 `D3D11CreateDevice` + `ID3D11VideoDevice::CheckVideoDecoderFormat`，对 H.264 NOFGT / HEVC Main / HEVC Main10 / AV1 Profile0 四类组合做布尔探测，缓存能力快照。

### 9.2 能力矩阵（normative）

| iGPU / dGPU | H.264 1080p | H.264 4K | H.265 8-bit 1080p | H.265 8-bit 4K | H.265 10-bit Main10 | AV1 |
|---|---|---|---|---|---|---|
| Intel UHD 620（KBL，7 代） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | ✗ |
| Intel UHD 630（CFL，8/9 代） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | ✗ |
| Intel UHD 730（ADL，12 代+） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ |
| Intel UHD 770（ADL，12 代+） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ |
| Intel Iris Xe（TGL，11 代）★ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ |
| AMD Vega 8/11（Ryzen APU） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | ✗ |
| AMD Radeon 660M+（Rembrandt APU，2022-01） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓（APU 维度首代 AV1；dGPU 维度 Navi 21 / RX 6000，2020-11，VCN 3.0 在前） |
| NVIDIA GTX 1050（Pascal） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | ✗ |
| NVIDIA RTX 3050+（Ampere+） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ |

> ★ Tiger Lake 11 代 Iris Xe 是 **Intel 首个**支持 AV1 硬解的架构（仅解码、无编码），不是 12 代 Alder Lake。

### 9.3 iGPU 延时对照（与 dGPU，内部实测）

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

- **HEVC Extension 缺失**（Win10/11 retail）→ mc-libcodec H.265 软解。
- **AV1 在 UHD 630 / Vega**：硬件不支持，软解延时 > 50 ms，仅作存档兜底。
- **多 GPU 系统选错 adapter**：智能选择因极端罕见的 driver bug 选错时，stats 暴露真实能力，用户可显式覆盖。

---

## 10. 部署与环境

### 10.1 硬件

| | 最低 | 推荐 | 极致低延时 |
|---|---|---|---|
| CPU | i3 8 代（含 UHD 630） | i5 12 代+（含 UHD 770） | i5 13 代+ + dGPU |
| GPU | UHD 620+ / Vega 8+ | UHD 770 / Iris Xe | RTX 3050+ |
| 内存 | 4 GB | 8 GB | 16 GB |
| 网络 | 100 Mbps | 千兆 | 千兆低抖动 + 关 URO/IM |
| 显示器 | 60 Hz | 144 Hz VRR | 240 Hz VRR + DisplayHDR |

纯 iGPU 最低部署：i3-8100 + UHD 630 + 8 GB + 千兆，可达 1080p H.264 端到端 < 100 ms。

### 10.2 软件

- Win10 1809+：H.264 OS 内置；H.265 取决于 HEVC Extension（缺失则 mc-libcodec 软解）。
- Win11 推荐：HEVC、AV1 Extension 通常可一并装；DCOMP 完整支持。
- DirectX 11（Feature Level 11.0+）。
- VC++ Runtime 2022。

### 10.3 依赖与 License

| 库 | License | 用途 | 大小 |
|---|---|---|---|
| Windows Media Foundation | OS | H.264/H.265/AAC 硬解 | 0 |
| mc-libcodec 子项目（自研） | Apache 2.0 | H.264/H.265 主流 profile 软解兜底 | ~3 MB |
| libdatachannel 0.24.x | MPL 2.0 | WebRTC PeerConnection | ~1.5 MB |
| libjuice（submodule） | MIT | ICE | bundled |
| libsrtp2 | BSD | SRTP | bundled |
| TLS backend（OpenSSL / GnuTLS / Mbed TLS 三选一） | 各自 | DTLS（libdatachannel backend） | ~0.5 MB |
| libopus | BSD-3 | Opus 解码（WHEP 音频） | ~0.7 MB |
| WinHTTP / DirectComposition | OS | WHEP HTTP 信令 / DCOMP 渲染 | 0 |

第三方 ~2.7 MB + 自研 mc-libcodec ~3 MB ≈ 5.7 MB 总体。

### 10.4 SEP 专利责任

> 注：MPEG-LA 与 Via Licensing 已于 2023-05 合并为 **Via Licensing Alliance LLC（Via LA）**，现行官网 [via-la.com](https://www.via-la.com/)。

| 路径 | 责任承担方 |
|---|---|
| 走 OS MFT 硬解 | Microsoft 已为 Windows 用户支付 license；mc-player 不承担 |
| 走 OS HEVC Extension（OEM 免费 / Store 付费） | 用户 / OEM 与 Microsoft / Via LA / Access Advance 之间；mc-player 不承担 |
| 走 mc-libcodec 软解 distribute | distributor 自负专利费。**Cisco OpenH264 模型**：源码 BSD-2-Clause（不含专利授权），Cisco 自付 AVC 专利费并允许第三方分发预编译二进制免费给最终用户；但 BINARY_LICENSE.txt 强制三项约束：①二进制必须由最终用户从 Cisco 服务器单独下载（**不得预捆绑发行**）、②应用须提供启用/禁用控制、③须含归属声明 "OpenH264 Video Codec provided by Cisco Systems, Inc."。mc-libcodec 不复刻该二进制下载机制，分发方需自评估专利暴露 |

商业分发场景需评估的现行 SEP 池（截至 2026-04，版图正在变化）：

- **AVC**：Via LA AVC pool 现行分级费率（2017–2025 条款，2026 起新签约者条款已变更）：前 10 万单位/年免费 → 超出后 $0.20/unit → 超 500 万单位降至 $0.10/unit；企业年度封顶 $9.75M/年。
- **HEVC**：
  - **Via LA HEVC pool — 2025-12-15 已被 Access Advance 收购**，新名称 **VCL Advance**（Video Codec Licensing，覆盖范围含 HEVC + VVC）。**过渡期暂与原 Access Advance HEVC pool 并行运营**，但 Access Advance 公开计划是 **2026 年底前合并为单一 program**，分发方需确认所交易池的最新治理实体与合并时点。
  - Access Advance HEVC pool（与 VCL Advance 处于过渡并行期）。
  - Velos Media — 2023 关闭联合 HEVC 许可项目，第三方专利返还原所有人；Marconi 仍保留约 400 件资产并通过个体许可执行，部分原成员后续自愿加入 Access Advance（个体决策，不是池间转移）。

Apache 2.0 §3 含明确专利授权 grant + 反诉终止条款，比 BSD-2-Clause（**不含**任何专利条款）对源码使用者专利风险更低；若需 BSD 阵营内的等价保护，应选 **BSD-2-Clause-Patent**（OSI 2017-04 批准、SPDX 维护、FSF 认 GPLv2 兼容）而非 BSD-2-Clause。

---

## 11. 风险与对策

只列**未被 §5 / §7 / §10 章节覆盖的剩余风险**——已经写入主流程的兜底策略不再重复。

| 风险 | 概率 | 影响 | 对策 |
|---|---|---|---|
| Windows 大版本行为破坏 | 低 | 功能退化 | CI 覆盖 Win10 1809 / 22H2 + Win11 23H2 / 24H2 / 25H2 |
| libdatachannel 维护体量小、DTLS 边缘 case | 中 | WHEP 偶发失败 | pin release tag；上游 CI 烟测每次 release |
| DCOMP 不进 Independent Flip（best-effort 路径） | 中 | 延时未达 ULTIMATE_DCOMP 预期 | PresentMon 实测 + stats 真实 present_mode；自动降至 EXTREME |
| WHEP draft-03 演进至 RFC | 低 | 互通失败 | 跟进 IETF；保留 PATCH 兜底；版本探测 |
| AV1 Extension 在 Win10 / 老硬件缺失 | 中 | AV1 流不能播 | stats 告警；mc-libcodec 后续追加 AV1 软解（路线在 §5.6.9） |
| H.264/H.265 SEP 专利责任（商业分发） | 低（自用） / 中（商业） | 法务 | README/UI 声明；distributor 自负；详见 §10.4 |
| mc-libcodec 合规覆盖率不足 | 中 | 极少数 stream 解码错误 | ITU-T JM/HM reference 100% bit-exact CI 闸 |
| URO / Interrupt Moderation 默认开 | 中 | +2 ms 延时 | stats 检测异常 + 安装文档 |

---

## 附录 A：参考规范与权威资料

**RFC 协议**：

- RFC 2326 RTSP 1.0（§1.4 引用 RFC 2069 Digest）· RFC 7826 RTSP 2.0（引用 RFC 7616 Digest）· RFC 3550 / 3551 RTP/RTCP · RFC 3611 RTCP XR
- RFC 4566 / 8866 SDP · RFC 4585 AVPF · RFC 4588 RTX · RFC 5104 CCM
- RFC 6184 H.264 Payload · RFC 7798 H.265 Payload · RFC 7587 Opus Payload
- RFC 3640 MPEG-4 (AAC, AAC-hbr) · RFC 6416 MP4A-LATM（与 3GPP PSS / 已部署 LATM 设备互通时使用；RFC 6416 自身建议新设计优先 RFC 3640）· RFC 8285 RTP Header Extensions
- RFC 8825 WebRTC overview · RFC 9143 BUNDLE · RFC 9429 JSEP
- RFC 5764 DTLS-SRTP（被 RFC 7983 / 9443 updated，未 obsoleted；DTLS 1.3 + SRTP 沿用本框架）· RFC 9147 DTLS 1.3 · RFC 9725 WHIP（已取代 draft-ietf-wish-whip）
- draft-ietf-wish-whep-03（datatracker 状态 "Expired & archived"，WG 标注 "Revised I-D Needed"，截至 2026-04 未升级 -04 未成 RFC；技术内容稳定）

**ITU-T / ITU-R / ISO**：

- ITU-T H.264 (ISO/IEC 14496-10) Annex D/E
- ITU-T H.265 (ISO/IEC 23008-2)
- ITU-T H.266 (ISO/IEC 23090-3)
- ITU-R BT.1359-1（A/V sync perception）
- ITU-R BT.709-6 / BT.2020-2

**Microsoft 官方**：

- Media Foundation Transforms · Asynchronous MFTs · MF_TRANSFORM_ASYNC_UNLOCK · CODECAPI_AVLowLatencyMode
- Supporting Direct3D 11 Video Decoding in Media Foundation
- DirectComposition · Composition Swap Chain
- DXGI flip model · Variable refresh rate displays · Handle device removed scenarios
- IAudioClient3 · IMMNotificationClient · WinHTTP API

**WebRTC / 工业实现**：

- libdatachannel (github.com/paullouisageneau/libdatachannel) — v0.24.x
- Chromium WebRTC `modules/video_coding/timing/frame_delay_variation_kalman_filter.cc`
- Chromium NetEQ `modules/audio_coding/neteq/{delay_manager, accelerate, preemptive_expand, expand}.cc`
- Chromium `modules/video_coding/nack_module2.cc` · `modules/rtp_rtcp/source/remote_ntp_time_estimator.cc`
- NVIDIA Video Codec SDK Programming Guide
- PresentMon (github.com/GameTechDev/PresentMon)

**对标 / 参考实现**：

- NDI Technical Requirements · SRT Protocol Specification · NVIDIA Reflex SDK
- OpenH264 (BSD-2) · libde265 (LGPL-3 / CC0) · x264 (GPL，仅 spec 参考)
- Intel oneVPL / Quick Sync Video media capability matrix

**SEP / License**：

- Via LA AVC / HEVC Patent Portfolio License — via-la.com（原 MPEG-LA）
- Access Advance HEVC — accessadvance.com
- Velos Media（部分专利权人 2023+ 转入 Access Advance）
- Cisco OpenH264 BINARY_LICENSE — openh264.org
- Apache License 2.0 §3 Patent Grant — apache.org/licenses/LICENSE-2.0

---

## 附录 B：对标表

| 方案 | 延时（LAN） | 延时（WAN） | 备注 |
|---|---|---|---|
| VLC 默认 | 1~2 s | 1~3 s | 保守 buffer |
| FFplay low-delay | 200~500 ms | 500 ms~1 s | |
| WebRTC 典型 | 100~200 ms | 150~300 ms | 自适应 jitter |
| WebRTC 极限调优 | 50~80 ms | ~150 ms | 参考目标 |
| NDI full bandwidth | ≈16-50 ms | n/a | 33 ms 单帧是这个量级 |
| NDI HX（H.264 压缩） | ≈80-200 ms | 需 NDI Bridge | 工业可用 |
| SRT default | 120 ms ARQ 窗 | 500 ms~2 s | |
| **mc-player RTSP + DCOMP + 240 Hz VRR** | **25~50 ms** | n/a | 极限档 |
| **mc-player WHEP + MediaMTX SFU** | 50~80 ms | 80~150 ms | 与 WebRTC 极限调优同档 |

---

## 附录 C：设计决策索引（ADR）

详细决策与依据见同目录 [`mc-player_ADR.md`](./mc-player_ADR.md)。本表仅列编号、标题与对应正文章节，便于交叉跳转。

| 编号 | 决策 | 正文章节 |
|---|---|---|
| ADR-001 | Media Foundation Transform 作主硬解后端 | §5.6 |
| ADR-002 | 硬件 MFT 走 async 事件驱动模型 | §5.6.2 / §5.6.3 |
| ADR-003 | dual-bind 通过 BindFlags 显式启用 + fence 同步 | §4.3 |
| ADR-004 | HEVC Extension 缺失自动 mc-libcodec 软解兜底 | §7.4 |
| ADR-005 | libdatachannel 作为 WebRTC 后端 | §5.2 |
| ADR-006 | mc-libcodec 自研 H.264 / H.265 主流 profile 软解 | §5.7 |
| ADR-007 | 智能 GPU 选择（HWND 跟随 + 能力兜底） | §7.3 |
| ADR-008 | DCOMP 第 4 档作为 best-effort 自动激活档 | §5.10.2 |
| ADR-009 | Opus 用 libopus（无 MFT 路径） | §5.8 |
| ADR-010 | mc-libcodec 拆为独立子项目 | §5.7.1 |
| ADR-011 | H.264 / H.265 SEP 专利责任声明 | §10.4 |
| ADR-012 | 智能 render profile 选择（默认 AUTO） | §5.10.1 |
| ADR-013 | 跨屏 adapter switch 用 double-buffered transition | §7.3.3 |
| ADR-014 | 正确性先于延时：Frame Validity Gate + Present Epoch | §5.13 / §5.10.5 |

---
