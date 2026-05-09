# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.


## 项目定位

mc-player 是 Windows 10 1809+ / Windows 11 x64 的多协议超低延时媒体播放器，目标客户端内部延时 8–25 ms。仓库为 monorepo：主项目 `mc-player/` + 并列子项目 `subprojects/mc-libcodec/` + 联调宿主 `demo-host/`。Windows-only — `CMakeLists.txt` 在非 WIN32 直接 `FATAL_ERROR`。


## 语言与平台

- 文档全部 zh-CN；新增技术文档延续中文叙述风格，技术术语 / API / 标识符保留英文原形。
- 目标平台 Windows 10 1809+ / Windows 11 x64；DirectX 11 Feature Level 11.0+；VC++ Runtime 2022。
- 当前开发主机：Windows 11 IoT 企业版 LTSC，Visual Studio 18 Enterprise（路径 `C:\Program Files\Microsoft Visual Studio\18\Enterprise`）。

## 约束
  - vlc和ffmpeg能硬解码成功的设备，mc-player 必须实现硬解码
  - 对比 vlc 源码 @resources/vlc-master
  - 确认硬件解码是否与vlc命中相同
  - 可以通过vlc cli 捕获日志 来对比分析
  - 当vlc 信息不全的情况下 通过 @resources/FFmpeg-master\ 源码来分析
  - 查阅一些相关资料 辅助分析 根因

## 权威文档（动手前先读）

代码不解释决策，决策由文档解释。**改任何架构相关行为前先确认对应文档章节**：

- `doc/mc-player_架构设计文档_v3.0.md` — 五层架构 / 数据流 / 线程视图 / 延时预算（ADD）。
- `doc/mc-player_ADR.md` — 23 条 ADR；ADR-014（Frame Validity Gate + Present Epoch）/ ADR-015（解码四级降级链）/ ADR-017（Capability-driven Preset）是必读核心。
- `doc/mc-player_性能量度规范.md` — metric 命名、phase 标签（`cold_start` / `warm_steady` / `error_recovery`）、CI 阈值。**CI 闸只对 `warm_steady` 设阈**。
- `doc/mc-player_capability_probe_设计.md` — 四维 capability probe 的字段与 selector 算法。
- `doc/mc-player_hdcm_设计.md` — Hardware Decode Component Manager（vendor SDK / OS feature / driver 的运行期获取）。
- `plan/mc-player_重构方案.md` — 14-phase / 20-commit roadmap；**每个 commit 必须命中本阶段 metric 阈值才能落地**。

写代码前若发现 ADD 假设错误：先开 ADR + 回标 ADD，再写代码。若发现 metric 字段不合理：先修 `性能量度规范.md`，再用新字段名落地。

## 构建

主开发流程：

```bat
:: RelWithDebInfo 默认；可选 Debug / Release。等价于 ninja-msvc-rwdi preset，
:: 自动 vcvars64 + 调用 VS 18 自带 cmake/ninja，构建 mc_player_demo + mc_player_dump_stats。
build.bat [Debug|RelWithDebInfo|Release]
```

CMake preset（VS / IDE 集成）：

```pwsh
cmake --preset ninja-msvc-rwdi   # 或 ninja-msvc-debug / vs
cmake --build build/ninja-msvc-rwdi --target mc_player_demo
cmake --build build/ninja-msvc-rwdi --target mc_player_dump_stats
```

CMake options：`MCP_ENABLE_LIBCODEC`（默认 ON，关掉则不链接软解兜底）/ `MCP_ENABLE_DEMO_HOST`（默认 ON）/ `MCP_ENABLE_TESTS`（默认 OFF）。

`mc_player_dump_stats` 是 internal CI 工具，直接链接 mc_player 静态库读取 `mcp::pal::Registry::snapshot()`，**不通过公开 C ABI**。`--self_test` 退出码 0 用于 Phase 0 验收。

联调运行：

```pwsh
.\build\ninja-msvc-RelWithDebInfo\demo-host\mc-player.exe rtsp://<host>/<path>
:: 或环境变量覆盖 demo-host 内置 URL：
$env:MCP_DEFAULT_RTSP_URL = "rtsp://..."
```

## 五层架构（`mc-player/src/`）

```
app/         → C ABI 实现 + Session（一个 mc_player_t = 一个 Session = 一条流）
controller/  → 生命周期 FSM + Adapter 选择 + Device Lost 恢复
transport/   → ts_rtsp_udp / ts_rtsp_tcp（WHEP 后续）；jitter buffer 之上协议合流
media/       → jitter / depack / codec_bridge / frame_validity_gate / render / av_sync
pal/         → Clock(QPC) / IOCP / SPSC / MMCSS / DXGI probe / Metric / ETW
hdcm/        → Hardware Decode Component Manager（detector + 4 类 installer）
probe/       → 四维 capability snapshot（hardware / network / encoder / render）
preset/      → Preset selector / apply / oscillation guard / live reload
```

`subprojects/mc-libcodec/` 不在五层内；通过 `mc_libcodec::mc_libcodec` target + 公开 API 接入 `codec_bridge`，独立 license（Apache 2.0）。

## 关键不变量（动代码前必须守住）

1. **解码档位 = ADR-015 四级链**：vendor SDK（NVDEC/oneVPL/AMF）→ DXVA-direct（D3D11VideoDevice）→ MFT hardware async（**仅 `hw_url=1 && async=1`**）→ mc-libcodec 软解。`MC_DECODER_MFT_SOFTWARE` 已被删除——sync software MFT **必须拒绝**（`MC_ERR_NO_HARDWARE`），不要再当成"硬件 MFT 同枚举兜底"。
2. **正确性 ≥ 延时（ADR-014）**：refs / params / recovery / reorder 四 bit 在任何 preset 下永远 strict。`mc.gate.drop_*_count` 在 `warm_steady` = 0 是所有 phase 的正确性闸。
3. **dual-bind 零拷贝（ADR-003）**：MFT 输出 / DXVA-direct 输出必须显式请求 `BIND_DECODER | BIND_SHADER_RESOURCE`；renderer 用 `TEXTURE2DARRAY` + `FirstArraySlice` 创建 SRV；array slice 用 `ID3D11Fence` 等读完才回收（防"slice 已 evict 但 shader 仍在采样"花屏）。
4. **公开 ABI 演进契约**：`include/mc-player/*.h` 所有 struct 第一字段 `size_t struct_size`，第二字段 `uint32_t struct_version`。改字段必须 bump 对应 `MC_*_VERSION` 宏；添加字段往尾部追加。
5. **没有硬编码 URL / 端口 / 超时**：`mc_open_options_t` 决定一切；超时为 0 时落到内部命名常量，不要散写 magic number。
6. **不要把命名常量改成 magic number**：v1 RTSP-only 但 transport 选择仍走 `protocol_hint`。

## 线程与 MMCSS（ADD §3.3）

| 线程 | MMCSS task | 备注 |
|---|---|---|
| T2 Network RX | 不挂 MMCSS；TIME_CRITICAL；P-core 绑定 | RTSP-UDP 收包 |
| T4 Video Decode | Playback | vendor SDK / DXVA-direct / MFT async / libcodec |
| T5 Video Render | Playback | D3D11 Present + DCOMP commit |
| T6 Audio Render | Pro Audio | WASAPI event-driven |
| T7 WHEP-PC | 不挂 MMCSS；HIGHEST | libdatachannel 内部 |

Hot path（per-frame / per-RTP-packet）严禁直接日志。用 `LogLevel::trace` + ETW，让 PerfView / WPR 离线分析。日志默认级别按 plan §8.E 走 `LogLevel::silent`（仅 ETW）。

## 性能量度规范要点

- 任何 metric 上报必须带 `phase ∈ {cold_start, warm_steady, error_recovery}` tag。
- 阶段 timer 用 `mcp::pal::scoped_timer`（`pal/clock.h`）；9 段 `mc.stage.*_ns` 全埋是 Phase 0 验收硬条件。
- Frame Validity Gate 六 bit 各自 counter 全部上报（即使值为 0）。
- ETW Provider GUID `F8E1A0C2-7A3B-4C1F-9D2E-6B5A4F8C1234`（self-describing TraceLogging，`logman query providers` 不可见是 Windows 已知行为；用 `mcp::pal::etw::is_registered()` 自检）。

## 重构节奏（plan/ 决定一切）

- 每个 commit = 一个 plan phase 完成。message 格式：`refactor(phase-N): <summary>` 或 `feat(phase-N): <summary>`，body 列阶段验收 metric 的 before/after 实测值。
- **变更范围闸**：commit 只能改本阶段计划列出的目标文件；未涉及模块禁止顺手改。
- **回归基线**：720p H.264 RTSP + 4K H.265 RTSP（Phase 4/5/6/7 强制必跑）。每阶段 vs VLC 同机并行播流，时钟差 ±2ms（档 1/档 2 命中时）。
- **stop conditions**：metric 退化 > 5% / `mc.gate.drop_*_count > 0` / `mc.err.device_lost_count` rate 在不该升的阶段升高 → 必停。

## 编码风格

C++20 / `/W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8`。命名空间 `mcp::<layer>`；公开 C ABI 用 `mc_*` snake_case。文件头注释引用对应 ADD 章节 / ADR 编号（已是仓库惯例）。COM / D3D / WASAPI / MF 直调，不引第三方包装层（ADD §2 #10 平台原生优先；例外仅限已开口的 libdatachannel / libopus / vendor SDK 见 ADR）。

## 跨会话记忆

会话级记忆走 `C:\Users\Echo\.claude\projects\D--Workspace-Code-mc-player-v3\memory\`（auto memory 系统）。**实施期发现的稳定性 / 性能模式（surprising）写 memory，不写 doc**——doc 是 design-only。
