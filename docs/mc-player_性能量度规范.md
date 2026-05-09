# mc-player — 性能量度规范

| 项目 | 内容 |
|---|---|
| 文档类型 | 实施性规范（不属于 ADD / ADR）—— 代码埋点 + 运行期 stats + 排障定位的契约 |
| 适用阶段 | 主项目源码落地、子项目 mc-libcodec 落地、CI 性能闸建立 |
| 上游依据 | ADD §1.2 设计目标 / §3.3 线程视图 / §5.13 Frame Validity Gate / §7.4 档位降级 / §8 延时预算 / ADR-015 四级降级链 |
| 不在范围 | 实施级 ETW Provider 二进制布局、stats 上报 IPC 协议、UI 监控面板设计 |

> 本规范定义 metric 名、含义、单位、上报时机、阈值与排障路径。在代码落地前先合意此规范，能避免"先写完才发现没法定位瓶颈"的回工。

---

## 1. 度量哲学

### 1.1 五维分类

| 维 | 关注 | 例 |
|---|---|---|
| **延时（latency）** | 时间从 A 到 B 多久 | 端到端、阶段性、首帧、握手 |
| **吞吐（throughput）** | 单位时间多少 | 帧率、字节率、包率 |
| **正确性（correctness）** | 输出是否合规 | 花屏防御 drop 数、污染态时长、SEP 正确性 |
| **资源（resource）** | 占用了多少 | CPU、GPU、内存、VRAM、队列水位 |
| **错误（error）** | 异常路径触发频次 | device_lost、adapter switch、网络断线、降档 |

**正确性与延时同等级**：与 ADD §2 #12 / ADR-014 一致——花屏 / 陈旧区域 / 错误下沉的 metric 与延时 metric 同等级别上报，**不允许只看延时不看正确性**。

### 1.2 三种场景

埋点必须能区分以下三种状态，否则 p95 / p99 会被冷启动 / 错误恢复污染：

| 场景 | 起止 | 期望特征 |
|---|---|---|
| `cold_start` | `mc_open` 调用 → 第一帧 emit | 含信令 / 探测 / 协商 / 等 IDR；不计入稳态延时 |
| `warm_steady` | 第一帧 emit + 1 s → 错误事件 | 主要观察对象；ADD §1.2 / §8 阈值仅约束本场景 |
| `error_recovery` | 任一污染源触发 → `recovery_complete` 解除 | 与 §5.13 Frame Validity Gate 污染态对应；多 freeze 1~3 帧是设计权衡 |

实现：每个 metric 上报时附加 `phase ∈ {cold_start, warm_steady, error_recovery}` tag；CI 闸只对 `warm_steady` 设阈。

### 1.3 层级关系

```
端到端延时（用户感知）
  └─ 客户端内部延时（mc-player 可控部分）
      ├─ 模块阶段延时（jitter / depack / decode / render / present）
      │   └─ 模块内子阶段（input alloc / 实际解码 / output 提取 / ...）
      └─ 跨模块同步延时（Frame Validity Gate 等待 / Present Epoch 配对）
```

低层 metric 必须能拼出上层 metric，否则定位无效。**严禁**只埋端到端而不埋阶段——出问题时看不到瓶颈在哪。

### 1.4 percentile 选择

| 用途 | 选 | 理由 |
|---|---|---|
| 体感延时基线 | **p95** | 用户体感对齐 95 分位，p99 太尖锐 / p50 太宽容 |
| SLO / 报警 | **p99** | 偶发抖动可见 |
| 平均行为 | **p50** | 用于排查"普遍变慢"vs"个别尖刺" |
| 极限抖动 | **max** | 与 p99 联合判断长尾 |

**禁用 mean**：mean 会被极少数尖刺拉偏，对延时 metric 无诊断价值。

### 1.5 单位约定

| 量纲 | 单位 | 字段后缀 | 例 |
|---|---|---|---|
| 时间 | 纳秒 | `_ns` | `decode_actual_ns` |
| 时间（汇总后展示） | 微秒 / 毫秒 | `_us` / `_ms` | `e2e_latency_p95_ms` |
| 字节 | 字节 | `_bytes` | `rtp_rx_bytes` |
| 计数 | 整数 | `_count` | `pli_sent_count` |
| 比率 | 0~1 浮点 | `_ratio` | `nack_recovery_ratio` |
| 帧数 | 整数 | `_frames` | `jitter_depth_frames` |

**采集精度统一 ns**（QPC 原生分辨率），仅在展示层折算到 ms。中间转换有损，不收。

---

## 2. 延时类指标（最核心）

### 2.1 端到端延时（`mc.e2e.*`）

**定义**：编码端 capture 时刻 → 显示器 scan-out 完成。受 ADD §1.2 表 / §8.2 表约束。

| 字段 | 类型 | 计算 | 阈值（warm_steady） |
|---|---|---|---|
| `mc.e2e.latency_ns` | histogram | 帧 PTS（经 §5.9.2 NTP-RTP 拟合反推到 encoder capture NTP，再以 SR 接收时刻校准到本机 QPC）→ Present epoch QPC（§5.10.5 commit 时刻） | p95：60Hz ≤110ms / 144Hz VRR ≤75ms / 240Hz VRR+DCOMP ≤50ms |
| `mc.e2e.client_internal_ns` | histogram | rx_first_byte_qpc → Present epoch QPC（mc-player 可控部分） | p95 ≤25ms（ADD §1.2） |
| `mc.e2e.network_one_way_ns` | histogram | RTCP RR 算出的 RTT 折半作为单向网络估计 | 单独上报便于剥离网络项 |

**注意**：编码端 latency 不可控（ADD §8.2 表把它单列即为此）。CI 闸看 `mc.e2e.client_internal_ns`（mc-player 可控部分），`mc.e2e.latency_ns` 只作端到端可观测面，不直接卡阈值。NTP-RTP 拟合的斜率 a 与偏移 b 是中间变量，不作为 metric 暴露。

### 2.2 客户端内部阶段延时（`mc.stage.*`）

直接对应 ADD §8.1 表，每行 = 一个 timer。**所有 timer 必埋**——缺一项都让"瓶颈定位"失效。

| 字段 | 阶段 | 埋点位置 | ADD §8.1 行 | 期望（warm，1080p H.264 30fps） |
|---|---|---|---|---|
| `mc.stage.udp_rx_ns` | UDP→jitter 入队 | `WSARecvFrom` 完成 → push to jitter | 行 1 | 0.2~3 ms |
| `mc.stage.jitter_dwell_ns` | jitter buffer 停留 | push → pop | 行 2 | 0~20 ms（按 jitter target） |
| `mc.stage.depack_ns` | depack | pop → 出 Annex-B AU | 行 3 | <1 ms |
| `mc.stage.decode_alloc_ns` | input alloc + memcpy | AU → decoder buffer 就绪 | 行 4 | 档1~30μs / 档2<10μs / 档3~50μs / 档4~50μs |
| `mc.stage.decode_actual_ns` | 实际解码 | submit → 解码完成 fence signal | 行 5 | 档1 3~8 / 档2 4~10 / 档3 4~12 / 档4 5~10 ms |
| `mc.stage.decode_output_ns` | output 提取 | 解码完成 → renderer 拿 view | 行 6 | 档1/2/4 ~5μs / 档3 ~50μs |
| `mc.stage.upload_ns` | NV12→D3D11 上传（仅档 4） | dynamic map → unmap | 行 7 | 档4 ~0.5 ms / 其它 0 |
| `mc.stage.yuv2rgb_ns` | dual-bind / CopySub + YUV→RGB shader | renderer dispatch → fence | 行 8 | <1~3 ms |
| `mc.stage.present_queue_ns` | Present 队列等待 | Present 调用 → frame-latency waitable signal | 行 9 | 0~4 ms（DCOMP 时 ~0） |

**总和约束**：`Σ mc.stage.* ≈ mc.client.internal_latency_ns ± 1ms`。差异 > 1ms 说明有未埋点的隐藏阶段或时钟漂移。

### 2.3 首帧延时（`mc.firstframe.*`）

对应 ADD §8.3 表。区别于 §2.2 的稳态阶段延时——首帧含一次性开销（caps_probe / IDR 等待 / MFT init）。

| 字段 | 含义 | 阈值 |
|---|---|---|
| `mc.firstframe.open_to_first_emit_ns` | `mc_open` 返回成功 → 第一帧 emit | ≤200ms 最优 / ≤500ms 一般 / ≤2s 大 GOP |
| `mc.firstframe.caps_probe_ns` | adapter 枚举 + profile 探测 | 一次性，建议 ≤30ms |
| `mc.firstframe.decoder_init_ns` | 选档后 codec 初始化（vendor SDK / DXVA / MFT） | ≤50ms |
| `mc.firstframe.wait_idr_ns` | 收到 IDR / IRAP / GDR-complete 前的等待 | 取决于流 GOP |
| `mc.firstframe.whep_handshake_ns` | WHEP 模式 ICE+DTLS+SRTP setup | LAN ≤500ms / 公网 ≤1s |

### 2.4 caps_probe 与 open 各阶段

档位选择是首帧延时的核心子项，需细化到每档探测耗时——便于发现"档 1 探测就慢 200ms"的问题。

| 字段 | 含义 |
|---|---|
| `mc.probe.tier1_ns` | vendor SDK probe（dll 存在 + LoadLibrary + 最小 API call） |
| `mc.probe.tier2_ns` | DXVA-direct probe（CheckVideoDecoderProfile + CheckVideoDecoderFormat） |
| `mc.probe.tier3_ns` | MFT probe（MFTEnumEx 全列表 + ASYNC 标记筛查） |
| `mc.probe.tier_selected` | gauge，最终选中档（1/2/3/4） |
| `mc.probe.tier_skip_reason{tier=N}` | counter，每档跳过原因（vendor_mismatch / sdk_missing / profile_unsupported / sync_software_only / disabled） |

### 2.5 队列与同步等待延时

ADD §3.3 线程视图中的 SPSC 队列每条单独埋。

| 字段 | 队列（ADD §6.1） |
|---|---|
| `mc.queue.rtp_to_jitter_dwell_ns` | RTP→JitterBuf |
| `mc.queue.depack_to_codec_dwell_ns` | Depack→Codec |
| `mc.queue.codec_to_render_dwell_ns` | Codec→Render |
| `mc.sync.gate_wait_ns` | Frame Validity Gate 等 bit 全集齐的等待（§5.13） |
| `mc.sync.fence_wait_ns` | dual-bind fence wait（§4.3） |
| `mc.sync.present_epoch_pair_ns` | Present 与 DCOMP commit 配对延时（§5.10.5） |

---

## 3. 吞吐与帧率（`mc.tput.*`）

### 3.1 输入端

| 字段 | 类型 | 含义 |
|---|---|---|
| `mc.tput.rtp_rx_pps` | gauge | RTP 包/秒 |
| `mc.tput.rtp_rx_bytes_per_sec` | gauge | RTP 字节/秒（含头） |
| `mc.tput.rtp_loss_count` | counter | seq number gap 累计 |
| `mc.tput.rtp_oo_count` | counter | 乱序包数（在 jitter 重排后归零） |

### 3.2 jitter buffer

| 字段 | 类型 | 含义 |
|---|---|---|
| `mc.jitter.depth_frames` | gauge | 当前缓存帧数 |
| `mc.jitter.target_delay_ms` | gauge | Kalman 估计的 target_delay（§5.3.1） |
| `mc.jitter.iat_p95_ms` | histogram | 包间到达间隔 95 分位（§5.3.2 直方图导出） |

### 3.3 解码器与渲染器

| 字段 | 含义 |
|---|---|
| `mc.decode.fps_in` | 推帧速率（Annex-B AU/秒） |
| `mc.decode.fps_out` | 出帧速率（NV12 帧/秒） |
| `mc.decode.fps_emit_to_gate` | 进 Frame Validity Gate 的速率 |
| `mc.decode.fps_emit_to_render` | 通过 Gate 的速率（差额 = drop） |
| `mc.render.present_fps` | Present 调用速率 |
| `mc.render.scan_out_fps` | PresentMon 实测刷新率 |

### 3.4 SPSC 队列水位

每条队列上报 `depth / capacity / drop_count`，配合 cache-line padding 状态（启动期一次性，确认无 false-sharing）。

| 字段 | 含义 |
|---|---|
| `mc.queue.<name>.depth` | gauge，当前深度 |
| `mc.queue.<name>.capacity` | gauge，上限 |
| `mc.queue.<name>.high_water_frames` | gauge，本会话最高水位 |
| `mc.queue.<name>.drop_oldest_count` | counter，按"丢最老"策略丢弃数（§6.1 满时策略） |

---

## 4. 正确性指标（Frame Validity Gate 收口）

ADD §5.13 把六类花屏防御机制收口到 Gate；本节是 Gate 的可观测面。**正确性 metric 与延时同优先级**——CI 必看。

### 4.1 六 bit drop 分类（`mc.gate.*`）

| 字段 | 对应 bit | 触发场景 |
|---|---|---|
| `mc.gate.drop_refs_resolved_count` | `refs_resolved` | DPB FrameNum / POC gap，参考帧丢失 |
| `mc.gate.drop_params_present_count` | `params_present` | SPS/PPS（H.265 含 VPS）未缓存或不匹配 |
| `mc.gate.drop_recovery_complete_count` | `recovery_complete` | 错误恢复期未到 refresh anchor 的中间帧 |
| `mc.gate.drop_color_meta_known_count` | `color_meta_known` | VUI 不可信且启发式未锁定 |
| `mc.gate.drop_reorder_resolved_count` | `reorder_resolved` | B 帧未排序就绪 |
| `mc.gate.drop_gpu_fence_signaled_count` | `gpu_fence_signaled` | dual-bind array slice 未写完 |
| `mc.gate.last_drop_pts_ns` | gauge | 最近一次 drop 帧 PTS |
| `mc.gate.last_drop_bit` | gauge | 最近一次先丢的 bit（诊断 stats，§5.13 偏序提示） |

### 4.2 污染态生命周期

| 字段 | 类型 | 含义 |
|---|---|---|
| `mc.gate.tainted_state` | gauge | 0/1，当前是否处于污染态 |
| `mc.gate.tainted_enter_count` | counter | 进入污染态总次数（按触发源拆 tag）|
| `mc.gate.tainted_duration_ns` | histogram | 单次污染持续时间（进入 → recovery_complete） |
| `mc.gate.tainted_source` | label | 触发源 ∈ {seq_gap, fnum_gap, mft_decode_err, device_lost, adapter_switch, resize_inflight} |

### 4.3 反馈频率与恢复

| 字段 | 含义 | 期望（warm） |
|---|---|---|
| `mc.fb.pli_sent_count` | PLI 发送数 | 稳态接近 0；> 1 Hz 报警 |
| `mc.fb.nack_sent_count` | NACK 发送总数 | — |
| `mc.fb.nack_retry_p50` | 重发次数中位 | ≤2 |
| `mc.fb.nack_recovery_ratio` | 收到对应 RTX/重传 / 发出 NACK 的比例 | LAN ≥0.95 / 公网 ≥0.7 |
| `mc.fb.gdr_wait_ns` | GDR 解 freeze 等待 | 取决于编码器 recovery_window |

### 4.4 颜色矩阵决策

| 字段 | 含义 | 取值 |
|---|---|---|
| `mc.color.matrix_source` | 三级兜底中哪一级生效 | `vui` / `heuristic` / `user_override` |
| `mc.color.matrix` | 实际生效矩阵 | `bt709` / `bt601` / `bt2020` |
| `mc.color.range` | range | `limited` / `full` |
| `mc.color.hdr_downgrade` | gauge 0/1 | PQ/HLG 流 SDR 输出（§5.12 末段权衡） |

### 4.5 A/V 同步偏差

| 字段 | 含义 | 阈值（§5.9.3 收紧） |
|---|---|---|
| `mc.sync.av_offset_ns` | gauge，audio_time − video_time | `−25ms ≤ x ≤ +15ms` 立即渲染区间 |
| `mc.sync.av_drop_late_count` | counter | video PTS > audio_clock 丢帧 |
| `mc.sync.av_delay_early_count` | counter | video PTS < audio_clock 延后 |

---

## 5. 资源占用（`mc.res.*`）

### 5.1 CPU 按线程（对位 ADD §3.3）

每个 MMCSS 线程单独埋；进程总和是硬约束（ADD §1.2），单线程列只作排障辅助——不同 vendor SDK / 不同 codec / 不同分辨率下 CPU 在线程间的分布差异很大，**单线程上限是诊断警示而非加和约束**，不要求 sum 严格 ≤ 进程总和。

| 字段 | 线程 | 单线程参考上限（诊断辅助） |
|---|---|---|
| `mc.res.cpu_thread_t2_ratio` | T2 Network RX | <2%（高于此值 → 检查 URO/IM 关闭与 SO_RCVBUF）|
| `mc.res.cpu_thread_t4_ratio` | T4 Video Decode | 档1/2/3 ≤6% / 档4 ≤15%（GPU 解码档 host 侧通常远低于 6%）|
| `mc.res.cpu_thread_t5_ratio` | T5 Video Render | <3%（高于此值 → 检查 dual-bind 是否命中、shader 复杂度）|
| `mc.res.cpu_thread_t6_ratio` | T6 Audio Render | <1%（高于此值 → driver 异常或 buffer period 配错）|
| `mc.res.cpu_thread_t7_ratio` | T7 WHEP-PC | <3%（DTLS/SRTP 解密；高于此值排查 AES-NI 是否启用）|
| `mc.res.cpu_process_ratio` | **进程总和（硬约束）** | **硬解 ≤8% / 软解 ≤18%（ADD §1.2，CI 闸）** |

### 5.2 GPU

| 字段 | 含义 |
|---|---|
| `mc.res.gpu_decode_ratio` | GPU 解码引擎占用（PerfCounter / Performance Data Helper） |
| `mc.res.gpu_3d_ratio` | 3D 引擎占用（render path） |
| `mc.res.gpu_vram_used_bytes` | 本进程 VRAM 占用 |

### 5.3 内存与池

| 字段 | 含义 |
|---|---|
| `mc.res.process_rss_bytes` | 进程驻留内存 |
| `mc.res.handle_count` | 句柄数（leak 检测） |
| `mc.res.thread_count` | 线程数 |
| `mc.pool.<name>.in_use_count` | 池在用对象数（ADD §6.2 各池） |
| `mc.pool.<name>.high_water` | 池本会话最高水位 |
| `mc.pool.<name>.alloc_overflow_count` | 池满后 fallback 到 malloc 的次数（应保持 0） |

---

## 6. 解码档位与降级（`mc.decoder.*`）

ADR-015 四级降级链的可观测面。运维主要靠这组 metric 判断"为什么没走到更高档"。

| 字段 | 类型 | 取值 / 含义 |
|---|---|---|
| `mc.decoder.kind` | gauge label | ∈ {`VENDOR_SDK_NVDEC`, `VENDOR_SDK_ONEVPL`, `VENDOR_SDK_AMF`, `DXVA_DIRECT`, `MFT_HARDWARE`, `LIBCODEC`}（ADD §7.4 / hardware-decode-dependencies §7） |
| `mc.decoder.tier_target` | gauge | 1/2/3/4，理论应到的最高档 |
| `mc.decoder.tier_actual` | gauge | 实际选中的档 |
| `mc.decoder.tier_drift` | gauge | `target − actual`，>0 表示档位下沉 |
| `mc.decoder.codec` | label | `h264 / h265 / av1` |
| `mc.decoder.profile` | label | `main / main10 / high / ...` |
| `mc.decoder.resolution` | label | `1280x720 / 1920x1080 / 3840x2160` |
| `mc.decoder.cross_tier_demote_count` | counter | 跨档降级次数（按 ADD §5.6 "降级一次性原则"，跨档仅由 `mc_open` 重入式探测触发：profile 永久失支 / hardware decode unit 永久失能 / device lost 后 LUID 找回失败）|
| `mc.decoder.in_tier_reset_count` | counter | 档内复位次数（flush + 重协商，未跨档） |
| `mc.decoder.sdk_cache_hit{vendor}` | gauge 0/1 | vendor SDK redistributable 在 `%LOCALAPPDATA%\mc-player\sdk\<vendor>\` 是否命中（ADR-016） |
| `mc.decoder.b_frame_policy_active` | gauge 0/1 | B-Frame Policy 是否激活（reorder>0 检出，§5.6.4） |
| `mc.decoder.reorder_cost_ms` | gauge | B-Frame Policy 激活时的 reorder 延时代价 |
| `mc.decoder.dual_bind_active` | gauge 0/1 | dual-bind 命中（§4.3）；0 表示 fallback 到 CopySubresource |

### 6.1 Hardware Decode Component Manager（HDCM；ADR-021）

`mc.decoder.sdk_cache_hit{vendor}` 是档 1 专用 backward-compatible alias；本组 metric 是 HDCM 引入的更通用视图（覆盖类别 A/B/C/D 共 7 类组件）。详 `mc-player_hdcm_设计.md` §6。

| 字段 | 类型 | 取值 / 含义 |
|---|---|---|
| `mc.hdcm.component{type, id}.state` | gauge label | `type` ∈ {A,B,C,D}；`id` ∈ {A_NVDEC, A_oneVPL, A_AMF, B_HEVC_Ext, B_AV1_Ext, C_MediaPlayback, D_NVIDIA, D_Intel, D_AMD}；`state` ∈ {already_installed, installable, unavailable_on_this_sku, installing, install_failed, restart_pending}（hdcm 设计 §3.2）|
| `mc.hdcm.install_attempt_count{type, id, result}` | counter | `result` ∈ {started, success, user_cancelled, net_error, checksum_mismatch, authenticode_invalid, uac_denied, store_unavailable, dism_payload_missing, helper_crashed, external_redirect} |
| `mc.hdcm.last_install_duration_ms{type, id}` | gauge | 单次安装从 `INSTALLING` → `SUCCESS` / `FAILED` 耗时；类别 D 跳浏览器记 `external_redirect`，不计耗时 |
| `mc.hdcm.restart_pending{feature}` | gauge 0/1 | 类别 C DISM 启用成功但 `restart_required > 0`，等用户重启电脑（`feature=MediaPlayback` 等）|
| `mc.hdcm.driver_below_threshold{vendor}` | gauge 0/1 | 类别 D 检测当前 driver 版本 < 编译期阈值（`vendor` ∈ {NVIDIA, Intel, AMD}）|

---

## 7. 渲染管线（`mc.render.*`）

### 7.1 PresentMode 实际值

ADD §5.10.4 强调 `EXTREME` / `ULTIMATE_DCOMP` 是 best-effort，**必须**实测上报。

| 字段 | 含义 | 取值 |
|---|---|---|
| `mc.render.profile_target` | label | 启动期智能选择目标档 ∈ {COMPAT, BALANCED, EXTREME, ULTIMATE_DCOMP} |
| `mc.render.profile_actual` | label | PresentMon 实测档 ∈ {`Composed_Flip`, `Hardware_Composed_Independent_Flip`, `Hardware_Independent_Flip`, `Composed_Composition_Atlas`, `Composed_Copy_GPU`} |
| `mc.render.allow_tearing_on` | gauge 0/1 | DXGI_PRESENT_ALLOW_TEARING 实际启用（仅 sync interval=0 路径） |
| `mc.render.frame_latency_waitable_on` | gauge 0/1 | Waitable swap chain 是否激活 |
| `mc.render.dcomp_on` | gauge 0/1 | DCOMP 是否激活 |

### 7.2 DCOMP 与 Present Epoch

| 字段 | 含义 |
|---|---|
| `mc.render.dcomp_commit_count` | DCOMP `IDCompositionDevice::Commit` 调用次数（应 = Present 次数 ± 极少差）|
| `mc.render.present_epoch_id` | gauge | 单调递增 epoch id |
| `mc.render.epoch_pair_skew_count` | counter | Present 与 commit 配对失败次数（§5.10.5 不变量违反） |
| `mc.render.watchdog_redraw_count` | counter | 陈旧区域 watchdog 触发的强制 redraw 次数 |

### 7.3 显示器与 VRR

| 字段 | 含义 |
|---|---|
| `mc.render.display_refresh_hz` | 当前显示器刷新率 |
| `mc.render.vrr_enabled` | gauge 0/1，显示器 VRR 启用 |
| `mc.render.scan_out_to_present_p95_ns` | 显示器 scan-out 到下一 Present 的间隔（验 BALANCED / EXTREME 档） |
| `mc.render.tear_count` | counter | 实际 tearing 帧数（PresentMon 标 `Tearing`） |

### 7.4 Resize / 跨屏

| 字段 | 含义 |
|---|---|
| `mc.render.resize_buffers_count` | counter | ResizeBuffers 调用次数 |
| `mc.render.resize_clearstate_violation_count` | counter | ResizeBuffers 前未 ClearState 触发 `DXGI_ERROR_INVALID_CALL`（§5.10.3，应永久 0） |
| `mc.render.adapter_switch_count` | counter | 跨屏 adapter switch 触发次数（§7.3.3） |
| `mc.render.adapter_switch_duration_ns` | histogram | switch 完成时间（双 buffered transition 上限 800ms） |

---

## 8. 错误与恢复（`mc.err.*`）

| 字段 | 含义 |
|---|---|
| `mc.err.device_lost_count` | DXGI/D3D11 `DEVICE_REMOVED` 或 `DEVICE_RESET` |
| `mc.err.device_recovered_duration_ns` | histogram | device-lost → device-recovered 总耗时 |
| `mc.err.tdr_count` | counter | TDR 触发（`IMFDXGIDeviceManager::TestDevice` 失败） |
| `mc.err.network_disconnect_count` | counter | UDP/SRTP 1s 无数据 / RTCP SR 10s 无 |
| `mc.err.network_reconnect_duration_ns` | histogram | 断线 → 首个可解码帧 |
| `mc.err.audio_device_change_count` | counter | WASAPI `OnDefaultDeviceChanged` |
| `mc.err.codec_unrecoverable_count{tier}` | counter | 各档不可恢复错误次数 |
| `mc.err.factory_recreate_count` | counter | DXGI factory 重建（§7.2.2 stale factory） |

---

## 9. 埋点实施约定

### 9.1 命名规范

```
mc.<domain>.<metric>[_<unit_suffix>]
                                  └── 单位后缀（_ns / _ms / _bytes / _ratio / _count / _frames）
                  └── 具体指标名（snake_case）
       └── 业务域（e2e / stage / queue / jitter / decode / render / gate / fb / color / sync / res / pool / decoder / err / probe / firstframe）
```

label / tag 用于切片维度（`tier=1`、`vendor=NVIDIA`、`codec=h264`、`phase=warm_steady`），**不与字段名拼接**。

### 9.2 metric 类型选择

| 类型 | 适用 | 例 |
|---|---|---|
| **counter** | 单调递增计数 | `mc.gate.drop_*_count` / `mc.err.*_count` |
| **gauge** | 当前值 | `mc.jitter.depth_frames` / `mc.decoder.kind` |
| **histogram** | 分布 + 分位数 | 所有 `_ns` 时延、`_bytes` 字节大小 |
| **timer** | histogram 的语义子类，便于自动 percentile | `mc.stage.*_ns` / `mc.firstframe.*_ns` |

**禁用 mean** 上报，分位数用 t-digest / HDR Histogram 实算。

### 9.3 上报通道

| 通道 | 用途 | 频率 |
|---|---|---|
| **ETW (TraceLogging)** | 高频事件、ns 时间戳、内核流水线（QPC 直采） | 每事件即发 |
| **stats API**（mc-player 公开 API） | 应用层订阅、UI 监控、运维告警 | 1 Hz 聚合 |
| **CSV / JSON dump** | 离线诊断、CI 验证 | 测试期 / 显式触发 |

ETW 是源头，stats 是聚合。CI 测试用 ETW 原始 + 后处理；运行期 UI 用 stats API。

### 9.4 抽样策略

- **延时类（`_ns`）**：100% 采样进 histogram，用 t-digest / HDR 算法压缩，不丢任何数据点。
- **吞吐类（`_pps` / `_fps`）**：1 Hz 聚合上报，减少 ETW 写量。
- **错误类（`_count`）**：全量；错误本身就是稀有事件。
- **正确性类（`mc.gate.*` / `mc.fb.*`）**：全量 + 附带 PTS 上下文，便于事后回溯"第 X 帧为什么 drop"。

### 9.5 时钟与时间戳

- **统一用 QPC**（ADD §2 #10）；不混用 `GetTickCount` / `std::chrono::steady_clock::now()`（后者实现可能基于 QPC 但精度未保证）。
- 单次时延采集模式：`auto t0 = qpc(); ...; auto t1 = qpc(); record(t1 - t0)`，**避免** `record(qpc() - last_qpc)` 跨调用的累积误差。
- 跨线程时间戳必须同源 QPC；不允许某线程用 QPC、另一线程用 `time_t`。

### 9.6 与 Frame Validity Gate 字段联动

ADD §5.13 末段已规定"每个 bit 累计 drop 计数 + 最近一次 drop 帧 PTS + 当前是否处于污染态 + 最近一次进入污染的触发源由 stats 暴露"——本规范的 §4.1 / §4.2 即该约定的字段映射，**实现必须一一对应**，不可缺项。

---

## 10. 排障 use case 速查

每个 use case 给出"先看哪些 metric"和"这些 metric 异常意味着什么"。

### 10.1 "比 VLC 慢 +100ms"（本仓库已踩过）

| 看 | 异常表现 | 对应根因 |
|---|---|---|
| `mc.decoder.kind` | `MFT_HARDWARE` 但 `mc.decoder.dual_bind_active=0` | sync software MFT 被错当硬解（已在 ADR-015 修） |
| `mc.decoder.tier_drift` | `>0`（如 target=2 actual=4） | 档位下沉，看 `mc.probe.tier_skip_reason` |
| `mc.decoder.b_frame_policy_active` + `mc.decoder.reorder_cost_ms` | `1` + 大值 | SPS reorder>0 但用了低延时配置导致花屏，或 Policy 激活带来 +30~60 ms |
| `mc.stage.decode_actual_ns` p95 | 远超 ADD §8.1 表对应档 | 解码本身慢，看 `mc.res.gpu_decode_ratio` 是否饱和 |

### 10.2 花屏 / 陈旧区域

| 看 | 异常表现 |
|---|---|
| `mc.gate.drop_*_count` | 哪一 bit 计数高就是哪类问题 |
| `mc.gate.tainted_state` + `mc.gate.tainted_duration_ns` | 长时间不解，看 `mc.gate.tainted_source` |
| `mc.render.epoch_pair_skew_count` | >0 即 Present / DCOMP commit 配对失败 |
| `mc.render.watchdog_redraw_count` | 高频触发 = 陈旧帧普遍存在 |
| `mc.render.resize_clearstate_violation_count` | >0 即架构不变量违反（必修） |
| `mc.fb.pli_sent_count` rate | 稳态非 0 = 持续参考链断裂 |

### 10.3 首帧延时大

| 看 | 异常表现 |
|---|---|
| `mc.firstframe.caps_probe_ns` | >100ms = 探测自身慢，看哪档拖（`mc.probe.tier{1,2,3}_ns`） |
| `mc.firstframe.decoder_init_ns` | >100ms 不正常 |
| `mc.firstframe.wait_idr_ns` | 受 GOP 决定；若 `mc.fb.pli_sent_count=0` 就发 PLI |
| `mc.firstframe.whep_handshake_ns` | LAN >500ms 看 ICE / DTLS 子项（建议补充 `mc.whep.{ice,dtls,first_rtp}_ns`） |

### 10.4 丢帧 / 卡顿

| 看 | 异常表现 |
|---|---|
| `mc.queue.<name>.drop_oldest_count` | 哪条队列丢就是哪段顶不住 |
| `mc.jitter.depth_frames` 过高 | jitter 累积，下游慢 |
| `mc.tput.rtp_loss_count` | 网络包丢 |
| `mc.sync.av_drop_late_count` | A/V 同步丢视频帧（视频跟不上音频时钟） |
| `mc.res.cpu_thread_t4_ratio` | T4 解码线程饱和 |
| `mc.res.gpu_decode_ratio` | GPU 解码引擎饱和（4K / 多流） |

### 10.5 档位下沉（应到档 1/2 实到档 3/4）

| 看 | 异常表现 |
|---|---|
| `mc.probe.tier_skip_reason{tier=1}` | `sdk_missing` → ADR-016 下载面板补；`vendor_mismatch` → 多卡场景看 §7.3 选 adapter |
| `mc.probe.tier_skip_reason{tier=2}` | `profile_unsupported` → driver 老旧 |
| `mc.probe.tier_skip_reason{tier=3}` | `sync_software_only` → 正确，按 ADR-015 设计走档 4 / 上溯档 1/2 |
| `mc.decoder.sdk_cache_hit{vendor=...}` | 0 → ADR-016 面板未触发 / 用户跳过 |
| `mc.hdcm.component{type=A, id=A_<vendor>}.state` | `installable` 长期 → 用户未点击安装；`install_failed` → 看 `mc.hdcm.install_attempt_count{result=...}` 细分（net_error / checksum_mismatch / authenticode_invalid 等）|
| `mc.hdcm.component{type=B, id=B_HEVC_Ext}.state` | `unavailable_on_this_sku` → 该 SKU 无 Microsoft Store（如 IoT LTSC）；按 `hardware-decode-dependencies.md` §4.4 走 Media Feature Pack MSU 离线方案兜底 |
| `mc.hdcm.component{type=C, id=C_MediaPlayback}.state` | `restart_pending` 长期 → 用户已 enable feature 但未重启；`unavailable_on_this_sku` → base image 不含 payload，同样 §4.4 |
| `mc.hdcm.driver_below_threshold{vendor}=1` + `mc.probe.tier_skip_reason{tier=2}=profile_unsupported` | driver 老旧导致档 2 profile 不暴露 → app 内已弹"GPU driver 落后"提示（类别 D），等用户跳浏览器更新 |

### 10.6 音画不同步

| 看 | 异常表现 |
|---|---|
| `mc.sync.av_offset_ns` | 长期偏离 `[−25ms, +15ms]` |
| `mc.sync.av_drop_late_count` 与 `_delay_early_count` | 哪侧累计高就是哪侧问题 |
| 音频线程 `mc.res.cpu_thread_t6_ratio` | 接近 100% 说明 audio 渲染顶不住 |

### 10.7 网络抖动 / 公网链路差

| 看 | 异常表现 |
|---|---|
| `mc.jitter.iat_p95_ms` | 远超 1/fps，需要抬 jitter target |
| `mc.fb.nack_recovery_ratio` | <0.7 即 NACK 不见效 |
| `mc.tput.rtp_loss_count` rate | 持续高 |

---

## 11. CI / 验收闸

### 11.1 必达指标（架构硬约束，违反即 fail）

对应 CLAUDE.md 中"v1 验收三条不可让步"（无花屏 / 无陈旧层 / 无内存泄露）+ ADD §1.2 表延时约束。

| metric | 闸 | 来源 |
|---|---|---|
| `mc.gate.drop_*_count` | warm_steady 期间 = 0（color_meta_known 允许首帧后 ≤3 帧） | ADD §5.13 |
| `mc.render.resize_clearstate_violation_count` | = 0 永久 | ADD §5.10.3 |
| `mc.render.epoch_pair_skew_count` | = 0 永久 | ADD §5.10.5 |
| `mc.res.handle_count` | 稳态 ±常数（leak 检测） | CLAUDE.md 验收 |
| `mc.res.process_rss_bytes` | 稳态 ±常数 | 同上 |
| `mc.pool.<*>.alloc_overflow_count` | = 0 | ADD §6.2 |
| `mc.e2e.latency_p95_ms`（减网络/编码项后） | 按 ADD §1.2 表 4 档显示器分别约束 | ADD §8.2 |
| `mc.stage.decode_actual_p95_ns` | 按 ADD §1.2 / §8.1 档位约束 | ADD §8.1 |

### 11.2 best-effort 指标（监控不闸）

- `mc.render.profile_actual` 是否到 `Hardware_Composed_Independent_Flip`：DCOMP 升级为 best-effort（ADD §5.10.2 / ADR-008），不 fail，仅监控
- `mc.decoder.dual_bind_active`：driver 不支持时 fallback 到 CopySub 是允许的（ADD §4.3 / ADR-003），仅监控
- `mc.decoder.sdk_cache_hit`：用户首次启动可能跳过下载（ADR-016），不 fail
- `mc.hdcm.component{*}.state`：用户可选择不安装任何 HDCM 组件（ADR-021），不 fail；运维可定义"X 天 INSTALLABLE 状态长期未安装"提醒
- `mc.hdcm.restart_pending{feature}`：类别 C 安装成功但用户未重启时长期为 1，不 fail；提示而非阻断

### 11.4 Preset 与 Probe 字段（ADR-017 / ADR-018 / ADR-019 / ADR-020）

> 本组字段定义对位 `mc-player_capability_probe_设计.md` §9 与 ADD §3.5 / §7.5；落地阶段对应 plan Phase 9.0 ~ 9.5 + Phase 10。design-detail 文档（capability_probe）与 plan 引用本节字段定义；新增字段需先在本节定义再用于其他文档。

#### Preset 状态与切换

| 字段 | 类型 | 含义 | 阈值（warm_steady） |
|---|---|---|---|
| `mc.preset.active_id` | gauge enum | 当前激活的 preset ∈ {`SDI_REPLACEMENT`, `REALTIME_LAN`, `STREAMING_WIFI`, `WAN_FALLBACK`, `SAFE_MODE`} | 与 capability 匹配（capability_probe §6.2） |
| `mc.preset.bootstrap_to_active_ms` | histogram | mc_open → 第一次完整 preset apply 完成 | p95 ≤ 1500ms |
| `mc.preset.reload_event{from,to,reason}` | counter | 每次 preset 切换事件 log（ADR-020） | — |
| `mc.preset.reload_latency_ns` | histogram | reload 触发 → apply 完成 | p95 ≤ 5ms（ADD §7.5.5） |
| `mc.preset.downgrade_count` | counter | 假设破坏触发的降档次数 | — |
| `mc.preset.upgrade_count` | counter | 长期稳定试探触发的升档次数 | — |
| `mc.preset.oscillation_count` | gauge | 升降振荡计数（≥2 锁定 5min；ADR-020 / capability_probe §8.3） | warm_steady ≤ 1 |
| `mc.preset.apply_atomic_violation_count` | counter | reload 期间引发帧丢失（违反 capability_probe §8.4 原子性闸） | = 0 永久 |
| `mc.preset.apply_partial_count{subsystem, target_tier, actual_tier}` | counter | 子系统 apply graceful degrade（capability_probe §7.1）；按 6 子系统 + 目标档位 + 实际档位拆 label | warm_steady 期 0；plan Phase 9.x 渐进 unlock 阶段允许非 0 |
| `mc.preset.apply_failure_count` | counter | 整体回滚到 SAFE_MODE 兜底次数（仅当所有子系统都 BOOTSTRAP 默认仍失败） | = 0 永久 |

#### 四维 Probe 完成时间

| 字段 | 类型 | 含义 | 阈值 |
|---|---|---|---|
| `mc.probe.hardware_complete_ms` | histogram | DXGI 枚举 + CheckVideoDecoderProfile + vendor SDK 探测 | p95 ≤ 200ms |
| `mc.probe.network_complete_ms` | histogram | RTT + iat jitter + loss + link_kind 推断（依赖首 GOP） | p95 ≤ 1000ms |
| `mc.probe.encoder_complete_ms` | histogram | SDP profile-level-id + SPS VUI + 首 GOP 实测 | p95 ≤ 1000ms |
| `mc.probe.render_complete_ms` | histogram | DXGI Output6 / DCOMP / EDID 本地查询 | p95 ≤ 50ms |
| `mc.probe.network_link_kind` | gauge enum | 推断的链路类型 ∈ {LAN_SWITCHED, LAN_WIFI, WAN_WIRED, WAN_WIRELESS, UNKNOWN} | — |
| `mc.probe.encoder_source` | gauge enum | 编码器特征采集源 ∈ {SDP_PROFILE_LEVEL_ID, SPS_VUI, FIRST_GOP_MEASURED} | 生产环境 SPS_VUI 占比应 > 60% |
| `mc.probe.encoder_reorder_depth` | gauge | 实测 reorder depth | 0 / 1 / 2+ |

#### 子系统能力实装位（plan Phase 9.x 渐进 unlock）

| 字段 | 类型 | 含义 | 期望 |
|---|---|---|---|
| `mc.render.dcomp_nv12_direct_active` | gauge bool | composition swapchain `DXGI_FORMAT_NV12` + MPO 多面合成生效（plan Phase 9.1） | NVDEC + 240Hz VRR + ULTIMATE_DCOMP 路径 = 1 |
| `mc.rtcp.reduced_size_active` | gauge bool | RFC 5506 Reduced-Size RTCP 与对端协商成功（plan Phase 9.2） | 对端支持时 = 1 |
| `mc.jitter.mode` | gauge enum | jitter buffer 当前模式 ∈ {ZeroJitter, KalmanAggressive, KalmanNormal, KalmanSafe} | LAN-switched + SDI_REPLACEMENT = ZeroJitter（plan Phase 9.3） |
| `mc.jitter.target_delay_ms` | gauge | jitter target | LAN-switched + ZeroJitter ≤ 1ms |
| `mc.present.race_to_display_active` | gauge bool | race_to_display 调度激活（plan Phase 9.4，依赖 ALLOW_TEARING + VRR） | SDI_REPLACEMENT preset = 1 |
| `mc.decoder.cuda_graph_active` | gauge bool | NVDEC CUDA Graphs 提交路径（plan Phase 9.5，仅 NVDEC 路径） | 实装成功 = 1；其他档位字段不上报 |

#### 滑窗 telemetry（ADR-020 LiveReload 用）

| 字段 | 类型 | 含义 | 阈值 |
|---|---|---|---|
| `mc.net.rtt_p95_window_5s_ns` | gauge | 5s 滑窗 RTT p95 | 由 `mc.preset.active_id` 上限决定 |
| `mc.net.loss_rate_window_5s` | gauge | 5s 滑窗 loss rate | 同上 |
| `mc.net.iat_jitter_ms` | gauge | 包间到达间隔标准差（即时） | 由 link_kind 决定 |

### 11.3 告警阈值（运维监控）

| metric | warn | crit |
|---|---|---|
| `mc.fb.pli_sent_count` rate | >0.1 Hz | >1 Hz（持续参考链断裂）|
| `mc.gate.tainted_state` | 持续 >1s | 持续 >5s |
| `mc.decoder.tier_drift` | ≥1 持续 >10s | ≥2 持续 |
| `mc.err.device_lost_count` rate | ≥1 / 小时 | ≥1 / 分钟 |
| `mc.render.watchdog_redraw_count` rate | >0.1 Hz | >1 Hz |
| `mc.queue.rtp_to_jitter.drop_oldest_count` rate | >0.5 Hz（jitter 大深度，可容偶发） | >5 Hz |
| `mc.queue.{depack_to_codec, codec_to_render}.drop_oldest_count` rate | >0（极低深度队列，drop 即异常） | >0.5 Hz |
| `mc.sync.av_offset_ns` `|x|` | >50ms 持续 | >100ms 持续 |
| `mc.res.cpu_process_ratio` | 硬解 >12% / 软解 >25% | 硬解 >20% / 软解 >40% |
| `mc.preset.oscillation_count` | ≥1 持续 >5min | ≥2（已锁定 5min 禁升） |
| `mc.preset.apply_partial_count` rate | >0（单子系统降级；plan Phase 9.x 期允许）| 所有 6 子系统都降级 |
| `mc.hdcm.install_attempt_count{result=helper_crashed}` rate | ≥1 / 天 | ≥1 / 小时 |

---

## 12. 与现有文档的交叉引用

| 本规范章节 | 上游 doc 章节 |
|---|---|
| §2.1 端到端延时 | ADD §1.2 / §8.2 |
| §2.2 阶段延时 | ADD §8.1 表（逐项对应） |
| §2.3 首帧延时 | ADD §8.3 |
| §2.4 caps_probe / open | ADD §2 #11 / §5.6.1 / ADR-015 |
| §3.4 队列水位 | ADD §3.3 / §6.1 |
| §4 Frame Validity Gate | ADD §5.13 / ADR-014 |
| §4.3 PLI / NACK | ADD §5.4 / §5.5.4 / ADR-005 |
| §4.4 颜色矩阵 | ADD §5.12 |
| §4.5 A/V 同步 | ADD §5.9.3 / ITU-R BT.1359 |
| §5.1 CPU 按线程 | ADD §3.3 / §5.14 |
| §6 解码档位 | ADD §5.6 / §7.4 / ADR-015 / ADR-016 |
| §7 渲染管线 | ADD §5.10 / ADR-008 / ADR-012 |
| §8 错误恢复 | ADD §7.2 / ADR-013 |
| §11.1 必达指标 | CLAUDE.md "v1 验收三条" + ADD §1.2 |
| §11.4 Preset 与 Probe 字段 | ADR-017 ~ ADR-020 / ADD §3.5 / §7.5 / capability_probe §9 |
| §6.1 HDCM | ADR-016 / ADR-021 / hdcm 设计 §6 |

---

## 附录 A：metric 字段速查（按域）

> 完整列表见正文；本附录仅作落地索引。新增字段需先在正文给出定义 + 阈值 + 上游引用，再回填本表。

```
mc.e2e.latency_ns                    histogram      §2.1   ADD §1.2/§8.2
mc.e2e.client_internal_ns            histogram      §2.1   ADD §1.2
mc.e2e.network_one_way_ns            histogram      §2.1   ADD §5.4

mc.stage.udp_rx_ns                   timer          §2.2   ADD §8.1 行1
mc.stage.jitter_dwell_ns             timer          §2.2   ADD §8.1 行2
mc.stage.depack_ns                   timer          §2.2   ADD §8.1 行3
mc.stage.decode_alloc_ns             timer          §2.2   ADD §8.1 行4
mc.stage.decode_actual_ns            timer          §2.2   ADD §8.1 行5
mc.stage.decode_output_ns            timer          §2.2   ADD §8.1 行6
mc.stage.upload_ns                   timer          §2.2   ADD §8.1 行7
mc.stage.yuv2rgb_ns                  timer          §2.2   ADD §8.1 行8
mc.stage.present_queue_ns            timer          §2.2   ADD §8.1 行9

mc.firstframe.open_to_first_emit_ns  timer          §2.3   ADD §8.3
mc.firstframe.caps_probe_ns          timer          §2.3   ADD §1.2 #首帧
mc.firstframe.decoder_init_ns        timer          §2.3   ADD §8.3 行1
mc.firstframe.wait_idr_ns            timer          §2.3   ADD §8.3 各行
mc.firstframe.whep_handshake_ns      timer          §2.3   ADD §8.3 WHEP 行

mc.probe.tier{1,2,3}_ns              timer          §2.4   ADR-015
mc.probe.tier_selected               gauge          §2.4   ADR-015
mc.probe.tier_skip_reason{tier=N}    counter        §2.4   ADR-015

mc.queue.<name>.{depth,capacity,
  high_water_frames,
  drop_oldest_count,dwell_ns}        gauge/counter  §3.4   ADD §6.1
mc.sync.{gate_wait,fence_wait,
  present_epoch_pair}_ns             timer          §2.5   ADD §4.3 / §5.10.5 / §5.13

mc.tput.rtp_{rx_pps,rx_bytes_per_sec,
  loss_count,oo_count}               gauge/counter  §3.1   —
mc.jitter.{depth_frames,
  target_delay_ms,iat_p95_ms}        gauge/hist     §3.2   ADD §5.3
mc.decode.fps_{in,out,emit_to_gate,
  emit_to_render}                    gauge          §3.3   ADD §5.6
mc.render.{present_fps,scan_out_fps} gauge          §3.3   ADD §5.10.4

mc.gate.drop_<bit>_count             counter        §4.1   ADD §5.13
mc.gate.last_drop_{pts_ns,bit}       gauge          §4.1   ADD §5.13
mc.gate.tainted_{state,enter_count,
  duration_ns,source}                gauge/...      §4.2   ADD §5.13
mc.fb.{pli,nack}_sent_count          counter        §4.3   ADD §5.4
mc.fb.{nack_retry_p50,
  nack_recovery_ratio,gdr_wait_ns}   hist/gauge     §4.3   ADD §5.5.3 / 5.5.4
mc.color.{matrix_source,matrix,
  range,hdr_downgrade}               gauge/label    §4.4   ADD §5.12
mc.sync.av_{offset_ns,
  drop_late_count,
  delay_early_count}                 gauge/counter  §4.5   ADD §5.9.3

mc.res.cpu_thread_t{2,4,5,6,7}_ratio gauge          §5.1   ADD §3.3
mc.res.cpu_process_ratio             gauge          §5.1   ADD §1.2
mc.res.gpu_{decode,3d}_ratio         gauge          §5.2   —
mc.res.gpu_vram_used_bytes           gauge          §5.2   —
mc.res.{process_rss_bytes,
  handle_count,thread_count}         gauge          §5.3   —
mc.pool.<name>.{in_use_count,
  high_water,alloc_overflow_count}   gauge/counter  §5.3   ADD §6.2

mc.decoder.{kind,tier_target,
  tier_actual,tier_drift,
  codec,profile,resolution}          gauge/label    §6     ADD §7.4 / ADR-015
mc.decoder.{cross_tier_demote_count,
  in_tier_reset_count}               counter        §6     ADD §5.6.5
mc.decoder.sdk_cache_hit{vendor}     gauge          §6     ADR-016
mc.decoder.b_frame_policy_active     gauge          §6     ADD §5.6.4
mc.decoder.reorder_cost_ms           gauge          §6     ADD §5.6.4
mc.decoder.dual_bind_active          gauge          §6     ADD §4.3 / ADR-003

mc.render.profile_{target,actual}    label          §7.1   ADD §5.10.2/4 / ADR-012
mc.render.{allow_tearing_on,
  frame_latency_waitable_on,
  dcomp_on}                          gauge          §7.1   ADD §5.10.1
mc.render.{dcomp_commit_count,
  present_epoch_id,
  epoch_pair_skew_count,
  watchdog_redraw_count}             counter/gauge  §7.2   ADD §5.10.5
mc.render.{display_refresh_hz,
  vrr_enabled,
  scan_out_to_present_p95_ns,
  tear_count}                        gauge/hist     §7.3   ADD §5.10
mc.render.{resize_buffers_count,
  resize_clearstate_violation_count,
  adapter_switch_count,
  adapter_switch_duration_ns}        counter/hist   §7.4   ADD §5.10.3 / §7.3.3

mc.err.{device_lost_count,
  device_recovered_duration_ns,
  tdr_count,
  network_disconnect_count,
  network_reconnect_duration_ns,
  audio_device_change_count,
  codec_unrecoverable_count{tier},
  factory_recreate_count}            counter/hist   §8     ADD §7.2

mc.hdcm.component{type,id}.state     gauge label    §6.1   ADR-021 / hdcm §6
mc.hdcm.install_attempt_count
  {type,id,result}                   counter        §6.1   ADR-021 / hdcm §6
mc.hdcm.last_install_duration_ms
  {type,id}                          gauge          §6.1   ADR-021 / hdcm §6
mc.hdcm.restart_pending{feature}     gauge          §6.1   ADR-021 / hdcm §6
mc.hdcm.driver_below_threshold
  {vendor}                           gauge          §6.1   ADR-021 / hdcm §6

mc.preset.active_id                  gauge enum     §11.4  ADR-017 / capability_probe §6
mc.preset.bootstrap_to_active_ms     histogram      §11.4  capability_probe §9
mc.preset.reload_event{from,to,
  reason}                            counter        §11.4  ADR-020 / capability_probe §8
mc.preset.reload_latency_ns          histogram      §11.4  ADD §7.5.5
mc.preset.{downgrade_count,
  upgrade_count,
  oscillation_count}                 counter/gauge  §11.4  ADR-020 / capability_probe §8.3
mc.preset.apply_atomic_violation_count counter      §11.4  capability_probe §8.4
mc.preset.apply_partial_count
  {subsystem,target_tier,
   actual_tier}                      counter        §11.4  capability_probe §7.1
mc.preset.apply_failure_count        counter        §11.4  capability_probe §7.1

mc.probe.hardware_complete_ms        histogram      §11.4  capability_probe §3 / §9
mc.probe.network_complete_ms         histogram      §11.4  capability_probe §4 / §9
mc.probe.encoder_complete_ms         histogram      §11.4  capability_probe §5 / §9
mc.probe.render_complete_ms          histogram      §11.4  capability_probe §3.5 / §9
mc.probe.network_link_kind           gauge enum     §11.4  ADR-018 / ADD §7.5.2
mc.probe.encoder_source              gauge enum     §11.4  ADR-019 / ADD §7.5.3
mc.probe.encoder_reorder_depth       gauge          §11.4  ADD §7.5.3

mc.net.rtt_p95_window_5s_ns          gauge          §11.4  ADR-018 / capability_probe §4.3
mc.net.loss_rate_window_5s           gauge          §11.4  同上
mc.net.iat_jitter_ms                 gauge          §11.4  同上

mc.jitter.mode                       gauge enum     §11.4  capability_probe §6.1 jitter_mode 行
mc.render.dcomp_nv12_direct_active   gauge bool     §11.4  plan Phase 9.1 / 子目标 1
mc.rtcp.reduced_size_active          gauge bool     §11.4  plan Phase 9.2 / 子目标 2
mc.present.race_to_display_active    gauge bool     §11.4  plan Phase 9.4 / 子目标 4
mc.decoder.cuda_graph_active         gauge bool     §11.4  plan Phase 9.5 / 子目标 5
```

---

## 附录 B：实施迭代建议（落地顺序）

代码实施时按以下顺序埋点，按 ADR-015 / Frame Validity Gate 等架构落地节奏对齐：

1. **基础时间戳与 ETW Provider**：`mc.stage.*` 九段 timer + QPC 统一时钟，先用 TraceLogging 直采。
2. **解码档位**：`mc.decoder.{kind, tier_*, *_count}` + `mc.probe.*`——选档与降级是头号排障入口。
3. **Frame Validity Gate**：`mc.gate.*` 全集——正确性 metric 与延时同优先级，不能落后。
4. **队列水位**：`mc.queue.*`——SPSC 队列饱和是丢帧最常见原因。
5. **首帧 / WHEP 握手**：`mc.firstframe.*` + WHEP 子项。
6. **资源占用**：`mc.res.*`——CPU/GPU/内存上报。
7. **渲染管线 PresentMode**：`mc.render.*` + PresentMon 集成。
8. **stats API 公开**：聚合 ETW 到 1 Hz 接口给 UI / 运维。
9. **CI 性能闸**：把 §11.1 必达指标接入 pipeline。

每步落地一组 metric 后，先在 ADR-015 已实装档（当前档 3 MFT + 档 4 libcodec）上跑通验证回路，再随档 1 / 档 2 实装扩展该 metric 的覆盖维度。
