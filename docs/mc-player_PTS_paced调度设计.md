# mc-player — PTS-paced 时序调度 + B 帧流畅播放设计

| 项目 | 内容 |
|---|---|
| 文档类型 | 技术设计(design-detail)|
| 上游决策 | ADR-014(Frame Validity Gate + Present Epoch)|
| 上游原理 | `mc-player_架构设计文档_v3.0.md` §3.3(线程视图) / §5.10(Render) / §8.4(延时预算)|
| 下游 roadmap | `plan/mc-player_重构方案.md` Phase 5(已完成 commit `09e1ad6`)|
| 关联代码 | `media/video_clock.{h,cpp}` / `media/render_d3d11.cpp` / `media/codec_dxva_video.cpp` / `media/jitter_buffer_video.cpp` |
| 不在范围 | RTCP-SR 拟合 server clock(下个 plan)/ audio master 接入 / vlc_clock prev_contexts 历史链 |

> 本文是 ADD §5.10 + §8.4 的展开,把"如何让带 B 帧 RTSP 流流畅播放"从原理→数据流→各组件协同写清楚。当本文与 ADR / ADD 冲突,以 ADR / ADD 为准、本文同步修正。

---

## 1. 设计动机

### 1.1 问题:RTSP H.264 with B 帧 + GOP burst 的双重挑战

源:`rtsp://192.168.1.171/live/11`,720p H.264 main profile,30 fps,GOP ≈ 30 帧,IBBBP 结构(reorder=2),server PLAY 后 burst 推 ~1.8s 累积内容再进入稳态。

观察到的具体问题(Phase 4 之前):
- **B 帧倒序解码 vs 升序显示**:codec 收到的 RTP 包顺序 = decode order(I/P 在 B 之前),但 display order 必须按 POC 升序。直接按 codec 输出顺序 present → 画面来回跳动。
- **GOP 边界 IDR 100ms decode starve**:IDR 帧解码时间 ~100ms(vs P/B 5-10ms),期间 codec 不 emit。如果 T5 端 backlog 不足 ~3 帧,IDR 期间 starve → watchdog 触发 redraw → 画面规律性卡顿。
- **codec emit 是 GOP-paced burst pattern**:IDR 解完后 reorder buffer drain 多帧 burst → instant_coeff 在 0.9~1.4 跳变,VLC 风格 EMA 永远不收敛(实测 reset_rate 50%)。
- **server burst 推流 startup 1.8s**:第 N 帧的 RTP arrival 远早于其 source PTS 推进时刻 → 用 first_arrival 锚 + source PTS 节奏推进 deadline 时,稳态 e2e = present - arrival 永远累积 ~1.8s 偏移(virtual latency)。

### 1.2 反复试错排除掉的方向

| 尝试方向 | 失败原因 |
|---|---|
| EMA coeff 学频率比(对齐 VLC `master_update_coeff`)| codec emit 是 burst,instant_coeff 跳变 EMA 不收敛 |
| last_present 为 anchor + source PTS delta 推进 | GOP burst 时 anchor 跟 codec emit 时刻漂移 |
| Phase 4 SyncInterval=2 + wait_until 双重等待 | T5 ≈ 62ms/帧 慢于 codec 33ms/帧 → backlog 累积 |
| `kPendingQueueCap = 12` drop_oldest 限 backlog | 丢帧破坏 PTS 序列让 deadline 误差,反而引入跳动 |
| frame_count locked deadline(N × 33ms 推进)| 仍按 first_arrival 锚,e2e 没改善 |

### 1.3 最终原理:三层 PTS-paced 节流 + first anchor 永久持有

```
[T2 jitter buffer]      包级 reorder + NACK,有序立即 emit(无 PTS 节奏)
        ↓
[T4 codec_dxva_video]   decode → reorder buffer(POC 升序)→ emit
                        【新】emit 前 PTS-paced sleep 把 burst 平滑成 source PTS 节奏
        ↓
[T5 render_d3d11]       master_update + convert_to_system + wait_until
                        first anchor 永久持有,deadline 严格按 source PTS 推进
        ↓
[swap_chain.present]    SyncInterval=0 + tearing 兜底,GPU 不锁帧
```

**关键认知**:
- source PTS 严格 30 fps,是唯一稳定的"节奏来源"(server 实时编码,server-side capture 时刻 = wall clock paced);
- player 端任何"自己造节奏"的尝试(frame_count locked / GPU vsync 锁 / 自己 EMA)都会与 source PTS 节奏发生冲突;
- 三层节流的统一目标:**让最终输出严格匹配 source PTS 节奏,且不在过程中调整 PTS**;
- B 帧由 codec 端 reorder buffer 解决,T5/render 不需要懂 B 帧;
- e2e = present - arrival 数字偏大主要是 server-side burst 推流的"虚延迟",真实客户端延迟 ≈ jitter(30) + decode(100) + render buffer(100) ≈ 230 ms 与视觉流畅一致。

---

## 2. 数据流总览(从 RTP 到 present)

```
┌───────────────────────────────────────────────────────────────┐
│ T2 RX (TIME_CRITICAL,P-core)                                  │
│   RTP packet → arrival_qpc_ns = QPC RX 戳                     │
│   ts_rtsp_udp.cpp:446 / ts_rtsp_tcp.cpp:442                   │
└─────────────────────────────┬─────────────────────────────────┘
                              ↓
┌───────────────────────────────────────────────────────────────┐
│ T2 jitter_buffer_video                                        │
│   ring=256 / dwell=30ms / NACK 限频 50ms                      │
│   包级 ext-seq reorder + drain consecutive                    │
│   有序立即 emit 给 depack(无 PTS 节奏)                       │
│   jitter_buffer_video.cpp                                     │
└─────────────────────────────┬─────────────────────────────────┘
                              ↓
┌───────────────────────────────────────────────────────────────┐
│ T2 depack_h264                                                │
│   RTP packet → AU(NAL units 拼接 + Annex-B start code)        │
│   AU.arrival_qpc = first packet QPC 透传                      │
│   depack_h264.cpp:436                                         │
└─────────────────────────────┬─────────────────────────────────┘
                              ↓
┌───────────────────────────────────────────────────────────────┐
│ T4 codec_dxva_video (Playback MMCSS, P-core)                  │
│   on_au_h264 → DXVA decode(IDR 100ms / P,B 5-10ms)            │
│   reorder buffer(`h264_delayed[]` POC 升序)                  │
│   select_and_emit_h264:                                       │
│     1. 选最小 POC + IDR barrier 检查                          │
│     2. PTS 单调性自检                                         │
│  ★ 3. 【新 Phase 5】emit pace sleep 到 expected_emit_qpc      │
│     4. cfg.emit(VideoFrame) → controller → renderer.post_frame │
│   codec_dxva_video.cpp:1196-1310                              │
└─────────────────────────────┬─────────────────────────────────┘
                              ↓
┌───────────────────────────────────────────────────────────────┐
│ T5 render_d3d11 (Playback MMCSS, P-core)                      │
│   pending_queue 入队(cap=64,wake_event signal)               │
│   T5 主循环 pop_front 按 PTS 顺序消费:                        │
│     1. master_update(arrival_qpc, pts) — bootstrap 锚 first   │
│     2. expected_arrival = first_arrival + (pts-first_pts)*1us │
│     3. deadline = expected_arrival + 100ms (kRenderTargetLat) │
│  ★ 4. wait_until(deadline) — cv.wait_for 可被 wake/stop 中断  │
│     5. last_good 更新(IDR 才更新,防 partial-decode 污染)     │
│     6. render_locked → swap_chain.present(0, tearing)         │
│   render_d3d11.cpp:render_thread_loop                         │
└───────────────────────────────────────────────────────────────┘
```

---

## 3. 三层节流设计

每层职责正交,组合起来覆盖完整时序问题。

### 3.1 Jitter buffer(T2 包级)

**目标**:吸收 RTP 包级乱序 + 网络抖动,触发 NACK 重传。**不做** PTS 节奏控制。

**关键参数** (`jitter_buffer_video.cpp`):
- `kRingCap = 256`(power-of-2,~430ms @ 600pps)
- `dwell_us = 30'000`(LAN 实测 reorder 95% 在 5-15ms,30ms 留 2x 余量)
- `nack_min_interval_us = 50'000`(同 PID 最少 50ms 重发,防 NACK 风暴)

**算法**:
- 16-bit RTP seq → 32-bit ext-seq(处理每 13.6h wrap-around);
- 维护 `next_expected_seq` 滑窗,有序就 drain emit;
- gap 超 dwell 触发 sweep_expired(放弃 + loss callback);
- 检测 missing seq 用 RTCP NACK PID+BLP 一次 16 个。

**该层不变量**:
- ring 容量足够覆盖 GOP burst(单 GOP IDR ≈ 20-60 packets,稳态 30 fps 流也仅 600 pps);
- emit 顺序严格 = ext-seq 升序(下游 depack 假设);
- AU.arrival_qpc 透传不丢失。

### 3.2 Codec emit PTS-paced 节流(T4 帧级,Phase 5 新增)

**目标**:把 IDR 解完后 reorder buffer drain 的 burst emit 平滑成严格 source PTS 节奏,不让下游 T5 看到 burst。

**位置**:`codec_dxva_video.cpp` 的 `select_and_emit_h264`,在 `cfg.emit(...)` 调用之前(line 1278-1306)。

**算法**:
```cpp
// 第一次 emit 锚定
if (h264_emit_pace_first_pts_us == INT64_MIN) {
    h264_emit_pace_first_pts_us = cur_pts;
    h264_emit_pace_first_qpc_ns = pal::Clock::now_ns();
} else {
    expected_qpc = first_qpc + (cur_pts - first_pts) * 1000;
    wait_ns = expected_qpc - now_qpc;
    if (wait_ns > 0 && wait_ns < 5'000'000'000LL) {
        std::this_thread::sleep_for(nanoseconds(wait_ns));
    } else if (wait_ns >= 5e9) {
        // PTS jump / wrap 重锚
    }
    // wait_ns <= 0 立即 emit(GOP IDR 100ms decode starve 后 catch-up)
}
```

**为什么节流位置选在 codec emit 端而非 T5 端**:
- codec decode 线程 wait 期间不影响 RTP 收包(在 T2)和 jitter buffer drain(在 T2);
- delayed reorder 队列容量足够吸收一个 GOP 的积压(典型 < 40 帧 @ reorder=2);
- T5 端 backlog 自然收敛到 `buffer / source_period ≈ 3 帧`,**queue_after 实测从 21 收敛到 1**;
- T5 端不再处理 burst → 简化 T5 调度逻辑。

**关键不变量**:
- emit 顺序 = display order(reorder buffer 已保证),sleep 不改变顺序;
- catch-up 路径(wait_ns ≤ 0)立即 emit,不阻塞 IDR decode 后的 backlog 消化;
- reset() 时清 anchor,device-lost / flush 重启后下次 emit 重锚。

### 3.3 T5 wait_until(渲染级)

**目标**:严格按 source PTS 节奏 present,GPU 不锁帧避免与应用层 wait_until 双重等待。

**位置**:`render_d3d11.cpp::render_thread_loop` T5 主循环。

**算法**(对齐 VLC `vlc_clock_Wait`):
```cpp
// pop frame from pending_queue (cap=64)
master_update(arrival_qpc, pts);                          // bootstrap or no-op
expected_arrival = video_clock.convert_to_system(pts);    // first anchor 永久
deadline = expected_arrival + kRenderTargetLatencyNs;     // +100ms client buffer
video_clock.wait_until(deadline);                         // cv.wait_for 可被中断
swap_chain.present(0, tearing);                           // SyncInterval=0 + tearing 兜底
```

**`kPendingQueueCap = 64` 设计**:
- 大 cap 容纳 startup burst(server PLAY 后 ~1.8s burst 推流期间 codec 同步 burst emit);
- 稳态 backlog 受 PTS-paced wait 严格锁定 = `buffer / frame_period ≈ 3 帧`,与 cap 大小**无关**;
- 不靠 cap 限 backlog(用户原则:`只调整 cap = 12 原理上不对`)。

**`SyncInterval = 0` + tearing fallback**:
- DXGI 限制:SyncInterval > 0 与 ALLOW_TEARING 互斥;
- 应用层 wait_until 已严格按 source PTS 节奏调度,GPU 锁帧多余;
- Phase 4 SyncInterval=2 实测让 T5 总周期 ≈ 62ms/帧 慢于 codec 33ms/帧 → backlog 累积。

---

## 4. video_clock 时序锚定(对齐 VLC `vlc_clock` 简化版)

### 4.1 First anchor 永久持有(关键设计选择)

```cpp
// bootstrap 第一次 master_update 时锚定
ctx_.start_pts_us = pts_us;          // source 第一帧 PTS
ctx_.start_qpc_ns = arrival_qpc_ns;  // source 第一帧 RTP first packet RX 戳
ctx_.coeff = 1.0;                    // 固定不学,见 §4.3
```

后续每次 `convert_to_system(pts)`:
```cpp
return ctx_.start_qpc_ns + (pts - ctx_.start_pts_us) * 1000;  // ns/us
```

**为什么用 first anchor 永久持有而不是 last_present anchor**:

| 维度 | first anchor 永久 | last_present anchor |
|---|---|---|
| GOP burst 期 | deadline 严格按 source PTS 推进,与 codec emit 时刻无关 | anchor 跟 codec emit 时刻漂移,deadline 不稳定 |
| 长期累积 | source/QPC 频率差 < 100ppm,30s < 3ms,无感 | 累积单帧抖动会放大 |
| 实现复杂度 | 一次 anchor + 简单加法 | 每帧重算 |
| discontinuity 处理 | abs(pts-last) > 200ms 重锚 | 自动滑动,但不能区分 burst vs seek |

### 4.2 Master / Slave / Wait 三个核心 API

**`master_update(arrival_qpc_ns, pts_us, rate)`** — `video_clock.cpp:72-171`:
- 第一次:bootstrap 锚 (first_pts, first_arrival),coeff=1.0;
- 之后:max-monotonic 维护 last_pts/last_qpc(给 stats 和 discontinuity 检测,**不动 anchor**);
- abs(pts_delta) > 200ms 重锚(seek / RTP TS wrap / 编码器异常);
- 周期 log + atomic stats 暴露给 controller。

**`convert_to_system(pts_us, now_qpc_fallback, rate)`** — `video_clock.cpp:173-191`:
- 未 bootstrap → 返回 now_qpc_fallback(立即 present);
- 已 bootstrap → `start_qpc + (pts - start_pts) * 1000`。

**`wait_until(deadline_qpc_ns)`** — `video_clock.cpp:193-226`:
- `cv_.wait_for(deadline - now)` 用 `std::condition_variable` + `std::mutex`;
- 谓词 = `stop_.load()` 让 stop / wake_all 即时中断;
- deadline ≤ now 立即返回 true(已过期);
- drift_ns 累积到 histogram(`mc.video_clock.drift_abs_ns`)供观测 wait 精度。

### 4.3 简化为固定 coeff = 1.0(为什么不学 EMA)

VLC `master_update_coeff`(`vlc-master/src/clock/clock.c`)三步走:
1. `instant_coeff = system_diff / stream_diff * rate`;
2. EMA `Average` range=10 平滑;
3. COEFF_THRESHOLD=0.2 异常重置。

**前提条件**:master 时序源严格 wall-clock 单调(VLC 默认 master = audio playback,audio device 严格按 wall clock 写入)。

**我们的实际情况**:
- 没有 audio path,video 自己当 master;
- master_update 输入 = (RTP arrival_qpc, pts);
- RTP arrival 是 decode order(B 帧的 RTP 在 P 帧 RTP 之后到达);
- pts 是 display order(reorder 后);
- B 帧 PTS < 前一 P 帧 PTS,但 B 帧 RTP 后到达 → instant_coeff 在 frame-level 出现负值 / 大幅振荡;
- EMA 永远不收敛,COEFF_THRESHOLD 频繁触发 reset(实测 reset_rate ≈ 50%)。

**简化方案**:
- 固定 coeff = 1.0,假设 source/QPC 频率差忽略不计(< 100ppm);
- 永远 anchor 在 first 帧,不重算 offset,deadline 严格按 source PTS 推进;
- 长期累积超 200ms 由 discontinuity 路径重 anchor;
- `video_clock.cpp:133-152` 的 `master_update` 正常路径仅 max-monotonic 维护 last_pts/last_qpc,不动 anchor。

升级路径见 `video_clock.h` 文件头注释 — Phase 3+ 接 RTCP-SR ntp/rtp_ts 拟合 + audio master + slave on_update 反馈。

---

## 5. B 帧处理(为什么这套架构正确)

### 5.1 B 帧的核心问题

H.264 B 帧依赖前后双向参考帧(I/P 在它之前 decode,但 display 在它之前/之间)。例:

```
解码顺序(RTP arrival): I0  P3  B1  B2  P6  B4  B5  ...
显示顺序(POC 升序):    I0  B1  B2  P3  B4  B5  P6  ...
PTS 单调升序:          ✓
```

如果按 codec 输出顺序直接 present,画面顺序错乱。

### 5.2 解决方案:codec 端 reorder buffer

**位置**:`codec_dxva_video.cpp::select_and_emit_h264`(line 1196-1283)。

**算法**(对齐 ffmpeg `h264_select_output_frame`):
1. `h264_delayed[]` 存放已解码但等待 reorder 输出的帧;
2. 扫描找最小 POC,但 IDR 作为 barrier 中断扫描(旧 GOP 必先 emit);
3. 决策:`out_of_order || over_buffer(size > reorder_size)` → emit;否则等更多帧;
4. emit 时按 POC 升序 ⇔ 在 progressive frame_mbs_only=1 流中 PTS 严格升序;
5. 跨 GOP IDR 重置 `next_output_poc = INT_MIN`(POC sequence 跨 GOP 重置回 0+)。

**reorder buffer 容量**:从 SPS `bitstream_restriction.max_num_reorder_frames` 解析(实测此源 reorder=2),容量 = 2 帧足够覆盖 IBBBP 模式。

### 5.3 PTS 单调性自检

emit 时比对 `h264_emit_last_pts_us`,倒退即 warn 上报 + 计入 `h264_emit_pts_violations`。

**理论保证**:reorder 输出按 POC 升序 ⇔ progressive frame_mbs_only=1 流中 POC 升序 = PTS 升序 = display 升序。倒退即:
- server PLI 重置 RTP TS;
- 32-bit RTP TS 每 13h wrap;
- 解码失败 frame 携带脏 PTS;
- 跨 GOP 边界(已被 next_output_poc 重置处理)。

### 5.4 IDR + reserved_anchor_slot 防 watchdog redraw 污染

**位置**:`codec_dxva_video.cpp:1275-1281`。

每次 IDR emit 时:
```cpp
reserved_anchor_slot = d.dpb_slot;
// alloc_dpb_slot 永远跳过该 slot,
// last_good 长期保护点不会被新 P 帧覆盖
```

**为什么必要**:
- T5 维护 `last_good` 用于 watchdog redraw(无新帧时显示最后一帧);
- `last_good` 持有 `(dpb_tex, slice=anchor_slot)` 引用 — 是 zero-copy SRV 路径;
- 如果 alloc_dpb_slot 选 victim 时不跳过 anchor_slot,driver 写新帧会污染该 slice → watchdog redraw 显示乱码。

### 5.5 T5 端不需要懂 B 帧

由于 codec 端已经按 display order emit,T5 端 `pending_queue.pop_front()` 就是 display 顺序:
- PTS 严格单调升序;
- B 帧 / P 帧 / IDR 帧的处理路径完全相同(除了 last_good 只对 IDR 更新);
- video_clock 用 first anchor + (pts - first_pts) 推算 deadline,B 帧 PTS 一样可以正确推算。

---

## 6. 关键参数总览

| 参数 | 值 | 位置 | 设计依据 |
|---|---|---|---|
| `kRingCap` | 256 | jitter_buffer_video.cpp | ~430ms @ 600pps,覆盖 burst loss + NACK 重传 window |
| `dwell_us` | 30'000 | jitter_buffer_video.cpp | LAN reorder 实测 95% 在 5-15ms,2x 余量 |
| `nack_min_interval_us` | 50'000 | jitter_buffer_video.cpp | 同 PID 最少 50ms 重发,防 NACK 风暴 |
| `kRenderTargetLatencyNs` | 100'000'000 | video_clock.h | IDR decode 100ms 期间 codec emit starve buffer |
| `kDiscontinuityUs` | 200'000 | video_clock.h | 30fps 33-67ms,3x 余量,区分正常 PTS delta vs seek |
| `kCoeffThreshold` | 0.2 | video_clock.h | 对齐 VLC COEFF_THRESHOLD(本实现固定 1.0 不用)|
| `Average::kRange` | 10 | video_clock.h | 对齐 VLC EMA range(本实现固定 1.0 不用)|
| `kPendingQueueCap` | 64 | render_d3d11.cpp | 吸收 startup burst,稳态 backlog 由 PTS-paced wait 锁 |
| `kWatchdogPeriodsThreshold` | 3 | render_d3d11.cpp | 3x frame_period 未推进强制 redraw(ADD §5.10.5)|

---

## 7. 实测验证

### 7.1 测试条件

- 流:`rtsp://192.168.1.171/live/11`,720p H.264 main profile,30 fps,reorder=2;
- 硬件:Intel UHD 730(集显,DXVA-direct);
- OS:Windows 11 IoT Enterprise LTSC 2024;
- 测试时长:30s steady-state。

### 7.2 实测数据(commit `09e1ad6` Phase 5)

| 指标 | 值 | 期望 | 通过 |
|---|---|---|---|
| `presents` | 30/s 稳定 | = source 30 fps | ✓ |
| `queue_after`(T5 backlog 稳态)| 1 帧 | ≤ buffer/period ≈ 3 帧 | ✓ |
| `coeff` | 1.000 | 固定 | ✓ |
| `discont_count` | 0 | 0(稳态)| ✓ |
| `coeff_reset_count` | 0 | 0(本实现不学)| ✓ |
| `pending_drops` | 0 | 0(cap=64 容纳 startup burst)| ✓ |
| `mc.gate.drop_*_count` | 0 | 0(warm_steady,Frame Validity Gate)| ✓ |
| 视觉 | 流畅,GOP 边界不跳动 | 用户验证 | ✓ |
| `e2e P50` | 1.79s | 见 §7.3 | ⚠ |

### 7.3 e2e 数字偏大的解释(virtual latency,非真实延迟)

**实测**:`e2e P50 = 1.79s`。

**根因**:RTSP server 在 PLAY 响应后 burst 推送 ~1.8s 累积内容(RTSP live server 常见行为),frame.arrival_qpc 锚到 burst 起点。后续 present 严格按 source PTS 节奏推进 → e2e = present - arrival 永久累积 ~1.8s 偏移。

**真实客户端延迟拆解**:
```
真实延迟 = jitter buffer dwell + decode + render buffer + render
         = 30 + 100 + 100 + 5
         ≈ 235 ms
```

视觉感受 ≈ 235ms,与 e2e 数字 1.79s 不一致。

**降低 e2e 数字的路径**:接 RTCP-SR 拟合 server clock,`e2e_real = present_qpc - server_capture_estimated_qpc`,跳过 burst 推流的虚延迟(`video_clock.h` 文件头 Phase 3+ 升级路径)。

---

## 8. 关键不变量(代码级断言)

1. **Source PTS 单调性**:codec reorder buffer 输出 PTS 严格升序(progressive frame_mbs_only=1 流);倒退即 `h264_emit_pts_violations` 计数 + warn。
2. **三层节流不调整 PTS**:jitter / codec emit pace / T5 wait_until 都是按 source PTS 计算 expected,不重写 frame.pts_us。
3. **first anchor 永久持有**:除 `reset()` / discontinuity 外,`ctx_.start_pts_us` / `ctx_.start_qpc_ns` 不变。
4. **VideoClock 单 mutex**:`master_update` / `convert_to_system` / `wait_until` / `reset` 全互斥,共享 `mu_`。
5. **wait_until 可中断**:`request_stop()` / `wake_all()` / `reset()` 通过 `cv_.notify_all()` 立即让 wait 返回。
6. **last_good 仅 IDR 更新**:`render_d3d11.cpp` T5 主循环 `if (frame.is_keyframe || !has_last_good)` 才覆盖,防 partial-decode 帧污染 watchdog redraw。
7. **anchor reserved slot**:codec IDR emit 时 `reserved_anchor_slot = d.dpb_slot`,`alloc_dpb_slot` 永远跳过该 slot,直到下一 IDR。
8. **SyncInterval 与 ALLOW_TEARING 互斥**:`swap_chain.present` 只在 `sync_interval == 0` 时设 `DXGI_PRESENT_ALLOW_TEARING`(DXGI 限制)。

---

## 9. 已知局限 + 升级路径

### 9.1 已知局限

- **e2e 数字偏大(virtual latency)**:见 §7.3,需 RTCP-SR 拟合 server clock。
- **video_clock 没有 audio master**:video 自己 master 简化为固定 coeff=1.0,无 audio sync 反馈。
- **没有 prev_contexts 历史链**:无 seek-back 场景,VLC `prev_contexts` 简化未实装。
- **discontinuity 阈值 200ms 单一**:不区分 seek vs RTP wrap vs server PLI 重置。
- **codec emit pace sleep 不可中断**:`std::this_thread::sleep_for` 不响应 reset / device-lost,最坏 wait 一帧期(33ms)。

### 9.2 升级路径(参考 `video_clock.h` 文件头注释)

| 升级项 | 适用场景 | 当前优先级 |
|---|---|---|
| RTCP-SR ntp/rtp_ts 拟合 server clock | 真实 e2e 延迟测量 + 跨流时钟同步 | 中(下个 plan)|
| audio master + slave on_update 反馈 | 接入 audio path 后的 A/V sync | 低(audio path 未实装)|
| `prev_contexts` 历史链 | seek-back 后从历史 anchor 恢复 | 低(无 seek 需求)|
| EMA coeff 学 source/QPC 频率差 | 长期跨小时播放精度 | 低(<100ppm 无感)|
| codec emit pace 用 cv 替代 sleep_for | reset / device-lost 即时响应 | 低(33ms 最坏可接受)|

---

## 10. 修订历史

| 日期 | commit | 内容 |
|---|---|---|
| 2026-05-09 | `09e1ad6` | Phase 5 落地:codec emit PTS-paced + T5 纯 PTS-based wait_until |
| 2026-05-09 | `97f2060` | Phase 4 SyncInterval=2 frame doubling(被 Phase 5 替代)|
| 2026-05-09 | `c86105f` | Phase 1 jitter buffer + RTCP NACK + video_clock 雏形 |
