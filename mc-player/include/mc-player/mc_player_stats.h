/*
 * mc-player public C ABI — runtime stats（ADD §5.13 + §5.10.4）
 *
 * Stats struct 与 mc_event 同样遵守 struct_size + struct_version 演进契约。
 * 字段含义以 ADD §5.13 Frame Validity Gate / §5.10.4 PresentMon 验证 / §5.6.4 B-Frame Policy 为准。
 */

#ifndef MC_PLAYER_STATS_H_
#define MC_PLAYER_STATS_H_

#include "mc_player_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mc_present_mode_e {
    MC_PRESENT_MODE_UNKNOWN                     = 0,
    MC_PRESENT_MODE_COMPOSED_FLIP               = 1,
    MC_PRESENT_MODE_HARDWARE_INDEPENDENT_FLIP   = 2,
    MC_PRESENT_MODE_HW_COMPOSED_INDEPENDENT_FLIP= 3,
    MC_PRESENT_MODE_TEARING                     = 4
} mc_present_mode_t;

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
} mc_stats_t;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MC_PLAYER_STATS_H_ */
