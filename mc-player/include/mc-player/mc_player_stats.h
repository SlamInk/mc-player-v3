/*
 * mc-player public C ABI — runtime stats（ADD §5.13 + §5.10.4 + 性能量度规范 §11.4）。
 *
 * Stats struct 与 mc_event 同样遵守 struct_size + struct_version 演进契约。
 * 字段含义以 ADD §5.13 Frame Validity Gate / §5.10.4 PresentMon 验证 / §5.6.4 B-Frame Policy
 * + 性能量度规范 §11 各域 为准。
 *
 * ABI 版本:
 *   v1: 初版（截至 plan Phase 0 之前）
 *   v2: plan Phase 0 加入 §2.2 9 段 timer + §3.4 队列水位 + §11.4 Preset/Probe/HDCM 字段
 *
 * 演进契约: 新字段一律加在结构体末尾;v1 调用方按 sizeof(v1) 调用,库内按
 * min(in.struct_size, sizeof(mc_stats_t)) 填字段——后段 v2 字段对 v1 调用方透明。
 */

#ifndef MC_PLAYER_STATS_H_
#define MC_PLAYER_STATS_H_

#include "mc_player_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 历史 alias；权威版本号在 mc_player.h::MC_STATS_VERSION（Phase 0 起 = 3，含 v2 字段+
 * Phase 1 mc_decoder_kind_t enum 数值变更）。新代码统一用 MC_STATS_VERSION。 */
#define MC_STATS_STRUCT_VERSION 3u

typedef enum mc_present_mode_e {
    MC_PRESENT_MODE_UNKNOWN                     = 0,
    MC_PRESENT_MODE_COMPOSED_FLIP               = 1,
    MC_PRESENT_MODE_HARDWARE_INDEPENDENT_FLIP   = 2,
    MC_PRESENT_MODE_HW_COMPOSED_INDEPENDENT_FLIP= 3,
    MC_PRESENT_MODE_TEARING                     = 4
} mc_present_mode_t;

/* Preset id（性能量度规范 §11.4 / capability_probe §6）。 */
typedef enum mc_preset_id_e {
    MC_PRESET_NONE              = 0,
    MC_PRESET_SDI_REPLACEMENT   = 1,
    MC_PRESET_REALTIME_LAN      = 2,
    MC_PRESET_STREAMING_WIFI    = 3,
    MC_PRESET_WAN_FALLBACK      = 4,
    MC_PRESET_SAFE_MODE         = 5
} mc_preset_id_t;

/* Jitter buffer mode（capability_probe §6.1 / 性能量度规范 §11.4）。 */
typedef enum mc_jitter_mode_e {
    MC_JITTER_MODE_UNKNOWN          = 0,
    MC_JITTER_MODE_ZERO_JITTER      = 1,    /* SDI_REPLACEMENT preset，target ≤1ms */
    MC_JITTER_MODE_KALMAN_AGGRESSIVE= 2,    /* REALTIME_LAN，target ~5ms */
    MC_JITTER_MODE_KALMAN_NORMAL    = 3,    /* STREAMING_WIFI，target ~15ms */
    MC_JITTER_MODE_KALMAN_SAFE      = 4     /* WAN_FALLBACK / SAFE_MODE，target ~80ms */
} mc_jitter_mode_t;

/* Network link kind（性能量度规范 §11.4 / capability_probe §4.2）。 */
typedef enum mc_link_kind_e {
    MC_LINK_KIND_UNKNOWN        = 0,
    MC_LINK_KIND_LAN_SWITCHED   = 1,
    MC_LINK_KIND_LAN_WIFI       = 2,
    MC_LINK_KIND_WAN_WIRED      = 3,
    MC_LINK_KIND_WAN_WIRELESS   = 4
} mc_link_kind_t;

/* HDCM 组件状态（hdcm 设计 §3.2 / 性能量度规范 §6.1）。 */
typedef enum mc_hdcm_state_e {
    MC_HDCM_STATE_UNKNOWN               = 0,
    MC_HDCM_STATE_ALREADY_INSTALLED     = 1,
    MC_HDCM_STATE_INSTALLABLE           = 2,
    MC_HDCM_STATE_UNAVAILABLE_ON_SKU    = 3,
    MC_HDCM_STATE_INSTALLING            = 4,
    MC_HDCM_STATE_INSTALL_FAILED        = 5,
    MC_HDCM_STATE_RESTART_PENDING       = 6
} mc_hdcm_state_t;

/* SPSC 队列水位快照（性能量度规范 §3.4）。
 * 调用方按 mc_stats_t 内嵌的 5 条队列读 high_water/drop_oldest 即可定位"哪条队列顶不住"。 */
typedef struct mc_queue_stats_s {
    uint32_t            depth;                  /* approx_size 即时值 */
    uint32_t            capacity;               /* 上限 */
    uint32_t            high_water_frames;      /* 本会话最高水位 */
    uint32_t            reserved0;              /* padding，对齐 */
    uint64_t            drop_oldest_count;      /* 累计"丢最老"次数 */
} mc_queue_stats_t;

/* HDCM 单组件状态快照（hdcm 设计 §6 metric 字段映射）。 */
typedef struct mc_hdcm_component_stats_s {
    mc_hdcm_state_t     state;
    uint32_t            reserved0;
    uint64_t            install_attempt_count;          /* type=A/B/C 累计 started 数 */
    uint64_t            install_success_count;
    uint64_t            install_failed_count;
    uint64_t            last_install_duration_ms;       /* 单次安装耗时；type=D 永久 0（跳浏览器）*/
} mc_hdcm_component_stats_t;

/* Frame Validity Gate 污染源（ADD §5.13 状态生命周期）：
 * 任一污染事件触发后，所有后续帧丢弃，直到下一 refresh anchor（IDR/IRAP/GDR-complete）。 */
typedef enum mc_gate_poison_source_e {
    MC_GATE_POISON_NONE             = 0,
    MC_GATE_POISON_DECODE_ERROR     = 1,    /* MFT decode error / mc-libcodec decode_error */
    MC_GATE_POISON_REFS_MISSING     = 2,    /* depack 报参考帧 gap */
    MC_GATE_POISON_PARAMS_MISSING   = 3,    /* SPS/PPS 未缓存 */
    MC_GATE_POISON_COLOR_MISSING    = 4,    /* 色彩元数据三级兜底未锁定 */
    MC_GATE_POISON_REORDER_MISSING  = 5,    /* B 帧依赖未到 */
    MC_GATE_POISON_FENCE_MISSING    = 6,    /* dual-bind fence 未 signal */
    MC_GATE_POISON_EXTERNAL         = 7     /* device_lost / 重连等外部事件 */
} mc_gate_poison_source_t;

typedef struct mc_stats_s {
    size_t              struct_size;
    uint32_t            struct_version;

    /* 协议层 */
    uint64_t            rtp_packets_received;
    uint64_t            rtp_packets_lost;
    uint64_t            rtp_packets_recovered_by_nack;
    uint64_t            rtcp_pli_sent;
    uint32_t            rtt_us;
    uint32_t            jitter_estimate_ms;     /* Kalman 估计 */
    uint32_t            target_delay_ms;

    /* 解码层 */
    uint64_t            video_frames_decoded;
    uint64_t            video_frames_dropped_pre_decode;
    uint64_t            video_frames_dropped_post_decode;
    uint32_t            decode_avg_ms;
    uint32_t            decode_p99_ms;
    uint32_t            reorder_cost_ms;        /* B-Frame Policy 触发时不为零 */

    /* Frame Validity Gate（每个 bit 独立计数，ADD §5.13） */
    uint64_t            gate_drops_refs;
    uint64_t            gate_drops_params;
    uint64_t            gate_drops_recovery;
    uint64_t            gate_drops_color;
    uint64_t            gate_drops_reorder;
    uint64_t            gate_drops_fence;
    int64_t             gate_last_drop_pts_us;  /* 最近一次 drop 的帧 PTS，便于诊断 */

    /* 污染状态（ADD §5.13 污染传播）：当前是否处于污染态、最近一次进入污染的源、
     * 累计进入污染次数、累计在污染期内被丢弃的帧数（refs_resolved+recovery_complete 已计入上方） */
    uint32_t                    gate_poisoned;              /* 0/1：当前是否处于污染态 */
    mc_gate_poison_source_t     gate_last_poison_source;
    uint64_t                    gate_poison_enter_count;
    uint64_t                    gate_poison_drops;          /* 因处于污染态而丢的帧数 */

    /* 渲染层 */
    mc_render_profile_t active_render_profile;
    mc_present_mode_t   active_present_mode;    /* PresentMon 真实观测 */
    uint64_t            present_count;
    uint64_t            present_skipped_by_watchdog;
    uint32_t            client_internal_latency_us;
    uint32_t            end_to_end_latency_us;  /* NTP-RTP 回归后估计 */

    /* 音频层 */
    uint32_t            audio_buffer_ms;
    uint64_t            audio_underruns;
    uint64_t            audio_resync_count;     /* WASAPI 设备切换次数 */

    /* 错误 / 恢复 */
    uint64_t            device_lost_events;
    uint64_t            device_recovered_events;
    uint64_t            adapter_switch_events;

    /* ───────────────────────────────────────────────────────────────────
     * v2 字段（plan Phase 0 引入；向后兼容：v1 调用方 struct_size=sizeof(v1)
     *   时库内只填到 adapter_switch_events 为止）
     * ─────────────────────────────────────────────────────────────────── */

    /* 9 段阶段 timer p95（性能量度规范 §2.2 / ADD §8.1）。 */
    uint64_t            stage_udp_rx_p95_ns;
    uint64_t            stage_jitter_dwell_p95_ns;
    uint64_t            stage_depack_p95_ns;
    uint64_t            stage_decode_alloc_p95_ns;
    uint64_t            stage_decode_actual_p95_ns;
    uint64_t            stage_decode_output_p95_ns;
    uint64_t            stage_upload_p95_ns;
    uint64_t            stage_yuv2rgb_p95_ns;
    uint64_t            stage_present_queue_p95_ns;

    /* 端到端延时（性能量度规范 §2.1）。 */
    uint64_t            e2e_latency_p95_ns;
    uint64_t            e2e_client_internal_p95_ns;

    /* 首帧分解（性能量度规范 §2.3）。 */
    uint64_t            firstframe_open_to_first_emit_ns;
    uint64_t            firstframe_caps_probe_ns;
    uint64_t            firstframe_decoder_init_ns;
    uint64_t            firstframe_wait_idr_ns;

    /* SPSC 队列水位（性能量度规范 §3.4）。 */
    mc_queue_stats_t    queue_rtp_to_jitter;
    mc_queue_stats_t    queue_jitter_to_depack;
    mc_queue_stats_t    queue_depack_to_codec;
    mc_queue_stats_t    queue_codec_to_render;
    mc_queue_stats_t    queue_audio_pcm;

    /* caps_probe 各档（性能量度规范 §2.4 / ADR-015 / capability_probe §3 ~ §5）。 */
    uint64_t            probe_tier1_p95_ns;             /* vendor SDK probe */
    uint64_t            probe_tier2_p95_ns;             /* DXVA-direct probe */
    uint64_t            probe_tier3_p95_ns;             /* MFT hardware async probe */
    uint8_t             probe_tier_selected;            /* 1/2/3/4 ADR-015 档位 */
    uint8_t             probe_reserved0;
    uint16_t            probe_reserved1;
    uint64_t            probe_tier1_skip_count;
    uint64_t            probe_tier2_skip_count;
    uint64_t            probe_tier3_skip_count;
    /* skip reason 用 enum string 表示在 ETW，stats 仅暴露累计 count */

    /* 四维 capability probe 完成时间（capability_probe §3 ~ §5）。 */
    uint64_t            probe_hardware_complete_p95_ns;
    uint64_t            probe_network_complete_p95_ns;
    uint64_t            probe_encoder_complete_p95_ns;
    uint64_t            probe_render_complete_p95_ns;
    mc_link_kind_t      probe_network_link_kind;
    uint32_t            probe_encoder_reorder_depth;

    /* Preset 状态（性能量度规范 §11.4）。 */
    mc_preset_id_t      preset_active_id;
    uint32_t            preset_oscillation_count;
    uint64_t            preset_bootstrap_to_active_p95_ns;
    uint64_t            preset_reload_count;
    uint64_t            preset_reload_latency_p95_ns;
    uint64_t            preset_downgrade_count;
    uint64_t            preset_upgrade_count;
    uint64_t            preset_apply_atomic_violation_count;    /* warm_steady 应永久 0 */
    uint64_t            preset_apply_partial_count;             /* phase 9.x 渐进 unlock 期允许非 0 */
    uint64_t            preset_apply_failure_count;             /* 整体回滚 SAFE_MODE 兜底，应永久 0 */

    /* 子系统能力实装位（plan Phase 9.x 渐进 unlock；性能量度规范 §11.4）。 */
    uint8_t             render_dcomp_nv12_direct_active;        /* phase 9.1 实装后可达 1 */
    uint8_t             rtcp_reduced_size_active;               /* phase 9.2 实装后可达 1 */
    mc_jitter_mode_t    jitter_mode;
    uint32_t            jitter_target_delay_ms;
    uint8_t             present_race_to_display_active;         /* phase 9.4 实装后可达 1 */
    uint8_t             decoder_cuda_graph_active;              /* phase 9.5，仅 NVDEC 路径 */
    uint8_t             res_v2_padding[2];

    /* HDCM 组件状态（ADR-021；hdcm 设计 §6）。
     * 7 类组件:类别 A 三家、类别 B 两家、类别 C MediaPlayback、类别 D 三家 driver。 */
    mc_hdcm_component_stats_t   hdcm_a_nvdec;
    mc_hdcm_component_stats_t   hdcm_a_onevpl;
    mc_hdcm_component_stats_t   hdcm_a_amf;
    mc_hdcm_component_stats_t   hdcm_b_hevc_ext;
    mc_hdcm_component_stats_t   hdcm_b_av1_ext;
    mc_hdcm_component_stats_t   hdcm_c_mediaplayback;
    mc_hdcm_component_stats_t   hdcm_d_nvidia;
    mc_hdcm_component_stats_t   hdcm_d_intel;
    mc_hdcm_component_stats_t   hdcm_d_amd;
    uint8_t                     hdcm_restart_pending;           /* 类别 C 重启等待 0/1 */
    uint8_t                     hdcm_driver_below_intel;        /* 类别 D 阈值检测 0/1 */
    uint8_t                     hdcm_driver_below_nvidia;
    uint8_t                     hdcm_driver_below_amd;
    uint32_t                    hdcm_reserved0;

    /* Frame Validity Gate 增量字段（与 v1 字段互补；ADD §5.13 / 性能量度规范 §4）。 */
    uint64_t                    gate_tainted_duration_p95_ns;   /* 单次污染持续时间 */

    /* 资源 (§5)。 */
    uint32_t                    res_cpu_thread_t2_ratio_pct;    /* x100，便于 int 表达 */
    uint32_t                    res_cpu_thread_t4_ratio_pct;
    uint32_t                    res_cpu_thread_t5_ratio_pct;
    uint32_t                    res_cpu_thread_t6_ratio_pct;
    uint32_t                    res_cpu_thread_t7_ratio_pct;
    uint32_t                    res_cpu_process_ratio_pct;
    uint32_t                    res_gpu_decode_ratio_pct;
    uint32_t                    res_gpu_3d_ratio_pct;
    uint64_t                    res_gpu_vram_used_bytes;
    uint64_t                    res_process_rss_bytes;
    uint32_t                    res_handle_count;
    uint32_t                    res_thread_count;
} mc_stats_t;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MC_PLAYER_STATS_H_ */
