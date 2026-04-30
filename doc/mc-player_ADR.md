# mc-player — 架构决策记录（ADR）

配套 `mc-player_架构设计文档_v3.0.md`。每条 ADR 只阐述决策与依据，正文章节是依据的展开。

---

## ADR-001 Media Foundation Transform 作 OS 标准抽象兼容档（正文 §5.6.2.3；原决策由 ADR-015 缩窄）

- **决策**：H.264 / H.265 / AAC 走 Windows MFT 作为**OS 标准抽象兼容档**——在 ADR-015 四级降级链中处于第 3 档（vendor SDK / DXVA-direct 之后），不再是默认主路径。仅 hardware async MFT (`hw_url=1 && async=1`) 接受为本档；sync software MFT 的处理由 ADR-015 唯一定义（不在本 ADR 重复）。AAC 音频路径不受 ADR-015 影响，仍走 AAC MFT。
- **依据**：
  - **原决策依据保留**：MFT 走 DXVA 与底层硬解管线一致；OS 内置零依赖、零 license 风险；Microsoft 官方持续维护。
  - **缩窄理由（追加，2026-04-29）**：Microsoft Learn 明示 hardware MFT 是 DXVA-DDI 的协议封装，硬件路径与 DXVA-direct 等价；MFT 多出的 host 侧开销在 SPS `max_num_reorder_frames>0` 流上强制累积 ≥3 帧延时（实测 +100ms vs VLC），且 driver 内部 DPB 管理是黑盒不可调，与"极限低延时"目标不一致。详见 ADR-015。
  - **保留 MFT 而非 Supersede 的理由**：driver 暴露 hardware MFT 但 DXVA-DDI 不暴露的边缘场景仍需要 MFT；AAC 路径无 DXVA 对位实现。

## ADR-002 硬件 MFT 走 async 事件驱动模型（正文 §5.6.2.3）

- **决策**：被 ADR-015 选中走 MFT 第 3 档时，激活硬件 MFT 必启用 async unlock，用事件生成器驱动 NeedInput / HaveOutput 循环。
- **依据**：Microsoft Learn 'Hardware MFTs' 文档原文 "Hardware MFTs must use the new asynchronous processing model" —— 协议要求而非性能选择；sync 调用模式不被支持。
- **范围澄清（追加）**：本 ADR 仅约束 hardware MFT (`hw_url=1`) 路径。sync software MFT (`hw_url=0 && async=0`，如 OS 内置 `Microsoft H264 Video Decoder MFT`) 不在本 ADR 约束范围——其处理方式由 ADR-015 唯一定义（不在本 ADR 重复）。

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
- **依据**：Via LA AVC / HEVC pool 与 Access Advance 现行条款；Apache 2.0 §3 含明确专利 grant（仅覆盖贡献者授予使用者贡献本身的专利，**不替代 H.264 / H.265 SEP 池授权**——商业分发 mc-libcodec 二进制需自行向 Via LA / Access Advance 申请 per-device 或 per-stream 许可，参考 Cisco OpenH264 BINARY_LICENSE 模型）。

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
  - **DXVA-direct 优于 MFT 的工程理由**：`(a)` MFT 内部 DPB / reorder buffer 在 SPS `max_num_reorder_frames > 0` 时强制累积 ≥3 帧延时（实测 +100ms vs VLC，本仓库 commit 历史已记录），且 driver 实装是黑盒不可调；`(b)` D3D11VA frame 留在 video memory 与 §4.3 dual-bind 零拷贝原生兼容（DXVA2 / cuvid copyback 不兼容）；`(c)` Chromium 主线选择 `D3D11VideoDecoder` 直驱（绕过 OS MFT），印证 DXVA-direct 路径的工程可行性；主驱动是控制深度 + 摆脱 MFT 注册依赖（Edge 因 license 约束仍走 `DXVAVideoDecodeAccelerator` 即 MFT 路径，与 Chromium 主线分支不同）。
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

## ADR-016 vendor SDK 内置下载面板：运行期 detect + 按需下载 + 本地缓存（正文 §5.6.6）

- **决策**：mc-player 不预 bundle vendor SDK redistributable（NVDEC / oneVPL / AMF）；在应用内提供"硬解组件"面板，按运行期检测到的 GPU vendor 引导用户下载对应 SDK 到本地缓存目录，后续启动按文件存在性 probe，不存在即降到 ADR-015 档 2。
- **依据**：
  - 三家 SDK redistributable 各自 ~30-100 MB；全 bundle 安装包翻 3-4 倍，对单 vendor 用户是冗余。
  - 纯运行期"首次缺失就阻塞"会延迟首次启动 UX；面板让用户感知"这是可选增强档"，符合 §1.1 "用户感知复杂度最低"。
  - 与 ADR-005 (libdatachannel) / ADR-009 (libopus) 直接 bundle 的对比：那两个是 baseline 能力（无之则 WHEP / Opus 不可用），bundle 合理；vendor SDK 是**可选性能增强**（无之则降到档 2 仍能放）——按需下载更对应其角色。
  - 下载源限定 vendor 官网直链 + 校验 SHA-256；不引第三方 redistributable 仓库。
- **关系**：执行 ADR-015 档 1 的工程化前置；与 §2 #10 "平台原生优先"的关系参考 ADR-005/009 的 "为关键能力可引第三方" 例外口子，本 ADR 进一步细化"可选增强档"的引入方式。
- **扩展记录（2026-04-30）**：本 ADR 范围（vendor SDK redistributable）作为 ADR-021 HDCM 类别 A 保持不变；类别 B（Microsoft Store 扩展）/ C（Windows Optional Feature）/ D（GPU driver 引导）由 ADR-021 新增覆盖。原决策与原依据正文不动；ADD §5.6.6 状态机由"单档 vendor SDK 下载面板"改写为"四类组件 HDCM 状态机"统一管理。

## ADR-017 Capability-Driven Adaptive Preset 架构（顶层决策机制；正文 §3.5 / §7.5.4 / §8.4）

- **决策**：放弃"每个子系统独立按 ADR 静态决策"模式，引入**三维 capability probe（硬件 ∩ 网络 ∩ 编码器）→ Preset 匹配 → 跨子系统协同配置**的运行期适配机制。Preset 是一组同时覆盖 decoder tier / jitter mode / render profile / present 调度 / RTCP 模式 / Frame Validity Gate 严格度的协同参数集；启动期一次性 apply，运行期由 ADR-020 热切。
- **依据**：
  - **跨子系统耦合**：当前各 ADR（007 GPU 选 / 008 DCOMP 档 / 012 render profile / 015 decoder 档 / Kalman jitter target）独立决策，最坏情况叠加；如 jitter buffer 不知道下游是否 ULTIMATE_DCOMP、RTCP 不知道编码器 zerolatency。在追求"客户端 8~12ms"极限延时时，子系统决策必须**联合优化**。
  - **应机适宜原则**：硬件能力 / 网络条件 / 编码器特征三者**只有运行期才能确知**——出厂时无法预测用户的 NIC、显示器、链路质量、对端编码器配置。把决策点推迟到启动期 + 运行期 telemetry，比静态绑定更优。
  - **行业对照**：NVIDIA Holoscan SDK / Microsoft Teams ZeroJitter / Cisco Webex / NVIDIA Maxine 均采用 capability-driven 自适应架构，是低延时实时影像领域行业最高水平模式。
  - **保留各子 ADR 不 Supersede**：原 ADR-007/008/012/015 等的"档位定义"作为 Preset 的可选输入仍有效；只是**最终决策点**从分散转移到 Preset Selector。
- **关系**：
  - 与 ADR-018（Network Probe）/ ADR-019（Encoder Probe）：两者是 Preset Selector 的输入维度；硬件维度复用 ADR-007 + ADR-015 caps_probe。
  - 与 ADR-020（Live Reload + Self-Upgrade）：本 ADR 定义启动期一次性 apply，Live Reload 定义运行期热切。
  - 与 ADR-014（Frame Validity Gate）：Gate 严格度可被 Preset 影响（极端低延时 preset 可放宽 fence/color 子集），但 refs / params / recovery / reorder 四 bit 永远 strict（正确性硬约束）。

## ADR-018 Network Capability Probe（运行期网络维度采集；正文 §7.5.2）

- **决策**：启动后 1-3 秒内（与 RTSP 握手 / WHEP ICE 并行）采集网络维度 capability：RTT p50/p95/p99、iat 抖动分布、短期丢包率、链路类型推断（LAN-switched / LAN-routed / Wi-Fi / WAN）、ECN 标记可用性、带宽 headroom。结果作为 ADR-017 Preset Selector 的输入维度。
- **依据**：
  - **网络维度对 jitter target 决策性影响**：LAN-switched RTT<1ms + jitter<0.5ms 场景下 jitter target 可压到 0~1ms（ZeroJitter 模式）；Wi-Fi jitter 5-30ms 突发场景必须 ≥15ms。当前 ADD §5.3.1 Kalman estimator 只看 iat，看不到链路特征——导致 LAN 场景过保守、Wi-Fi 场景偶尔过激进。
  - **链路类型推断启发式**：RTT + jitter pattern 区分链路（LAN-switched RTT<1ms + jitter<0.5ms / LAN-routed RTT 1-5ms + jitter 1-3ms / Wi-Fi RTT 1-10ms + jitter 5-30ms 突发 / WAN RTT>20ms 长尾）已被 Microsoft Teams / WebRTC 实践验证。
  - **延后到信令握手期采集**：probe 采集与 RTSP DESCRIBE/SETUP/PLAY 并行不增加首帧延时；首 GOP 数据足够给出 p95 估计。
- **关系**：与 ADR-019（Encoder Probe）/ ADR-007（GPU 选）一起构成 ADR-017 Preset Selector 的输入。

## ADR-019 Encoder Capability Probe（编码器维度采集；正文 §7.5.3）

- **决策**：从 SDP `profile-level-id` + SPS VUI（`max_num_reorder_frames`）+ 第一 GOP 实测（NAL type 序列、IDR/GDR 周期、bitrate 模式）推导编码器特征：codec/profile / reorder depth / GOP 类型 / IDR 周期 / bitrate 模式（CBR/VBR）/ zerolatency 提示 / 编码端 capture-to-send 延时估测。
- **依据**：
  - **B-Frame Policy 已部分采集**（ADD §5.6.4 优先级 1-2 解析 SDP + SPS reorder），但 mc-player 当前没把这些信息**联合**作为顶层决策输入。
  - **GDR-only 流的判别**：编码器仅发 GDR 不发 IDR 的 zerolatency 模式越来越普遍（x264 `--intra-refresh` / x265 `--refresh-rate`）；运行期检测前 N 帧 NAL type 即可识别。
  - **zerolatency 提示推断**：SDP fmtp + 实测无 reorder + 短 GOP 联合可推断对端是否 zerolatency 编码——这直接决定 mc-player 能否启用 SDI_REPLACEMENT preset。
- **关系**：与 ADR-018 / ADR-007 一起喂入 ADR-017 Preset Selector。

## ADR-020 Preset Live Reload + Self-Upgrade Loop（运行期适配协议；正文 §7.5.4 / §7.5.5）

- **决策**：运行期持续 telemetry 反馈，当**假设破坏**触发即立即热切到下一档 Preset；当**长期延时低于设计阈值**且无 tainted 事件，主动尝试**自驱升档**到更激进 Preset。失败立即降级 + 加入本会话 blacklist 不再尝试。
- **决策范围（hw.tier 维度限制）**：本 ADR 的 Preset 热切**不跨 ADR-015 解码档**——decoder 档位仍按 ADD §5.6.1 "降级一次性原则"在 `mc_open` 时一次确定，运行期不主动升档（如档 2 → 档 1）。原因：① 性能量度规范 §6 已有 `mc.decoder.cross_tier_demote_count`（降级）但故意不设 `cross_tier_promote_count`（升级），该缺失即设计声明；② 跨档升级要求重做完整的 decoder probe + reconfigure，与 ADR-013 双 buffered transition 类似但更复杂，超出本 ADR 范围；③ ADR-016 内置下载面板补 vendor SDK 后，仍需用户重启 / 重 `mc_open` 才能命中档 1。**SDI_REPLACEMENT preset 命中前提因此追加：启动期硬件 probe 已选中档 1 vendor SDK**——若启动时档 1 未就位、运行期再下载好 SDK，本会话保持档 2 + REALTIME_LAN preset，不切到 SDI_REPLACEMENT。
- **依据**：
  - **网络环境运行期变化**：Wi-Fi 信号强度 / WAN 拥塞 / 用户跨网络（家网→热点→蜂窝）。Preset 一次性绑定无法应对。
  - **设计裕度可被自动消化**：性能量度规范每个 metric 的 warm_steady 阈值是保守值；实际 LAN 场景延时常远低于阈值，主动升档可拿到额外 5~10ms 收益。
  - **失败回退 + blacklist**：避免在边界条件附近反复抖动（hysteresis）；本会话 blacklist 不持久化（下次 mc_open 重新评估）。
  - **正确性闸不变**：Preset 切换期间 Frame Validity Gate 永远 strict，热切自身不引入花屏；切换涉及子系统 reset（如 ZeroJitter→Kalman 切换）按"flush + 标 in-flight 帧 invalid → recovery_complete 解 freeze"的 Gate 路径处理（ADD §5.13）。
- **关系**：
  - 与 ADR-017：本 ADR 定义运行期热切协议；ADR-017 定义启动期一次性 apply。
  - 与 ADR-014（Frame Validity Gate）：切换路径所有 in-flight 帧标 invalid，等下一 refresh anchor 解 freeze——这是 Gate 污染态生命周期已规定的标准路径。
  - 与 ADR-015（四级降级链）：本 ADR 的 Preset 热切**不跨 decoder 档**；档间升级仅在用户重 `mc_open` 时发生（见决策范围段）。
  - 与性能量度规范 §11.3：Preset 切换次数 / blacklist 命中作为新增 metric 上报（具体字段在性能规范实施期补完）。

## ADR-021 Hardware Decode Component Manager（HDCM）：扩展 ADR-016 范围至 OS 媒体扩展 / Optional Feature / GPU driver（正文 §5.6.6）

- **决策**：ADR-016 "vendor SDK 内置下载面板"扩展为统一的 **Hardware Decode Component Manager（HDCM）**，按"用户层零命令"原则覆盖四类硬解组件、三种 in-app 安装模式：

  | 类别 | 组件 | 触发档位 | in-app 安装模式 |
  |---|---|---|---|
  | A | Vendor SDK redistributable（NVDEC / oneVPL / AMF） | 档 1 | 沿 ADR-016：WinHTTP 异步下载 + SHA-256 + Authenticode + 缓存到 `%LOCALAPPDATA%\mc-player\sdk\<vendor>\<version>\` |
  | B | Microsoft Store 媒体扩展（HEVC / AV1 Video Extension） | 档 3 codec 覆盖 | UWP `Windows.Services.Store::StoreContext::RequestDownloadAndInstallStorePackagesAsync`；用户视角 = app 内进度条；Store 不可用 SKU（典型：IoT LTSC）自动隐藏入口 |
  | C | Windows Optional Feature（MediaPlayback feature） | 档 3 整档可用 | helper.exe（`requireAdministrator` manifest）+ `DismApi::DismEnableFeature(L"MediaPlayback", ...)`；UAC 弹窗 → helper 执行 → 完成提示重启（DISM `DismRestartRequired_Possible` / `_Required` 状态）|
  | D | GPU driver | 档 2 + 档 3 driver-side 能力 | 检测 `Win32_VideoController.DriverVersion` vs 编译期内嵌阈值；落后即 `ShellExecuteEx` 一键跳浏览器到 vendor 官网驱动页（**setup.exe 不在 app 进程内运行**）|

  HDCM 默认 log level `silent`，诊断走"调高 log level → 复现 → 用户导出 log"，无主动 collector。

- **依据**：
  - **用户层零命令**：原 ADR-016 已实现类别 A in-app 全自动；让用户跑 `Enable-WindowsOptionalFeature` PowerShell 或外跳 Microsoft Store 客户端会割裂一致 UX。helper.exe + UAC 是 Windows 平台对系统级修改的标准 elevation 模式（COM elevation moniker / UAC manifest），不引第三方依赖。
  - **类别 B 法律合规**：HEVC Video Extension 是付费 MSIX/Appx 包，license 不允许第三方 redistribute；Store API 走官方分发通道是 in-app install 唯一合规路径。AV1 Extension 免费，同走 Store API。
  - **类别 D 工程边界**：GPU driver setup.exe 是大型侵入式安装（切显卡 / 黑屏 / 通常要求重启），与 mc-player 进程生命周期严重耦合时风险过高。"app 内检测 + 跳 vendor 官网"符合"硬件安装责任归 vendor 自家工具"的工程惯例。
  - **silent default 日志**：主动诊断 collector 在企业部署场景容易触发安全合规审计；按需调级 + 用户导出 log 是更轻的运维路径。
  - **保留 ADR-016 不 supersede**：ADR-016 vendor SDK 路径是 HDCM 类别 A 的全部实装；本 ADR 扩展类别 B/C/D，ADR-016 决策与依据正文不动，末尾追加扩展记录指向本 ADR。

- **关系**：
  - **扩展 ADR-016**：本 ADR 包含 ADR-016 全部内容（类别 A），新增类别 B/C/D。
  - 与 ADR-015：HDCM 是四级降级链的工程化前置——类别 A 提升档 1 命中率；类别 B 间接提升档 3 codec 覆盖（HEVC / AV1）；类别 C 让档 3 MFT subsystem 在 IoT LTSC 等裁剪 SKU 上可用；类别 D 影响档 2 / 档 3 driver-side 能力。**HDCM 不影响档间优先级**——硬件 path 最短优先按 ADR-015 不变，HDCM 只补全各档的 OS 前置。
  - 与 ADR-007 / ADR-013：HDCM 类别 A vendor 匹配 / 类别 D driver 检测均基于 active adapter（ADR-007 智能 GPU 选择结果）；多卡共存（如 NVIDIA dGPU + Intel iGPU）按 active 一侧分发组件；跨屏 transition（ADR-013）切 adapter 时不重新弹安装面板——HDCM 状态在 mc_open 生命周期内 immutable，下次重 `mc_open` 才重新 detect。
  - 与 ADR-020：HDCM 类别 A 在运行期下载好 vendor SDK 后，本会话保持当前档位（不跨档升档），与 ADR-020 "Preset 热切不跨 decoder 档" 决策范围一致；档 1 命中需用户重 `mc_open` / 重启 app。
  - 与 §2 #10 "平台原生优先"：类别 B 调用 UWP `Windows.Services.Store`（C++/WinRT interop）/ 类别 C 调用 `DismApi.dll` 均是 OS 原生 API，不引第三方依赖。类别 A 沿用 ADR-016 既定例外口子。
  - 与性能量度规范：metric 字段集中在 §6 末"`mc.hdcm.*`"块，排障扩展到 §10.5。
  - 与 plan Phase 8：原 vendor SDK 单点下载面板扩为 HDCM 四类组件，改动文件清单按本 ADR 切分。
  - 与 `doc/mc-player_hdcm_设计.md`（design-detail）：组件 manifest / 状态机伪代码 / helper.exe IPC 协议 / driver 阈值表 等实施细节统一在该 design-detail 文档。

---

## 修订规范

每条 ADR 的"决策"与"依据"正文是**该 ADR 写就时的历史快照**，原则上不再回头修改。当一条决策因实证或新约束被推翻或缩窄时：

1. **首选**：新增 ADR 引用旧 ADR 编号，旧 ADR 标 `Superseded by ADR-XXX` 或 `Narrowed by ADR-XXX`，旧 ADR 决策 / 依据正文不动。
2. **折中允许**：在旧 ADR 末尾追加 `**修订记录（YYYY-MM-DD）**` 段补充 narrowed 后的范围说明（如 ADR-001 / ADR-002 / ADR-004 已实践此模式），但**不修改原决策与原依据正文，标题中可加"原决策由 ADR-XXX 缩窄"标注**。
3. **禁止**：直接重写原决策段或原依据段；改写历史等于让 ADR 失去作为"决策时间线"的价值。

ADR 之间的内容引用应优先单向（新 ADR 引用旧 ADR）；多条 ADR 重复同一规则时，确定唯一权威 ADR，其余以"参见 ADR-XXX"代替全文重述（见 ADR-001 / ADR-002 引用 ADR-015 sync software MFT 排除规则的方式）。
