# mc-player — 代码重构方案

| 项目 | 内容 |
|---|---|
| 文档类型 | 实施性 roadmap（不属于 ADD / ADR / 性能量度规范）|
| 上游依据 | `doc/mc-player_架构设计文档_v3.0.md` / `doc/mc-player_ADR.md` / `doc/mc-player_性能量度规范.md` / `doc/mc-player_capability_probe_设计.md` / `doc/hardware-decode-dependencies.md` / `CLAUDE.md` |
| 落地节奏 | 16 个阶段（Phase 0 ~ Phase 8 + Phase 9.0 ~ 9.5 + Phase 10），每阶段 = 一次 `git commit + push` |
| 验收门槛 | 每阶段必须达到对应**性能量度规范字段阈值**才进入下一阶段；未达不 commit |
| 不在范围 | 具体 API 头文件签名、cmake target 名、UI 面板视觉稿——这些在阶段实施时按需细化 |

> 当前代码状态摘要：五层架构（PAL / transport / media / controller / app）+ mc-libcodec 子项目均已成型；Codec Bridge / Frame Validity Gate / Present Epoch 已实装但需对位修正；`codec_mft_video.cpp` 同时支持 async hardware MFT 与 sync software MFT，**与 ADR-015 不一致**（sync software MFT 应被排除而非接受）；`codec_dxva_video.cpp` 仅 HEVC；vendor SDK 档全部未实装；性能 metric 仅简单计数，缺 histogram / 分位数 / phase 标签。

---

## 0. 总则

### 0.1 阶段交付不变量

每个阶段提交（git commit + push）必须满足：

1. **正确性闸**：`mc.gate.drop_*_count` warm_steady 期间 = 0（color_meta 允许首帧后 ≤3 帧）；`mc.render.resize_clearstate_violation_count = 0`；`mc.render.epoch_pair_skew_count = 0`。三条无论阶段聚焦何处都必须维持。
2. **延时闸**：阶段定义的对应 metric 通过；不能让上一阶段已通过的 metric 退化（无回归）。
3. **资源闸**：`mc.res.handle_count` / `mc.res.process_rss_bytes` 稳态 ±常数（leak 检测）；`mc.pool.<*>.alloc_overflow_count = 0`。
4. **变更范围**：commit 改动文件仅限本阶段计划列出的目标文件；未涉及的模块禁止触碰（避免 review 噪音 + 回归责任不清）。
5. **commit message 格式**：`refactor(phase-N): <summary>` 或 `feat(phase-N): <summary>`，body 列出该阶段验收 metric 的实测值（before / after）。

> **基础设施 phase 验收豁免**：Phase 0（性能量度地基）是引入 metric 字段本身的阶段，**正确性闸 / 延时闸 / 资源闸三条仅看埋点完整性而非业务 metric 达标值** —— 此时业务路径可能仍有未修的 +100ms sync MFT 误用等已知问题，本阶段验收只要求 metric 字段全部上报、ETW Provider 注册可见、stats JSON dump 工具可输出。具体业务 metric 达标由 Phase 1 起的"延时闸 / 正确性闸"按本表常规执行。该豁免仅适用于"引入度量基础设施"的 phase，不适用于其他业务 phase。

### 0.2 stop conditions（任一触发即暂停 + 重新评估）

- 任一 metric 退化 > 5%（即上一阶段实测的 baseline）→ 回滚或修复后重测
- 出现花屏（任一 `mc.gate.drop_*_count > 0` warm_steady 内）→ 必停
- `mc.err.device_lost_count` rate 在阶段实施期间显著升高（设计未引入新 device 路径却有 lost）→ 必停
- 任何"看起来过的但 metric 未跑全"的情况——一律不算过

### 0.3 与文档体系的协同

- 实施期发现 ADD 设计假设错误（如某 metric 永远到不了阈值）→ 先开 ADR 修正决策、回标 ADD 章节，**再写代码**。代码不解释决策，决策由文档解释。
- 实施期发现性能量度规范字段名 / 阈值不合理 → 先修订 `doc/mc-player_性能量度规范.md` 并 commit 文档，**再用新字段名落地**。
- 实施期发现 capability probe 的 struct 字段 / selector 算法 / preset 配置不合理（如新增 vendor-specific 字段、selector 边界条件需调整、preset 子系统参数表与实测不符）→ 先修订 `doc/mc-player_capability_probe_设计.md` 并回标 ADD §3.5 / §7.5，**再用新字段或新算法落地**。design-detail 层文档与 ADD/ADR 同等权威。
- 阶段实施完成后若发现新的稳定性 / 性能模式（surprising），写入 `memory/`（auto memory 系统，跨会话），不写入 `doc/`（design-only）。

### 0.4 测试基线

- **回归基线流**：本仓库 `mc-player-dump.h265` + 一路 720p H.264 RTSP（参考 `hardware-decode-dependencies.md` §5）+ 一路 **4K H.265 RTSP（必测，Phase 4 / 5 / 6 / 7 强制 4K 验收）**。仓库应在 CI 工件区或测试目录提供 4K H.265 测试流（推荐使用 ITU-T HM 参考流或自录 4K 样本），缺失时阶段不予 commit。
- **VLC 对照**：每阶段实测时同一台机器同一路 RTSP 流并行播 mc-player 与 VLC，比时钟差。VLC 是事实基线（应用层管 DPB + D3D11VA），mc-player 偏差应稳定收敛到 ±2ms 以内（档 1 / 档 2 命中时）。
- **平台覆盖**：开发机（Win11 IoT LTSC + Intel UHD 730）必测；CI 上 Win11 / Win10 22H2 / Win11 IoT LTSC 三档。

---

## Phase 0 — 性能量度地基

> **不能跳过**——后续每个阶段的验收都依赖本阶段产出的 metric 字段。

### 0.0 目标

把性能量度规范 §2 / §3 / §4 / §5 的关键 metric 字段从"文档定义"落到"运行期可采集"——9 段阶段 timer + Frame Validity Gate 六 bit drop counter + 队列水位 + CPU 按线程 + decoder_kind / probe 信息全部上报。

### 0.1 改动范围

| 文件 / 目录 | 动作 |
|---|---|
| `mc-player/src/pal/metric.{h,cpp}`（新增） | 引入 t-digest（小型嵌入式实现，~300 行）+ HDR Histogram fallback；`Counter / Gauge / Histogram / Timer` 四类原语 |
| `mc-player/src/pal/etw_provider.{h,cpp}`（新增） | TraceLogging Provider GUID 申请；按 metric 类型映射 ETW Event |
| `mc-player/src/pal/clock.{h,cpp}`（已有，扩展） | 暴露 `qpc_now_ns()` 统一接口；`scoped_timer{name, phase}` RAII 工具 |
| `mc-player/include/mc-player/mc_player_types.h` | 扩展 `mc_stats_t` 暴露 §2/§3/§4/§5 关键 metric 的快照（保持 ABI `struct_size + version` 演进契约） |
| `mc-player/src/app/mc_player_session.cpp` | `get_stats` 实现填充新字段 |
| `mc-player/src/controller/controller.cpp` 全套 hot path | 在 9 个 ADD §8.1 阶段插入 `scoped_timer` |
| `mc-player/src/media/codec_bridge.cpp` | 阶段内 emit 处加 timer 边界 + drop counter 调用 |
| `mc-player/src/media/frame_validity_gate.cpp` | 六 bit drop counter / 污染态 enter / duration / source 上报 |
| `mc-player/src/pal/spsc_queue.h` | 加 depth / high_water / drop_oldest_count 暴露接口 |

### 0.2 实施步骤

1. **第一周**：metric 库 + ETW Provider + clock 扩展。单元测试（pal/test/metric_test.cpp）覆盖 t-digest 分位数计算正确性。
2. **第二周**：在 controller hot path 落 9 段阶段 timer + 队列水位 + Frame Validity Gate drop counter。手动对比 ETW 输出与设计目标。
3. **第三周**：`mc_stats_t` ABI 扩展 + `get_stats` 实装；命令行工具 `tools/dump_stats.cpp` 跑一段流后导出 JSON 给 CI 校验。

### 0.3 验收 metric（必达后才 commit）

| metric | 阈值 |
|---|---|
| 9 段 `mc.stage.*_ns` | 全部出现在 ETW 输出 + stats API；缺 1 项即 fail |
| `mc.queue.*.{depth, high_water, drop_oldest_count}` | 6 条 SPSC 队列全埋（ADD §6.1 表 5 队列 + Frame Validity Gate 入队） |
| `mc.gate.drop_<bit>_count` | 六 bit 各自 counter 上报（即使值为 0） |
| `mc.decoder.kind` | 上报为 enum 实际值（非 NONE）|
| `mc.res.cpu_thread_t{2,4,5,6,7}_ratio` + `mc.res.cpu_process_ratio` | 全部 gauge 实时更新 |
| ETW Provider GUID | 已申请；`logman query providers` 看得见 |
| stats JSON dump 工具 | 跑 60 秒回归基线流后输出符合 schema 的 JSON |

### 0.4 commit 模板

```
refactor(phase-0): 引入性能量度地基（t-digest + ETW Provider + 9 段 timer）

按性能量度规范 §2.2 / §3.4 / §4.1 / §5.1 落地：
- pal/metric: Counter/Gauge/Histogram/Timer 四类原语 + t-digest 嵌入实现
- pal/etw_provider: TraceLogging Provider，按 metric 类型映射 ETW Event
- 9 段 mc.stage.*_ns timer 全埋，对位 ADD §8.1 表
- mc.queue.* 6 条 SPSC 队列水位 + drop counter
- mc.gate.* 六 bit drop counter + 污染态生命周期
- mc_stats_t ABI 扩展（保持 struct_size + version 契约）

before/after 对比（基线流 720p H.264 60 秒）：
- ETW 输出: 0 → 9 段 + 6 队列 + 6 gate bit + ...
- stats JSON dump: 不可用 → 完整 schema
```

---

## Phase 1 — ADR-015 拒绝 sync software MFT + 档 4 fallback 修正

> 修复实测的 +100ms 延时根因（sync software MFT 误当硬解）。这是当前的"火"，必须先扑。

### 1.0 目标

- `codec_mft_video::start` 检测到仅 sync software MFT 可用时，**直接返回 `MC_ERR_NO_HARDWARE`**；不再把 sync MFT 当作"硬件 MFT 的同枚举兜底"。
- `mc_decoder_kind_t` 枚举扩展为 ADR-015 + 性能量度规范定义的 6 值 + NONE = 7 值；删除已不合规的 `MC_DECODER_MFT_SOFTWARE`。
- `controller::start_decode_pipeline` 改为四档调度雏形（档 1 / 档 2 / 档 3 / 档 4），档 1 留 stub（Phase 5/6/7 实装），档 2 H.264 留 stub（Phase 4 实装），档 3 / 档 4 已实装连接好。
- `mc.probe.tier_skip_reason{tier=N}` 埋点完整。

### 1.1 改动范围

| 文件 | 动作 |
|---|---|
| `mc-player/include/mc-player/mc_player_types.h` | `mc_decoder_kind_t` 改 7 值（NONE / VENDOR_SDK_NVDEC / _ONEVPL / _AMF / DXVA_DIRECT / MFT_HARDWARE / LIBCODEC）；ABI version bump |
| `mc-player/src/media/codec_mft_video.cpp` | `start()` 增加 `is_sync_software_mft_only()` 检测；只有 sync software 可用时返 `MC_ERR_NO_HARDWARE`；删除整段 sync_worker_loop（不再支持 sync 路径） |
| `mc-player/src/media/codec_mft_video.h` | 删除 `is_sync_mft` 字段语义、`sync_worker_loop` 声明 |
| `mc-player/src/controller/controller.cpp` | `start_decode_pipeline()` 改为按 ADR-015 顺序逐档尝试；每档失败上报 `mc.probe.tier_skip_reason`；选档结果上报 `mc.decoder.kind` |
| `mc-player/src/media/codec_bridge.cpp` | `active_kind` 同步使用新枚举；删除 `MC_DECODER_MFT_SOFTWARE` 处理分支 |

### 1.2 实施步骤

1. enum 扩展 + 调用方编译错误一次性修通（依赖 ABI version bump 的 ADR 已隐含在 ADR-015）。
2. `is_sync_software_mft_only`：检测 `MFTEnumEx(ALL)` 列表中所有候选都 `hw_url=0 && async=0` 即返 true（参考 `hardware-decode-dependencies.md` §1 "MFT 默认即 SW"案例）。
3. `start_decode_pipeline` 四档骨架：档 1 stub 永远返 `MC_ERR_NO_HARDWARE`（vendor 不匹配）；档 2 H.264 stub 永远返 `MC_ERR_NO_HARDWARE`（H.264 未实装），HEVC 走原 codec_dxva_video；档 3 走改造后的 codec_mft_video；档 4 走 codec_libcodec。
4. 每档 `mc.probe.tier{N}_ns` timer + `mc.probe.tier_skip_reason{tier=N}` counter 上报。
5. 在 IoT LTSC + Intel UHD 730 主机上跑回归基线流（720p H.264）：以前会进 sync software MFT，现在应进 codec_libcodec（因为档 1/2 stub 失败 + 档 3 拒绝 sync）。

### 1.3 验收 metric

| metric | 阈值（warm_steady） | 对比基线 |
|---|---|---|
| `mc.decoder.kind` | 720p H.264 IoT LTSC：`LIBCODEC`（不是 `MFT_SOFTWARE`） | 旧：错误的 `MFT_HARDWARE` 标签|
| `mc.probe.tier_skip_reason{tier=3}` | 同环境出现 `sync_software_only` reason | 新增 |
| `mc.e2e.client_internal_ns` p95 | ≤25ms（ADD §1.2，前提是 mc-libcodec SIMD 已达标） | 旧：~125ms（含 +100ms 误用 sync MFT 的延时）|
| `mc.stage.decode_actual_ns` p95 | 档 4 软解：≤25ms | 新基线 |
| 性能量度规范 `MFT_SOFTWARE` 字段 | 不再出现（已删除） | 旧：存在 |

vs VLC 时钟差：从 +101ms → 取决于 mc-libcodec SIMD 实装，预期 +5~30ms（档 4 软解延时 baseline）。**不再有"被误用 sync MFT 的 +100ms"**。

### 1.4 已知风险

- 删除 sync_worker_loop 后，HEVC Microsoft Extension 用户原本走 sync MFT，现在走档 4 软解；如果 mc-libcodec H.265 软解还有 bug 会暴露。**应对**：跑 mc-libcodec H.265 单元测试套件 + ITU-T HM reference 对比 100% bit-exact。
- enum 删值是 ABI break；外部调用者需重新编译。**应对**：本仓库 ABI 由 `struct_size + version` 演进契约保护，version bump 即可；下游 demo-host 一次性更新。

### 1.5 commit 模板

```
refactor(phase-1): ADR-015 拒绝 sync software MFT + decoder_kind 6 值扩展

- codec_mft_video::start 检出仅 sync software MFT 可用即返 MC_ERR_NO_HARDWARE
- mc_decoder_kind_t 改 7 值（含 NONE）；删除不合规的 MC_DECODER_MFT_SOFTWARE
- controller::start_decode_pipeline 四档调度（档 1/2 stub，档 3/4 实装）
- 删除 codec_mft_video sync_worker_loop（与 ADR-002 范围澄清一致）

before/after（IoT LTSC + UHD 730，720p H.264）：
- mc.decoder.kind:        MFT_HARDWARE(误)  → LIBCODEC(正)
- mc.e2e.client_internal_p95: 125ms          → 22ms
- vs VLC 时钟差:          +101ms             → +12ms
```

---

## Phase 2 — Frame Validity Gate 完整六 bit 收口

> 当前 Gate 已实装，但需对照 ADD §5.13 / 性能规范 §4 验完整性，补缺漏 + 加完整 metric。

### 2.0 目标

- 六 bit 全部按 ADD §5.13 表实装（refs / params / recovery / color / reorder / fence）。
- 污染态生命周期（进入 → recovery_complete 解除）trace 完整。
- `mc.gate.tainted_source` 6 类触发源全部能区分（seq_gap / fnum_gap / mft_decode_err / device_lost / adapter_switch / resize_inflight）。
- `mc.fb.pli_sent_count` / `mc.fb.nack_recovery_ratio` 上报。

### 2.1 改动范围

| 文件 | 动作 |
|---|---|
| `mc-player/src/media/frame_validity_gate.{h,cpp}` | 对照 ADD §5.13 表自查六 bit；补缺；加污染态生命周期与 source 标签 |
| `mc-player/src/media/depack_h264.cpp` / `depack_h265.cpp` | seq_gap / FrameNum gap 检测后置位污染源标签 |
| `mc-player/src/media/codec_mft_video.cpp` / `codec_dxva_video.cpp` / `codec_libcodec.cpp` | decode error flag → 污染源 |
| `mc-player/src/controller/device_lost.cpp` | device_lost / adapter_switch 触发污染源 |
| `mc-player/src/media/render_swap_chain.cpp` | resize_inflight 触发污染源 |
| `mc-player/src/transport/rtcp.cpp` / `mc-player/src/media/nack_module.cpp` | PLI / NACK 计数 + recovery_ratio 计算 |

### 2.2 实施步骤

1. 把当前 frame_validity_gate.cpp 与 ADD §5.13 表逐 bit 对位审计；补 reorder_resolved / gpu_fence_signaled 如未到位。
2. 污染态结构：`enum class TaintSource { SeqGap, FnumGap, MftDecodeErr, DeviceLost, AdapterSwitch, ResizeInflight };` 通过 channel 上报到 metric 层。
3. 每个污染源点（5 个文件）调用 `gate.taint(source, pts)`；`gate.untaint(pts)` 仅在 recovery_complete 处。
4. NACK 模块跟踪每条 NACK 是否收到对应 RTX；ratio = 收到数 / 发出数，1 Hz 聚合上报。

### 2.3 验收 metric

| metric | 阈值（warm_steady） |
|---|---|
| `mc.gate.drop_<bit>_count` | 六 bit 各自上报，warm_steady 期间总和 = 0（color_meta 允许首帧后 ≤3 帧）|
| `mc.gate.tainted_state` | 故意制造 SeqGap → 进入污染 → 等到下一 IDR/IRAP 解除，gauge 0→1→0 整路径 trace 出现 |
| `mc.gate.tainted_source` | 六类触发源各做一遍故障注入，能区分 |
| `mc.fb.pli_sent_count` | 故障注入后每参考链断裂触发 1 次 PLI |
| `mc.fb.nack_recovery_ratio` | LAN 模拟丢包 1% 时 ≥0.95 |

### 2.4 commit 模板

```
refactor(phase-2): Frame Validity Gate 六 bit 完整收口 + 污染态生命周期 metric

- 验证 frame_validity_gate.cpp 对位 ADD §5.13 表，补全缺失 bit
- 6 类污染源（SeqGap/FnumGap/MftDecodeErr/DeviceLost/AdapterSwitch/ResizeInflight）trace 完整
- mc.gate.tainted_{state,source,duration_ns,enter_count} 上报
- mc.fb.{pli_sent,nack_sent,nack_recovery_ratio} 上报
- 故障注入测试：6 类污染源各跑一遍验证 → recovery_complete 路径

before/after（基线流 + 1% 模拟丢包）：
- mc.gate.tainted_state 故障注入: 不可见 → trace 完整
- mc.fb.nack_recovery_ratio:        N/A → 0.97 (LAN)
- warm_steady mc.gate.drop_*_count: 不确定 → 0
```

---

## Phase 3 — Present Epoch + Watchdog 完整收口

> 当前 present_epoch 已实装，对照 ADD §5.10.5 验完整性 + watchdog redraw 路径 + 不变量 metric。

### 3.0 目标

- T5 是 `IDCompositionDevice::Commit` **唯一**调用方（grep 全仓库验证）。
- Present 与 DCOMP commit 同 epoch 配对，`mc.render.epoch_pair_skew_count` 永久 0。
- 陈旧区域 watchdog：超过 N×frame_period 未 Present 推进强制 redraw。
- ResizeBuffers 前 ClearState + SRV/RTV/UAV 释放不变量自动检测。

### 3.1 改动范围

| 文件 | 动作 |
|---|---|
| `mc-player/src/media/render_present_epoch.{h,cpp}` | 对照 ADD §5.10.5 自查；补 watchdog 时间检测 + DCOMP commit 单点权威 |
| `mc-player/src/media/render_swap_chain.cpp` | ResizeBuffers 前 ClearState 调用 + SRV/RTV/UAV 显式释放；违反时 `mc.render.resize_clearstate_violation_count++` |
| `mc-player/src/media/render_dcomp.cpp` | 验证 Commit 唯一调用点；移除任何非 T5 路径的 Commit |
| `mc-player/src/media/render_d3d11.cpp` | 加 PresentMon 集成（运行期读 `Hardware: Independent Flip` 等实际 PresentMode）|
| `mc-player/src/media/ui_overlay.cpp` | HUD visual 内容更新经 T5 队列汇总（不绕开 Commit 唯一权威）|

### 3.2 实施步骤

1. grep `IDCompositionDevice::Commit` 调用点；只允许出现在 render_present_epoch 内一处。如多处 → 重构归一。
2. epoch id 单调递增 + Present / commit 配对状态机；任一缺失置 `epoch_pair_skew_count++`。
3. watchdog 在 T5 1Hz 检查"最近 Present QPC vs now"；超过 3×frame_period 未推进 → 强制 redraw last-good。
4. ResizeBuffers 前自动调 ClearState + 遍历释放本进程持有的 back buffer view。
5. PresentMon 集成（用 `IDXGISwapChain1::GetFrameStatistics` + ETW Microsoft-Windows-DxgKrnl）拿真实 PresentMode。

### 3.3 验收 metric

| metric | 阈值 |
|---|---|
| `mc.render.epoch_pair_skew_count` | = 0 永久 |
| `mc.render.resize_clearstate_violation_count` | = 0 永久 |
| `mc.render.dcomp_commit_count` | 与 `mc.render.present_fps` 大致同步（差额 < 1%） |
| `mc.render.watchdog_redraw_count` | 故意制造 freeze → 至少 1 次触发 |
| `mc.render.profile_actual` | EXTREME 档：`Hardware_Independent_Flip`（实测 PresentMon）|

### 3.4 commit 模板

```
refactor(phase-3): Present Epoch + Watchdog 单点权威收口

- IDCompositionDevice::Commit 唯一调用点（T5 render_present_epoch）
- Present 与 DCOMP commit 同 epoch 配对，skew counter 永久 0
- 陈旧区域 watchdog: 3×frame_period 未推进强制 redraw last-good
- ResizeBuffers 前 ClearState + SRV/RTV/UAV 释放不变量自动检测
- PresentMon 集成 mc.render.profile_actual 实测上报

before/after:
- IDCompositionDevice::Commit 调用点数: N → 1
- mc.render.epoch_pair_skew_count: 不确定 → 0
- mc.render.profile_actual:        猜测 → 实测
```

---

## Phase 4 — DXVA-direct 实装 H.264（档 2 H.264 覆盖）

> 当前 codec_dxva_video.cpp 仅 HEVC；H.264 是绝大多数监控摄像机协议，必须覆盖。

### 4.0 目标

- `codec_dxva_video.cpp` 补 H.264 实装：DXVA_PicParams_H264 / Slice_H264_Short / DPB（参考 mc-libcodec §5.7.3 SPS/PPS/SliceHeader parser 复用）。
- `controller::start_decode_pipeline` H.264 流先尝试档 2 DXVA-direct，失败才到档 3 / 档 4。
- HEVC 路径不回归（regression test）。

### 4.1 改动范围

| 文件 | 动作 |
|---|---|
| `mc-player/src/media/codec_dxva_video.cpp` | 新增 ~600 行 H.264 实装（PicParams / SliceShort / DPB 表 / RefPicList 管理 / POC 计算） |
| `mc-player/src/media/codec_dxva_video.h` | 工厂 / 参数支持 H.264 codec |
| `mc-player/src/media/dxva_h264_picparams.{h,cpp}`（新增）| DXVA_PicParams_H264 填表逻辑 |
| `mc-player/src/media/dxva_h264_slice.{h,cpp}`（新增）| Slice_H264_Short 填表 |
| `subprojects/mc-libcodec/include/mc-libcodec/parser_h264.h`（新增公开 API） | SPS/PPS/SliceHeader parser 抽出供 dxva 复用 |
| `mc-player/src/controller/controller.cpp` | start_decode_pipeline 档 2 stub 替换为真实 codec_dxva_video for H.264 |

### 4.2 实施步骤

1. **mc-libcodec 公开 SPS/PPS/SliceHeader parser**：把 `subprojects/mc-libcodec/src/decoder_h264.cpp` 内的 SPS/PPS/slice header 解析逻辑抽到独立 `parser_h264.h`，加到公开 API（演进式 ABI）；`decoder_h264.cpp` 反向引用自己的公开头。
2. **DXVA picparams 填表**：参考 ITU-T H.264 spec + Microsoft DXVA 2.0 H.264 Annex（位字段语义对照）；填 `wFrameNum`, `field_pic_flag`, `RefFrameList[16]`, `FieldOrderCntList[16][2]`, ...
3. **DPB**：在应用层维护 `{frame_num, poc, refs_used_by, mark}` 表；按 §5.6.2.2 行 4 复用 mc-libcodec DPB 设计。
4. **每帧 submit 流程**：`DecoderBeginFrame(view)` → 4 个 `D3D11VideoDecoderBuffer`（PicParams / Bitstream / SliceControl）→ `SubmitDecoderBuffer × N` → `DecoderEndFrame` → fence wait。
5. 接通 controller → 验证 `mc.decoder.kind=DXVA_DIRECT` for H.264。
6. **回归**：HEVC 流不要变到档 4——必须保持 `DXVA_DIRECT`。

### 4.3 验收 metric

| metric | 阈值 |
|---|---|
| `mc.decoder.kind`（720p H.264）| `DXVA_DIRECT` |
| `mc.decoder.kind`（720p HEVC）| `DXVA_DIRECT`（不回归） |
| `mc.stage.decode_actual_ns` p95（H.264 档 2）| ≤8ms（ADD §1.2 表）|
| `mc.gate.drop_*_count` warm_steady | = 0（不能因为 H.264 实装新引入花屏） |
| vs VLC 时钟差（H.264 720p） | ≤+5ms |

### 4.4 commit 模板

```
feat(phase-4): codec_dxva_video 补 H.264 实装（档 2 H.264 覆盖）

- mc-libcodec 公开 SPS/PPS/SliceHeader parser（演进式 ABI 加新头）
- codec_dxva_video.cpp 补 ~600 行 H.264 实装（PicParams/Slice/DPB/POC）
- controller H.264 流先档 2 DXVA-direct 探测，失败再降档
- HEVC 路径回归测试通过

before/after（720p H.264，UHD 730）：
- mc.decoder.kind:           LIBCODEC → DXVA_DIRECT
- mc.stage.decode_actual_p95: 22ms     → 6ms
- vs VLC:                    +12ms     → +2ms
```

---

## Phase 5 — Vendor SDK 档 1：NVDEC（NVIDIA 优先）

> NVIDIA dGPU 在低延时直播场景部署最多；先做 NVDEC 受益面最大。

### 5.0 目标

- NVDEC 实装：`cuvidParseVideoData` + `cuvidDecodePicture` + `cuvidMapVideoFrame` + `cudaGraphicsD3D11RegisterResource`。
- 严格按性能量度规范 §6 + ADR-015 §5.6.2.1：`CUVIDPARSERPARAMS::ulMaxDisplayDelay = 0` + 每包 `CUVID_PKT_ENDOFPICTURE`。
- runtime LoadLibrary `nvcuvid.dll` + GetProcAddress 动态绑定（缺失时档 1 跳过，不阻断启动）。
- 抽象 vendor SDK 公共接口（CodecVendorBase），便于 Phase 6/7 复用。

### 5.1 改动范围

| 文件 | 动作 |
|---|---|
| `mc-player/src/media/codec_vendor_base.{h,cpp}`（新增） | vendor SDK 抽象基类：probe / open / submit / pull / close 五件套 |
| `mc-player/src/media/codec_nvdec.{h,cpp}`（新增 ~800 行） | NVDEC 实装；动态加载 nvcuvid.dll + cudart64_*.dll |
| `mc-player/src/media/cuda_d3d11_interop.{h,cpp}`（新增） | `cudaGraphicsD3D11RegisterResource` + `cuGraphicsMapResources` 互通 |
| `mc-player/src/controller/controller.cpp` | start_decode_pipeline 档 1 NVDEC stub 替换为真实实装；`DXGI_ADAPTER_DESC1::VendorId == 0x10DE` 才尝试 |
| `mc-player/src/pal/dxgi_caps_probe.cpp` | 加 `nvcuvid.dll` 存在性 probe + version 字符串 |

### 5.2 实施步骤

1. CodecVendorBase 抽象：纯虚 5 方法（与 mft_video / dxva_video 接口对位）。
2. NVDEC 加载顺序：`LoadLibraryW("nvcuvid.dll")` → 13 个关键 entry point GetProcAddress；任一失败立即 unload + 档 1 跳过。
3. CUDA-D3D11 互通：mc-player 主 D3D11 device → `cuD3D11CtxCreate` → `cudaGraphicsD3D11RegisterResource`；NV12 帧通过 `cuGraphicsMapResources` 映射到注册的 ID3D11Texture2D，零 CUDA→D3D 拷贝。
4. parser 配置：`ulMaxDisplayDelay = 0`，回调里直接 `cuvidDecodePicture`；每个 pktBitstream 来自 depack 时置 `CUVID_PKT_ENDOFPICTURE`。
5. 错误恢复（性能量度规范 §6）：CUDA 错误（如 `CUDA_ERROR_INVALID_VALUE`）→ flush parser + 重新 open；连续 3 次失败降到档 2。
6. 在 NVIDIA dGPU 测试机上跑回归基线流；验证 `mc.decoder.kind=VENDOR_SDK_NVDEC`。

### 5.3 验收 metric

| metric | 阈值（NVIDIA 测试机）|
|---|---|
| `mc.decoder.kind`（H.264 / H.265）| `VENDOR_SDK_NVDEC` |
| `mc.stage.decode_actual_ns` p95 | ≤5ms（ADD §1.2 档 1）|
| `mc.stage.decode_alloc_ns` | ~30μs（SDK 自管 staging）|
| vs VLC 时钟差 | ≤+2ms |
| 在 Intel / AMD 机器（无 nvcuvid.dll）| `mc.probe.tier_skip_reason{tier=1}` = `vendor_mismatch` 或 `sdk_missing` |

### 5.4 已知风险

- `nvcuvid.dll` 与 `cudart64_*.dll` 版本耦合；可能不同 CUDA toolkit 版本不兼容。**应对**：动态加载只用 driver API（cuvid* / cu*）不用 cudart，规避 toolkit 依赖。
- D3D11VA NV12 array slice 与 CUDA mapped pointer 的 fence 同步：CUDA 写完后下一步 shader 采样需要 D3D11 fence wait。**应对**：参考 NVIDIA NvCodec sample 中 `NvDecD3D11.cpp` 的 fence 模式。

### 5.5 commit 模板

```
feat(phase-5): codec_nvdec 档 1 实装（NVIDIA Video Codec SDK 直驱）

- codec_vendor_base 抽象基类（probe/open/submit/pull/close 5 件套）
- codec_nvdec ~800 行：动态加载 nvcuvid.dll，CUDA-D3D11 互通注册
- ulMaxDisplayDelay=0 + CUVID_PKT_ENDOFPICTURE 严格按性能规范 §6
- controller 档 1 NVDEC 实装连接（VendorId=0x10DE 才尝试）

before/after（NVIDIA RTX 3050 dGPU，720p H.264）：
- mc.decoder.kind:           DXVA_DIRECT     → VENDOR_SDK_NVDEC
- mc.stage.decode_actual_p95: 6ms             → 4ms
- mc.stage.decode_alloc:     <10μs (D3D11VA)  → ~30μs (SDK staging)
- vs VLC:                    +2ms            → +1ms
```

---

## Phase 6 — Vendor SDK 档 1：oneVPL（Intel iGPU 大量场景覆盖）

> Intel iGPU 在 IoT / 工控 / 中低成本部署中占比最高；oneVPL 实装受益面与 NVDEC 相当。

### 6.0 目标

- oneVPL 实装：`MFXVideoDECODE_DecodeHeader` + `_DecodeFrameAsync` + `mfxFrameSurface1` 直拿 D3D11 texture。
- 严格按 ADR-015 + 性能量度规范修订后的正确配置：`mfxVideoParam.AsyncDepth = 1` + `mfxBitstream.DataFlag |= MFX_BITSTREAM_COMPLETE_FRAME`（**不是** LowDelayBRC / LookAheadDepth，那俩是 encoder 参数）。
- runtime LoadLibrary 优先 `vpl.dll`，回退 `libmfx.dll`（Media SDK 后继关系）。

### 6.1 改动范围

| 文件 | 动作 |
|---|---|
| `mc-player/src/media/codec_onevpl.{h,cpp}`（新增 ~600 行）| oneVPL 实装；动态加载 + D3D11 device 共享 |
| `mc-player/src/controller/controller.cpp` | 档 1 oneVPL：`VendorId == 0x8086` 才尝试 |
| `mc-player/src/pal/dxgi_caps_probe.cpp` | 加 `vpl.dll` / `libmfx.dll` 存在性 probe |

### 6.2 实施步骤

1. 复用 Phase 5 的 `CodecVendorBase` 抽象。
2. dispatcher：先 LoadLibrary `vpl.dll`，符号表对应 oneVPL 2.x；失败回退 `libmfx.dll` 对应 Media SDK 1.x。
3. session 创建：`MFXLoad` + `MFXCreateSession` + impl 选择 (`MFX_IMPL_HARDWARE` + 设置 `mfxIMPL` 关联到 mc-player 主 D3D11 device 的 adapter index)。
4. `DecodeHeader` 后填 `mfxVideoParam`：`AsyncDepth = 1`、`mfx.DecodedOrder = 0`、`IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY`。
5. 每个输入 bitstream 置 `mfxBitstream.DataFlag |= MFX_BITSTREAM_COMPLETE_FRAME`（depack 已经按 AU 切片，正好对应）。
6. 输出 `mfxFrameSurface1::Data.MemId` 通过 oneVPL 内部即指向 D3D11 NV12 texture，直接给 renderer。
7. 错误处理：`MFX_ERR_INCOMPATIBLE_VIDEO_PARAM` → reset session；`MFX_ERR_DEVICE_LOST` → 上报 device-lost 全恢复。

### 6.3 验收 metric

| metric | 阈值（Intel UHD 730 测试机）|
|---|---|
| `mc.decoder.kind` | `VENDOR_SDK_ONEVPL` |
| `mc.stage.decode_actual_ns` p95 | ≤5ms |
| vs VLC 时钟差 | ≤+2ms |
| 不影响 NVIDIA / AMD 机器（不进档 1 oneVPL 路径）| 验证 |

### 6.4 commit 模板

```
feat(phase-6): codec_onevpl 档 1 实装（Intel oneVPL 直驱）

- codec_onevpl ~600 行：dispatcher 优先 vpl.dll 回退 libmfx.dll
- AsyncDepth=1 + MFX_BITSTREAM_COMPLETE_FRAME 严格按性能规范
- D3D11 device 共享给 oneVPL session（MFX_IMPL_HARDWARE）
- 输出 mfxFrameSurface1 直接拿 D3D11 NV12 texture，零拷贝

before/after（Intel UHD 730，720p H.264）：
- mc.decoder.kind:           DXVA_DIRECT      → VENDOR_SDK_ONEVPL
- mc.stage.decode_actual_p95: 6ms              → 4ms
- vs VLC:                    +2ms             → +1ms
```

---

## Phase 7 — Vendor SDK 档 1：AMF（AMD GPU 覆盖）

### 7.0 目标

- AMF 实装：`AMFFactory` + `AMFComponent::Submit/QueryOutput` + `AMFSurface` 拿 D3D11 texture。
- 严格按修订后的性能规范：`AMF_VIDEO_DECODER_REORDER_MODE = AMF_VIDEO_DECODER_MODE_LOW_LATENCY`（**不是** `AMF_VIDEO_DECODER_LOW_LATENCY` 这个错的属性名）。
- runtime LoadLibrary `amfrt64.dll`（AMD GPU 驱动通常自带）。

### 7.1 改动范围

| 文件 | 动作 |
|---|---|
| `mc-player/src/media/codec_amf.{h,cpp}`（新增 ~500 行）| AMF 实装 |
| `mc-player/src/controller/controller.cpp` | 档 1 AMF：`VendorId == 0x1002` 才尝试 |

### 7.2 实施步骤

1. 复用 `CodecVendorBase`。
2. AMF 加载：`amfrt64.dll` → `AMFInit` → `AMFFactory::CreateContext` → `InitDX11(d3d11_device)`。
3. 创建 decoder：`CreateComponent(AMFVideoDecoderUVD_H264_AVC / _H265_HEVC)` → 设 `AMF_VIDEO_DECODER_REORDER_MODE = AMF_VIDEO_DECODER_MODE_LOW_LATENCY`。
4. 异步管线：`Submit(buffer)` → `QueryOutput(&surface)`；surface 通过 `AMFSurface::GetPlane(AMF_PLANE_PACKED)::GetNative()` 拿到 D3D11 texture。
5. 错误处理：`AMF_NEED_MORE_INPUT` → idle；`AMF_RESOLUTION_CHANGED` → reset；`AMF_DECODER_NOT_PRESENT` → 永久失支降档。

### 7.3 验收 metric

| metric | 阈值（AMD 测试机）|
|---|---|
| `mc.decoder.kind` | `VENDOR_SDK_AMF` |
| `mc.stage.decode_actual_ns` p95 | ≤5ms |
| vs VLC 时钟差 | ≤+2ms |

### 7.4 commit 模板

```
feat(phase-7): codec_amf 档 1 实装（AMD AMF 直驱）

- codec_amf ~500 行：动态加载 amfrt64.dll
- AMF_VIDEO_DECODER_REORDER_MODE = AMF_VIDEO_DECODER_MODE_LOW_LATENCY
- AMFSurface 拿 D3D11 NV12 texture，零拷贝

before/after（AMD Radeon 660M，720p H.264）：
- mc.decoder.kind:           DXVA_DIRECT  → VENDOR_SDK_AMF
- mc.stage.decode_actual_p95: 6ms          → 4ms
```

---

## Phase 8 — ADR-016 + ADR-021 Hardware Decode Component Manager（HDCM）

> 让 mc-player 在用户层"零命令"完成所有硬解组件的安装：vendor SDK / Microsoft Store 媒体扩展 / Windows Optional Feature / GPU driver 引导。原 vendor SDK 单点下载面板（ADR-016）扩为统一管理 4 类组件、3 种 in-app 安装模式（ADR-021）。
>
> ⚠️ scope 拆分：本 Phase 8 拆为 **8-A / 8-B / 8-C / 8-D / 8-E** 五个子 phase，各自独立 commit。原 ADR-016 单点面板对应 8-A；8-B / 8-C / 8-D 是 ADR-021 新增类别的实装；8-E 是日志框架 + controller 集成的横切收尾。

### 8.0 目标

- 启动期 batch detect 7 类组件状态（A_NVDEC / A_oneVPL / A_AMF / B_HEVC_Ext / B_AV1_Ext / C_MediaPlayback / D_<vendor>），按 `already_installed` / `installable` / `unavailable_on_this_sku` 三态分发到面板（详 hdcm 设计 §5.1）。
- 用户主动展开"硬解组件"面板可一键安装：
  - **类别 A**：WinHTTP 异步下载 + SHA-256 + Authenticode；缓存到 `%LOCALAPPDATA%\mc-player\sdk\<vendor>\<version>\`（沿 ADR-016）。
  - **类别 B**：Store API `StoreContext::RequestDownloadAndInstallStorePackagesAsync`（C++/WinRT interop）；用户视角 = app 内进度条。
  - **类别 C**：`ShellExecuteEx(verb="runas")` + helper.exe + `DismApi::DismEnableFeature` 启用 MediaPlayback feature；完成提示重启。
  - **类别 D**：检测 driver 版本 < 阈值即一键 `ShellExecuteEx` 跳浏览器到 vendor 官网；driver setup.exe **不在 mc-player 进程内运行**。
- 离线 / 用户取消 / UAC 拒绝 / 校验失败 → 静默回退（不阻断启动），按 ADR-015 四级降级链兜底。
- 默认 log level `silent`；诊断走"调高 log level → 复现 → 用户导出 log"，无主动 collector。

### 8.1 改动范围

| 文件 | 动作 |
|---|---|
| `mc-player/src/hdcm/manifest_table.{h,cpp}`（新增）| HDCM_MANIFEST_TABLE 静态表（7 类组件 manifest，详 hdcm 设计 §2）|
| `mc-player/src/hdcm/detector.{h,cpp}`（新增；原 sdk_panel/detector 重构合入）| 类别 A/B/C/D 各自 probe_fn；启动期 batch detect 输出 `HdcmComponentState[]` |
| `mc-player/src/hdcm/installer_a_vendor_sdk.{h,cpp}`（新增；原 sdk_panel/downloader + installer 合入）| 类别 A：WinHTTP 异步下载 + SHA-256 + WinTrust Authenticode + 解压到缓存 |
| `mc-player/src/hdcm/installer_b_store.{h,cpp}`（新增）| 类别 B：UWP `Windows.Services.Store` C++/WinRT interop 调 `RequestDownloadAndInstallStorePackagesAsync` |
| `mc-player/src/hdcm/installer_c_feature.{h,cpp}`（新增）| 类别 C：主 app 侧 `ShellExecuteEx(verb="runas")` + 命名管道 IPC client（与 helper.exe 通信）|
| `mc-player/src/hdcm/installer_d_driver.{h,cpp}`（新增）| 类别 D：`Win32_VideoController.DriverVersion` 检测 + `ShellExecuteEx` 跳浏览器 |
| `mc-player/src/hdcm/ui_panel.{h,cpp}`（新增；原 sdk_panel/ui_panel 重构）| "硬解组件"面板（与 ui_overlay 风格一致）；按类别折叠展开；每组件显示状态 + 一键按钮 |
| `mc-player/tools/mc_hdcm_helper/`（新工程）| helper.exe；manifest `requireAdministrator`；DismApi 调用 + 命名管道 IPC server；与主 app 同 vendor 签名；**无网络能力** |
| `mc-player/include/mc-player/mc_player.h` | `mc_hdcm_progress_callback_t` 暴露安装进度（含 category / id / state / progress%）|
| `mc-player/src/controller/controller.cpp` | 启动期触发 HDCM batch detect（异步，不阻塞 mc_open）；面板入口默认折叠 |
| `mc-player/src/pal/log.{h,cpp}` | log level enum 新增 `silent`（默认）/ `error` / `warn` / `info` / `debug` / `trace`；运行期可调级 |

### 8.2 实施步骤（按子 phase 拆分）

**8-A. 类别 A vendor SDK（沿 ADR-016 实装）**
1. detector：`IDXGIAdapter1::GetDesc1` 拿 VendorId；按 manifest 表分发到 A_NVDEC / A_oneVPL / A_AMF 各自 probe_fn（含 `%LOCALAPPDATA%` cache + System32 双查）。
2. installer_a_vendor_sdk：WinHTTP 异步 GET 到临时文件；SHA-256 比对（manifest 表 hardcoded）；通过 → WinTrust Authenticode 签名校验；通过 → 解压 / 复制到 `%LOCALAPPDATA%\mc-player\sdk\<vendor>\<version>\`。
3. ui_panel A 类入口：与 ui_overlay 风格一致；按钮"下载并启用 / 取消"。
4. 验收：NVIDIA 测试机 detect → 用户点击下载 → 安装成功 → 下次启动 `mc.hdcm.component{type=A,id=A_NVDEC}.state=already_installed` + `mc.decoder.kind=VENDOR_SDK_NVDEC`。

**8-B. 类别 B Store API（HEVC / AV1 Video Extension）**
1. detector B：`Windows::Management::Deployment::PackageManager::FindPackagesForUser` 查找 `Microsoft.HEVCVideoExtension_*` / `Microsoft.AV1VideoExtension_*` PackageFamilyName；未装则 `Windows::Services::Store::StoreContext::GetDefault()` 检测 Store 客户端可用性。
2. installer_b_store：C++/WinRT 项目配置 + `Windows.Services.Store` namespace；`StoreContext::RequestDownloadAndInstallStorePackagesAsync({product_id})` async wrap；进度通过 `IAsyncOperationWithProgress::Progress` callback 上报。
3. SKU 限制：IoT LTSC / 无 Store SKU 自动归 `unavailable_on_this_sku`，面板隐藏入口。
4. 验收：Win11 retail + Store 在线机器：detect → 用户点击 HEVC Ext 下载 → Store API 安装完成 → `Microsoft.HEVCVideoExtension` package 已注册 → 重启 mc-player → 档 3 H.265 codec 覆盖增加。

**8-C. 类别 C MediaPlayback feature + helper.exe**
1. helper.exe 工程：新建 `mc-player/tools/mc_hdcm_helper/`，manifest `<requestedExecutionLevel level="requireAdministrator" uiAccess="false" />`；与主 app 同 SignTool 证书。
2. helper main：parent process 校验（`OpenProcess` + `GetModuleFileNameExW` + `WinVerifyTrust` cert chain → 同主 app 证书 thumbprint 才继续）；通过则监听命名管道 `\\.\pipe\mc-player\hdcm-helper-<pid>`；接收 EnableFeature 请求；调 `DismOpenSession` + `DismEnableFeature(L"MediaPlayback", ...)` + `DismGetFeatureInfo` → 返回 hr + restart_required + error_message；发送响应后退出。
3. installer_c_feature：主 app 侧 `ShellExecuteEx(verb="runas", lpFile="mc_hdcm_helper.exe", lpParameters=pipe_name)` → UAC 弹窗；用户同意后通过命名管道发 EnableFeature 请求 → 等响应 → 解析 restart_required → 弹"立即重启 / 稍后重启"对话框。
4. 离线场景：用户拒绝 UAC → `result=uac_denied` → 面板提示重试。
5. 验收：IoT LTSC 主机 detect MediaPlayback=Disabled → 用户点击启用 → UAC 通过 → DISM 启用成功 → restart_required=Required → 用户立即重启 → 下次启动 `Get-WindowsOptionalFeature MediaPlayback=Enabled` + `MFTEnumEx` 返回 ≥1 hardware MFT → ADR-015 档 3 可用。

**8-D. 类别 D GPU driver 引导**
1. installer_d_driver：`IWbemServices::ExecQuery(WQL)` 查 `Win32_VideoController.DriverVersion` → 解析四段版本 → vs manifest 表编译期内嵌阈值；落后即 `mc.hdcm.driver_below_threshold{vendor}=1`。
2. ui_panel D 类入口：仅 `driver_below_threshold=1` 时可见；按钮"前往 vendor 官网下载新驱动"。
3. 点击 → `ShellExecuteEx({lpVerb=L"open", lpFile=vendor_url})` → 默认浏览器打开 → 用户在浏览器 + vendor 自家 installer 完成。
4. 不监控 driver 安装结果（外部进程；mc-player 无能力跟踪）；下次启动重新 detect。
5. 验收：driver 版本人为降到阈值之下 → 启动后 D 类入口可见 → 点击跳浏览器 → 用户更新 driver + 重启 → 下次启动 detect `driver_below_threshold=0` + `mc.probe.tier_skip_reason{tier=2}=profile_unsupported` 消失。

**8-E. 日志框架 + controller 集成**
1. pal/log：log level enum {silent, error, warn, info, debug, trace}；默认 silent；运行期可调级（设置面板）；不引第三方日志库。
2. controller：mc_open 启动期异步触发 HDCM batch detect，**不阻塞** `mc_open` 首帧路径（性能规范 §3）。
3. ui_overlay 集成"硬解组件"面板入口（默认折叠，仅 INSTALLABLE 组件存在时面板入口可见）。
4. 整体 E2E 测试：单机覆盖 7 类组件全状态切换；面板 UX 评审。

### 8.3 验收 metric

| metric | 阈值 |
|---|---|
| `mc.hdcm.component{type=A, id=A_NVDEC}.state`（NVIDIA 测试机首启后） | `installable` → 用户点击下载 → `installing` → `already_installed` |
| `mc.hdcm.install_attempt_count{type=A, result=success}` | NVDEC 下载 + 校验 + 解压成功 ≥ 1 |
| `mc.hdcm.component{type=B, id=B_HEVC_Ext}.state` | Win11 retail + Store: 用户点击安装 → `already_installed`；IoT LTSC: 自动 `unavailable_on_this_sku` |
| `mc.hdcm.component{type=C, id=C_MediaPlayback}.state` | IoT LTSC + Disabled: `installable` → UAC + DISM → `restart_pending` → 用户重启 → `already_installed` |
| `mc.hdcm.driver_below_threshold{vendor=Intel}` | 老 driver 测试机: 1；用户更新后: 0 |
| `mc.decoder.kind`（IoT LTSC + 用户启用 C + 重启） | 从 `LIBCODEC` 升到 `MFT_HARDWARE` 或 `DXVA_DIRECT` |
| 离线 / UAC 拒绝 / 校验失败 | 启动不阻断；档位降到 ADR-015 兜底档；面板提示重试 |
| `mc.firstframe.open_to_first_emit_ns` | HDCM batch detect 异步执行，不影响首帧延时阈值（性能规范 §3） |

### 8.4 commit 模板

按 8-A ~ 8-E 拆 5 次 commit：

```
feat(phase-8-A): ADR-016 vendor SDK 子模块（HDCM 类别 A 实装）

- hdcm/manifest_table: A_NVDEC / A_oneVPL / A_AMF 三组件 manifest（vendor_id / 下载源 / SHA-256 / 缓存路径）
- hdcm/detector: IDXGIAdapter1::GetDesc1 + cache 双查 probe
- hdcm/installer_a_vendor_sdk: WinHTTP 异步下载 + SHA-256 + WinTrust Authenticode + 解压
- hdcm/ui_panel A 类入口
- 离线 / 校验失败 → 静默回退到 ADR-015 档 2

before/after（NVIDIA 测试机首启）：
- mc.hdcm.component{type=A,id=A_NVDEC}.state: installable → already_installed
- mc.decoder.kind 下次启动:                     DXVA_DIRECT → VENDOR_SDK_NVDEC
```

```
feat(phase-8-B): ADR-021 类别 B Microsoft Store 媒体扩展安装

- hdcm/installer_b_store: UWP Windows.Services.Store C++/WinRT interop
- HEVC / AV1 Video Extension 通过 RequestDownloadAndInstallStorePackagesAsync 安装
- IoT LTSC / 无 Store SKU: 自动归 unavailable_on_this_sku，面板隐藏入口

before/after（Win11 retail + 无 HEVC Ext）：
- mc.hdcm.component{type=B,id=B_HEVC_Ext}.state: installable → already_installed
- 重启 mc-player 后 H.265 流: 档 3 codec 覆盖增加
```

```
feat(phase-8-C): ADR-021 类别 C Windows Optional Feature 启用（helper.exe + DismApi）

- tools/mc_hdcm_helper/ 工程: requireAdministrator manifest + 命名管道 IPC server + DismApi 调用 + 同 vendor 签名 + 无网络能力
- hdcm/installer_c_feature: ShellExecuteEx(verb="runas") + 命名管道 client + restart_required 处理
- 启用成功后弹"立即重启 / 稍后重启"对话框

before/after（IoT LTSC + MediaPlayback Disabled）:
- mc.hdcm.component{type=C,id=C_MediaPlayback}.state: installable → restart_pending → already_installed（用户重启后）
- mc.decoder.kind: LIBCODEC → MFT_HARDWARE（重启后档 3 可用）
```

```
feat(phase-8-D): ADR-021 类别 D GPU driver 阈值检测 + 跳浏览器引导

- hdcm/installer_d_driver: Win32_VideoController.DriverVersion 检测 vs 编译期阈值表
- 落后则 ShellExecuteEx 一键跳 vendor 官网驱动页
- driver setup.exe 不在 mc-player 进程内运行（工程边界）

before/after（老 Intel driver 测试机）：
- mc.hdcm.driver_below_threshold{vendor=Intel}: 1
- 用户跳浏览器更新 driver + 重启后:                0
- mc.probe.tier_skip_reason{tier=2}=profile_unsupported: 消失
```

```
feat(phase-8-E): pal/log silent 默认 + HDCM 集成到 controller

- pal/log: log level enum {silent, error, warn, info, debug, trace}; 默认 silent
- controller: mc_open 异步触发 HDCM batch detect，不阻塞首帧路径
- ui_overlay 集成"硬解组件"面板入口（默认折叠，installable 组件存在时入口可见）
```

---

## Phase 9 — Capability Probe Suite + Preset 架构基座（ADR-017 ~ ADR-019）

> Phase 0 ~ 8 让"每个子系统单独最优"；Phase 9 让"子系统协同最优"。无 Phase 9 之前每个子系统按 worst-case 配置，端到端延时被最差档拖；有 Phase 9 后按 active_preset 联合调档，实测端到端 p95 可再降 30~50%。
>
> ⚠️ **scope 拆分（修订自初版）**:本 Phase 9 拆为 6 个子 phase——主线 9.0 + 5 个子目标 9.1-9.5,各自独立 commit。原"5 子目标加分项"模式被废弃,因为子目标缺失会让 SDI_REPLACEMENT preset 退化为 REALTIME_LAN 实质效果（ADD §8.4 关键洞察:SDI_REPLACEMENT ≤25ms 端到端目标依赖子目标 1-4 全命中）。新模式让"preset 命中 == 实质效果达标",避免用 metric 名字遮蔽真实落地。

### 9.0 主线:Capability Probe + Selector + Apply

#### 9.0.0 目标

- 启动期完成四维 capability probe（hardware ∩ network ∩ encoder ∩ render）；输出 `CapabilitySnapshot`（ADD §7.5.1 时序;render 维度详见 capability_probe §3.5）。
- Preset Selector 落地 5 档：SDI_REPLACEMENT / REALTIME_LAN / STREAMING_WIFI / WAN_FALLBACK / SAFE_MODE（capability_probe §6.2 匹配规则）。
- 一次性 apply preset 到 6 个子系统（decoder / jitter / render / present / RTCP / gate），首帧前确定 active_preset。
- **SDI_REPLACEMENT preset 主线降级行为**:Phase 9.0 内 SDI_REPLACEMENT 命中条件中"子目标 1-4 命中"以默认 false 返回（hardware/render probe 检出 supports_dcomp_nv12_direct=false / present 调度无 race_to_display 路径 / rtcp 无 reduced-size 协商 / jitter 无 ZeroJitter mode）;实际命中 SDI_REPLACEMENT 需 Phase 9.1-9.4 各自完成。Phase 9.0 实测稳态命中 REALTIME_LAN 为期望。

#### 9.0.1 改动范围

| 文件 | 动作 |
|---|---|
| `mc-player/src/probe/hardware_probe.{h,cpp}`（重构）| 把现有零散 probe（DXGI 枚举 / CheckVideoDecoderProfile / vendor SDK 探测）合并到统一接口，输出 `HardwareSnapshot` |
| `mc-player/src/probe/network_probe.{h,cpp}`（新增）| RTSP keepalive RTT 或 RTCP RR/SR 反推；首 GOP 内 iat 抖动 + loss 统计；link_kind 推断（ADD §7.5.2 阈值表） |
| `mc-player/src/probe/encoder_probe.{h,cpp}`（新增）| SDP profile-level-id 解析 + SPS VUI bitstream_restriction_flag / max_num_reorder_frames 读取 + 首 GOP DTS/PTS 实测兜底（ADD §7.5.3 三档可信度） |
| `mc-player/src/probe/render_probe.{h,cpp}`（新增）| 显示器刷新率 / VRR / DCOMP / MPO planes 数 / hardware composition support 探测,输出 `RenderSnapshot`(capability_probe §3.5) |
| `mc-player/src/probe/capability_snapshot.{h,cpp}`（新增）| 聚合 4 维 snapshot（hw/net/enc/render），暴露给 selector |
| `mc-player/src/preset/preset_selector.{h,cpp}`（新增）| capability_probe §6.2 匹配算法落地，输出 `active_preset_id` |
| `mc-player/src/preset/preset_definitions.h`（新增）| 5 档 preset 的具体参数表（jitter mode / decoder hint / render profile / present scheduling / rtcp mode），与 capability_probe §6.1 表格 1:1 |
| `mc-player/src/preset/preset_apply.cpp`（新增）| 一次性 apply preset 到 6 个子系统的协同入口 |
| `mc-player/src/controller/controller.cpp` | 启动序列改为：`signaling 握手 ‖ probe suite` → `probe 完成` → `preset apply` → `首帧` |
| `subprojects/mc-libcodec/...` | **不动**（preset 不影响 libcodec 内部，仅在 selector 决定不走档 4 时绕过它） |

#### 9.0.2 实施步骤

1. **probe 框架先行**：以 `capability_snapshot.h` 为契约头，先把现有 hardware probe 重构进新接口（行为零变化），再补 network / encoder / render probe；这样四个 probe 可并行实现而不阻塞 selector。
2. **selector 与 preset 表落地**：完成 capability_probe §6.2 匹配算法 + §6.1 子系统参数表；初版 selector 输入 hardcoded snapshot 单测。
3. **6 个子系统 apply 接口**：每个子系统暴露 `apply_preset(const Preset&)` 接口，幂等（同一 preset 多次 apply 等价单次）。
4. **启动序列改造**：BOOTSTRAP 默认 preset = REALTIME_LAN（保守版）；probe 完成后第一次 reload 切到正式 preset。

#### 9.0.3 验收 metric

| metric | 阈值 |
|---|---|
| `mc.preset.active_id` | 在 LAN + NVDEC + 240Hz VRR 测试机：稳定 = `REALTIME_LAN`（不到 SDI_REPLACEMENT,因 9.1-9.4 未做） |
| `mc.preset.bootstrap_to_active_ms` | < 1500ms（首帧后第一次 reload 完成时间） |
| `mc.probe.hardware_complete_ms` | < 200ms |
| `mc.probe.network_complete_ms` | < 1000ms（首 GOP 到齐） |
| `mc.probe.encoder_complete_ms` | < 1000ms（首 GOP 到齐） |
| `mc.probe.render_complete_ms` | < 50ms（本地 DXGI / DCOMP 探测） |
| `mc.gate.drop_*_count` warm_steady | 0（preset 切换不能引发花屏；ADR-014 不让步） |
| `mc.preset.apply_atomic_violation_count` | 0（apply 全程帧间缝隙完成，不丢帧） |
| `mc.e2e.client_internal_p95` | LAN + NVDEC + 144Hz VRR REALTIME_LAN 命中 → ≤20ms |

#### 9.0.4 commit 模板

```
feat(phase-9.0): Capability Probe 主线 + Preset Selector + Apply（ADR-017 ~ ADR-019）

- probe/hardware_probe: 现有 probe 重构进统一 HardwareSnapshot 接口
- probe/network_probe:  RTT p50/p95/p99 + iat jitter + loss + link_kind 推断
- probe/encoder_probe:  SDP profile-level-id + SPS VUI + 首 GOP 实测
- probe/render_probe:   refresh_rate / VRR / DCOMP / MPO planes / hardware composition 探测（capability_probe §3.5）
- probe/capability_snapshot: 聚合四维 snapshot
- preset/preset_selector: capability_probe §6.2 匹配算法 → 5 档 active_preset_id
- preset/preset_definitions / preset_apply: 5 档 preset 表 + 一次性 apply 到 6 子系统
- controller: 启动序列改造，BOOTSTRAP → probe → reload to active preset
- SDI_REPLACEMENT 主线降级：子目标 1-4 命中位默认 false，命中需 9.1-9.4 完成

before/after（NVIDIA + 240Hz VRR + LAN）：
- mc.preset.active_id:                   <无字段> → REALTIME_LAN
- mc.preset.bootstrap_to_active_ms:                → 800ms
- mc.e2e.client_internal_p95:            22ms     → ≤20ms（REALTIME_LAN）
- mc.gate.drop_*_count warm_steady:      0        → 0（不退化）
```

---

### Phase 9.1 — 子目标 1:DCOMP NV12 直显（ADD §5.10 / 子目标 1）

#### 9.1.0 目标

composition swapchain `DXGI_FORMAT_NV12` + MPO 多面合成,绕过 shader RGB 转换。命中后让 SDI_REPLACEMENT preset 在 render 维度的硬性条件 `render.supports_dcomp_nv12_direct == true` 满足。

#### 9.1.1 改动范围

| 文件 | 动作 |
|---|---|
| `mc-player/src/render/dcomp_swapchain.cpp` | swapchain DESC 的 Format 在 SDI_REPLACEMENT preset 下设 `DXGI_FORMAT_NV12`；探测 MPO planes 数 |
| `mc-player/src/probe/render_probe.cpp` | 在 9.0 基础上补 supports_dcomp_nv12_direct 试探(尝试创建 NV12 composition swapchain) |

#### 9.1.2 验收 metric

| metric | 阈值 |
|---|---|
| `mc.render.dcomp_nv12_direct_active` | NVDEC + 240Hz VRR 路径下 = 1 |
| `mc.preset.active_id` | 配合 9.2-9.4 完成后才能命中 SDI_REPLACEMENT;单 9.1 完成不影响命中 |

#### 9.1.3 commit 模板

```
feat(phase-9.1): DCOMP NV12 直显 + MPO 多面合成（子目标 1）

- render/dcomp_swapchain: SDI_REPLACEMENT preset 下 swapchain Format = DXGI_FORMAT_NV12
- probe/render_probe: 试探创建 NV12 composition swapchain 输出 supports_dcomp_nv12_direct
- 命中后 SDI_REPLACEMENT preset 在 render 维度的 supports_dcomp_nv12_direct 条件满足

before/after（NVIDIA + 240Hz VRR + DCOMP_INDEPENDENT_FLIP）：
- mc.render.dcomp_nv12_direct_active:    0 → 1
- mc.gate.drop_*_count warm_steady:      0 → 0（不退化）
```

---

### Phase 9.2 — 子目标 2:RFC 5506 Reduced-Size RTCP

#### 9.2.0 目标

RFC 5506 Reduced-Size RTCP 与 AVPF Immediate `trr-int=0` 协同减少反馈链路开销;协商位与对端确认。

#### 9.2.1 改动范围

| 文件 | 动作 |
|---|---|
| `mc-player/src/transport/rtcp_*.cpp` | Reduced-Size RTCP 报文构造 + 接收方协商位 + SDP `a=rtcp-rsize` 协商 |

#### 9.2.2 验收 metric

| metric | 阈值 |
|---|---|
| `mc.rtcp.reduced_size_active` | 与对端协商成功后 = 1 |

#### 9.2.3 commit 模板

```
feat(phase-9.2): RFC 5506 Reduced-Size RTCP（子目标 2）

- transport/rtcp_*: Reduced-Size RTCP 报文构造 + 接收方协商位
- 信令 SDP answer 增 a=rtcp-rsize；按对端能力降级到完整 RTCP
- 命中后 SDI_REPLACEMENT preset 的 rtcp 维度满足 reduced_size + AVPF Immediate

before/after（对端支持 RFC 5506）：
- mc.rtcp.reduced_size_active:           0 → 1
- mc.gate.drop_*_count warm_steady:      0 → 0（不退化）
```

---

### Phase 9.3 — 子目标 3:ZeroJitter 模式（jitter target 0~1ms）

#### 9.3.0 目标

LAN-switched 链路替代 Kalman aggressive。命中后让 SDI_REPLACEMENT preset 的 jitter buffer 子系统配置可达 `target_delay = 0~1ms`。

#### 9.3.1 改动范围

| 文件 | 动作 |
|---|---|
| `mc-player/src/media/jitter_buffer.cpp` | 增 ZeroJitter mode 分支；与 Kalman 共用接口;切换不丢帧（双缓冲交替读） |

#### 9.3.2 验收 metric

| metric | 阈值 |
|---|---|
| `mc.jitter.mode` | LAN-switched 链路 + SDI_REPLACEMENT preset 下 = `ZeroJitter` |
| `mc.jitter.target_delay_ms` | LAN-switched 链路下 ≤ 1ms |

#### 9.3.3 commit 模板

```
feat(phase-9.3): ZeroJitter mode（子目标 3）

- media/jitter_buffer: ZeroJitter mode 分支，与 Kalman 共用接口
- 双缓冲实现：reload 时新 mode 后台加热 → swap 指针 → 旧 buffer 排空 GC，不丢帧
- LAN-switched 链路下 target_delay 0~1ms；SDI_REPLACEMENT preset 激活
- 命中后 SDI_REPLACEMENT preset 的 jitter 维度满足

before/after（LAN-switched + 240Hz VRR + zerolatency 编码）：
- mc.jitter.mode:                        Kalman → ZeroJitter
- mc.jitter.target_delay_ms:             5ms    → 1ms
- mc.gate.drop_*_count warm_steady:      0      → 0（不退化）
```

---

### Phase 9.4 — 子目标 4:Race-to-display Present

#### 9.4.0 目标

Reflex 风格——一帧解码就绪即 Present,依赖 ALLOW_TEARING + VRR。命中后让 SDI_REPLACEMENT preset 的 present 子系统配置可达"零等待"。

#### 9.4.1 改动范围

| 文件 | 动作 |
|---|---|
| `mc-player/src/render/present_scheduler.cpp` | race_to_display 模式 + ALLOW_TEARING flag 协同;SDI_REPLACEMENT 下激活 |

#### 9.4.2 验收 metric

| metric | 阈值 |
|---|---|
| `mc.present.race_to_display_active` | SDI_REPLACEMENT preset 下 = 1 |

#### 9.4.3 commit 模板

```
feat(phase-9.4): Race-to-display Present（子目标 4）

- render/present_scheduler: race_to_display 模式 + ALLOW_TEARING flag 协同
- 一帧解码就绪即 Present；依赖 VRR + DCOMP_INDEPENDENT_FLIP；非 VRR 路径不激活
- SDI_REPLACEMENT preset 下激活；其他 preset 走 vsync-aligned 调度
- 命中后 SDI_REPLACEMENT preset 的 present 维度满足

before/after（240Hz VRR + ALLOW_TEARING）：
- mc.present.race_to_display_active:     0 → 1
- mc.gate.drop_*_count warm_steady:      0 → 0（不退化）
- mc.render.epoch_pair_skew_count:       0 → 0（race_to_display 与 epoch 严格配对）
```

---

### Phase 9.5 — 子目标 5:CUDA Graphs 探索（NVDEC 路径专属）

#### 9.5.0 目标

把"alloc + parse + decode + map"打包成单次 CUDA Graph 提交（仅 NVDEC 档评估）。本子目标是探索性,命中收益小(预期 -1~2ms),失败不影响 SDI_REPLACEMENT 命中。

#### 9.5.1 改动范围

| 文件 | 动作 |
|---|---|
| `mc-player/src/media/codec_nvdec.cpp` | 增 CUDA Graphs 提交路径;失败 fallback 到原 streaming 提交 |

#### 9.5.2 验收 metric

| metric | 阈值 |
|---|---|
| `mc.decoder.cuda_graph_active`（NVDEC 专属） | NVDEC 路径下 = 1（实装失败 / 不命中则字段为 0 或不上报） |

#### 9.5.3 commit 模板

```
feat(phase-9.5): CUDA Graphs 探索（子目标 5，NVDEC 路径专属）

- media/codec_nvdec: CUDA Graphs 提交路径，把 alloc + parse + decode + map 打包单次提交
- 失败 fallback 到原 streaming 提交；不影响 SDI_REPLACEMENT 命中（探索性）
- 仅在 NVDEC 档下评估；其他档不参与

before/after（NVDEC + CUDA 12+）：
- mc.decoder.cuda_graph_active:          0 → 1（实装成功；预期收益 -1~2ms）
- mc.decoder.kind:                       VENDOR_SDK_NVDEC（不变）
- mc.gate.drop_*_count warm_steady:      0 → 0（不退化）
- 若实装失败：mc.decoder.cuda_graph_active 字段不上报，本子目标不阻塞收官
```

---

### Phase 9.x SDI_REPLACEMENT 命中前提

完成 9.0 + 9.1 + 9.2 + 9.3 + 9.4(子目标 5 可选) 后,在 NVDEC + 240Hz VRR + LAN-switched + zerolatency 编码 + DCOMP_INDEPENDENT_FLIP 测试机上,`mc.preset.active_id` 应稳定命中 `SDI_REPLACEMENT`,`mc.e2e.client_internal_p95` ≤ 8ms(ADD §8.4 SDI_REPLACEMENT 行,前提:子目标 1-4 全命中)。

端到端验收：

| metric | 阈值（按 ADD §8.4 SDI_REPLACEMENT 行） |
|---|---|
| `mc.e2e.client_internal_p95`（LAN + NVDEC + 240Hz VRR + zerolatency 编码 + 子目标 1-4 全命中） | ≤ 8ms |
| `mc.e2e.client_internal_p95`（LAN + 任 vendor SDK / DXVA + 144Hz VRR） | ≤ 20ms |
| `mc.e2e.client_internal_p95`（公网回退） | 不退化于 Phase 0 ~ 8 baseline |

> commit 模板按子 phase 分散到 §9.0.4 / §9.1.3 / §9.2.3 / §9.3.3 / §9.4.3 / §9.5.3 各自小节。本 §9.x 不再单独给 commit 模板（拆分前的旧合并模板已废弃）。

---

## Phase 10 — Preset Live Reload + Self-Upgrade Loop（ADR-020）

> Phase 9 完成"启动期一次决策"；Phase 10 完成"运行期持续适配"。网络抖动 / 编码器切档 / 显示器切档（用户拖窗到不同 monitor）等假设破坏，必须能毫秒级降档；长期稳定时主动试升档收回保守配置。

### 10.0 目标

- 落地 ADD §7.5.5 降级触发表：5 类信号（loss / rtt / decoder error / present underrun / tearing）任一触发立即降一档 preset。
- 落地 ADD §7.5.5 升级试探：60s 全维稳定 + 0 tainted 事件 + 振荡保护（`oscillation_count`）。
- Reload 原子性：T0~T3 不中断；T4/T5 帧间缝隙换档；jitter 双缓冲交替读不丢帧。
- 与 §5.10.5 Present Epoch 协同：preset reload 视作 epoch 切换源；旧 epoch 在飞帧由 watchdog 兜底丢弃。

### 10.1 改动范围

| 文件 | 动作 |
|---|---|
| `mc-player/src/preset/live_reload.{h,cpp}`（新增）| 5 类降级信号订阅 + 降级动作派发；升级试探主循环（60s 窗口 telemetry 滑窗） |
| `mc-player/src/preset/oscillation_guard.{h,cpp}`（新增）| `oscillation_count` 状态机：升档 → 30s 内再降 → count++；count >=2 锁定 5min 禁升 |
| `mc-player/src/probe/network_probe.cpp` | 增 5s 滑窗版 `loss_rate_window_5s` / `rtt_p95_window_5s`，给 live_reload 订阅 |
| `mc-player/src/render/present_epoch.cpp` | preset reload 视作 epoch 切换，commit 时升 epoch；旧 epoch frame 兜底丢弃 |
| `mc-player/src/media/jitter_buffer.cpp` | 双缓冲实现（reload 时新 mode 后台加热，切换时 swap 指针，旧 buffer 排空后 GC） |
| `mc-player/src/media/codec_bridge.cpp` | decoder hint reload（NVDEC ulMaxDisplayDelay / oneVPL AsyncDepth / AMF REORDER_MODE）的 GOP-aligned 切换闸 |
| `mc-player/src/transport/rtcp_sender.cpp` | RTCP mode reload（Reduced-Size on/off + AVPF Immediate / Regular 切换） |

### 10.2 实施步骤

1. **滑窗 telemetry 先行**：所有 5 类降级信号都需 5s / 60s 滑窗形态，先在 metric 层把滑窗形态做对，再做 live_reload。
2. **降级路径实装**：每个降级信号 → 写降级测试，触发条件人工注入（loss 注入 / rtt 注入 / 错误帧注入），验证降级动作正确。
3. **升级路径实装**：升档触发严苛——60s 全维稳定 + 0 tainted；写一组"前 30s 完美 + 第 31s 注入抖动"的对抗测试，验证不会误升。
4. **振荡保护**：写"反复升降"循环测试，验证 `oscillation_count >= 2` 后正确锁定 5 min 禁升。
5. **原子性闸**：reload 期间 `mc.preset.apply_atomic_violation_count` 必须 0；任何"切档时丢帧"都是 P0 bug。

### 10.3 验收 metric

| metric | 阈值 |
|---|---|
| `mc.preset.reload_event{from,to,reason,latency_ns}` | 全部 reload 事件可追溯；`latency_ns` p95 < 5ms |
| `mc.preset.downgrade_count` rate | 注入 loss 突增 → 1 次 / 10s（按测试场景设计） |
| `mc.preset.upgrade_count` rate | 60s 稳定后 → 1 次（升一档）；继续 60s 稳定 → 1 次（再升一档） |
| `mc.preset.oscillation_count` | 反复抖动场景下达到 2 后稳定不再升 |
| `mc.preset.apply_atomic_violation_count` | 0（reload 全程不丢帧） |
| `mc.gate.drop_*_count` warm_steady | 0（reload 不引发花屏；ADR-014 不让步） |
| `mc.render.epoch_pair_skew_count` | 0（preset reload 与 epoch 切换严格配对） |

### 10.4 commit 模板

```
feat(phase-10): Preset Live Reload + Self-Upgrade Loop（ADR-020）

- preset/live_reload: 5 类降级信号订阅 → 立即降档；60s 全维稳定 + 0 tainted 试升档
- preset/oscillation_guard: 升降振荡 ≥2 次 → 锁定 5min 禁升
- probe/network_probe: 5s/60s 滑窗 loss_rate / rtt_p95
- render/present_epoch: preset reload = epoch 切换源；旧 epoch 兜底丢弃
- media/jitter_buffer: 双缓冲 reload，不丢帧
- media/codec_bridge: decoder hint GOP-aligned 切换
- transport/rtcp_sender: RTCP mode reload（Reduced-Size / AVPF Immediate）

before/after（注入 loss 突增 → 恢复稳定 → 注入 rtt 突增）：
- mc.preset.reload_event 序列:           SDI_REPLACEMENT → REALTIME_LAN → SDI_REPLACEMENT → STREAMING_WIFI
- mc.preset.reload_latency_ns p95:                       → 3.2ms
- mc.preset.apply_atomic_violation_count:                → 0
- mc.gate.drop_*_count warm_steady:      0              → 0（不退化）
```

---

## 收官清单

完成 Phase 0 ~ Phase 10 后，仓库应达到的稳态：

| 项 | 状态 |
|---|---|
| ADR-015 四级降级链 | 全部实装；vendor SDK + DXVA-direct + MFT hardware + libcodec 全档可用 |
| ADR-016 下载面板 | 实装；用户首次启动可选下载 vendor SDK |
| ADR-017 Capability-Driven Preset 架构 | 四维 probe（hardware/network/encoder/render）+ 5 档 preset selector + 一次性 apply 到 6 子系统 |
| ADR-018 Network Probe | rtt p50/p95/p99 + iat jitter + loss + link_kind 推断稳定 |
| ADR-019 Encoder Probe | SDP profile-level-id + SPS VUI + 首 GOP 实测三档可信度 |
| ADR-020 Preset Live Reload + Self-Upgrade | 降级信号即降；60s 全维稳定试升；振荡保护 |
| Frame Validity Gate | 六 bit 完整收口；6 类污染源 trace |
| Present Epoch + Watchdog | 单点权威；陈旧区域防御；preset reload 与 epoch 切换严格配对 |
| 性能量度规范 | 所有 metric 字段全部上报；CI 闸接入 |
| `mc.decoder.kind` 6 值 | 在不同 GPU 测试机上各自验证到位 |
| `mc.preset.active_id` 5 值 | 在不同链路 / 显示器 / 编码器场景验证到位 |
| `mc.e2e.client_internal_p95` | SDI_REPLACEMENT preset ≤8ms（子目标 1-4 全命中）；REALTIME_LAN ≤20ms（ADD §8.4，warm_steady） |
| `mc.gate.drop_*_count` | warm_steady 永久 0（含 preset reload 全过程） |
| `mc.preset.apply_atomic_violation_count` | 永久 0（reload 不丢帧） |
| vs VLC 时钟差 | 档 1/2 命中：≤+2ms；档 4 兜底：≤+15ms |

**收官后建议做但不是阻塞项**：
- AV1 / H.266 路线（ADD §5.6.7）按需推进；硬件支持成熟即开下一阶段
- HDR10 / scRGB 输出（ADD §5.12 末段）
- WHEP 协议从 draft-03 跟进到正式 RFC（draft 状态见 §1 ADD 头表）

---

## 附录 A：文档与代码的双向溯源（reverse cross-reference）

| 代码位置 | 上游设计文档锚点 |
|---|---|
| `mc_player_types.h::mc_decoder_kind_t` | ADR-015 6 值 / 性能规范 §6 |
| `codec_bridge.cpp::active_kind` | ADD §5.6.1 / 性能规范 §6 |
| `codec_mft_video::start::is_sync_software_mft_only` | ADR-001 / ADR-002 / hardware-decode §1 |
| `codec_dxva_video::H264 实装` | ADD §5.6.2.2 |
| `codec_nvdec.cpp::ulMaxDisplayDelay = 0` | ADD §5.6.2.1 / 性能规范 §6 |
| `codec_onevpl.cpp::AsyncDepth = 1 + COMPLETE_FRAME` | ADD §5.6.2.1 / 性能规范 §6 |
| `codec_amf.cpp::AMF_VIDEO_DECODER_REORDER_MODE` | ADD §5.6.2.1 / ADR-015 |
| `frame_validity_gate.cpp::six_bits` | ADD §5.13 / ADR-014 |
| `render_present_epoch.cpp::commit_authority` | ADD §5.10.5 / ADR-014 |
| `pal/metric.cpp::Histogram` | 性能规范 §9.2 |
| `sdk_panel/*` | ADR-016 / 性能规范 §6 sdk_cache_hit |
| `probe/hardware_probe.cpp::HardwareSnapshot` | ADD §3.5 / ADR-017 / capability_probe 设计 §3 |
| `probe/network_probe.cpp::NetworkSnapshot` | ADD §7.5.2 / ADR-018 / capability_probe 设计 §4 |
| `probe/encoder_probe.cpp::EncoderSnapshot` | ADD §7.5.3 / ADR-019 / capability_probe 设计 §5 |
| `probe/render_probe.cpp::RenderSnapshot` | ADD §3.5 / ADR-017 / capability_probe 设计 §3.5 |
| `preset/preset_selector.cpp::match` | ADD §7.5.4 / ADR-017 / capability_probe 设计 §6 |
| `preset/preset_apply.cpp::apply_to_six_subsystems` | ADD §3.5 / §7.5.4 / capability_probe 设计 §7 |
| `preset/live_reload.cpp::downgrade_dispatch / upgrade_loop` | ADD §7.5.5 / ADR-020 / capability_probe 设计 §8 / §8.3.1（不跨 decoder 档原则） |
| `preset/oscillation_guard.cpp::count + lockout` | ADD §7.5.5 / ADR-020 / capability_probe 设计 §8.3 |

---

## 附录 B：每阶段执行 checklist（commit + push 前）

每次准备 commit + push 前必须勾完以下项：

- [ ] 阶段定义的所有验收 metric 实测达标（不是估算，不是单元测试，是回归基线流跑出来的实测值）
- [ ] 0.1 三条阶段交付不变量（正确性闸 / 延时闸 / 资源闸）全部通过
- [ ] commit message 按 §0.1 格式；body 含 before / after metric 对比
- [ ] 改动文件仅限本阶段计划列出的目标（diff stat 检查）
- [ ] 本仓库 build pass；下游 demo-host 也能编译过
- [ ] CLAUDE.md 中"仓库当前阶段"章节如已过期则同步更新（首次 phase 完成时把"目前处于架构设计阶段，无 C/C++ 源码"那段拿掉）
- [ ] 如本阶段引入了非 obvious 的稳定性 / 性能约束，写入 `memory/`（auto memory）以便跨会话保留
- [ ] git status 干净；untracked 文件该 add 的 add 该 ignore 的 ignore（build*.log 等不入仓）
- [ ] git push 后通过 GitHub Actions（如有）查看 CI 状态绿
