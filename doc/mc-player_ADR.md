# mc-player — 架构决策记录（ADR）

配套 `mc-player_架构设计文档_v3.0.md`。每条 ADR 只阐述决策与依据，正文章节是依据的展开。

---

## ADR-001 Media Foundation Transform 作 OS 标准抽象兼容档（正文 §5.6.4；原决策由 ADR-015 缩窄）

- **决策**：H.264 / H.265 / AAC 走 Windows MFT 作为**OS 标准抽象兼容档**——在 ADR-015 四级降级链中处于第 3 档（vendor SDK / DXVA-direct 之后），不再是默认主路径。仅 hardware async MFT (`hw_url=1 && async=1`) 接受为本档；sync software MFT 由 ADR-015 直接降到第 4 档软解。AAC 音频路径不受 ADR-015 影响，仍走 AAC MFT。
- **依据**：
  - **原决策依据保留**：MFT 走 DXVA 与底层硬解管线一致；OS 内置零依赖、零 license 风险；Microsoft 官方持续维护。
  - **缩窄理由（追加，2026-04-29）**：Microsoft Learn 明示 hardware MFT 是 DXVA-DDI 的协议封装，硬件路径与 DXVA-direct 等价；MFT 多出的 host 侧开销在 SPS `max_num_reorder_frames>0` 流上强制累积 ≥3 帧延时（实测 +100ms vs VLC），且 driver 内部 DPB 管理是黑盒不可调，与"极限低延时"目标不一致。详见 ADR-015。
  - **保留 MFT 而非 Supersede 的理由**：driver 暴露 hardware MFT 但 DXVA-DDI 不暴露的边缘场景仍需要 MFT；AAC 路径无 DXVA 对位实现。

## ADR-002 硬件 MFT 走 async 事件驱动模型（正文 §5.6.4）

- **决策**：被 ADR-015 选中走 MFT 第 3 档时，激活硬件 MFT 必启用 async unlock，用事件生成器驱动 NeedInput / HaveOutput 循环。
- **依据**：Microsoft 文档明确 "hardware MFT always process data asynchronously" —— 协议要求而非性能选择；sync 调用模式会 stall。
- **范围澄清（追加）**：本 ADR 仅约束 hardware MFT (`hw_url=1`) 路径。sync software MFT (`hw_url=0 && async=0`，如 OS 内置 `Microsoft H264 Video Decoder MFT`) 不在约束范围——它由 ADR-015 直接降到第 4 档软解，不再被当作"硬件 MFT 的同枚举兜底"。

## ADR-003 dual-bind 通过 BindFlags 显式启用（正文 §4.3）

- **决策**：在 MFT 输出流属性上显式请求 `BIND_DECODER | BIND_SHADER_RESOURCE`；renderer 用 `TEXTURE2DARRAY` + `FirstArraySlice` 创建 SRV；array slice 用 `ID3D11Fence` 等 SRV 读完才回收。
- **依据**：MFT 默认仅 `BIND_DECODER`，无此属性必须显式 GPU 内拷贝（破坏零拷贝）；fence 是防止 "slice 已 evict 但 shader 仍在采样" 的花屏不变量。

## ADR-004 HEVC Extension 缺失自动 mc-libcodec 软解兜底（正文 §7.4）— **Superseded by ADR-015**

- **决策（已废止，保留作历史）**：HEVC MFT 枚举为空时无感切 mc-libcodec 软解。
- **依据（已废止，保留作历史）**：Win10 / 11 retail 默认无 HEVC Extension；硬失败 UX 不可接受。
- **Superseded 说明（2026-04-29）**：原决策的二元 fallback（MFT → libcodec）在 IoT LTSC / 缺 MediaPlayback feature 等场景把硬件 GPU 直接跳过，违反"硬件 path 最短"原则。新策略：HEVC MFT 缺失 → 优先尝试 ADR-015 档 1 (vendor SDK) 与档 2 (DXVA-direct，覆盖 HEVC Main / Main10)，再不行才到档 4 软解。HEVC Extension 缺失不再特殊对待——它只是 ADR-015 档 3 探测失败的一个普通触发条件。

## ADR-005 libdatachannel 作为 WebRTC 后端（正文 §5.2）

- **决策**：libdatachannel 0.24.x（C/C++、SRTP/DTLS/ICE 一站式、MPL 2.0；自 v0.18 起从 LGPLv2.1+ 切换）。
- **依据**：vs libwebrtc（50 MB+ → 约 1–3 MB，~30× 缩减）；MPL 2.0 文件级 copyleft，与闭源主项目静态链接合法；活跃维护。

## ADR-006 mc-libcodec 自研 H.264 / H.265 主流 profile 软解（正文 §5.7）

- **决策**：自研子项目，参考 OpenH264 / libde265 架构（不复制代码），SSE2 / AVX2 SIMD。
- **依据**：OpenH264 BSD-2 仅 Constrained Baseline up to Level 5.2；dav1d AV1-only；自研换独立可复用 + 无外部 license 责任。

## ADR-007 智能 GPU 选择（正文 §7.3）

- **决策**：内部按 「HWND 跟随 + 能力兜底 + 跨屏热切换」 自动选 adapter；保留 LUID 显式覆盖。
- **依据**：HWND 跟随是延时最低的物理一致性策略；用户层无感，符合「专业播放器」隐形原则。

## ADR-008 DCOMP 第 4 档作为 best-effort 自动激活档（正文 §5.10.2）

- **决策**：实施 ULTIMATE_DCOMP；智能选择条件满足时自动激活；HUD 通过独立 DCOMP visual + vsync 与 video plane 解耦。
- **依据**：DCOMP 真正卖点是多 visual 的 plane 分离能力；Independent Flip 升级是 best-effort，运行时由 PresentMon 验证；不当作必胜档。

## ADR-009 Opus 用 libopus（正文 §5.8）

- **决策**：bundled libopus（BSD-3，~700 KB）。
- **依据**：OS 无 Opus MFT；OvenMediaEngine / MediaMTX / Cloudflare 全用 Opus；放弃「全 OS 自带」原则但 Opus 是 WebRTC 事实标准。

## ADR-010 mc-libcodec 拆为独立子项目（正文 §5.7.1）

- **决策**：monorepo 内独立子项目 `subprojects/mc-libcodec/`，独立 target / API / license（Apache 2.0）/ 测试套件。
- **依据**：独立可复用于 mc-encoder、其他工具；清晰 license 边界。

## ADR-011 H.264 / H.265 SEP 专利责任声明（正文 §10.4）

- **决策**：OS MFT 路径无责任；mc-libcodec 软解 distribute 时 distributor 自负；参考 Cisco OpenH264 源码-二进制责任分割模型。
- **依据**：Via LA AVC / HEVC pool 与 Access Advance 现行条款；Apache 2.0 §3 含明确专利 grant。

## ADR-012 智能 render profile 选择默认 AUTO（正文 §5.10.1）

- **决策**：运行期探测 ALLOW_TEARING + Hardware composition + DCOMP 可用性，自动选最佳档。
- **依据**：与 ADR-007 一致的「专业播放器隐形」哲学；ALLOW_TEARING 是应用层 VRR 探测唯一权威 API，MPO / DirectFlip 依赖 `CheckHardwareCompositionSupport`。

## ADR-013 跨屏 adapter switch 用 double-buffered transition（正文 §7.3.3）

- **决策**：在新 adapter 上预创建 D3D11 device + IMFDXGIDeviceManager，旧 device 持续显示直到新 device 出第一帧再切换；上限 800 ms，超时 fallback 旧 adapter。
- **依据**：单 adapter 重建链 + 网络 RTT + I 帧等待经常超 200 ms；double-buffered 避免黑屏 UX。

## ADR-014 正确性先于延时：Frame Validity Gate + Present Epoch（正文 §2 #12 / §5.13 / §5.10.5 / §5.5.3 / §5.6.4 / §5.12）

- **决策**：在 codec 与 render 之间设置协议无关的 Frame Validity Gate（§5.13）；render 侧设置 Present Epoch + Watchdog（§5.10.5）。把 refresh anchor、B-Frame Policy、color VUI 三级兜底、DPB 引用追踪、dual-bind fence、DCOMP 多 visual 同 epoch 提交六类机制收口。任一 validity bit 缺失即丢帧不 emit；任一 epoch 超时未推进即强制 redraw last-good。
- **依据**：花屏与陈旧区域未刷新比延时多 1 帧更不可接受。原架构对花屏只有 §11 一行 "decode error flags + freeze" 兜底、对陈旧区域无任何机制；散落 freeze 易因竞态被 Present。代价是错误恢复 / 首帧 / VUI 缺失场景多 freeze 1~3 帧——文档级明文权衡。

## ADR-015 硬件解码四级降级链：vendor SDK → DXVA-direct → MFT hardware async → mc-libcodec 软解（正文 §5.6）

- **决策**：视频解码后端按"硬件 path 最短优先"四级降级，逐档独立探测；前一档失败立即降级，不混淆"硬"与"软"。
  1. **vendor SDK 直驱**：NVIDIA Video Codec SDK (NVDEC) / Intel oneVPL / AMD AMF；按运行期 GPU vendor 选其一；SDK redistributable 由 ADR-016 内置下载面板按需获取。
  2. **DXVA-direct（D3D11VA）**：`ID3D11VideoDevice` 直驱；跨 vendor 通用；不依赖 MFT category 注册（绕开 IoT LTSC 缺 MediaPlayback feature 的环境约束）。
  3. **MFT hardware async**：仅 `hw_url=1 && async=1` 才接受；sync software MFT 在 enum 阶段被识别即降级（ADR-002 不约束 sync software MFT）。
  4. **mc-libcodec 软解**：最终兜底；性能取决于 §5.7.5 SIMD 实装进度。
- **依据**：
  - **Microsoft Learn 明示 hardware MFT 是 DXVA-DDI 的协议封装**（"Direct3D 11 uses the same data structures as DXVA 2.0 for decoding operations"；"DXVA integrates with Media Foundation and allows DXVA pipelines to be exposed as Media Foundation Transforms"）——硬件路径上 MFT 与 DXVA-direct 等价，MFT 多出的开销纯在 host 侧（ProcessInput/Output 协议、内部 reorder buffer / DPB 黑盒管理）。
  - **vendor SDK path 最短**：直接对 vendor 私有 driver DDI（NVDEC 自带 LowLatency mode + 4-frame pipeline 但首帧立即解；Intel oneVPL 自带 LowDelay mode；AMD AMF 同理），绕过 OS MFT 与通用 DXVA-DDI 两层抽象。
  - **DXVA-direct 优于 MFT 的工程理由**：`(a)` MFT 内部 DPB / reorder buffer 在 SPS `max_num_reorder_frames > 0` 时强制累积 ≥3 帧延时（实测 +100ms vs VLC，本仓库 commit 历史已记录），且 driver 实装是黑盒不可调；`(b)` D3D11VA frame 留在 video memory 与 §4.3 dual-bind 零拷贝原生兼容（DXVA2 / cuvid copyback 不兼容）；`(c)` Chromium 主线已从 MFT 迁到 D3D11VideoDecoder，主驱动是控制深度 + 摆脱 MFT 注册依赖。
  - **MFT 仍保留的理由**：driver 暴露 hardware MFT 但 DXVA DDI 不暴露的边缘场景；OS 标准抽象兼容档（见修订后的 ADR-001）。
  - **sync software MFT 排除**：实测 SPS reorder=2 时硬性缓 ≥3 帧才输出第一帧（30fps ⇒ ~100ms），在"极限低延时 + 极限解码速度"目标下不可接受；走 mc-libcodec SIMD 软解也比 software MFT 更可控。
  - **mc-libcodec 兜底承认风险**：极限场景下若前三档全失败、且 §5.7.5 SIMD 实装尚未达标，则系统延时无法保证——文档级权衡，运维需在 stats 上监测 `decoder_kind` 与 `e2e_latency_p95`。
- **每档触发条件**：
  - 档 1：caps_probe 检出 vendor 匹配 + SDK redistributable 已存在（ADR-016）+ codec/profile 支持。
  - 档 2：`ID3D11VideoDevice::CheckVideoDecoderProfile` + `CheckVideoDecoderFormat` 通过。
  - 档 3：`MFTEnumEx(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT)` 返回 ≥1 个且 `MFT_ENUM_HARDWARE_URL_Attribute` 非空 + `MF_TRANSFORM_ASYNC=1`。
  - 档 4：`allow_software_decode=1`（用户显式禁用软解时直接上报 `MC_ERR_NO_HARDWARE`）。
- **关系**：
  - 缩窄 ADR-001 的 "MFT 作主硬解后端"为"OS 标准抽象兼容档"；不 Supersede。
  - ADR-002 不变；明确"sync software MFT 不在约束范围"。
  - **Supersedes ADR-004**：HEVC Extension 缺失场景统一走本 ADR 的四级链。

## ADR-016 vendor SDK 内置下载面板：运行期 detect + 按需下载 + 本地缓存（正文 §5.6.2）

- **决策**：mc-player 不预 bundle vendor SDK redistributable（NVDEC / oneVPL / AMF）；在应用内提供"硬解组件"面板，按运行期检测到的 GPU vendor 引导用户下载对应 SDK 到本地缓存目录，后续启动按文件存在性 probe，不存在即降到 ADR-015 档 2。
- **依据**：
  - 三家 SDK redistributable 各自 ~30-100 MB；全 bundle 安装包翻 3-4 倍，对单 vendor 用户是冗余。
  - 纯运行期"首次缺失就阻塞"会延迟首次启动 UX；面板让用户感知"这是可选增强档"，符合 §1.1 "用户感知复杂度最低"。
  - 与 ADR-005 (libdatachannel) / ADR-009 (libopus) 直接 bundle 的对比：那两个是 baseline 能力（无之则 WHEP / Opus 不可用），bundle 合理；vendor SDK 是**可选性能增强**（无之则降到档 2 仍能放）——按需下载更对应其角色。
  - 下载源限定 vendor 官网直链 + 校验 SHA-256；不引第三方 redistributable 仓库。
- **关系**：执行 ADR-015 档 1 的工程化前置；与 §2 #10 "平台原生优先"的关系参考 ADR-005/009 的 "为关键能力可引第三方" 例外口子，本 ADR 进一步细化"可选增强档"的引入方式。

---

> 当一条决策因实证或新约束被推翻，新增 ADR 引用旧 ADR 编号并标 Superseded，不修改历史条目。
