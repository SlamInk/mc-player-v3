# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 仓库当前阶段

本仓库目前处于**架构设计阶段**，无 C/C++ 源码、无构建脚本、无测试套件。所有"代码"产物只有 `mc-player-ui-ux/` 下的 React UI/UX 原型（浏览器直跑、无构建步骤）。新增主项目源码、子项目 `subprojects/mc-libcodec/`、构建系统时，需要回填本节的命令清单。

## 目录拓扑

```
doc/                                          架构与运维文档（ADD / ADR / design-detail）
  ├── mc-player_架构设计文档_v3.0.md           ADD — 原理 / 架构 / 可行性
  ├── mc-player_ADR.md                         ADR-001 ~ ADR-020 决策记录，与 ADD 交叉引用
  ├── mc-player_capability_probe_设计.md       design-detail — Capability Probe Suite + Preset 的 struct/算法/状态机
  ├── mc-player_性能量度规范.md                 metric 字段 / 阈值 / 排障(实施性规范)
  └── hardware-decode-dependencies.md          硬件解码依赖与故障排查（IoT LTSC 等场景）
plan/                                          实施性 roadmap
  └── mc-player_重构方案.md                     Phase 0 ~ Phase 10 阶段交付计划
mc-player-ui-ux/                              React UI/UX 设计稿,浏览器渲染
```

未来按 ADD §3.1 与 ADR-010 落地的目录：
```
mc-player/         主项目（五层架构：App / Controller / TS / MediaCore / PAL）
subprojects/
  mc-libcodec/     并列子项目：自研 H.264 / H.265 软解兜底，独立 target / API / license（Apache 2.0）
```

## UI/UX 原型运行方式

`mc-player-ui-ux/` 是无构建步骤的浏览器原型，依赖 React UMD + `@babel/standalone` 实时编译 JSX。

```bash
# 在 mc-player-ui-ux/ 目录下任选一种本地静态服务器
python -m http.server 8000
# 然后浏览器打开 http://localhost:8000/MC%20Player%20UI_UX.html
```

`.design-canvas.state.json` 是 Section 标题 / Artboard 排序的持久化 sidecar；编辑功能依赖 `window.omelette` host bridge，普通 HTTP server 下只读、不可编辑。

## 文档协作规范

修改文档时遵循以下结构性约束（按文档类型）：

### 文档分类

| 类型 | 范围 | 允许内容 | 不允许内容 |
|---|---|---|---|
| **ADD**（doc/mc-player_架构设计文档_v3.0.md） | "原理 + 可行性"层 | 数据流图 / 模块拓扑 / 协议 / 算法形式化 / 风险对策 | 具体 API 头文件 / cmake target / 排期 / shell 命令 |
| **ADR**（doc/mc-player_ADR.md） | 决策记录 | 一条决策 = 一个编号 + 决策 + 依据 | 不展开正文实现细节 |
| **design-detail**（doc/mc-player_capability_probe_设计.md / doc/mc-player_性能量度规范.md） | ADD 与 plan 之间的实施细化层 | 数据结构定义 / 伪代码算法 / 状态机 / metric 字段名 + 单位 + 阈值 | cmake target 命名 / 具体 C/C++ 头文件签名 / 阶段排期 / shell 命令 |
| **plan**（plan/mc-player_重构方案.md） | 实施 roadmap | 阶段排期 / commit 模板 / 验收 metric / 改动文件清单 | 决策依据（在 ADR）/ 原理论证（在 ADD） |
| **运维文档**（doc/hardware-decode-dependencies.md） | 故障排查 + 平台特例 | PowerShell / WMIC 诊断命令 / 修复路径 | 决策（在 ADR）/ 原理（在 ADD） |
| **本文件 CLAUDE.md** | 项目级硬约束 + 给 AI 的协作指引 | 不可妥协约束清单 / 路由速查 / 风险点 | 完整原理或决策依据（指向 ADD/ADR） |

### 协作原则

1. **新增章节前先确认归属**：决策依据 → ADR；原理 / 数据流 / 模块拓扑 / 风险对策 → ADD；struct + 伪代码 + metric 字段 → design-detail；命令 / 排期 / 改动文件清单 → plan；shell 命令 + 故障排查 → 运维文档。
2. **ADR 修订规范**：见 `doc/mc-player_ADR.md` 文末"修订规范"章节——首选新开 ADR 引用旧编号；折中允许在旧 ADR 末尾追加"修订记录"段，但不修改原决策与原依据正文；禁止重写历史。
3. **ADD ↔ ADR 双向交叉引用**：ADD 关键章节末尾标注对应 ADR 编号，ADR 在标题括号内反向标注 ADD 章节号。新增决策需同步两处。
4. **ADD 附录 B（ADR 索引）必须与 ADR 文件保持一一对应**。
5. **跨文档去重**：同一规则在多文档重述时确定唯一权威源（如 ADR-015 四档链权威 + ADD §5.6.1 表权威），其他位置改用"参见 ADR-XXX / ADD §X.X"引用而非重述。

## 设计原则中**不可妥协**的硬约束

写代码时必须把这些当作不变量，违反即架构破坏。它们散在 ADD 多个章节，集中列出避免遗漏：

- **正确性优先于延时**（ADD §2 #12 / ADR-014）：花屏与陈旧区域比延时多 1 帧更不可接受。所有解码 / 渲染异常路径默认 freeze last-good，不输出半态帧。
- **Frame Validity Gate**（ADD §5.13）：codec 与 render 之间唯一闸门。六类 validity bit（refs / params / recovery / color / reorder / fence）任一缺失即丢帧；不允许在 codec 层或 render 层各自做散落兜底。
- **Present Epoch + Watchdog**（ADD §5.10.5）：T5（render thread）是 `IDCompositionDevice::Commit` **唯一**调用方；视频 Present 与 DCOMP commit 同 epoch 配对；超时未推进强制 redraw last-good。
- **硬件解码四级降级链**（ADR-015 / ADD §5.6）：硬件 path 最短优先。`vendor SDK 直驱 (NVDEC/oneVPL/AMF) → DXVA-direct (D3D11VideoDevice) → MFT hardware async → mc-libcodec 软解`。前一档失败立即降级；sync software MFT 不算硬解（直接归到档 4）。Vendor SDK redistributable 不预 bundle，由 ADR-016 内置下载面板按需获取。
- **硬件 MFT 永远 async**（ADR-002 / ADD §5.6.2.3）：Microsoft 协议要求，非性能选择。**仅约束档 3 hardware MFT 路径**（`hw_url=1 && async=1`）；激活时必须显式 unlock async + 用事件生成器驱动 NeedInput / HaveOutput / DrainComplete 事件循环。sync software MFT 不在约束范围。
- **dual-bind 的 fence 同步**（ADR-003 / ADD §4.3）：`BIND_DECODER | BIND_SHADER_RESOURCE` 必须显式请求；array slice 在 DPB evict 前必须用 `ID3D11Fence` 等 SRV 读完，否则花屏。
- **B-Frame Policy**（ADD §5.6.4）：检测到 reorder > 0 必须取消 `CODECAPI_AVLowLatencyMode`，否则与 low-latency 模式冲突 → **必然花屏**。检测优先 SDP `profile-level-id`，兜底扫 SPS VUI `max_num_reorder_frames` / `sps_max_num_reorder_pics`。
- **GDR ≠ IDR**（ADD §5.5.3）：仅发 GDR 不发 IDR 的低延时编码器场景，必须等 `recovery_point` SEI `recovery_frame_cnt == 0` 才解 freeze；中间 P/B 帧仍带 invalid 下沉。把两者等价是花屏常见根因。
- **平台原生 + 性能例外**（ADD §2 #10）：HTTP → WinHTTP，TLS → Schannel，时钟 → QPC，COM/D3D11/DCOMP 直调；视频解码 OS 内置档走 DXVA-DDI / MFT。引第三方需要 ADR 明示理由（当前已开口：libdatachannel ADR-005 / libopus ADR-009 / mc-libcodec ADR-006 / TLS backend / vendor SDK ADR-015 + ADR-016）。例外不是默认值——必须给出具体性能 / 能力依据。
- **能力探测 fail-open**（ADD §2 #11）：所有硬件加速路径 open 前先探测，探测失败立即降级；不允许 fail-late 抛运行时异常。
- **mc-libcodec 不依赖第三方解码库代码**（ADD §5.7.7 / ADR-006）：可参考 OpenH264 / libde265 算法描述；x264 仅作 spec 参考，不读源码（GPL 污染规避）。正确性基准对齐 ITU-T JM/HM reference 100% bit-exact。
- **mc-libcodec ABI 演进契约**（ADD §5.7.2）：公开 API 头首字段 `struct_size + version`，未来追加字段不破坏旧调用方。
- **WHEP 选 onMessage 路径**（ADD §5.2）：libdatachannel 的 `onMessage`（单包明文 RTP）而非 `onFrame`，让 RTSP / WHEP 在 jitter buffer + depack 层完全合流，并保留 SEI / GDR / 字节级控制权。

## 线程与 MMCSS 绑定（ADD §3.3）

| 线程 | MMCSS task | 实际优先级 |
|---|---|---|
| T0 App/UI | — | Normal |
| T1 Signaling | — | Normal |
| T2 Network RX（RTSP） | 不挂 MMCSS；TIME_CRITICAL + 绑 P-core | 15 |
| T3 RTCP | — | Normal |
| T4 Video Decode | Playback | ~23 |
| T5 Video Render | Playback | ~23 |
| T6 Audio Render | Pro Audio | ~26 |
| T7 WHEP-PC | 不挂 MMCSS；HIGHEST | 10 |

WHEP 模式下 T2 由 libdatachannel 内部 socket 线程承担、T7 不存在 RTSP 模式；MFT worker 跑在 MMCSS 上需先创建 MMCSS-aware 共享 work queue 整体注册再绑定。Intel 12 代+ T2/T5/T6 绑 P-core；libdatachannel 内部线程不主动绑（对齐 Chromium 主线）。

## 协议 / 编解码路由速查

| 入口协议 | 信令 | 数据 | 加密 |
|---|---|---|---|
| RTSP/RTP over UDP | 自管 TCP（RFC 2326） | own UDP | 无 |
| RTSP interleaved TCP | 同上 | TCP `$ ch len` 帧化 | 无 |
| WHEP（draft-ietf-wish-whep-03） | WinHTTP 异步 POST/PATCH/DELETE | libdatachannel SRTP | DTLS-SRTP（RFC 5764） |

| Codec | 解码后端（ADR-015 四级降级链按档探测）|
|---|---|
| H.264 / H.265 | 档1: vendor SDK (NVDEC/oneVPL/AMF) → 档2: DXVA-direct (D3D11VideoDevice) → 档3: MFT hardware async (OS 兼容档) → 档4: mc-libcodec 软解 (NV12 RAM 上传 dynamic texture) |
| AAC | Microsoft AAC MFT；显式协商 `MFAudioFormat_Float` 32-bit（音频不在 ADR-015 范围）|
| Opus | bundled libopus（无 OS MFT，ADR-009）|
| G.711 a/μ-law | 256-entry LUT 自实现 |
| AV1 / H.266 | 同 H.264/H.265 四档链；H.266 待 vendor / OS 支持 |

## 关键风险点（已写进设计、实施时易踩）

- **`CODECAPI_AVLowLatencyMode` 类型陷阱**（ADD §5.6.3）：Microsoft H.264 decoder 用 `VT_UI4`（MS Learn 明示），其它 codec 按惯例用 `VT_BOOL` 但**未在 MS 文档明示**，`SetValue` 前应实测验证。
- **DXGI factory `IsCurrent()` 语义**（ADD §7.2.2）：device removed 后旧 factory stale，强烈建议重建，不要复用。
- **ResizeBuffers 必须先 ClearState**（ADD §5.10.3）：未清场 + 未释放 back buffer 引用的 SRV/RTV/UAV 会返回 `DXGI_ERROR_INVALID_CALL`，常表现为窗口 resize / 跨屏拖拽后陈旧内容残留或黑屏。
- **HEVC Extension 默认缺失**（ADD §7.4，ADR-004 已 Superseded by ADR-015）：Win10/11 retail / IoT LTSC 默认无 HEVC Video Extension。**新策略不再特殊对待**——直接按 ADR-015 四级链兜底（vendor SDK → DXVA-direct H.265 Main/Main10 → libcodec），不依赖 OS Extension。
- **H.265 NAL header 是 2 字节**（ADD §5.5.2）：FU 重组与 H.264 不同，需保留 LayerId/TID 并按 FU header 内的 type 字段重组。
- **拒绝 `packetization-mode=2`**（ADD §5.5.1）：SDP answer 阶段拒，避免 DON 重排复杂度；STAP-A + FU-A 覆盖现网。
- **AVPF Immediate Feedback mode + `trr-int 0` 是两个独立机制**（ADD §5.4），需同时满足才能近实时反馈。
- **SDP `mode=AAC-hbr` 与 MFT payload type 不同语义**（ADD §5.8）：前者字符串、后者整数（0=Raw / 1=ADTS / 2=ADIF / 3=LATM-LOAS），LATM/LOAS over RTP 走 RFC 6416 而非 RFC 3640。

## 队列与零拷贝原则（ADD §3 / §4 / §6）

- 所有 SPSC 队列 cache-line padding 必做（false-sharing 性能降 3-5×）；深度极低，满时丢最老。
- 零拷贝定义：**不存在 GPU→CPU readback，且 GPU 内部至多 1 次 NV12 array slice 复制**。
- MFT 与 renderer 共享同一 `ID3D11Device` 并启用多线程保护；解码 texture 与渲染 texture 共生命周期。

## 语言与平台

- 文档全部 zh-CN；新增技术文档延续中文叙述风格，技术术语 / API / 标识符保留英文原形。
- 目标平台 Windows 10 1809+ / Windows 11 x64；DirectX 11 Feature Level 11.0+；VC++ Runtime 2022。
- 当前开发主机：Windows 11 IoT 企业版 LTSC，Visual Studio 18 Enterprise（路径 `C:\Program Files\Microsoft Visual Studio\18\Enterprise`）。
