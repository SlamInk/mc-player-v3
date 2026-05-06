# mc-player — 多协议超低延时媒体播放器架构设计文档 v3.0

| 项目 | 内容 |
|---|---|
| 项目名称 | mc-player (Multi-protocol Client Player) |
| 文档类型 | 架构设计文档（ADD）—— 原理 / 架构 / 可行性 |
| 目标平台 | Windows 10 1809+ / Windows 11 x64 |
| 协议（一） | RTSP/RTP over UDP（含 interleaved TCP 降级） |
| 协议（二） | WebRTC WHEP（draft-ietf-wish-whep-03，IETF datatracker 注册；该 draft 已过期（expiration 2026-02-19），技术内容稳定，按 -03 实施） |
| 视频解码后端 | ADR-015 四级降级链：vendor SDK 直驱（NVDEC / oneVPL / AMF）→ DXVA-direct（D3D11VideoDevice）→ MFT hardware async（OS 标准抽象兼容档）→ mc-libcodec 软解（最终兜底） |
| 音频解码后端 | AAC：Microsoft AAC MFT；Opus：bundled libopus；G.711 a/μ-law：256-entry LUT 自实现 |
| WebRTC 栈 | libdatachannel 0.24.x（自 v0.18 起切换为 MPL 2.0；当前最新 0.24.2 / 2026-03；SRTP + DTLS + ICE） |
| 客户端内部延时 | 8~25 ms（含 DCOMP 极致档） |
| 端到端延时（LAN） | 35~75 ms（144 Hz VRR） / 25~50 ms（240 Hz VRR + DCOMP） |
| 子项目 | `subprojects/mc-libcodec/`（独立 target、独立 API、独立测试套件） |

> 本文聚焦**原理与架构**的设计陈述与**可行性**论证：分层、模块拓扑、数据流、关键算法的形式化原理、延时预算、硬件能力矩阵、依赖与 License、风险与对策。具体实施（API 头文件、参数清单、代码骨架、目录结构、迭代排期、测量工具命令）不在本文范围。

---

## 1. 概述

### 1.1 项目定位

mc-player 是 Windows 平台的多协议超低延时媒体播放器，对标 NDI full bandwidth（≈16-50 ms 单帧）与 WebRTC LAN 极限调优（50~80 ms）水平；NDI HX（H.264 压缩流）实际典型 80-200 ms，本播放器目标优于该量级。核心命题是把"用户感知到的复杂度"降到最低：用户给一个 URL（`rtsp://...` 或 `whep://...`），mc-player 自动选最佳 GPU、最佳渲染档位、最佳解码后端，无需任何配置。

**核心性能命题**：极限低延时 + 极限解码速度。每条数据通路按"硬件 path 最短"原则选择（§2 #5），解码后端按 ADR-015 四级降级链动态选档；任一档失败立即降级，不在 host 侧引入额外缓冲层级。

> 端到端延时由四方共同决定：**编码端 + 网络 + 客户端 + 显示器 scan-out**。客户端可控部分压到 8~25 ms；端到端 35~75 ms 的前提是编码器 low-delay、内网低抖动、现代显示器（120 Hz+ 优）+ DCOMP Independent Flip。

### 1.2 设计目标

| 维度 | 目标 | 前提 |
|---|---|---|
| 客户端内部延时（按 ADR-017 Preset 分档，warm_steady p95）| 各 preset 客户端内部小计以 §8.4 表为权威：SDI_REPLACEMENT 5~8ms / REALTIME_LAN 8~24ms / STREAMING_WIFI 15~46ms / WAN_FALLBACK 57~189ms（SDI_REPLACEMENT 需 5 子目标全命中） | 硬件 ∩ 网络 ∩ 编码器 三维 capability 联合命中对应 preset；详见 §3.5 / §8.4 |
| **单帧解码耗时（1080p@30，按 ADR-015 档位）** | 档1 vendor SDK ≤5 ms / 档2 DXVA-direct ≤8 ms / 档3 MFT-async ≤12 ms / 档4 libcodec ≤25 ms（SIMD 完备时） | 不含网络抖动与 render；caps_probe 选档后稳态值 |
| 端到端延时（按 Preset 与显示器） | SDI_REPLACEMENT (LAN+240Hz VRR+zerolatency 编码) ≤25ms / REALTIME_LAN (任 vendor+VRR) 30~45ms / 60Hz 通用 60~110ms | 编码端 low-delay + ADR-015 档 1/2 命中 + ADR-017 preset 命中；详见 §8.2 / §8.4 |
| 首帧渲染 | 最优 ≤200 ms / 一般 ≤500 ms / 大 GOP ≤2 s | sprop-parameter-sets / 等 IDR / GOP≥60 |
| WHEP 首帧 | LAN ≤600 ms / 公网 ≤1.2 s | 含 ICE+DTLS；公网含 NAT/TURN |
| CPU 占用（1080p@30，i5-12500） | 档1/2/3 硬解 ≤8% / 档4 mc-libcodec 软解 ≤18% | 单核含 SIMD |
| 丢包恢复 / 断流恢复 | <100 ms / ≤1 s 首次重连 | NACK+PLI / 指数退避 |
| 无独显场景（仅 iGPU） | 上述指标全部满足，优先档 1 oneVPL → 档 2 DXVA-direct | UHD 730/770、Iris Xe、Vega 全集显 |

> **延时基线说明**：四档解码差异主要来自 host 侧抽象层数（vendor SDK = 0 层 / DXVA-direct = D3D11 video API 1 层 / MFT = MFT 协议 + DXVA 2 层 / libcodec = CPU 全软解），硬件 decode unit 在前三档相同。**Preset 进一步把跨子系统决策（jitter / render / present 调度 / RTCP）联合优化**——同硬件档下不同 preset 可有 ≥10ms 客户端延时差（详见 §3.5 / §8.4）。"极限低延时"目标要求 Preset Selector 命中当前系统能落地的最激进 preset；档位降级在 stats 上明示（`decoder_kind` + `preset_active`），便于运维定位。

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
5. **硬件 path 最短优先**：解码按 ADR-015 四级降级链动态选档（vendor SDK > DXVA-direct > MFT hardware async > 软解），优先 host 侧抽象层数最少的档；合成 → DCOMP；时钟 → QPC；同步等待 → 高分辨率 Waitable Timer。本原则比"OS 标准抽象优先"权重更高——见 #10 例外口子。
6. **自适应**：jitter buffer、NACK 调度、丢帧策略动态调整（Kalman 滤波 / iat 直方图）。
7. **协议抽象**：核心管线协议无关；RTSP 与 WHEP 在 jitter buffer 之上完全合流。
8. **OS 协同**：MMCSS、共享音频引擎低周期、高精度等待原语。
9. **可观测**：关键路径 ETW；end-to-end stats 暴露端到端指标。
10. **平台原生 + 性能例外**：能用 Windows OS 自带能力时不引第三方。HTTP → WinHTTP，TLS → Schannel（libdatachannel 内部走 OpenSSL / GnuTLS / Mbed TLS 三选一作 DTLS 后端），时钟 → QPC，COM/D3D11/DCOMP 全部直调；解码默认走 OS 内置的 DXVA-DDI / MFT。理由：减小攻击面、避免第三方 license、跟随 OS 安全更新、优化器对 OS API 的深度优化。**例外口子**：当 #5"硬件 path 最短"或具体能力目标超出 OS 抽象层提供能力时，按需引第三方。已开口示例：libdatachannel（ADR-005，WHEP baseline）、libopus（ADR-009，WebRTC 事实标准 codec）、vendor SDK 直驱（ADR-015 档 1 + ADR-016 内置下载面板，硬解 path 最短的极致档）。例外不是默认值——必须在 ADR 里给出具体性能 / 能力依据。
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
│  Codec Bridge 四档降级链 (ADR-015，按 host 抽象层数从短到长)：│
│    [1] Vendor SDK (NVDEC / oneVPL / AMF) — 0 抽象层          │
│    [2] DXVA-direct (D3D11VideoDevice)    — 1 抽象层          │
│    [3] MFT hardware async                — 2 抽象层（兼容档）│
│    [4] mc-libcodec 软解                  — CPU 全软（兜底）  │
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

> Codec Bridge 统辖四档解码路径，档 1/2/3 出 D3D11 texture（直供 SRV，零拷贝），档 4 出 NV12 RAM buffer（dynamic texture 上传），渲染器对四档透明。
> mc-libcodec 是 monorepo 内的并列子项目，不在 mc-player 五层架构内，通过公开 API 接入 Codec Bridge。
> Vendor SDK 不预 bundle，由 ADR-016 内置下载面板按需获取（运行期 detect GPU vendor → 引导用户从 vendor 官网下载 → 缓存到本地）。

### 3.2 模块视图

```
  ┌──────────────┐
  │  App / UI    │
  └──────┬───────┘
         ↓
  ┌──────────────────┐
  │  Controller      │  生命周期 FSM · 智能 Adapter / Profile
  └──────┬───────────┘
         ↓
  ┌─────────────────────────────────────────────┐
  │  Transport Session                          │
  │  ts_rtsp_udp / ts_rtsp_tcp │ ts_whep        │
  │  (own UDP)                 │ (libdatachannel + WinHTTP；DTLS/SRTP 解密)
  └──────┬──────────────────────────┬───────────┘
         └──────── RTP datagram ────┘
                   ↓
  ┌──────────────────┐
  │  Adaptive Jitter │  ← 协议无关，两路在此合流
  └──────┬───────────┘
         ↓
  ┌──────┴──────┐
  ↓ Video       ↓ Audio
  H264/5 Depack   AAC / Opus / G.711 Depack
  (STAP-A/FU-A)
  ↓               ↓
  ┌─────────────────────────────────────────────┐
  │  Codec Bridge — ADR-015 四档降级链           │
  │  [1] vendor SDK (NVDEC / oneVPL / AMF)       │
  │      → vendor 私有 driver DDI → NV12 tex     │
  │  [2] DXVA-direct (D3D11VideoDevice)          │
  │      → 共享 D3D11 → NV12 tex（dual-bind）    │
  │  [3] MFT hardware async                      │
  │      → DXVA-DDI（含协议层）→ NV12 tex        │
  │  [4] mc-libcodec 软解 → NV12 RAM (上传 tex)  │
  │  libopus（WebRTC 音频，float PCM）           │
  └──────┬──────────────────────────┬───────────┘
         ↓ video frame              ↓ PCM
  ┌──────────────────┐              │
  │  Frame Validity  │ ← §5.13 协议无关门控（refs / params /
  │  Gate            │   recovery / color / reorder / fence）
  └──────┬───────────┘              │
         ↓                          ↓
  ┌──────────────┐         ┌──────────────┐
  │  D3D11 Render│◄────────┤  WASAPI      │
  │  4 档 + MPO  │ A/V 同步│ IAudioClient3│
  │  Present     │         │  设备切换    │
  │  Epoch §5.10.5│         │              │
  └──────────────┘         └──────────────┘

  RTCP Feedback ◄── jitter buffer (SR/RR/NACK/PLI)
    RTSP: own UDP   ·   WHEP: libdatachannel SRTCP
```

### 3.3 线程视图

| 线程 | MMCSS 任务 | 实际优先级 | 职责 |
|---|---|---|---|
| T0 App/UI | — | Normal | API、事件回调、Win32 消息泵 |
| T1 Signaling | — | Normal | RTSP 握手 / WHEP HTTP O/A、保活、状态机驱动 |
| T2 Network RX | **不挂 MMCSS**；TIME_CRITICAL + 绑 P-core | 15 | RTSP-UDP 收包 + 入 jitter buffer（WHEP 模式下被替代为 T7） |
| T3 RTCP | — | Normal | SR/RR/NACK/PLI（RTSP 路径走 own UDP；WHEP 路径走 SRTCP） |
| T4 Video Decode | **Playback** | ~23 | 按 ADR-015 选档：vendor SDK 事件循环 / DXVA-direct 同步驱动 / MFT 异步事件循环 / mc-libcodec 软解；出队纹理 / NV12 buffer |
| T5 Video Render | **Playback** | ~23 | D3D11 Present，含 DCOMP commit |
| T6 Audio Render | **Pro Audio** | ~26 | WASAPI 回调填缓冲 |
| T7 WHEP-PC | **不挂 MMCSS**；HIGHEST | 10 | libdatachannel 内部线程（DTLS/SRTP 解密、ICE 心跳） |

优先级在 `NORMAL_PRIORITY_CLASS` 下 `TIME_CRITICAL=15` / `HIGHEST=10`；T4/T5 走 MMCSS Playback (base ≈23)、T6 走 Pro Audio (base ≈26)。MFT worker 跑在 MMCSS 上需先创建 MMCSS-aware 共享 work queue 整体注册再绑定。WHEP 协议下 T2 由 libdatachannel 内部 socket 线程承担（应用层无需创建），RTSP 下 T7 不存在。T7 不挂 MMCSS 因 DTLS/SRTP 是 CPU 密集但非确定性时序敏感（对齐 Chromium 主线）。Intel 12 代+ T2/T5/T6 绑 P-core；libdatachannel 内部线程不主动绑。

### 3.4 控制流与数据流（Y 形）

两条 transport 在 jitter buffer 之上完全合流（depack / codec / render / sync / rtcp 协议无感），在之下完全分离（信令、socket、加密不同）。

```
RTSP                                WHEP
TCP 信令 + UDP RTP×2 + UDP RTCP×2   HTTPS (WinHTTP) + UDP SRTP×1 (BUNDLE)
                                    + DTLS (libdatachannel) + ICE (libjuice)
       │                                       │
       └──────────── RTP datagram ─────────────┘
                            ↓
                    Adaptive JB  ← 在此合流
                            ↓
                    ...一致管线...
```

### 3.5 Capability-Driven Preset 架构（顶层决策；ADR-017）

mc-player 的子系统决策不是"每个模块自己按 ADR 静态选档"，而是**四维 capability probe（硬件 ∩ 网络 ∩ 编码器 ∩ 渲染）联合驱动一个 Preset，由 Preset 协同配置整个管线**。这是与"先 mc_open 再各自硬绑参数"传统播放器架构最大的区别——决策点统一、子系统协同优化、运行期可热切。

> **维度标注（2026-04-30 修订）**：原 ADR-017 文档表述为"三维"，把显示器 / VRR / MPO 等 render 字段并入 hardware 维度；为避免 hardware vs render 字段归属混乱，capability_probe §3.5 已把 render 拆出独立 RenderSnapshot 作第 4 维。本节顶层图示遵循"四维"结构,字段定义以 capability_probe §3 / §3.5 / §4 / §5 为权威。

```
                    [ mc_open(url) ]
                          │
                          ↓
   ┌────────────── Capability Probe Suite (四维) ───────────┐
   │                                                         │
   │   [Hardware Probe]          [Network Probe]             │
   │   (启动期一次,~200ms)        (信令期+1~3s,~800ms)          │
   │   - GPU vendor              - RTT p50/p95/p99            │
   │   - DXVA profile 矩阵        - iat 抖动分布              │
   │   - vendor SDK 命中          - 短期丢包率                │
   │   - dual-bind / fence        - 链路类型推断              │
   │   - CPU / NIC                - ECN 标记                  │
   │                                                         │
   │   [Encoder Probe]           [Render Probe]              │
   │   (信令+第一 GOP,~800ms)      (启动期本地,~50ms)            │
   │   - codec/profile/reorder   - 显示器刷新率 / VRR         │
   │   - GOP 类型 / IDR/GDR 周期  - DCOMP / MPO planes        │
   │   - bitrate 模式             - render_profile 智能选档   │
   │   - zerolatency 推断         - NV12 直显支持             │
   │                              - HDR / EDID color gamut   │
   └────────────────────────┬────────────────────────────────┘
                            │
                            ↓
              ┌─── Preset Selector ───┐
              │  匹配最优 Preset       │
              │                       │
              │  SDI_REPLACEMENT      │ ← 极致档（NV+240Hz+LAN+0reord）
              │  REALTIME_LAN         │ ← 通用 LAN 档
              │  STREAMING_WIFI       │ ← Wi-Fi / 中等 RTT
              │  WAN_FALLBACK         │ ← 公网 / 高 RTT
              │  SAFE_MODE            │ ← 终档兜底
              └────────────┬──────────┘
                            │
                            ↓
              ┌── Apply Preset (一次性) ──┐
              │  - decoder tier 偏好       │ ← 输入到 ADR-015 选档
              │  - jitter mode 与 target   │ ← 输入到 §5.3 Kalman
              │  - render profile         │ ← 输入到 §5.10 选档
              │  - present 调度策略        │ ← race_to_display / immediate / vsync
              │  - RTCP 模式（reduced/std）│ ← RFC 5506 协商
              │  - Frame Validity Gate    │ ← refs/params/recovery/reorder 永远 strict
              └────────────┬──────────────┘
                            │
                            ↓ warm_steady
                            │
                   ┌─ Live Reload Loop ─┐
                   │  ADR-020 协议       │ ← §7.5
                   └────────────────────┘
```

**Preset 与各子 ADR 的关系**：

| 子 ADR | 在 Preset 体系中的角色 |
|---|---|
| ADR-007 智能 GPU 选择 | Hardware Probe 输入项 |
| ADR-008 DCOMP 第 4 档 | render profile 候选档（输入 MPO plane 数）|
| ADR-012 智能 render profile 选择 | render profile 候选档列表 |
| ADR-015 四级降级链 | decoder tier 偏好的输入；Preset 给"偏好"，最终仍按四级链 fallback |
| ADR-016 SDK 下载面板 | Hardware Probe 的获取手段（vendor SDK cache） |
| ADR-014 Frame Validity Gate | **不让步**——refs/params/recovery/reorder 四 bit 任何 preset 永远 strict；fence/color 在 SDI_REPLACEMENT preset（NV12 直显路径）下不参与决策（因该路径无 SRV 消费方），其余 preset 仍 strict（详见 §7.5.4 注 + capability_probe 设计 §6.3） |

具体三维 probe 的字段定义、Preset 数据结构、5 典型 Preset 详表与 Selector 算法见独立技术设计文档 [`mc-player_capability_probe_设计.md`](./mc-player_capability_probe_设计.md)。

---

## 4. 数据流

### 4.1 视频路径（四档解码降级链，ADR-015）

```
[网卡 DMA]
    ↓
[Kernel Socket Buffer]      大 SO_RCVBUF；URO 关闭；Interrupt Moderation 关
    ↓
[用户态 RTP 缓冲]    WSARecvFrom/IOCP (RTSP) 或 libdatachannel decrypt (WHEP)
    ↓ 解 RTP 头 + 扩展头 + 去 padding
[Adaptive Jitter Buffer]    动态 target_delay；NACK 调度
    ↓
[H.264/H.265 Depacketizer]  STAP-A/FU-A/AP/FU 重组；输出 Annex-B 字节流
    ↓
[Codec Bridge]  open 时按 ADR-015 四档降级链选档，本会话稳态运行：
  档 1：vendor SDK 直驱（NVDEC / oneVPL / AMF）
        — Annex-B AU → vendor 私有 driver DDI → D3D11 NV12 texture（互通注册）
        — host 抽象层数 0；首帧解码立即出
  档 2：DXVA-direct（ID3D11VideoDevice）
        — Annex-B → 应用层 DPB → D3D11VideoDecoderBuffer → DecoderEndFrame
        — D3D11 NV12 texture（dual-bind 直绑 SRV）；host 抽象层数 1
  档 3：MFT hardware async（OS 标准抽象兼容档）
        — CPU memcpy 入 MF buffer (~50 μs) → 异步事件驱动
        — ProcessInput/Output → D3D11 NV12 array slice；host 抽象层数 2
  档 4：mc-libcodec 软解（最终兜底）
        — 提交 Annex-B AU → NAL/CABAC/IDCT/MC/deblock
        — NV12 RAM → CPU→GPU 上传 D3D11 dynamic texture (~0.5 ms)
    ↓
[D3D11 Pixel Shader: NV12→RGB]  矩阵 / range 按 SPS VUI（§5.12 三级兜底）
    ↓
[Swap Chain Back Buffer]  4 档自动 COMPAT / BALANCED / EXTREME / ULTIMATE_DCOMP
    ↓ Present + ALLOW_TEARING
[显示器 scan-out]    8~16 ms @60Hz / ~7 ms @144Hz / ~4 ms @240Hz VRR

CPU 拷贝：档 1 0~1 次（vendor SDK 自管 staging）/ 档 2 0 次 / 档 3 1 次 / 档 4 2 次。
GPU→CPU readback：0（所有档）。GPU 内拷贝：0（dual-bind 命中）或 1（CopySub 兜底）。
```

### 4.2 音频路径

```
[UDP/SRTP 接收]
    ↓
[Adaptive Jitter Buffer]    iat 直方图 + NetEQ 三相位
    ↓
[Codec]
  AAC (RFC 3640)：MFT AAC Decoder 输入 Raw AAC + AudioSpecificConfig
  Opus (RFC 7587)：libopus opus_decode_float
  G.711 a/μ-law：256-entry LUT
  → 统一输出 float 32-bit interleaved
    ↓
[环形缓冲 3~20 ms]
    ↓
[IAudioClient3 Event Callback]   driver min period（典型 2.67~3 ms）
    ↓
[WASAPI Audio Engine]            共享模式 mix tail ~10 ms 固有
    ↓
[声卡 DAC]
```

MFT AAC Decoder 默认 16-bit PCM；本设计在 `SetOutputType` 显式协商 `MFAudioFormat_Float` 32-bit interleaved 避免后置重采样。

### 4.3 零拷贝原理（dual-bind）

零拷贝定义：**不存在 GPU→CPU readback，且 GPU 内部至多 1 次 NV12 array slice 复制**。

实现要点：

- **探测**：在目标 D3D11 device 上尝试创建一张 `BIND_DECODER | BIND_SHADER_RESOURCE` 的 NV12 texture，成功即说明 driver 支持 dual-bind。
- **启用**：在 MFT 输出流的属性集上显式请求该 BindFlags 组合（这是 hint，driver 可忽略；忽略时降级为单次 GPU 内拷贝路径）。
- **共享**：MFT 与 renderer 共享同一 ID3D11Device，并启用多线程保护，从而解码 texture 与渲染 texture 共生命周期、零跨 device 拷贝。
- **SRV 绑定**：texture 实为 array slice，SRV 用 `TEXTURE2DARRAY` 维度 + `FirstArraySlice` 指向真实子资源。
- **生命周期与 fence**：每个待显示帧持有 `IMFSample` 强引用直到 Present 完成；array slice 在 DPB 被 evict 前用 `ID3D11Fence`（D3D11.3+）等待 SRV 读完，避免 "slice 已回收但 shader 仍在采样" 的花屏。该 fence 信号同时是 §5.13 Frame Validity Gate 的 `gpu_fence_signaled` bit 来源。

driver 兼容性（截至 2026-04）：Intel Gen 9.5+（含 UHD 630）、AMD GCN 2.0+、NVIDIA Pascal+ 全部支持；老代驱动（pre-WDDM 1.2 已基本绝迹）回退到单次 GPU 内拷贝（CopySubresource intra-device blit）≈ 0.1~0.2 ms（区别于路径 B 的 CPU→GPU dynamic upload ≈ 0.5 ms）。

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
- **target_delay**：取直方图高分位数（典型 0.95，对齐 Chromium NetEQ `delay_manager.cc` Q30 定点常量）+ 一次平滑。
- **三相位**：buffer 高于 target+30 ms → Accelerate（WSOLA 压缩 5–10%）；低于 target−10 ms → Preemptive Expand（拉伸）；丢包 → Normal Expand（PLC）。
- **时钟跳变检测**（NTP 校时引发）：相邻 RTP 时间戳差超过阈值 → reset 估计参数。

#### 5.3.3 NACK 调度

按 Chromium `nack_module2.cc` 主线模型：

- 收到 RTP 包，比对 sequence number 期望值，缺失 → 加入 nack_list。
- 首次调度立即触发；后续重传调度按 `rtt × backoff_factor` 指数退避。
- 重发上限 10 次（`kMaxNackRetries`），按 RTT × backoff 退避自然累积至上百毫秒~秒级量级，超出后放弃 + 升级 PLI。

### 5.4 RTCP Feedback

RTCP 收发与协议解耦：通过一个发送 shim 抽象 transport 出口。

- **RTSP 路径**：shim 走 own UDP socket。
- **WHEP 路径**：shim 走 libdatachannel SRTCP 加密发送。

复合包构造：SR/RR + SDES + RTPFB-NACK + PSFB-PLI。AVPF 反馈策略采用 RFC 4585 的 **Immediate Feedback mode**（按组规模与 FB 阈值切换）配合 SDP `a=rtcp-fb:* trr-int 0`（默认）——前者保证 FB 不积压、后者不强制 Regular RTCP 最小间隔，二者是相互独立的两个机制，需同时满足才能近实时反馈。SR 接收并喂入 A/V 同步的 NTP-RTP 线性回归。

### 5.5 视频 Depacketizer

#### 5.5.1 H.264（RFC 6184）

支持 `packetization-mode=1`（non-interleaved）：单 NAL（1-23）、STAP-A（24）、FU-A（28）；缓存 SPS(7)/PPS(8)/IDR(5) 作为 extradata；SEI(6) 透传含 recovery_point GDR。**SDP answer 阶段拒绝 `packetization-mode=2`**（即拒绝 STAP-B/MTAP/FU-B：25/26/27/29），避免 DON 重排复杂度——STAP-A + FU-A 足以覆盖现网 IPC / NVR / WebRTC SFU。

帧边界识别：RTP timestamp 变化 或 marker bit。

#### 5.5.2 H.265（RFC 7798）

支持 VCL 单 NAL（0-31）、AP 聚合（48）、FU 分片（49）；缓存 VPS(32)/SPS(33)/PPS(34) 作为 extradata；Prefix/Suffix SEI（39/40）透传含 recovery_point GDR；EOS/EOB（36/37）上报流结束事件。**不支持** PACI(50)。归属：NAL Type 0-40 由 ITU-T H.265 Table 7-1 定义；41-47 ITU 保留；48-63 unspecified，RFC 7798 §1.1.4 在该范围占用 48 (AP) / 49 (FU) / 50 (PACI)。

**H.265 NAL header 是 2 字节**（F(1) + Type(6) + LayerId(6) + TID(3) = 16 bits），FU 重组与 H.264 的差异：FU 携带 1 字节 FU header，重组时前置 2 字节重建 NAL header，需保留 LayerId/TID、按 FU header 内的 type 字段重组 Type 域。

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

### 5.6 视频 Codec Bridge — 四级硬件降级链（ADR-015）

#### 5.6.1 总体原理与降级链

Codec Bridge 是协议无关解码抽象层，按 ADR-015 定义的四级降级链动态选档，原则是**硬件 path 最短优先**（host 侧抽象层数从短到长）。open 流时逐档探测，第一个通过的档启用为本会话稳态；运行期任一档出错（device removed / TDR / profile unsupported）按 §5.6.5 状态机走全恢复，必要时跨档降级。

| 档 | 后端 | host 抽象层数 | 触发条件 | 退档条件 |
|---|---|---|---|---|
| 1 | Vendor SDK 直驱（NVDEC / oneVPL / AMF） | 0（vendor 私有 driver DDI） | GPU vendor 匹配 + SDK redistributable 已存在（ADR-016）+ codec/profile 支持 | SDK 不存在 / vendor 不匹配 / probe 失败 |
| 2 | DXVA-direct（`ID3D11VideoDevice`） | 1（D3D11 video API） | `CheckVideoDecoderProfile` + `CheckVideoDecoderFormat` 通过 | profile 不支持 / config 不可用 |
| 3 | MFT hardware async | 2（MFT 协议 + DXVA-DDI） | `MFTEnumEx(HARDWARE\|ASYNCMFT)` 返回 ≥1 + `MFT_ENUM_HARDWARE_URL_Attribute` 非空 + `MF_TRANSFORM_ASYNC=1` | 仅枚举到 sync software MFT 即降级（不当作硬解） |
| 4 | mc-libcodec 软解 | CPU 全软（SIMD） | `allow_software_decode=1`（用户显式禁用时直接报 `MC_ERR_NO_HARDWARE`） | 不再降级，最终兜底 |

**输出契约（跨档统一）**：档 1/2/3 输出 D3D11 NV12 texture（dual-bind shader resource view 直采，零拷贝）；档 4 输出 NV12 RAM buffer，render 端通过 dynamic texture 上传走与硬解一致的 SRV 路径。renderer 对四档透明，仅看 `VideoFrame` 结构。

**降级一次性原则**：每次 `mc_open` 内部仅做一次降级链探测，确定档位后稳态运行；运行期错误恢复优先在档内复位（flush + 重新协商 input/output type），仅 device-lost 与 profile 永久失支才触发跨档降级。这保证从 open 到首帧的路径是确定的，没有 fallback 抖动（§2 #11 能力探测原则的具体执行）。

#### 5.6.2 各档实施原理

##### 5.6.2.1 档 1 — Vendor SDK 直驱

按运行期 GPU vendor 选其一：

- **NVIDIA：NVDEC（Video Codec SDK）** ——`cuvidParseVideoData` 推送 Annex-B；`cuvidDecodePicture` 触发硬件解码；`cuvidMapVideoFrame` 拿到 CUDA device pointer 后通过 `cudaGraphicsD3D11RegisterResource` 与 mc-player 的共享 D3D11 NV12 texture 互通，避免 CUDA→D3D 拷贝。NVDEC driver 内部维持 4 帧 pipeline 但首帧立即解码（NVIDIA 文档明示 "this pipeline does not imply any decoding delay"）；low-latency 模式由 `CUVIDPARSERPARAMS::ulMaxDisplayDelay = 0` 启用（zero-display-delay parser），配合每个输入包置 `CUVID_PKT_ENDOFPICTURE` 标记完整 AU 边界，迫使 parser 拿到一帧立即出。
- **Intel：oneVPL（原 Media SDK 后继）**——`MFXVideoDECODE_DecodeHeader` 解析 SPS/PPS；`MFXVideoDECODE_DecodeFrameAsync` 推帧；`mfxFrameSurface1` 直接拿到 D3D11 texture handle（VPL 内部用 D3D11VA）。decoder 真正的 low-latency 配置：`mfxVideoParam.AsyncDepth = 1` + 每个输入 bitstream 置 `mfxBitstream.DataFlag |= MFX_BITSTREAM_COMPLETE_FRAME`（不写则 decoder 等下一帧到达才知边界，强制额外 1 帧延时）。注意 `LowDelayBRC` / `LookAheadDepth` 等是 encoder rate-control 参数，与 decoder 无关。
- **AMD：AMF（Advanced Media Framework）**——`AMFComponent::Submit/QueryOutput` 异步管线；`AMFSurface` 拿到 D3D11 texture；启用 `AMF_VIDEO_DECODER_REORDER_MODE = AMF_VIDEO_DECODER_MODE_LOW_LATENCY`（属于 `AMF_VIDEO_DECODER_MODE_ENUM` 的 `REGULAR / COMPLIANT / LOW_LATENCY` 三档；`LOW_LATENCY` 要求流无 reorder，B/P 帧不可造成重排）。

**运行期 detect**：`DXGI_ADAPTER_DESC1::VendorId`（NVIDIA=0x10DE / Intel=0x8086 / AMD=0x1002）→ probe 对应 SDK 库（`nvcuvid.dll` / `libmfx.dll` 或 `vpl.dll` / `amfrt64.dll`）→ 库存在则 LoadLibrary + GetProcAddress 动态绑定，缺失则降到档 2。**SDK redistributable 不预 bundle，由 ADR-016 内置下载面板按需获取**（§5.6.6）。

##### 5.6.2.2 档 2 — DXVA-direct（`ID3D11VideoDevice`）

绕开 MFT 协议层，直接调 D3D11 video API：

1. `ID3D11VideoDevice::GetVideoDecoderProfile` 枚举 → 匹配目标 codec profile（H.264：`D3D11_DECODER_PROFILE_H264_VLD_NOFGT`；H.265 Main：`D3D11_DECODER_PROFILE_HEVC_VLD_MAIN`；H.265 Main10：`D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10`；AV1 Profile 0：`D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0`）。
2. `CheckVideoDecoderFormat` 验证 NV12 输出格式支持。
3. `GetVideoDecoderConfig` 选 `ConfigBitstreamRaw=1`（slice 短格式）。
4. 应用层自管 DPB：`{frame_num, poc, refs_used_by, mark}` 表，参考 mc-libcodec §5.7.3 实现。
5. 每帧：`DecoderBeginFrame(view, ...)` → 填 `D3D11VideoDecoderBuffer` (PicParams / Bitstream / SliceControl) → `SubmitDecoderBuffer` → `DecoderEndFrame`；GPU 完成后用 `ID3D11Fence` 等 SR 读取就绪再 emit。

**优势**：（a）frame 留在 video memory，与 §4.3 dual-bind 零拷贝原生兼容；（b）不依赖 MFT category 注册，绕开 IoT LTSC 缺 MediaPlayback feature 的环境约束；（c）应用层完全控 DPB，能精确按 SPS reorder 配置 buffer 深度而非 driver 黑盒决定。

**复用**：本档与 mc-libcodec §5.7.3 共用 SPS/PPS/SliceHeader parser（H.264 与 H.265 解析层抽到 mc-libcodec 公开 API），把语法元素填进 DXVA picparams 结构。

##### 5.6.2.3 档 3 — MFT hardware async（OS 标准抽象兼容档）

仅当档 1/2 探测均失败但 driver 暴露了 hardware MFT 时启用（实践中很罕见——driver 既然提供了 hardware MFT，几乎一定也提供了 D3D11VA DDI）。

**激活原则**：**硬件 MFT 永远 async**（Microsoft Learn "Hardware MFTs" 原文 "Hardware MFTs must use the new asynchronous processing model"——协议要求而非性能选择；sync 调用模式不被支持），激活时显式 unlock async + 用事件生成器驱动；通过 `IMFDXGIDeviceManager` 把 renderer 的 `ID3D11Device` 注入 MFT 确保同 device。**sync software MFT 不在本档接受范围**——它由 ADR-015 直接降到档 4。

**异步事件循环**：三类事件 `METransformNeedInput`（从 jitter 取 AU 喂入）/ `METransformHaveOutput`（拉 sample 发 renderer）/ `METransformDrainComplete`（flush 完成），处理后 re-arm。零拷贝路径：输出 sample 的 D3D11 buffer 通过 `IMFDXGIBuffer` 拿到原生 texture + array slice index；driver 不支持 dual-bind 时 fallback CopySub 到自管 SR-only pool。

##### 5.6.2.4 档 4 — mc-libcodec 软解兜底

mc-libcodec（§5.7）以同步 API 推 Annex-B AU、拉 NV12 RAM buffer。NV12 RAM 走 dynamic texture 上传走与硬解一致的渲染路径，对 renderer 透明。**性能取决于 §5.7.5 SIMD 实装进度**——SIMD 不达标时无法保证 §1.2 表中"档 4 ≤25ms"的延时基线，运维需在 stats 上监测 `decoder_kind=LIBCODEC` 与 `e2e_latency_p95`，超阈值时考虑降级到更轻 codec/分辨率或排查档 1/2/3 的 probe 失败原因。

#### 5.6.3 各档低延时配置不变量

跨档共用：**禁 reorder buffer**（除非检出 B 帧，见 §5.6.4 Policy）+ **显式启用对应硬件 path**（防 driver 静默 fallback 软解）+ **颜色元数据必须显式设**（nominal range / YUV matrix / transfer function；不设则 renderer 误判，§5.12）。

各档差异（速查表;详细原理与代码级使用见 §5.6.2.* 各小节）：

- **档 1（vendor SDK）**：NVDEC 用 `CUVIDPARSERPARAMS::ulMaxDisplayDelay = 0` + 每包 `CUVID_PKT_ENDOFPICTURE`；oneVPL 用 `AsyncDepth = 1` + `mfxBitstream.DataFlag |= MFX_BITSTREAM_COMPLETE_FRAME`；AMF 用 `AMF_VIDEO_DECODER_REORDER_MODE = AMF_VIDEO_DECODER_MODE_LOW_LATENCY`。**权威源**：本节速查 + §5.6.2.1 详细原理共同构成档 1 配置规范,其他文档（capability_probe §6.1 / 重构方案 Phase 5-7 / CLAUDE.md）以本节为准引用。
- **档 2（DXVA-direct）**：surface pool 上限动态按 `max_num_reorder_frames + 2` 设（H.264）或 `sps_max_num_reorder_pics + 2`（H.265）；无 reorder 流时收紧到 4-6（兼顾管线深度）。
- **档 3（MFT hardware async）**：`CODECAPI_AVLowLatencyMode` 启用低延时模式。**Microsoft H.264 decoder 该属性用 `VT_UI4`（UINT32）是 MS Learn AVLowLatencyMode 页 "Warning" 块明示的特殊实现**；其它 codec（H.264 encoder、H.265 decoder/encoder）按惯例用 `VT_BOOL` 但**未在 MS 文档中明示**，SetValue 前应实测验证。surface pool 上限典型 6-8。
- **档 4（mc-libcodec）**：`single-thread` 模式下天然单帧延时；可选 frame-level threading 仅 4K 离线兜底场景启用（+8~20 ms 排序窗口）。

#### 5.6.4 B 帧检测与 Policy（跨档共用）

低延时管线必须在 open 前确认有无 B 帧重排序：

1. **优先（零成本，SDP 层）**：解析 `profile-level-id`。Constrained Baseline / Baseline / Constrained High 必无 B 帧，可直接判定。
2. **兜底（in-band SPS）**：用最小 Exp-Golomb 解码器扫到 VUI 后立即退出，读 `max_num_reorder_frames`（H.264）或 `sps_max_num_reorder_pics`（H.265），等于 0 才确切无 B 帧 / 重排。

**B-Frame Policy（正确性优先）**：检出 reorder > 0 时强制采用以下措施，否则与 §5.6.3 各档"禁 reorder buffer"配置直接冲突 → **必然花屏**。措施按档分流：

- **档 1（vendor SDK）**：NVDEC 取消 `ulMaxDisplayDelay = 0`（恢复 driver 默认 reorder window，让 parser 自管 picture buffer 深度），`CUVID_PKT_ENDOFPICTURE` 仍按包边界置位；oneVPL 取消 `MFX_BITSTREAM_COMPLETE_FRAME` hint，由 decoder 按 SPS `max_num_reorder_frames` 自管 reorder；AMF 切到 `AMF_VIDEO_DECODER_REORDER_MODE = AMF_VIDEO_DECODER_MODE_COMPLIANT`（允许 reorder，符合 spec 的稳态）。
- **档 2（DXVA-direct）**：DPB 表深度抬到 `max_num_reorder_frames + 2`（H.264）或 `sps_max_num_reorder_pics + 2`（H.265），按 POC 排序后 emit。
- **档 3（MFT hardware async）**：取消 `CODECAPI_AVLowLatencyMode`（不再 disable reorder buffer），surface pool 上限同步抬高。
- **档 4（mc-libcodec）**：DPB + reorder buffer 是软解必经路径（§5.7.3），无需 Policy 切换；天然按 reorder 配置工作。

跨档统一：stats 上报 `reorder_cost_ms`（典型 +30~60 ms）；不在 codec 层悄悄做权衡，由用户感知该流不适合极致延时档。§5.13 Frame Validity Gate 的 `reorder_resolved` bit 由 codec bridge 在 reorder buffer 排序完成时置位。

#### 5.6.5 错误恢复状态机（跨档共用）

错误码按档分流，但收敛到统一的"档内复位 → 跨档降级 → device lost 全恢复"三级响应：

**档 3（MFT hardware async）—— 原 ProcessOutput 状态机保留**

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

**档 1（vendor SDK）**：NVDEC 的 `CUDA_ERROR_INVALID_VALUE` / oneVPL 的 `MFX_ERR_INCOMPATIBLE_VIDEO_PARAM` / AMF 的 `AMF_NEED_MORE_INPUT` 等 vendor 特定错误按"档内 reset → 不可恢复降到档 2"两级处理；device removed 等 D3D 层错误同样上交全恢复。

**档 2（DXVA-direct）**：`DecoderBeginFrame` 失败 / `SubmitDecoderBuffer` 返回 `E_INVALIDARG` 等先尝试重置 DPB + 重新 `CreateVideoDecoder`，连续 3 次失败降到档 3 / 档 4。

**档 4（mc-libcodec）**：纯 CPU 路径，主要错误是参考帧丢失（FrameNum gap > 0 / POC 不连续，§5.7.3）→ 标 invalid + PLI；不再降级（已是终档）。

**跨档统一**：任意 FAILED 或输出携带 decode error flag → 标记本帧及所有 outstanding 待显示帧 invalid，由 §5.13 Frame Validity Gate 统一丢弃；解码器侧不再尝试"输出但加 freeze"的兜底路径，避免错误帧因竞态被 Present。

**永久性失败的跨档降级**：profile 不支持 / hardware decode unit 永久失能（如 driver 重新签发后 D3D11VA profile 列表收缩）→ 触发 `mc_open` 重入式降级链探测，从当前档之后逐档尝试。降级触发由 stats 事件上抛，调用方决定是否提示用户。

#### 5.6.6 Hardware Decode Component Manager（HDCM；ADR-016 + ADR-021 实现）

档 1 启用前置（vendor SDK redistributable）+ 档 3 启用前置（MediaPlayback feature / HEVC 或 AV1 Video Extension）+ 档 2/3 driver-side 能力（GPU driver 版本）共同决定四级链各档实际覆盖率。综合方案：**应用内置 Hardware Decode Component Manager（HDCM），按"用户层零命令"原则统一管理四类硬解组件、三种 in-app 安装模式**——用户既不需要跑 PowerShell，也不需要外跳 Microsoft Store 客户端。

**四类组件 / 三种安装模式**：

| 类别 | 组件 | 触发档位 | in-app 安装模式 | 缓存位置 |
|---|---|---|---|---|
| A | Vendor SDK redistributable（NVDEC / oneVPL / AMF） | 档 1 | WinHTTP 异步下载 + SHA-256 + Authenticode；进度条全程在 app 内（沿 ADR-016） | `%LOCALAPPDATA%\mc-player\sdk\<vendor>\<version>\` |
| B | Microsoft Store 媒体扩展（HEVC / AV1 Video Extension） | 档 3 codec 覆盖 | UWP `Windows.Services.Store::StoreContext::RequestDownloadAndInstallStorePackagesAsync`（C++/WinRT interop）；用户视角 = app 内进度条，底层 Store 客户端在线安装 | OS 受管，app 不持有 |
| C | Windows Optional Feature（MediaPlayback feature） | 档 3 整档可用 | helper.exe（`requireAdministrator` manifest）+ `DismApi::DismEnableFeature(L"MediaPlayback", ...)`；UAC 弹窗 → helper 执行 → 完成提示重启 | OS 受管，app 不持有 |
| D | GPU driver | 档 2 + 档 3 driver-side 能力 | 检测当前 driver 版本 vs 编译期内嵌"minimum recommended"阈值；落后即一键跳浏览器到 vendor 官网驱动页（**driver setup.exe 不在 app 进程内运行**——侵入式系统操作超出播放器边界；工程边界与具体 API 详 ADR-021 类别 D + hdcm 设计 §2.4）| vendor installer 自管 |

**HDCM 状态机**（每个组件三态 `already_installed` / `installable` / `unavailable_on_this_sku`）：

```
启动期 detect:
  for each component in {A_NVDEC, A_oneVPL, A_AMF, B_HEVC_Ext, B_AV1_Ext, C_MediaPlayback, D_GPU_Driver}:
    state = probe(component)
    ├─ already_installed     → 隐藏入口（不打扰）
    ├─ unavailable_on_this_sku → 隐藏入口
    │                            （IoT LTSC 无 Store → B_* 隐藏；GPU vendor=Intel → A_NVDEC/A_AMF 隐藏）
    └─ installable           → 入口可见，默认折叠到"硬解组件"面板（用户主动展开）

用户主动展开面板（按类别显示组件 + 当前状态 + 一键安装按钮）:
  A: 点击 → in-app 下载进度 + SHA-256 + Authenticode 校验 + 解压到缓存
  B: 点击 → Store API 调起 + in-app 进度（Store 后端在线下载 + install）
  C: 点击 → ShellExecuteEx(verb="runas") → UAC 弹窗 → helper.exe 跑 DISM → 完成对话框（"立即重启 / 稍后重启"）
  D: 点击 → ShellExecuteEx 打开默认浏览器 → vendor 官网驱动页

每次成功 / 失败:
  上报 mc.hdcm.install_attempt_count{type, id, result}
  类别 A/B 成功 → 下次启动 probe 自动命中 → ADR-015 档 1/3 直接启用
  类别 C 成功 → 用户重启电脑后 MediaPlayback feature 注册 → 档 3 MFT subsystem 可用
  类别 D 成功 → 用户在浏览器完成 vendor installer + 重启后 driver-side 能力刷新
  失败 → 面板提示重试或跳过；启动不阻断（保持 ADR-016 既定行为）
```

**实现约束**：

- 类别 A 下载源限定 vendor 官网直链（`developer.nvidia.com` / `intel.com/oneapi` / `gpuopen.com/amf`）；不引第三方 redistributable 仓库。SHA-256 编译期 hardcoded，每次升级 mc-player 一并升级。
- 类别 B 通过 UWP `Windows.Services.Store` C++/WinRT interop 调用，不引 .NET / 第三方 wrapper；Store API 仅在 Store 客户端就绪 SKU 工作，IoT LTSC 等裁剪 SKU 自动归 `unavailable_on_this_sku`。HEVC Video Extension 付费 license 由 Store API 与 Microsoft 账户体系管理，mc-player 不持有 license token。
- 类别 C helper.exe 是与主 app 同 vendor 签名的轻量 PE，仅承担 elevation + DISM 调用 + 命名管道 / 退出码 IPC；**不带网络能力**（最小攻击面）。DISM 调用后判断 `DismRestartRequired_Possible` / `_Required`，若需重启则 app 主界面给"立即重启 / 稍后重启"对话框，不静默吞下。
- 类别 D 仅检测 + 跳浏览器；driver 版本阈值表编译期内嵌（按 codec / profile 分别给最低推荐版本），每次 mc-player 升级一并更新。**driver setup.exe 永不在 mc-player 进程内运行**——driver 安装期会切显卡 / 黑屏 / 通常要重启，与播放器进程生命周期严重耦合时风险过高，归 vendor 自家工具。
- HDCM 默认 log level `silent`；运维需要诊断时按 §10.5 / `hardware-decode-dependencies.md` 排障路径调高 log level → 复现 → 用户导出 log，无主动 collector（避免企业部署合规摩擦）。

**与 ADR-005/009 的对比**：libdatachannel / libopus 是 baseline 能力（无之则 WHEP / Opus 不可用），bundle 合理；HDCM 管理的四类组件均是**可选性能增强**——无之则按 ADR-015 四级降级链兜底（档 1 不可用降档 2 / 档 3 OS 缺失走 vendor SDK / DXVA-direct / libcodec），按需 in-app 安装更对应其角色。

#### 5.6.7 AV1 / H.266 路线

| Codec | 档 1 vendor SDK | 档 2 DXVA-direct | 档 3 MFT | 档 4 libcodec | 硬件支持（2026） |
|---|---|---|---|---|---|
| AV1 | NVDEC ≥ Ampere / oneVPL ≥ Tiger Lake / AMF ≥ RDNA2 | `D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0` | Microsoft Store 装免费 AV1 Video Extension 后可用 | 暂未实装（dav1d 参考） | Intel Tiger Lake 11 代+、AMD RDNA2+、NVIDIA Ampere+ |
| H.266/VVC | 暂无 vendor SDK 暴露 | 暂无 D3D11VA profile | 暂无 MFT | 暂未实装 | Intel Lunar Lake LP 试探性 |

AV1 在四档间的探测顺序与 H.264 / H.265 一致；H.266 待 OS / vendor 支持，mc-libcodec 子项目结构允许后续追加。

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

H.264：Baseline / Constrained Baseline / Main（含 CABAC、B 帧）/ High（含 8×8 transform）当前支持；High 10 / 4:2:2 / 4:4:4 留后续。H.265：Main / Main10 当前支持；Main12 / Main 4:4:4 留后续。

#### 5.7.5 SIMD 策略

SSE2 baseline（x86_64 ABI 强制无需 CPUID，覆盖 IDCT / MC 插值 / deblocking 主路径）+ AVX2 runtime CPUID detect（覆盖 H.265 SAO / 大块 IDCT / 批量小块）。不做 AVX-512（消费 CPU 覆盖率低、降频代价大）/ NEON（仅 Windows x64）。dispatch 启动期填表，hot loop 零 indirect call。

#### 5.7.6 多线程模型

默认 single-thread（延时最低、可预测，符合超低延时定位）；可选 frame-level threading 仅 4K 离线兜底场景启用（+8~20 ms 排序窗口）；不做 slice / tile 并行。

#### 5.7.7 与第三方实现的关系

不依赖第三方解码库代码。可参考算法描述：OpenH264（BSD-2，SIMD/CABAC 思路）、libde265（LGPL-3，H.265 模块拆分/SAO）；x264 仅作 spec 参考不读源码（避免 GPL 污染）；正确性基准对齐 ITU-T JM/HM reference（BSD-3，CI 100% bit-exact 闸）。

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

符号沿用 ITU-R BT.1359-1：`av_offset = audio_time − video_time`，正值音频领先。

| 阈值 | 音频领先 | 音频滞后 |
|---|---|---|
| 察觉（detectability） | +45 ms | −125 ms |
| 可接受（tolerance） | +90 ms | −185 ms |

策略保守收紧到 ±15 / −25 ms：`−25 ≤ av_offset ≤ +15` 立即渲染；`> +15`（视频迟于音频主时钟）丢帧；`< −25`（视频 PTS 早于音频）延迟渲染。

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

> ULTIMATE_DCOMP 是 best-effort：Microsoft 未声明 DCOMP + ALLOW_TEARING 必然进 Hardware Composed: Independent Flip——只有系统授予 hardware overlay plane 时才升级，否则落 `Composed: Flip`，运行时由 PresentMon 验证。真正卖点是 DCOMP 多 visual 的 **plane 分离能力**：video visual 走 ALLOW_TEARING、HUD visual 走 vsync 互不撕裂；具体每个 visual 是否拿独立 hardware plane 由 DWM + 显卡驱动综合判定（受 Z-order / 像素格式 / 缩放 / alpha 约束），不当作必胜档。

#### 5.10.3 Waitable Swap Chain（4 档共有）

`SetMaximumFrameLatency(1) + GetFrameLatencyWaitableObject + WaitForSingleObjectEx` 是延时控制核心。两条不变量：

- **ALLOW_TEARING 一致性**：标志必须在 Create / ResizeBuffers / Present 三处一致出现，否则运行时拒绝。
- **ResizeBuffers 前清场**：ResizeBuffers / device-lost 全恢复前必须先 `ImmediateContext::ClearState` 并显式释放所有引用 back buffer 的 SRV/RTV/UAV，否则 ResizeBuffers 返回 `DXGI_ERROR_INVALID_CALL`，常表现为窗口 resize / 跨屏拖拽后**陈旧内容残留**或黑屏。该路径由 §5.10.5 Present Epoch watchdog 兜底强制 redraw。

#### 5.10.4 PresentMon 验证

EXTREME 期望 `Hardware: Independent Flip`；ULTIMATE_DCOMP 期望 `Hardware Composed: Independent Flip`，允许降级 `Composed: Flip`（仍正确，少 1 帧合成节省）。运行期通过 stats 字段实时上报实际 PresentMode。

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

- **模式**：默认共享（IAudioClient3，事件驱动）；可选独占以消除 mix tail（要求设备格式精确匹配）。
- **共享模式 buffer period**：取 `GetSharedModeEnginePeriod` 的 driver-reported `pMinPeriodInFrames`；HDAudio 典型下限 128 frames @ 48 kHz ≈ 2.66 ms；default engine period 约 10 ms。
- **AutoConvert**：开启 PCM auto-convert + default-quality SRC，由 WASAPI 完成设备 mix 转换。
- **设备切换（拔耳机）**：`IMMNotificationClient::OnDefaultDeviceChanged` → tear down → 新设备 rebuild。

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

**HDR transfer 当前行为（PQ / HLG，v1 不做 tone mapping）**：检出 `transfer_characteristics ∈ {16, 18}` 时按 BT.2020 矩阵解 YUV→RGB（避免色度旋转错误），但 EOTF 留作线性近似不做 PQ/HLG → SDR 的 tone mapping，输出在 SDR 显示器上整体偏暗、亮部细节压缩；不拒绝该流，保证可见。stats 暴露 `hdr_downgrade=true` + 实际 transfer 编号，用户可感知该流需要后续 HDR milestone 才能正确显示。HDR 输出（Independent Flip + scRGB / HDR10）整体留 milestone，与 §5.10 渲染档位升级同步推进。

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

**状态生命周期与污染传播**：bit 不是孤立的 per-frame 标志，而是分两类生命周期：

- **Stream 级（一次锁定后常驻）**：`color_meta_known`。VUI 自洽 / 启发式 / 用户覆盖三级中任一锁定后整流稳定，仅在分辨率变化或 SPS 重协商时重评。
- **Frame 级 + 污染传播**：`refs_resolved` / `params_present` / `recovery_complete` / `reorder_resolved` / `gpu_fence_signaled`。这五个 bit 共享同一污染规则——**任一参考链断裂事件触发后，所有后续帧默认 invalid，直至下一 refresh anchor（§5.5.3）才解除**。

污染源（任一触发即进入污染态）：

- depack 检出 sequence number gap 命中参考帧 / FrameNum gap > 0 / POC 不连续（mc-libcodec §5.7.3 路径）。
- MFT sample 携带 decode error flag 或 ProcessOutput 返回 FAILED（§5.6.5）。
- Device Lost 全恢复（§7.2.2），所有 outstanding 帧旧 fence 失效。
- 跨屏 soft adapter switch（§7.3.3）过渡中提交的帧。
- ResizeBuffers 前未完成的 in-flight 帧。

恢复点（**唯一统一入口**）：refresh anchor 的 `recovery_complete` bit 在 depack 层置位 —— IDR / IRAP 或 `recovery_point SEI && recovery_frame_cnt == 0`。anchor 帧自身要求其他 frame 级 bit 也独立 set 才 emit，并不因为是 anchor 就豁免 fence 或 reorder 校验。anchor emit 之后，污染态才解除，后续 P/B 帧按各自 bit 独立判定。

> 反例：把"参考帧丢就 freeze 一帧再继续"作为兜底是花屏常见根因——非 anchor 的 P 帧无论间隔多远都不解污染。GDR 流尤其敏感，refresh window 内的 P 帧必须保持 invalid 直到 `recovery_frame_cnt` 计到 0。

bit 之间的隐含偏序：`refs_resolved` 是 `reorder_resolved` 的前置（reorder 排序前提是 ref 链完整）；其他 bit 互相独立。Gate 实现不依赖该偏序，五个 frame 级 bit 平等 AND；偏序仅用于诊断 stats 输出"哪个 bit 先丢"以定位根因。

**与延时的折中**：错误恢复 / 首帧 / VUI 缺失场景下会多 freeze 1~3 帧。这是文档级明文权衡——**正确性优先于延时**（§2 设计原则 #12），延时让位于"画面不出错"。

**统计**：每个 bit 累计 drop 计数 + 最近一次 drop 的帧 PTS + 当前是否处于污染态 + 最近一次进入污染的触发源，由 stats 暴露，用于诊断"为什么这一帧没显示"以及"还要等多久才解 freeze"。

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

任一 D3D11 / DXGI 调用返回 `DEVICE_REMOVED` 或 `DEVICE_RESET` 时：销毁解码器/swap chain/view 链 + ClearState + 释放 D3D11 device → **重建 DXGI factory**（旧 factory `IsCurrent()` 返回 false 后 stale，强烈建议重建）→ LUID 找回同 adapter（失败则智能重选 §7.3）→ 重建 device / MFT 或软解 / swap chain → 清空 jitter（数据依赖旧 device）→ 标记所有 outstanding 帧 invalid（旧 fence 失效，由 §5.13 Frame Validity Gate 丢弃）→ PLI 请求新 I 帧 → 触发 device-lost / device-recovered 事件序列。

每帧通过 `IMFDXGIDeviceManager::TestDevice` 周期检测 TDR；所有 D3D11 / DXGI 调用统一拦截 device removed。

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

### 7.4 HEVC Extension 缺失场景的硬件兜底（ADR-015 四档降级链特例）

Win10/11 retail 默认无 HEVC Video Extension，导致 MFT 枚举缺 H.265 hardware async MFT。**早期方案 ADR-004 把这种情况直接降到 mc-libcodec 软解**（已被 ADR-015 supersedes）。

新策略按 ADR-015 四档降级链处理（**完整档位定义与每档触发条件见 ADR-015 + §5.6.1**，本节仅说明 HEVC Extension 缺失这一特例）：HEVC MFT 枚举为空时档 3 跳过；档 1 vendor SDK 与档 2 DXVA-direct 都不依赖 OS 端 HEVC Extension（vendor SDK 自带 H.265 能力 / DXVA-direct 仅依赖 driver 暴露 `D3D11_DECODER_PROFILE_HEVC_VLD_MAIN` / `_MAIN10`），因此这两档是 IoT LTSC / 缺 MediaPlayback feature 场景下的真兜底；前三档全失败时落档 4 mc-libcodec 软解。

stats 暴露 `mc.decoder.kind` 字段的真实档位（取值见性能量度规范 §6），用户视角无感（H.265 流照常播放）；运维可定位"为什么没走到更高档"——档 1 缺 SDK 走 ADR-016 下载面板补；档 2 不可用一般是 driver 老旧需更新。

### 7.5 Capability Probe Suite + Preset Live Reload（ADR-017 ~ ADR-020）

§7.4 的"四档降级链"是**单维**决策（仅看硬件解码档位）；§3.5 引入的 Capability-Driven Preset 则是**三维联合**决策（hardware ∩ network ∩ encoder）。本节给出 probe 与 reload 的运行期协议细节，是 §3.5 顶层架构图的展开。

#### 7.5.1 Probe Suite 时序与产出

启动序列与 RTSP 握手 / WHEP ICE **并行**（不阻塞首帧），四个 probe 输出聚合到 `CapabilitySnapshot`：

```
T0=0    Hardware Probe 开始     ┐
T0=0    Network Probe 开始      │
T0=0    Render Probe 开始       ├ 并行
T0=0    Signaling 握手开始      ┘
T0+50   Render Probe 完成（DXGI Output6 / DCOMP / EDID，纯本地查询）
T0+200  Hardware Probe 完成（DXGI 枚举 + CheckVideoDecoderProfile，本地操作）
T0+800  Network Probe 完成（200 packets / RTT 探测，1G LAN 一般 <500ms）
T0+RECV_FIRST_GOP   Encoder Probe 完成（依赖首 GOP 实测）
T0+T_match  Preset Selector 输出 active_preset，apply 到所有子系统
T0+T_first_frame  首帧上屏
```

> Probe 完成前播放器先沿用 REALTIME_LAN preset 保守版（**BOOTSTRAP 是阶段名，不是独立第 6 档** —— 仍属 §6.1 5 档之一的实例）启动；首 GOP 后第一次 reload 切换到正式 preset，对用户表现为"前 0.5~1 秒延时偏高，之后稳定到目标值"。

#### 7.5.2 Network Capability Probe（ADR-018）

**采集量**：

| 指标 | 单位 | 采集方式 | 用途 |
|---|---|---|---|
| `rtt_p50` / `rtt_p95` / `rtt_p99` | ns | RTSP TCP keepalive RTT 或 RTCP RR/SR 反推；WHEP 用 STUN binding pairs RTT | Preset 链路类型判定 |
| `iat_jitter_ms` | ms | 第一 GOP 内 RTP 包 inter-arrival time 标准差 | jitter buffer 模式选择 |
| `loss_rate_short` | % | 第一 GOP 累计 seq gap / 总包数 | Preset 严格度调档 |
| `link_kind` | enum | rtt+iat 组合推断 ∈ {`LAN_SWITCHED`, `LAN_WIFI`, `WAN_WIRED`, `WAN_WIRELESS`, `UNKNOWN`} | Preset 主分类轴 |

**判定阈值**（统一用 RTT 与 jitter 的 p95，避免被极个别包扭曲）：

| `link_kind` | RTT p95 | iat_jitter_ms | loss_rate_short |
|---|---|---|---|
| `LAN_SWITCHED` | <2ms | <1ms | <0.01% |
| `LAN_WIFI` | <10ms | <5ms | <0.1% |
| `WAN_WIRED` | 10-80ms | <10ms | <1% |
| `WAN_WIRELESS` | 20-200ms | 5-50ms | 0.1-3% |
| `UNKNOWN` | 任意 | 任意 | 任意 | 兜底，对应 SAFE_MODE preset |

> 探测窗口受首包到达时机制约——RTSP TCP setup 后 SETUP/PLAY 完成约 200-500ms，PLAY 后第一 GOP 到齐再额外 200-500ms。整段 probe ≤ 1s 不阻塞应用启动 UI。

#### 7.5.3 Encoder Capability Probe（ADR-019）

**采集源**（按可信度从高到低，先取得即停）：

1. **SDP `profile-level-id`** —— H.264 用 `profile_idc + profile_iop + level_idc` 三段（RFC 6184 §8.1），仅 baseline/constrained baseline + level≤3.1 可强声明 0 reorder；其余按 SPS 实测。
2. **SPS VUI**（解析首 IDR 内 SPS）：
   - `bitstream_restriction_flag == 1` 时直接读 `max_num_reorder_frames`；
   - HEVC 读 `sps_max_num_reorder_pics[sps_max_sub_layers_minus1]`；
   - `bitstream_restriction_flag == 0` 时回退方案 3。
3. **第一 GOP DTS/PTS 实测**：观察 PTS 是否单调递增（递增即 0 reorder）；20 帧内见乱序即记 reorder>0。

**导出特征**：

| 字段 | 取值 | 说明 |
|---|---|---|
| `reorder_depth` | 0 / 1 / 2+ | 0 才能开 `CODECAPI_AVLowLatencyMode` / `ulMaxDisplayDelay=0` |
| `gop_size` | 整数 | 影响断流恢复延时 / Adaptive Jitter target |
| `is_zerolatency` | bool | x264/x265 zerolatency tune 标记（VUI `low_delay_hrd_flag` + reorder_depth==0） |
| `has_b_frames` | bool | 同 reorder_depth>0；冗余字段便于 Selector 表达 |
| `slice_struct` | enum | `single` / `multi-slice` / `picture-segment` ；用于 GDR 恢复路径判定 |

> ⚠️ 错把"reorder_depth>0 + zerolatency=true"当矛盾会误判——zerolatency tune 编码器有时仍发 1 个 B（IBBP），需以 reorder_depth 实测为准，is_zerolatency 仅为 hint。

#### 7.5.4 Preset Selector 匹配算法

**输入**：`HardwareSnapshot` ∪ `NetworkSnapshot` ∪ `EncoderSnapshot` ∪ `RenderSnapshot`

**输出**：`active_preset_id` ∈ {`SDI_REPLACEMENT`, `REALTIME_LAN`, `STREAMING_WIFI`, `WAN_FALLBACK`, `SAFE_MODE`}

**匹配规则概览**（先匹配先停，从严到宽）：

| Preset | 触发概览（详细字段阈值见 capability_probe §6.2） |
|---|---|
| `SDI_REPLACEMENT` | 档 1 vendor SDK + LAN-switched (RTT p95 < 1ms, loss < 0.01%) + 0 reorder + zerolatency hint + 240Hz VRR + render.profile == ULTIMATE_DCOMP + supports_dcomp_nv12_direct |
| `REALTIME_LAN` | 档 1 vendor SDK / 档 2 DXVA-direct + LAN (RTT p95 < 5ms) + 0 reorder + render.profile ∈ {ULTIMATE_DCOMP, EXTREME} |
| `STREAMING_WIFI` | 档 1 / 档 2 / 档 3 MFT_HARDWARE + LAN_WIFI / WAN_WIRED + RTT p95 < 20ms |
| `WAN_FALLBACK` | link_kind ∈ {WAN_WIRED, WAN_WIRELESS} **或** loss_rate_short ≥ 3%（高 loss 兜底，避免 LAN 链路高丢包时 jitter 不足；详见 capability_probe §6.2） |
| `SAFE_MODE` | 探测失败 / 档 4 LIBCODEC + 公网 / 全档保守兜底 |

> **匹配算法权威源**：完整伪代码与字段级阈值（`net.rtt_p95_ns`, `net.loss_rate_short` 等）见 `mc-player_capability_probe_设计.md` §6.2。本表仅为 ADD 层概览，与 capability_probe §6.2 冲突时**以 capability_probe §6.2 为准**（design-detail 层包含 struct 定义与精确阈值，是匹配算法的实施权威）。

**Preset 应用范围**（一次 apply 横跨 6 个子系统）：

| 子系统 | 跨 preset 行为概览 |
|---|---|
| Decoder（vendor SDK low-latency hint） | 各档具体 vendor 配置参数（NVDEC `ulMaxDisplayDelay` / oneVPL `AsyncDepth` / AMF `REORDER_MODE`）见 ADD §5.6.2.1（权威） + capability_probe §6.1 表 |
| Jitter Buffer | ZeroJitter / Kalman aggressive / Kalman normal / Kalman safe 四档（target 0~1ms / 5ms / 15ms / 80ms）；详见 capability_probe §6.1 |
| Render Profile | SDI → ULTIMATE_DCOMP + NV12 直显（子目标 1，9.1）；REALTIME → ULTIMATE_DCOMP（dual-bind shader）/ EXTREME；STREAMING → EXTREME / BALANCED；WAN → BALANCED；SAFE → COMPAT。enum ∈ {COMPAT, BALANCED, EXTREME, ULTIMATE_DCOMP}（ADR-012 / ADD §5.10.2 权威）|
| Present 调度 | SDI/REALTIME → race_to_display;STREAMING/WAN → vsync-aligned |
| RTCP 反馈 | SDI/REALTIME → RFC 5506 Reduced-Size + AVPF Immediate;STREAMING → AVPF Immediate;WAN → AVPF Regular |
| Frame Validity Gate | 6 bit 跨 preset 严格度详见 ADR-014 / ADD §5.13 / capability_probe §6.3（权威）——简言之 4 核心 bit 永远 strict;color / fence 在 SDI_REPLACEMENT NV12 直显路径下因代码路径无消费方而不参与决策（**不是放宽正确性**）|

> ⚠️ Frame Validity Gate 跨 preset 严格度的完整说明（含"代码路径无消费方"vs"放宽正确性"的语义辨析）见 capability_probe §6.3 + ADR-014;不在本节展开,避免与 ADR-014 / ADD §5.13 重复。

#### 7.5.5 Live Reload + Self-Upgrade Loop（ADR-020）

**降级触发**（"假设破坏即降"，一帧内决策）：

| 信号 | 监控源 | 阈值 | 降级动作 |
|---|---|---|---|
| `loss_rate_window_5s` 突增 | RTCP RR + 本地 seq gap | 超 preset 上限 2× | 立即降一档 preset |
| `rtt_p95_window_5s` 突增 | RTSP keepalive / RTCP RR | 超 preset 上限 1.5× | 立即降一档 preset |
| `decoder_error_rate` 抖增 | Frame Validity Gate 拒帧率 | >5% / 5s | 降一档 preset + 重置 jitter |
| `present_underrun` 反复 | T5 watchdog | >3 次 / 30s | 降一档 preset + 切 BEST_EFFORT |
| `tearing_detected` 视觉异常 | 内部模式不匹配 | 1 次即降 | preset 降一档 + render profile 退到 SWAPCHAIN |

**升级试探**（"长期稳定才升"，分钟级决策）：

```
if  current_preset != SDI_REPLACEMENT (升档可能)
 && network/encoder/render telemetry 在过去 60s 全部满足下一档 preset 触发条件
 && 期间无任何 tainted 事件（Frame Validity Gate 拒帧 == 0 / present_underrun == 0）
 && 升档目标 preset 不要求当前档 hw.tier 之外的 decoder 档（即不跨 ADR-015 档位）
 then 主动升一档 preset
      ├ 升档生效 30s 内若再次触发降级 → 标记 oscillation_count++
      └ oscillation_count >= 2 → 锁定当前档 5 分钟禁升
```

> ⚠️ **不跨 decoder 档原则（ADR-020 / ADR-015）**：preset 升档**不会跨越 ADR-015 解码档**——当前会话的 decoder tier（档 1 vendor SDK / 档 2 DXVA-direct / 档 3 MFT hardware async / 档 4 libcodec）在 `mc_open` 时由 §5.6.1 一次性确定，运行期不主动升档。即:启动时若 vendor SDK 未就位（走档 2），即使运行期 ADR-016 面板已下载好 SDK，本会话仍保持档 2 + REALTIME_LAN preset，不切到 SDI_REPLACEMENT preset（后者要求 hw.tier ∈ vendor SDK 集合）。命中档 1 + SDI_REPLACEMENT 需用户主动重 `mc_open`，触发新一轮 hardware probe。性能量度规范 §6 设计上不暴露 `mc.decoder.cross_tier_promote_count`（仅有 demote）即此约束的可观测面。

**Reload 实施约束**：

- **原子切换**：T0/T1/T2/T3 连续运行不中断；T4/T5 在帧间缝隙换 decoder + render profile（一次 GOP 内最多一次 reload，避免抖动）；jitter buffer mode 切换通过双缓冲交替读，不丢帧。
- **降级响应优先级 > 升级试探**：reload 队列里有降级请求时，挂起所有升级。
- **Reload 全程发 telemetry**：`mc.preset.reload_event{from,to,reason,latency_ns}`，运维可复盘自动决策。

> 与 §5.10.5 Present Epoch 的协同：preset reload 视作 epoch 切换的同源事件——T5 commit dcomp 时同时升 epoch；旧 epoch 在飞的帧由 watchdog 兜底丢弃。Frame Validity Gate（§5.13）独立于 preset，preset 切换不会绕过 gate。

---

## 8. 延时预算

### 8.1 客户端内部延时（按阶段，1080p H.264 30 fps）

四档解码后端的延时差异主要在"input alloc / 实际解码 / output 提取"三段；其余阶段（jitter / depack / yuv→rgb / present）跨档共用。

| 阶段 | 档1 vendor SDK | 档2 DXVA-direct | 档3 MFT-async | 档4 libcodec |
|---|---|---|---|---|
| UDP 接收 + 组帧 | 0.2~3 ms | 0.2~3 ms | 0.2~3 ms | 0.2~3 ms |
| Adaptive Jitter Buf | 0~20 ms | 0~20 ms | 0~20 ms | 0~20 ms |
| Depack | <1 ms | <1 ms | <1 ms | <1 ms |
| 解码 input alloc + memcpy | ~30 μs（SDK 自管 staging） | <10 μs（应用直填 buffer） | ~50 μs | ~50 μs |
| 实际解码 | 3~8 ms（vendor LowLatency） | 4~10 ms（应用控 DPB） | 4~12 ms（含 MFT 协议 host 开销） | 5~10 ms（1080p H.264 含 SIMD） |
| 解码 output 提取 | ~5 μs（互通注册） | ~5 μs（本地 view） | ~50 μs（IMFDXGIBuffer） | ~5 μs |
| NV12→D3D11 拷贝（仅 SW） | n/a | n/a | n/a | ~0.5 ms |
| dual-bind 或 CopySub + YUV→RGB | <1~3 ms | <1~3 ms | <1~3 ms | <1~3 ms |
| Present 队列 | 0~4 ms（DCOMP 时降至 ~0） | 0~4 ms | 0~4 ms | 0~4 ms |
| **客户端总计（典型）** | **15~19 ms** | **16~20 ms** | **17~22 ms** | **18~24 ms** |

> 档 1 / 档 2 与档 3 的差异主要在 host 抽象层数（vendor SDK 与 DXVA-direct 绕开 MFT 协议层 + 应用层精控 DPB），硬件 decode unit 在前三档相同。档 4 仅在 4K 与高 reorder 场景显著退化（参见 §9.3）。

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

### 8.4 Preset 延时预算对照（ADR-017，端到端 p95）

§8.1 / §8.2 是按"档"分析；本节按"Preset"——即跨子系统协同后的实际延时分布。

| 段 | SDI_REPLACEMENT | REALTIME_LAN | STREAMING_WIFI | WAN_FALLBACK |
|---|---|---|---|---|
| 触发条件 | NVDEC 命中 + 240Hz VRR + DCOMP MPO 8+ + LAN-switched RTT<1ms + 0 reorder + zerolatency 编码 | 任 vendor SDK / DXVA + LAN RTT<5ms + 0 reorder | Wi-Fi / 中等 RTT 5-15ms + 1+ reorder | WAN/公网 RTT>20ms or loss>1% |
| UDP 接收 + 组帧 | 0.3 ms | 0.5 ms | 1 ms | 2 ms |
| Adaptive Jitter Buf | **~1 ms** (ZeroJitter mode) | 2~8 ms (Kalman aggressive) | 8~25 ms (Kalman normal) | 50~150 ms (Kalman safe) |
| Depack | 0.3 ms | 0.5 ms | 0.5 ms | 0.5 ms |
| 解码 (alloc + actual + output) | ~3.5 ms (NVDEC ulMaxDisplayDelay=0) | 4~10 ms (任 vendor / DXVA) | 5~12 ms | 5~25 ms (libcodec 兜底) |
| dual-bind / NV12 直显 | **0 ms** (DCOMP NV12 直显，绕 shader) | <3 ms (dual-bind + shader) | <3 ms | <3 ms |
| Present 队列 | **0 ms** (race_to_display + VRR + ALLOW_TEARING) | 0~2 ms | 0~4 ms | 0~4 ms |
| 客户端内部小计 | **~5~8 ms** | **8~24 ms** | **15~46 ms** | **57~189 ms** |
| 显示器 scan-out | ~2 ms (240Hz VRR + OLED) | ~4 ms (144Hz VRR) | ~7 ms (144Hz 非 VRR) | 8~16 ms (60Hz) |
| 编码端 (zerolatency / 通用) | ~10 ms (zerolatency 编码) | ~15 ms | 30~50 ms | 50~80 ms |
| 网络 | 0.3 ms | 0.5 ms | 5~15 ms | 20~80 ms |
| **端到端 p95** | **~17~25 ms** | **~30~45 ms** | **~57~123 ms** | **~135~365 ms** |
| 与 NDI / SDI 对照 | 同 NDI Full Bandwidth (16-50ms) / 同 ST 2110-22 JPEG-XS (<10ms 编码层) | NDI HX 中段（80-200ms 范围内偏低端）| NDI HX 中段 | 适合非实时 |

**关键洞察**：
- SDI_REPLACEMENT preset 端到端 p95 ≤25ms，**逼近消费 PC + 通用 H.264 codec 的物理极限**——超出此值需要硬件协同（GPUDirect RDMA NIC / ST 2110-22 JPEG-XS / SDI 直传）。**前提:重构方案 Phase 9.1-9.4 子目标全命中**(DCOMP NV12 直显 / RFC 5506 Reduced-Size RTCP / ZeroJitter mode / Race-to-display Present);子目标 5 (CUDA Graphs) 仅 NVDEC 路径专属可选,缺失不影响命中。Phase 9.0 主线完成但子目标未做时,SDI_REPLACEMENT 不会被命中(selector 在 render / present / rtcp / jitter 维度判断未满足),实际命中 REALTIME_LAN(端到端 p95 ~30~45ms)。
- 同硬件档（如档 1 vendor SDK）下 SDI_REPLACEMENT vs WAN_FALLBACK preset 客户端内部延时差可达 **~150ms**——显示 Preset 联合优化对延时的决定性影响远大于解码档单一维度。
- Preset 选择由 ADR-017 Selector 自动完成，用户无感；运行期假设破坏自动降档（ADR-020）。

---

## 9. iGPU 验证与能力矩阵（normative）

### 9.1 探测原理

启动时枚举 IDXGIAdapter1，逐个 `D3D11CreateDevice` + `ID3D11VideoDevice::CheckVideoDecoderFormat`，对 H.264 NOFGT / HEVC Main / HEVC Main10 / AV1 Profile0 四类组合做布尔探测，缓存能力快照。

### 9.2 能力矩阵（normative）

| iGPU / dGPU | H.264 1080p | H.264 4K | H.265 8-bit 1080p | H.265 8-bit 4K | H.265 10-bit Main10 | AV1 |
|---|---|---|---|---|---|---|
| Intel HD/UHD 620（KBL 7 代 HD 620 / WHL 8 代 UHD 620） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | ✗ |
| Intel UHD 630（CFL，8/9 代） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | ✗ |
| Intel UHD 730（ADL，12 代+） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ |
| Intel UHD 770（ADL，12 代+） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ |
| Intel Iris Xe（TGL，11 代）★ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ |
| Intel Arc Alchemist / Battlemage（A380/A750/A770/B580） | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ | HW ✓ |
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

- **HEVC Extension 缺失**（Win10/11 retail / IoT LTSC）→ ADR-015 四档降级链按 vendor SDK → DXVA-direct → libcodec 顺序兜底（§7.4），无需 HEVC Extension。
- **MFT category 注册缺失**（IoT LTSC 默认 MediaPlayback Disabled）→ MFT 第 3 档自动跳过；vendor SDK + DXVA-direct 不依赖 MFT 注册，仍能硬解。
- **Vendor SDK redistributable 未下载**（首次启动）→ ADR-016 内置下载面板引导用户下载；用户跳过则降到 DXVA-direct，仍硬解，仅"额外 2-4 ms"延时差。
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

- Win10 1809+：H.264 OS 内置 MFT；H.265 / AV1 按 ADR-015 四档降级链 — vendor SDK / DXVA-direct 不依赖 OS Extension；HEVC / AV1 Extension 缺失时档 3 跳过，前两档与档 4 仍工作。
- Win11 推荐：MediaPlayback feature + HEVC / AV1 Extension 一并装让档 3 也可用（提升边缘场景兼容性）；DCOMP 完整支持。
- Windows IoT LTSC：默认 MediaPlayback Disabled 导致档 3 不可用；档 1 / 档 2 / 档 4 全工作，硬解仍由档 1 / 档 2 提供，参见 `hardware-decode-dependencies.md`。
- DirectX 11（Feature Level 11.0+）。
- VC++ Runtime 2022。

### 10.3 依赖与 License

| 库 | License | 用途 | 大小 |
|---|---|---|---|
| Windows Media Foundation（MFT 平台） | OS | 档 3 视频 hardware async MFT（兼容档）+ AAC MFT 解码 | 0 |
| D3D11 Video API（`ID3D11VideoDevice`） | OS | 档 2 DXVA-direct 视频解码 | 0 |
| 各 vendor SDK redistributable（NVDEC / oneVPL / AMF） | 各自 | 档 1 vendor 直驱视频解码（按需下载，ADR-016） | 单家 ~30-100 MB，本地缓存 |
| mc-libcodec 子项目（自研） | Apache 2.0 | H.264/H.265 主流 profile 软解兜底 | ~3 MB |
| libdatachannel 0.24.x | MPL 2.0 | WebRTC PeerConnection | ~1.5 MB |
| libjuice（submodule） | MIT | ICE | bundled |
| libsrtp2 | BSD | SRTP | bundled |
| TLS backend（OpenSSL / GnuTLS / Mbed TLS 三选一） | 各自 | DTLS（libdatachannel backend） | ~0.5 MB |
| libopus | BSD-3 | Opus 解码（WHEP 音频） | ~0.7 MB |
| WinHTTP / DirectComposition | OS | WHEP HTTP 信令 / DCOMP 渲染 | 0 |

第三方 ~2.7 MB + 自研 mc-libcodec ~3 MB ≈ 5.7 MB 总体。

### 10.4 SEP 专利责任

| 路径 | 责任承担方 | distributor 具体义务 |
|---|---|---|
| OS MFT 硬解 / HEVC Extension | Microsoft（含 OEM）已支付；mc-player 不承担 | 无 |
| 档 1 vendor SDK 直驱（NVDEC / oneVPL / AMF） | GPU 厂商已支付 SEP；mc-player 不承担 | 无 |
| 档 2 DXVA-direct（D3D11VideoDevice） | 同 OS MFT 路径，硬件 decode unit 由 GPU OEM 已支付 | 无 |
| mc-libcodec 软解 distribute | distributor 自负 | ①向 Via LA AVC pool 申请 per-device / per-stream H.264 许可；②向 Access Advance HEVC pool（含 Via LA HEVC 过渡并行）申请 H.265 许可；③参考 Cisco OpenH264 BINARY_LICENSE 模型评估"二进制单独下载 + 启用控制 + 归属声明"是否适用。**Apache 2.0 §3 不替代 SEP 池授权**——它只覆盖贡献者授予的专利，与 H.264 / H.265 标准必要专利无关。 |

**Cisco OpenH264 责任分割模型**（mc-libcodec 不复刻该机制，分发方需自评估）：源码 BSD-2-Clause 不含专利授权；Cisco 自付 AVC 专利费、允许第三方分发预编译二进制免费给终端用户，但 BINARY_LICENSE 强制 ①二进制由终端用户从 Cisco 服务器单独下载（不得预捆绑）②应用提供启用/禁用控制 ③归属声明。

商业分发现行 SEP 池（2026-04，版图变化中，需查 Via LA / Access Advance 当前条款）：AVC 走 Via LA AVC pool；HEVC 同时存在 Via LA HEVC（已被 Access Advance 收购更名 VCL Advance，过渡并行中）+ Access Advance HEVC pool。Apache 2.0 §3 含专利 grant + 反诉终止条款保护源码使用者免受贡献者反诉，但 **SEP 是第三方专利持有人** —— Apache 2.0 §3 在 SEP 语境下既不授权也不豁免，distributor 仍需独立向 SEP 池申请许可。

---

## 11. 风险与对策

只列**未被 §5 / §7 / §10 章节覆盖的剩余风险**——已经写入主流程的兜底策略不再重复。

| 风险 | 概率 | 影响 | 对策 |
|---|---|---|---|
| Windows 大版本行为破坏 | 低 | 功能退化 | CI 覆盖 Win10 1809 / 22H2 + Win11 23H2 / 24H2 / 25H2 |
| libdatachannel 维护体量小、DTLS 边缘 case | 中 | WHEP 偶发失败 | pin release tag；上游 CI 烟测每次 release |
| DCOMP 不进 Independent Flip（best-effort 路径） | 中 | 延时未达 ULTIMATE_DCOMP 预期 | PresentMon 实测 + stats 真实 present_mode；自动降至 EXTREME |
| WHEP draft-03 演进至 RFC | 低 | 互通失败 | 跟进 IETF；保留 PATCH 兜底；版本探测 |
| AV1 Extension 在 Win10 / 老硬件缺失 | 中 | AV1 流不能播 | stats 告警；mc-libcodec 后续追加 AV1 软解（路线在 §5.6.7） |
| H.264/H.265 SEP 专利责任（商业分发） | 低（自用） / 中（商业） | 法务 | README/UI 声明；distributor 自负；详见 §10.4 |
| mc-libcodec 合规覆盖率不足 | 中 | 极少数 stream 解码错误 | ITU-T JM/HM reference 100% bit-exact CI 闸 |
| URO / Interrupt Moderation 默认开 | 中 | +2 ms 延时 | stats 检测异常 + 安装文档 |

---

## 附录 A：参考规范

**RFC 协议**：

- RTSP / RTP / RTCP：RFC 2326 / 7826 / 3550 / 3551 / 3611
- SDP / 反馈 / RTX：RFC 4566 / 8866 / 4585 / 4588 / 5104
- 媒体 payload：RFC 6184（H.264）/ 7798（H.265）/ 7587（Opus）/ 3640（AAC-hbr）/ 6416（MP4A-LATM）/ 8285（header ext）
- WebRTC：RFC 8825 / 9143（BUNDLE）/ 9429（JSEP）/ 9725（WHIP）/ draft-ietf-wish-whep-03（draft 已过期 2026-02-19，技术内容稳定）
- DTLS-SRTP：RFC 5764（updated by 7983 / 9443，未 obsoleted）/ 9147（DTLS 1.3）

**ITU-T / ITU-R / ISO**：

- ITU-T H.264 (ISO/IEC 14496-10) / H.265 (ISO/IEC 23008-2) / H.266 (ISO/IEC 23090-3)
- ITU-R BT.1359-1（A/V sync）/ BT.709-6 / BT.2020-2

**SEP / License**：

- Via LA AVC / HEVC（原 MPEG-LA）· Access Advance HEVC / VCL Advance · Cisco OpenH264 BINARY_LICENSE · Apache 2.0 §3 Patent Grant

---

## 附录 B：设计决策索引（ADR）

详细决策与依据见同目录 [`mc-player_ADR.md`](./mc-player_ADR.md)。本表仅列编号、标题与对应正文章节，便于交叉跳转。

| 编号 | 决策 | 正文章节 |
|---|---|---|
| ADR-001 | Media Foundation Transform 作 OS 标准抽象兼容档（原"主硬解后端"由 ADR-015 缩窄） | §5.6.2.3 |
| ADR-002 | 硬件 MFT 走 async 事件驱动模型（仅约束 hardware MFT，sync software MFT 例外） | §5.6.2.3 |
| ADR-003 | dual-bind 通过 BindFlags 显式启用 + fence 同步 | §4.3 |
| ADR-004 | HEVC Extension 缺失自动 mc-libcodec 软解兜底 — **Superseded by ADR-015** | §7.4 |
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
| ADR-015 | 硬件解码四级降级链（vendor SDK → DXVA-direct → MFT hardware async → libcodec） | §5.6 |
| ADR-016 | Vendor SDK 内置下载面板（运行期 detect + 按需下载 + 本地缓存） | §5.6.6 |
| ADR-017 | Capability-Driven Adaptive Preset 顶层架构（hardware ∩ network ∩ encoder → preset → 跨子系统协同） | §3.5 / §8.4 |
| ADR-018 | Network Capability Probe（rtt p50/p95/p99 + iat jitter + loss + link_kind 推断） | §7.5.2 |
| ADR-019 | Encoder Capability Probe（SDP profile-level-id + SPS VUI + 首 GOP 实测） | §7.5.3 |
| ADR-020 | Preset Live Reload + Self-Upgrade Loop（假设破坏即降 + 长期稳定试升） | §7.5.4 / §7.5.5 |
| ADR-021 | Hardware Decode Component Manager（HDCM）：扩展 ADR-016 范围至 OS 媒体扩展 / Optional Feature / GPU driver | §5.6.6 |

---
