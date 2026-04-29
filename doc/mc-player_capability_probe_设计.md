# mc-player — Capability Probe Suite + Preset Architecture 技术设计

| 项目 | 内容 |
|---|---|
| 文档类型 | 技术设计（design-detail）|
| 上游决策 | ADR-017 / ADR-018 / ADR-019 / ADR-020 |
| 上游设计 | `mc-player_架构设计文档_v3.0.md` §3.5（顶层架构）/ §7.5（运行期协议）/ §8.4（延时预算）|
| 下游实施 | `plan/mc-player_重构方案.md` Phase 9 / Phase 10 |
| 不在范围 | 具体 C++ API 头文件签名（Phase 9 实施时按需细化）/ UI 视觉稿 / cmake target 命名 |

> 本文是 ADD §3.5 / §7.5 的展开，把"原理 + 决策"层补到"数据结构 + 算法 + 状态机"层；不替代 ADD（顶层）和 plan（实施步骤）。当三者冲突，以 ADR + ADD 为准、本文与 plan 同步修正。

---

## 1. 设计动机（与现状对比）

### 1.1 v1 现状：每个子系统独立按 ADR 静态决策

```
[Decoder]   按 ADR-015 四档降级链选档      → 不知道网络是否能配合
[Jitter]    按硬编码 target 值              → 不知道编码器有无 reorder
[Render]    按 ADR-008 BEST_EFFORT 自动激活 → 不知道显示器 VRR / 刷新率
[Present]   按 vsync-aligned 默认           → 不知道是否 Reflex 链路
[RTCP]      按 AVPF Regular 兜底           → 不知道对端是否支持 Reduced-Size
[Gate]      Frame Validity 六 bit 严格      → 不动（ADR-014 不让步）
```

**问题**：每个子系统按 worst-case 配置，端到端延时 = 各自最差段累加。同一台 NVDEC + 240Hz VRR + LAN 测试机上实测 e2e p95 在 22ms 左右，**远高于硬件物理极限**。

### 1.2 ADR-017 后：三维 probe → 5 档 preset → 一次性 apply

```
[Probe Suite] hardware ∩ network ∩ encoder ∩ render → CapabilitySnapshot
                                                              ↓
                                                      [Preset Selector]
                                                              ↓
                                                  active_preset_id (5 选 1)
                                                              ↓
                                              [Preset Apply (atomic, idempotent)]
                                                              ↓
                  ┌──────────┬──────────┬──────────┬──────────┬──────────┬──────────┐
                  ↓          ↓          ↓          ↓          ↓          ↓
             Decoder    Jitter Buf  Render Pf   Present    RTCP Mode    Gate
            (LowLat hint) (mode)  (DCOMP NV12) (race/vsync) (Reduced/Full) (固定严格)
```

**收益**（理论 + 实测预期，参见 ADD §8.4）：

| Preset | LAN 延时档 | 客户端 p95 | 端到端 p95 |
|---|---|---|---|
| SDI_REPLACEMENT | 0~1ms jitter + 0ms NV12 直显 + race_to_display | **5~8 ms** | **17~25 ms** |
| REALTIME_LAN | 5ms jitter + dual-bind shader | 8~24 ms | 30~45 ms |
| STREAMING_WIFI | 15ms jitter | 15~46 ms | 57~123 ms |
| WAN_FALLBACK | 80ms jitter（safe）| 57~189 ms | 135~365 ms |
| SAFE_MODE | 退回 Phase 0~8 baseline 行为 | 同 baseline | 同 baseline |

### 1.3 与现有 ADR 的关系

| ADR | 在 Preset 架构里的角色 |
|---|---|
| ADR-001 / ADR-002 | 档 3 hardware async MFT 的协议约束——仍是 Decoder 子系统 apply 的输入之一 |
| ADR-003 | dual-bind fence 同步——SDI_REPLACEMENT preset 走 NV12 直显时绕过，其余 preset 仍依赖 |
| ADR-007 / ADR-012 | 智能 GPU / render profile 选择——HardwareSnapshot 与 RenderSnapshot 的来源 |
| ADR-008 | DCOMP best-effort 第 4 档——SDI_REPLACEMENT preset 把它从 best-effort 升到 strict requirement |
| ADR-014 | 正确性先于延时——**任何 preset 都不放宽 Frame Validity Gate** |
| ADR-015 | 四级降级链——HardwareSnapshot.tier 字段的语义 |
| ADR-016 | 下载面板——影响 HardwareSnapshot.tier 是否能命中档 1 |

> ADR-014 的"正确性先于延时"是 preset 架构的**地基**——preset 调延时维度，不调正确性维度。任何 preset 切换都不会在 Frame Validity Gate 上让步。

---

## 2. 总体数据流

```
T0 boot
 ├ async (并行) ──┬─ HardwareProbe   → HardwareSnapshot   (~200ms)
 │                ├─ NetworkProbe    → NetworkSnapshot    (~800ms, 依赖首 GOP)
 │                ├─ EncoderProbe    → EncoderSnapshot    (~800ms, 依赖首 GOP)
 │                └─ RenderProbe     → RenderSnapshot     (~50ms,  本地查询)
 │                          │
 │                          ↓
 │                  CapabilitySnapshot (聚合)
 │                          ↓
 │                  PresetSelector::match()
 │                          ↓
 │                  active_preset_id  ∈ {SDI_REPLACEMENT, REALTIME_LAN, STREAMING_WIFI, WAN_FALLBACK, SAFE_MODE}
 │                          ↓
 │                  PresetApply::apply()  ─→ 6 个子系统 idempotent apply
 │
 └ sync (主线) ─── BOOTSTRAP preset (REALTIME_LAN 保守版) → 首帧
                              ↓
                          首 GOP 完成
                              ↓
                  从 BOOTSTRAP 切到 active_preset_id (一次性 reload)
                              ↓
                  进入 LiveReloadLoop（持续 telemetry 监控）
                              ↓
                  ┌─────────┴─────────┐
                  ↓                   ↓
            DowngradeDispatch    UpgradeLoop
            (一帧内决策)          (60s 滑窗决策)
                  ↓                   ↓
            preset reload        preset reload
            (帧间缝隙)           (帧间缝隙)
```

---

## 3. HardwareSnapshot（ADR-007 / ADR-012 / ADR-015）

**采集源**：本地 D3D11 / DXGI / vendor SDK 探测（无网络依赖）；可在 200ms 内完成。

```
struct HardwareSnapshot {
    // ADR-015 解码档位
    decoder_tier_t      tier;              // {VENDOR_SDK_NVDEC, VENDOR_SDK_ONEVPL, VENDOR_SDK_AMF, DXVA_DIRECT, MFT_HARDWARE, LIBCODEC}
    bool                supports_h264;
    bool                supports_h265_main;
    bool                supports_h265_main10;
    bool                supports_av1;

    // ADR-007 智能 GPU
    LUID                primary_adapter_luid;
    gpu_vendor_t        gpu_vendor;        // {NVIDIA, INTEL, AMD, OTHER}
    uint32_t            gpu_arch_gen;      // 例如 NVIDIA Ampere=8 / Ada=9 / Blackwell=10

    // dual-bind 与 fence 能力（ADR-003）
    bool                supports_dual_bind_nv12;
    bool                supports_d3d11_fence;

    // 子目标 1 / 5 探针
    bool                supports_dcomp_nv12_direct;   // composition swapchain DXGI_FORMAT_NV12 + MPO 多 plane
    uint32_t            mpo_max_planes;               // OS 报告的 MPO planes 数（DCOMP_INDEPENDENT_FLIP 前提）
    bool                supports_cuda_graphs;          // 仅 NVDEC tier 时有意义
};
```

**采集步骤**：

1. `IDXGIFactory7::EnumAdapterByGpuPreference(HIGH_PERFORMANCE)` → 取 primary_adapter；
2. `D3D11CreateDevice(adapter, ...)` → `ID3D11VideoDevice::CheckVideoDecoderProfile` 四组合（H264/H265/H265-10bit/AV1）；
3. vendor SDK 命中检测：按 ADR-016 的 `%LOCALAPPDATA%\mc-player\sdk\<vendor>\<version>\` cache 探测 + System32 fallback；
4. `CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS5)` 取 fence 支持；
5. DCOMP MPO planes 数：`IDXGIOutput6::GetCurrentOutputDuplicationDescription` 或 PresentMon 实测兜底；
6. CUDA Graphs：仅 NVDEC tier 时调 `cuGraphCreate` 试探（失败即 false，不 panic）。

**fail-open 原则**（ADD §2 #11）：任何探测失败 → 对应字段置 false，**不抛异常**。后续 selector 会按"能力差"路径选 preset。

---

## 4. NetworkSnapshot（ADR-018，ADD §7.5.2）

**采集源**：RTSP keepalive RTT 或 RTCP RR/SR 反推 + 首 GOP 内 RTP 包分布；依赖首包，~800ms 完成。

```
struct NetworkSnapshot {
    uint64_t            rtt_p50_ns;
    uint64_t            rtt_p95_ns;
    uint64_t            rtt_p99_ns;
    double              iat_jitter_ms;           // 首 GOP 内 RTP 包 inter-arrival time 标准差
    double              loss_rate_short;         // 首 GOP 累计 seq gap / 总包数
    link_kind_t         link_kind;               // {LAN_SWITCHED, LAN_WIFI, WAN_WIRED, WAN_WIRELESS, UNKNOWN}

    // 滑窗形态（仅 Phase 10 LiveReload 用）
    uint64_t            rtt_p95_window_5s_ns;
    double              loss_rate_window_5s;
    double              loss_rate_window_60s;
};
```

### 4.1 RTT 采集

| 协议 | 来源 |
|---|---|
| RTSP/RTP | 控制通道 TCP keepalive RTT；或 RTP 包到达时间反推（**不准，仅 fallback**） |
| RTSP interleaved | 同上，TCP keepalive |
| WHEP | STUN binding indication 与 response 的 RTT；ICE 完成后 RTCP RR/SR 反推 |

### 4.2 link_kind 推断

按 ADD §7.5.2 阈值表，**用 p95 而非 p50**（避免被极少数好包扭曲）：

```
if rtt_p95 < 2ms && iat_jitter_ms < 1 && loss_rate_short < 0.01% → LAN_SWITCHED
elif rtt_p95 < 10ms && iat_jitter_ms < 5 && loss_rate_short < 0.1% → LAN_WIFI
elif rtt_p95 < 80ms && iat_jitter_ms < 10 && loss_rate_short < 1% → WAN_WIRED
elif rtt_p95 < 200ms && iat_jitter_ms < 50 && loss_rate_short < 3% → WAN_WIRELESS
else → UNKNOWN
```

> ⚠️ "WAN_WIRELESS" 是 4G/5G/卫星这类**非家用 Wi-Fi**链路。家用 Wi-Fi 在 LAN_WIFI 范畴；公网 + 任何无线接入都进 WAN_WIRELESS。

### 4.3 滑窗（Phase 10 LiveReload 才需要）

- 5s 窗口：用于即时降级触发——loss/rtt 突增 detection；
- 60s 窗口：用于稳定性升级试探——必须全程满足下一档 preset 才升；
- 实现：环形缓冲 + 每秒滚动；CPU 开销 < 0.1%。

---

## 5. EncoderSnapshot（ADR-019，ADD §7.5.3）

**采集源 3 档可信度**（先取得即停）：

### 5.1 SDP `profile-level-id`（最高可信度）

H.264 RFC 6184 §8.1：`profile-level-id` 是 6 hex digits = 3 bytes：

```
profile_idc    = bytes[0]    e.g. 66 = Baseline / 77 = Main / 100 = High
profile_iop    = bytes[1]    constraint flags
level_idc      = bytes[2]    e.g. 1f = Level 3.1
```

**严声明 0 reorder 的条件**：

- profile_idc == 66 (Baseline) **且** constraint_set0_flag == 1 → constrained baseline，**不可能有 B 帧**；
- 或 profile_idc == 66 **且** level_idc <= 0x1f (3.1) → baseline level 限制下不发 B（仍可能 reorder 为 0，但不强制）；
- 其余情况一律按 SPS VUI 实测，不强声明。

H.265 RFC 7798：`profile-id` + `tier-flag` + `level-id`，无类似强声明的能力，全靠 SPS。

### 5.2 SPS VUI（次可信度）

解析首 IDR 内的 SPS NAL，关键字段：

```
H.264:
  bitstream_restriction_flag  ← 必须 == 1 才能信下面字段
    motion_vectors_over_pic_boundaries_flag
    max_num_reorder_frames    ← 0 表示编码器承诺无 reorder
    max_dec_frame_buffering

H.265:
  sps_max_num_reorder_pics[sps_max_sub_layers_minus1]  ← 直接读
```

> ⚠️ x264 默认 `bitstream_restriction_flag == 0`，VUI 中 reorder 字段全 default。这种情况下**不能信 VUI**，必须降到第 3 档实测。

### 5.3 第一 GOP DTS/PTS 实测（兜底）

- 观察 RTP 包 timestamp 与解码后 frame 的 PTS 关系；
- 若 PTS 单调递增 → reorder_depth = 0；
- 若 20 帧内见乱序 → reorder_depth = max_observed_reorder（在窗口内取最大值）。

```
struct EncoderSnapshot {
    uint8_t     reorder_depth;          // 0 / 1 / 2+
    uint16_t    gop_size;               // 第一 GOP 实测
    bool        is_zerolatency_hint;    // VUI low_delay_hrd_flag + reorder_depth==0
    bool        has_b_frames;           // ≡ reorder_depth > 0；冗余字段便于 selector 表达
    slice_struct_t slice_struct;        // {single, multi_slice, picture_segment}
    encoder_source_t  source;           // {SDP_PROFILE_LEVEL_ID, SPS_VUI, FIRST_GOP_MEASURED}
};
```

> 字段 `source` 用于 telemetry 调试——如果生产环境大量场景跑到 `FIRST_GOP_MEASURED`，可能需要要求编码端开 VUI bitstream_restriction_flag。

---

## 6. PresetSelector（ADR-017，ADD §7.5.4）

### 6.1 5 档 preset 定义表

| 字段 | SDI_REPLACEMENT | REALTIME_LAN | STREAMING_WIFI | WAN_FALLBACK | SAFE_MODE |
|---|---|---|---|---|---|
| `decoder_low_latency_hint` | full（NVDEC ulMaxDisplayDelay=0 + EOP / oneVPL AsyncDepth=1 + COMPLETE_FRAME / AMF REORDER_MODE=LOW_LATENCY） | full | partial（允许 1 reorder） | off | off |
| `jitter_mode` | ZeroJitter（target 0~1ms） | Kalman aggressive（target 5ms） | Kalman normal（target 15ms） | Kalman safe（target 80ms） | Kalman safe |
| `render_profile` | DCOMP_INDEPENDENT_FLIP + NV12 直显 + ALLOW_TEARING | 同左 | DCOMP_BEST_EFFORT 或 SWAPCHAIN_FLIP | SWAPCHAIN_FLIP | SWAPCHAIN_FLIP |
| `present_scheduling` | race_to_display | race_to_display | vsync-aligned | vsync-aligned | vsync-aligned |
| `rtcp_mode` | RFC 5506 Reduced-Size + AVPF Immediate trr-int=0 | 同左 | AVPF Immediate | AVPF Regular | AVPF Regular |
| `gate_strictness` | 4 核心 bit（refs/params/recovery/reorder）strict；color/fence 见 §6.3 注 | 6 bit 全 strict | 同左 | 同左 | 同左 |
| `frame_validity_six_bits` | refs / params / recovery / reorder strict；color/fence 在 NV12 直显路径不参与决策 | 6 bit 全 strict | 6 bit 全 strict | 6 bit 全 strict | 6 bit 全 strict |

### 6.2 匹配算法（ADD §7.5.4 重申）

**先严后宽，先匹配先停**：

```
def match(snapshot: CapabilitySnapshot) -> PresetId:
    hw, net, enc, render = snapshot

    # SDI_REPLACEMENT — 仅在硬件 + 网络 + 编码器 + 显示器全部最优时
    if (hw.tier in {VENDOR_SDK_NVDEC, VENDOR_SDK_ONEVPL, VENDOR_SDK_AMF}
        and hw.supports_dcomp_nv12_direct
        and net.link_kind == LAN_SWITCHED
        and net.rtt_p95_ns < 1_000_000
        and net.loss_rate_short < 0.0001
        and enc.reorder_depth == 0
        and enc.is_zerolatency_hint
        and render.profile == DCOMP_INDEPENDENT_FLIP
        and render.refresh_rate_hz >= 240
        and render.vrr_enabled):
        return SDI_REPLACEMENT

    # REALTIME_LAN — 任 vendor SDK 或 DXVA-direct + LAN + 0 reorder
    if (hw.tier in {VENDOR_SDK_NVDEC, VENDOR_SDK_ONEVPL, VENDOR_SDK_AMF, DXVA_DIRECT}
        and net.link_kind in {LAN_SWITCHED, LAN_WIFI}
        and net.rtt_p95_ns < 5_000_000
        and enc.reorder_depth == 0
        and render.profile in {DCOMP_INDEPENDENT_FLIP, DCOMP_BEST_EFFORT}):
        return REALTIME_LAN

    # STREAMING_WIFI — 中等链路 + 任硬解档 + 允许 1+ reorder
    if (hw.tier in {VENDOR_SDK_NVDEC, VENDOR_SDK_ONEVPL, VENDOR_SDK_AMF, DXVA_DIRECT, MFT_HARDWARE}
        and net.link_kind in {LAN_WIFI, WAN_WIRED}
        and net.rtt_p95_ns < 20_000_000):
        return STREAMING_WIFI

    # WAN_FALLBACK — 公网或较高 loss
    if (net.link_kind in {WAN_WIRED, WAN_WIRELESS}
        or net.loss_rate_short < 0.03):
        return WAN_FALLBACK

    # SAFE_MODE — 任何探测失败或 hw.tier == LIBCODEC + 公网
    return SAFE_MODE
```

### 6.3 Gate 严格度的细节（与 ADR-014 / ADR-017 严格一致）

`frame_validity_six_bits` 在 6 bit 维度上的语义：

| Bit | 含义 | SDI_REPLACEMENT preset 下 | 其他 preset 下 |
|---|---|---|---|
| `refs` | 参考帧链完整 | strict（任何 preset 不让步）| strict |
| `params` | SPS/PPS 已就位 | strict | strict |
| `recovery` | GDR `recovery_point` 完整或 IDR | strict | strict |
| `reorder` | reorder_depth 与 PTS/DTS 对账 | strict | strict |
| `color` | BT.709/2020 color metadata 已就位 | **不参与决策**（NV12 直显路径不消费 color metadata，由显示器 EDID + DCOMP color profile 兜底） | strict |
| `fence` | dual-bind shared NV12 array slice 已被 SRV 读完 | **不参与决策**（NV12 直显绕开 shader + 无 dual-bind） | strict |

**不让步的 4 bit**：refs / params / recovery / reorder——任何 preset 失败这 4 bit 都直接花屏 / 解码失败 / 帧序错乱。

**preset 路径无关的 2 bit**：color / fence——SDI_REPLACEMENT 走"DCOMP composition swapchain Format=NV12 + MPO 多面合成"路径，硬件直接消费 NV12 平面，**没有 SRV 读取 + 没有 RGB 转换 shader**，所以 fence 与 color metadata 在该路径下无消费方。

> 这与 ADR-017 reference 中 "Gate 严格度可被 Preset 影响（极端低延时 preset 可放宽 fence/color 子集）" 的"放宽"措辞含义一致——"放宽"是代码路径无该消费方，不是放宽正确性。

### 6.4 边缘场景

| 场景 | 选择结果 |
|---|---|
| LAN_SWITCHED + 编码器 reorder_depth=1（IBP）| 不命中 SDI_REPLACEMENT（缺 0 reorder），命中 REALTIME_LAN |
| NVDEC + WAN（出差远程播放）| 不命中 SDI_REPLACEMENT / REALTIME_LAN，命中 STREAMING_WIFI 或 WAN_FALLBACK |
| Intel UHD 730（命中 oneVPL）+ 60Hz 显示器 + LAN | 命中 REALTIME_LAN（不要求 240Hz；那是 SDI_REPLACEMENT 专属） |
| MFT_HARDWARE 档 + LAN_SWITCHED | 命中 STREAMING_WIFI（MFT 协议 host 开销在 LAN 也无法逼到 SDI 档延时） |
| 任何 probe timeout / fail | 命中 SAFE_MODE（fail-open） |

---

## 7. PresetApply（一次性原子配置 6 个子系统）

### 7.1 idempotent 契约

`apply(preset)` 必须满足：

- 同一 `preset` 多次 apply 等价单次（实施时每个子系统按"目标态 vs 当前态"diff，无变化即跳过）；
- apply 失败任一子系统 → **回滚** 已成功子系统到 BOOTSTRAP 默认；写 `mc.preset.apply_failure_count` 并降到 SAFE_MODE。

### 7.2 6 个子系统的 apply 方式

| 子系统 | apply 方式 | 帧间缝隙要求 |
|---|---|---|
| Decoder | NVDEC: `cuvidReconfigureDecoder` / oneVPL: `MFXVideoDECODE_Reset` / AMF: re-set property bag；MFT: `IMFTransform::ProcessMessage(RESET)` | GOP 边界 |
| Jitter Buffer | 双缓冲实现：新 mode 后台 spin-up + warm-up；切换时原子 swap 指针；旧 buffer 排空后 GC | 帧间 |
| Render Profile | DCOMP visual tree 重建（IndependentFlip → BestEffort 切换）；NV12 swapchain Format 切换需 `IDXGISwapChain::ResizeBuffers` | 帧间，需 ClearState（ADD §5.10.3）|
| Present 调度 | 切 race_to_display ↔ vsync-aligned 仅切 ALLOW_TEARING flag + scheduler 模式 | 帧间，无重建 |
| RTCP Mode | Reduced-Size on/off + AVPF Immediate / Regular 切换；与对端协商位需在下次 RTCP 包生效 | 下一 RTCP 心跳 |
| Frame Validity Gate | **不动**（preset 不调正确性维度） | n/a |

### 7.3 与 Present Epoch 的协同（ADD §5.10.5）

- preset reload 视作 epoch 切换源——T5 commit DCOMP 时同时升 epoch；
- 旧 epoch 在飞帧由 watchdog 兜底丢弃，不让"旧 preset 配置下解码的帧"进入"新 preset 配置下的 render"。

---

## 8. LiveReloadLoop（ADR-020，ADD §7.5.5）

### 8.1 状态机

```
                         BOOTSTRAP
                              ↓ (probe complete)
                       ┌─→ ACTIVE ←─────────────┐
                       │     ↓                   │
                       │  ┌─ DowngradeDispatch ──┘ (降级触发)
                       │  └─ UpgradeLoop ────────┐ (60s 稳定)
                       │     ↓                   │
                       └── reloading ────────────┘
                              ↓ (apply 完成)
                          ACTIVE (新 preset)
```

### 8.2 降级触发表（ADD §7.5.5）

| 信号 | 监控源 | 阈值 | 降级动作 |
|---|---|---|---|
| `loss_rate_window_5s` 突增 | NetworkSnapshot 滑窗 | 超 preset 上限 2× | 立即降一档 |
| `rtt_p95_window_5s` 突增 | NetworkSnapshot 滑窗 | 超 preset 上限 1.5× | 立即降一档 |
| `decoder_error_rate` 抖增 | Frame Validity Gate 拒帧率 | >5% / 5s | 降一档 + 重置 jitter |
| `present_underrun` 反复 | T5 watchdog | >3 次 / 30s | 降一档 + 切 BEST_EFFORT |
| `tearing_detected` 视觉异常 | 内部模式不匹配 | 1 次即降 | 降一档 + render 退到 SWAPCHAIN |

降级响应 SLA：信号触发 → preset reload 完成 ≤ 5ms（ADD §7.5.5）。

### 8.3 升级试探 + 振荡保护

```
升级前提（必须全满足）：
  - current_preset != SDI_REPLACEMENT
  - 过去 60s 滑窗 NetworkSnapshot / EncoderSnapshot / RenderSnapshot 持续满足下一档 preset 触发条件
  - 过去 60s tainted_event_count == 0
    （tainted_event = Frame Validity Gate 拒帧 + present_underrun + tearing_detected 任一）

升档后 30s 内若再被降级触发：
  oscillation_count++

oscillation_count >= 2 时：
  锁定当前档 5 min 禁升（lockout_until = now + 5min）；
  每次降级仍可正常执行；
  超过 lockout_until 后 oscillation_count 清零。
```

> **设计理由**：升档误判的代价是"用户视觉抖动 + 振荡循环"，远超"延时偏高"的代价。所以升档严苛、降档宽松。

### 8.4 reload 原子性闸

- 队列优先级：降级请求 > 升级请求；reload 期间收到降级请求立即取消进行中的升级；
- `mc.preset.apply_atomic_violation_count` 监控：reload 全程不能丢帧（即不能引发 Frame Validity Gate 拒帧 + 不能引发 present_underrun）；
- reload 失败或部分应用：回滚到 reload 前 preset，不停留在中间态。

---

## 9. 性能 metric（与性能量度规范对齐）

按 `mc.<domain>.<metric>_<unit>` 命名约定：

```
mc.preset.active_id                          enum {SDI_REPLACEMENT, REALTIME_LAN, STREAMING_WIFI, WAN_FALLBACK, SAFE_MODE}
mc.preset.bootstrap_to_active_ms             histogram, p95 阈值 1500
mc.preset.reload_event{from,to,reason}       counter (event log)
mc.preset.reload_latency_ns                  histogram, p95 阈值 5_000_000
mc.preset.downgrade_count                    counter
mc.preset.upgrade_count                      counter
mc.preset.oscillation_count                  gauge
mc.preset.apply_atomic_violation_count       counter, 阈值 0
mc.preset.apply_failure_count                counter, 阈值 0

mc.probe.hardware_complete_ms                histogram, p95 阈值 200
mc.probe.network_complete_ms                 histogram, p95 阈值 1000
mc.probe.encoder_complete_ms                 histogram, p95 阈值 1000
mc.probe.tier_skip_reason{tier}              counter by reason
mc.probe.network_link_kind                   gauge enum
mc.probe.encoder_source                      gauge enum  // {SDP_PROFILE_LEVEL_ID, SPS_VUI, FIRST_GOP_MEASURED}
mc.probe.encoder_reorder_depth               gauge

mc.net.rtt_p95_window_5s_ns                  gauge, 阈值由 active_preset 决定
mc.net.loss_rate_window_5s                   gauge, 阈值由 active_preset 决定
mc.net.iat_jitter_ms                         gauge

mc.jitter.mode                               enum {ZeroJitter, KalmanAggressive, KalmanNormal, KalmanSafe}
mc.render.dcomp_nv12_direct_active           gauge bool
mc.present.race_to_display_active            gauge bool
mc.rtcp.reduced_size_active                  gauge bool
mc.decoder.cuda_graph_active                 gauge bool (NVDEC only)
```

---

## 10. 测试策略

### 10.1 单元测试（PresetSelector::match）

| 用例 | 输入 snapshot | 预期 active_preset_id |
|---|---|---|
| best-case | NVDEC + LAN_SWITCHED RTT 0.3ms + 0 reorder + 240Hz VRR + DCOMP_INDEPENDENT_FLIP | SDI_REPLACEMENT |
| LAN-1080p | UHD 730 oneVPL + LAN_SWITCHED RTT 0.5ms + 0 reorder + 144Hz VRR | REALTIME_LAN |
| LAN-with-B | NVDEC + LAN_SWITCHED + reorder=1 + 240Hz VRR | REALTIME_LAN（不命中 SDI 因有 B）|
| Wi-Fi | NVDEC + LAN_WIFI RTT 4ms + 0 reorder + 144Hz | REALTIME_LAN（命中 LAN_WIFI 上限） |
| WAN | NVDEC + WAN_WIRED RTT 30ms + 0 reorder | STREAMING_WIFI |
| 公网高 loss | DXVA + WAN_WIRELESS RTT 80ms + loss 2% | WAN_FALLBACK |
| probe timeout | 任 snapshot 字段 = invalid | SAFE_MODE |

### 10.2 集成测试（probe + selector + apply 全链路）

- 真实 NVDEC 测试机上跑回归基线流（CLAUDE.md §测试基线），实测 `mc.preset.bootstrap_to_active_ms < 1500`；
- 启动后 5s 内 `mc.preset.active_id` 稳定不抖（不应该 reload）。

### 10.3 LiveReload 注入测试（Phase 10）

- 注入 loss 突增（tc qdisc / clumsy）→ 5ms 内 downgrade 完成；
- 注入 60s 完美链路 → 一次 upgrade；继续注入 30s 内抖动 → oscillation_count = 1；再来一次 → count = 2 → 锁定 5min；
- reload 全程 `mc.gate.drop_*_count` warm_steady 增量 = 0（ADR-014 不让步验证）。

---

## 11. 风险与对策

| 风险 | 对策 |
|---|---|
| Probe 期间首帧延时偏高（用户感知"刚启动卡 1 秒"）| BOOTSTRAP preset 用 REALTIME_LAN 保守版先播；probe 完成后 reload；用户视角"先有画再渐入低延时" |
| Encoder reorder 实测窗口太短误判 0 reorder | 实测 reorder_depth 在前 N 帧滑窗，N=20；之后改 reorder>0 时立即从 SDI_REPLACEMENT 降到 REALTIME_LAN |
| 用户跨屏拖窗 → render profile 突变 | 跨屏属"假设破坏"事件，触发立即 reload；DCOMP_BEST_EFFORT 优雅切换 |
| 升档振荡导致用户视觉抖动 | oscillation_guard 锁定机制；运维可在 stats 看到 lockout 状态 |
| 三维 probe 之间数据竞态 | 每个 probe 独立线程；CapabilitySnapshot 聚合时用 atomic snapshot pattern（任一未到先用 BOOTSTRAP 默认） |
| Preset 配置表与子系统能力 drift | `apply_failure_count` metric + 集成测试覆盖；任何 apply 失败回滚到 SAFE_MODE 不放过 |
| 公网 + UDP 严重 jitter 时 SAFE_MODE 也保不住延时 | 文档明确 SAFE_MODE 仅保正确性，不保延时；用户 UI 提示当前网络不适合实时播放 |

---

## 12. 与 ADR / ADD / plan 的对应

| 本文章节 | ADR | ADD 章节 | plan 阶段 |
|---|---|---|---|
| §1 ~ §2 总体 | ADR-017 | §3.5 | Phase 9 §9.0 |
| §3 HardwareSnapshot | ADR-007 / ADR-012 / ADR-015 | §7.3 / §7.4 / §9.1 | Phase 9 §9.1 hardware_probe |
| §4 NetworkSnapshot | ADR-018 | §7.5.2 | Phase 9 §9.1 network_probe |
| §5 EncoderSnapshot | ADR-019 | §7.5.3 | Phase 9 §9.1 encoder_probe |
| §6 PresetSelector | ADR-017 | §7.5.4 | Phase 9 §9.1 preset_selector |
| §7 PresetApply | ADR-017 | §3.5 / §7.5.4 | Phase 9 §9.1 preset_apply |
| §8 LiveReloadLoop | ADR-020 | §7.5.5 | Phase 10 §10.0 ~ §10.2 |
| §9 metric | — | — / 性能量度规范 | Phase 9 §9.3 / Phase 10 §10.3 |
| §10 测试 | — | — | Phase 9 §9.2 步骤 5 / Phase 10 §10.2 步骤 |
| §11 风险 | ADR-014（不让步前提） | §11 | Phase 9/10 验收 metric |

---

## 13. 后续演进口子（不在本文范围）

- **HDR10 / scRGB preset 维度**：色彩管线作为第 4 维 probe，在 SDI_REPLACEMENT 上叠加 HDR10_PERFECT 子档；
- **AV1 / H.266 preset 适配**：硬件普及后扩 EncoderSnapshot 字段；
- **多路并发场景的 preset 协同**：单进程播多路时按"最差路决定 worker pool"策略；
- **远程协同视频会议场景**：与编码端 preset 协商（client send hint 给 server SFU），目前 mc-player 是纯接收端、不在范围。

---

> 本文与 ADD / ADR / plan 一起构成 ADR-017 ~ ADR-020 的完整文档闭环。修改本文 + ADD §7.5 + plan Phase 9/10 任一处时务必同步另两处；如发现本文与 ADD 矛盾，以 ADD 为准、本文做修订；如发现 ADD 与 ADR 矛盾，以 ADR 为准。
