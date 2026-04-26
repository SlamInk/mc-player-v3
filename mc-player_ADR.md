# mc-player — 架构决策记录（ADR）

配套 `mc-player_架构设计文档_v3.0.md`。每条 ADR 只阐述决策与依据，正文章节是依据的展开。

---

## ADR-001 用 Media Foundation Transform 作主硬解后端

- **决策**：H.264 / H.265 / AAC 主硬解走 Windows Media Foundation Transform。
- **依据**：MFT 走 DXVA 与底层硬解管线一致；OS 内置零依赖、零 license 风险；Microsoft 官方持续维护。
- **正文**：§5.6

## ADR-002 硬件 MFT 走 async 事件驱动模型

- **决策**：同时枚举 hardware async + software sync MFT；激活硬件 MFT 后必启用 async unlock，用事件生成器驱动 NeedInput / HaveOutput 循环。
- **依据**：Microsoft 文档明确 "hardware MFT always process data asynchronously" —— 这不是性能选择，是协议要求；sync 调用模式会 stall。
- **正文**：§5.6.2 / §5.6.3

## ADR-003 dual-bind 通过 BindFlags 显式启用

- **决策**：在 MFT 输出流属性上显式请求 `BIND_DECODER | BIND_SHADER_RESOURCE`；renderer 用 `TEXTURE2DARRAY` + `FirstArraySlice` 创建 SRV；array slice 用 `ID3D11Fence` 等 SRV 读完才回收。
- **依据**：MFT 默认仅 `BIND_DECODER`，无此属性必须显式 GPU 内拷贝到独立 texture（破坏零拷贝）；fence 是防止 "slice 已 evict 但 shader 仍在采样" 的花屏不变量。
- **正文**：§4.3

## ADR-004 HEVC Extension 缺失自动 mc-libcodec 软解兜底

- **决策**：HEVC MFT 枚举为空时无感切 mc-libcodec 软解，UI 仅展示 hint。
- **依据**：Win10 / 11 retail 默认无 HEVC Extension；硬失败 UX 不可接受。
- **正文**：§7.4

## ADR-005 libdatachannel 作为 WebRTC 后端

- **决策**：libdatachannel 0.24.x（C/C++、SRTP/DTLS/ICE 一站式、MPL 2.0；自 v0.18 起从 LGPLv2.1+ 切换）。
- **依据**：vs libwebrtc（50 MB+ → 约 1–3 MB，~30× 缩减）；MPL 2.0 文件级 copyleft，与闭源主项目静态链接合法；活跃维护。
- **正文**：§5.2

## ADR-006 mc-libcodec 子项目（自研 H.264/H.265 主流 profile 软解）

- **决策**：自研 mc-libcodec 子项目 H.264 + H.265 主流 profile，参考 OpenH264 / libde265 架构（不复制代码），SSE2 / AVX2 SIMD。
- **依据**：OpenH264 BSD-2 仅 Constrained Baseline up to Level 5.2；dav1d AV1-only；自研换独立可复用 + 无外部 license 责任。
- **正文**：§5.7

## ADR-007 智能 GPU 选择（移除 GPU 偏好用户字段）

- **决策**：内部按 「HWND 跟随 + 能力兜底 + 跨屏热切换」 自动选 adapter；保留 LUID 显式覆盖。
- **依据**：HWND 跟随是延时最低的物理一致性策略；用户层无感，符合「专业播放器」隐形原则。
- **正文**：§7.3

## ADR-008 DCOMP 第 4 档作为智能 profile 自动激活档（best-effort）

- **决策**：实施 ULTIMATE_DCOMP；智能选择条件满足时自动激活；HUD 通过独立 DCOMP visual + vsync 与 video plane 解耦。
- **依据**：DCOMP 真正卖点是多 visual 的 plane 分离能力；Independent Flip 升级是 best-effort，运行时由 PresentMon 验证；不当作必胜档。
- **正文**：§5.10.2

## ADR-009 Opus 用 libopus（无 MFT 路径）

- **决策**：bundled libopus（BSD-3，~700 KB）。
- **依据**：OS 无 Opus MFT；OvenMediaEngine / MediaMTX / Cloudflare 全用 Opus；放弃「全 OS 自带」原则但 Opus 是 WebRTC 事实标准。
- **正文**：§5.8

## ADR-010 mc-libcodec 拆为独立子项目

- **决策**：monorepo 内独立子项目 `subprojects/mc-libcodec/`，独立 target、独立 API、独立 license（Apache 2.0）、独立测试套件。
- **依据**：独立可复用于 mc-encoder、其他工具；清晰 license 边界。
- **正文**：§5.7.1

## ADR-011 H.264 / H.265 SEP 专利责任声明

- **决策**：OS MFT 路径无责任；mc-libcodec 软解 distribute 时 distributor 自负；参考 Cisco OpenH264 源码-二进制责任分割模型。
- **依据**：Via LA AVC / HEVC pool 与 Access Advance 现行条款；Apache 2.0 §3 含明确专利 grant。
- **正文**：§10.4

## ADR-012 智能 render profile 选择（默认 AUTO）

- **决策**：运行期探测 ALLOW_TEARING + Hardware composition + DCOMP 可用性，自动选最佳档。
- **依据**：与 ADR-007 一致的 「专业播放器隐形」 哲学；VRR 探测依赖 ALLOW_TEARING 是应用层唯一权威 API，MPO / DirectFlip 依赖 `CheckHardwareCompositionSupport`。
- **正文**：§5.10.1

## ADR-013 跨屏 adapter switch 用 double-buffered transition

- **决策**：在新 adapter 上预创建 D3D11 device + IMFDXGIDeviceManager，旧 device 持续显示直到新 device 出第一帧再切换；上限 800 ms，超时 fallback 旧 adapter。
- **依据**：单 adapter 重建链 + 网络 RTT + I 帧等待经常超 200 ms；double-buffered 避免黑屏 UX。
- **正文**：§7.3.3

## ADR-014 正确性先于延时：Frame Validity Gate + Present Epoch

- **决策**：在 codec 与 render 之间设置协议无关的 Frame Validity Gate（§5.13）；render 侧设置 Present Epoch + Watchdog（§5.10.5）。把 refresh anchor、B-Frame Policy、color VUI 三级兜底、DPB 引用追踪、dual-bind fence、DCOMP 多 visual 同 epoch 提交六类机制收口。任一 validity bit 缺失即丢帧不 emit；任一 epoch 超时未推进即强制 redraw last-good。
- **依据**：花屏与陈旧区域未刷新比延时多 1 帧更不可接受。原架构对花屏只有 §11 一行 "decode error flags + freeze" 兜底、对陈旧区域无任何机制；散落 freeze 易因竞态被 Present。代价是错误恢复 / 首帧 / VUI 缺失场景多 freeze 1~3 帧——文档级明文权衡。
- **正文**：§2 设计原则 #12 / §5.13 / §5.10.5 / §5.5.3 / §5.6.6 / §5.12

---

> 当一条决策因实证或新约束被推翻，新增 ADR 引用旧 ADR 编号并标 Superseded，不修改历史条目。
