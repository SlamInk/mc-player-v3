/*
 * mc-player public C ABI — types & enums.
 *
 * 本文件只定义稳定的公开类型。任何 struct 第一个字段固定为 `size_t struct_size`，
 * 第二个字段固定为 `uint32_t struct_version`，与 mc-libcodec ABI (ADD §5.7.2)
 * 保持一致——未来追加字段不破坏旧调用方。
 *
 * 调用方填写 struct 前必须置 `struct_size = sizeof(<该类型>)`。
 * 库内部按 size 切片、按 version 路由。
 */

#ifndef MC_PLAYER_TYPES_H_
#define MC_PLAYER_TYPES_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
   /* 必须用 Windows.h（而非 windef.h），让 _AMD64_/_X86_ 等架构宏在 winnt.h 之前到位。 */
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <Windows.h>   /* HWND */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────
 * 错误码（0 = 成功）
 * ─────────────────────────────────────────────────────────────*/
typedef int32_t mc_status_t;

#define MC_OK                          ((mc_status_t)0)
#define MC_ERR_INVALID_ARG             ((mc_status_t)-1)
#define MC_ERR_NULL_HANDLE             ((mc_status_t)-2)
#define MC_ERR_OUT_OF_MEMORY           ((mc_status_t)-3)
#define MC_ERR_INVALID_STATE           ((mc_status_t)-4)
#define MC_ERR_UNSUPPORTED             ((mc_status_t)-5)
#define MC_ERR_TIMEOUT                 ((mc_status_t)-6)
#define MC_ERR_IO                      ((mc_status_t)-7)
#define MC_ERR_PROTOCOL                ((mc_status_t)-8)
#define MC_ERR_AUTH                    ((mc_status_t)-9)
#define MC_ERR_DEVICE_LOST             ((mc_status_t)-10)
#define MC_ERR_NO_HARDWARE             ((mc_status_t)-11)
#define MC_ERR_INTERNAL                ((mc_status_t)-99)

/* ──────────────────────────────────────────────────────────────
 * 协议 / Codec 标识
 * ─────────────────────────────────────────────────────────────*/
typedef enum mc_protocol_e {
    MC_PROTOCOL_AUTO            = 0,    /* 由 URL scheme 解析 */
    MC_PROTOCOL_RTSP_UDP        = 1,
    MC_PROTOCOL_RTSP_TCP        = 2,    /* RFC 2326 §10.12 interleaved */
    MC_PROTOCOL_WHEP            = 3     /* v1 暂不启用，预留 */
} mc_protocol_t;

typedef enum mc_video_codec_e {
    MC_VIDEO_CODEC_UNKNOWN      = 0,
    MC_VIDEO_CODEC_H264         = 1,
    MC_VIDEO_CODEC_H265         = 2,
    MC_VIDEO_CODEC_AV1          = 3
} mc_video_codec_t;

typedef enum mc_audio_codec_e {
    MC_AUDIO_CODEC_UNKNOWN      = 0,
    MC_AUDIO_CODEC_AAC          = 1,
    MC_AUDIO_CODEC_OPUS         = 2,    /* WHEP 路径，v1 暂不启用 */
    MC_AUDIO_CODEC_G711_ALAW    = 3,
    MC_AUDIO_CODEC_G711_ULAW    = 4
} mc_audio_codec_t;

/* ADR-015 四级降级链 + 性能量度规范 §6 一一对应（Phase 1 由 ABI v3 起,旧 v2 三值
 * 在本仓库内一次性切换;外部调用方按 struct_version 路由）。
 *
 *   档 1 Vendor SDK 直驱（按 GPU vendor 选其一，Phase 5/6/7 实装）：
 *     1 = NVIDIA NVDEC / 2 = Intel oneVPL / 3 = AMD AMF
 *   档 2 DXVA-direct（D3D11VideoDevice 直驱，跨 vendor 通用，Phase 4 H.264 + 既有 HEVC）：
 *     4 = DXVA_DIRECT
 *   档 3 MFT hardware async（OS 标准抽象兼容档，hw_url=1 && async=1）：
 *     5 = MFT_HARDWARE
 *   档 4 mc-libcodec 软解（CPU SIMD 兜底）：
 *     6 = LIBCODEC
 *
 * 旧值 MC_DECODER_MFT_SOFTWARE = 2 已删除——sync software MFT 由 ADR-015 直接归
 * 到档 4 软解，不再当作硬解;codec_mft_video::start 在仅 sync software 可用时
 * 直接返回 MC_ERR_NO_HARDWARE,由 controller::start_decode_pipeline 路由到档 4。
 */
typedef enum mc_decoder_kind_e {
    MC_DECODER_NONE                 = 0,
    MC_DECODER_VENDOR_SDK_NVDEC     = 1,    /* 档 1 NVIDIA Video Codec SDK (NVDEC) */
    MC_DECODER_VENDOR_SDK_ONEVPL    = 2,    /* 档 1 Intel oneVPL */
    MC_DECODER_VENDOR_SDK_AMF       = 3,    /* 档 1 AMD AMF */
    MC_DECODER_DXVA_DIRECT          = 4,    /* 档 2 ID3D11VideoDevice 直驱 */
    MC_DECODER_MFT_HARDWARE         = 5,    /* 档 3 hardware async MFT */
    MC_DECODER_MFT_SOFTWARE         = 7,    /* 档 3 sync software MFT (IoT LTSC 兜底,+100ms) */
    MC_DECODER_LIBCODEC             = 6     /* 档 4 mc-libcodec 自研软解 */
} mc_decoder_kind_t;

/* GPU 类型（用于诊断日志：硬解到底跑在集显还是独显）。
 * 启发式：DedicatedVideoMemory >= 1 GB 视作 dGPU；is_software flag 视作软件 adapter。 */
typedef enum mc_gpu_kind_e {
    MC_GPU_KIND_UNKNOWN         = 0,
    MC_GPU_KIND_IGPU            = 1,    /* 集显（Intel UHD/Iris、AMD APU Vega/Radeon Graphics） */
    MC_GPU_KIND_DGPU            = 2,    /* 独显（NVIDIA / AMD Radeon dGPU / Intel Arc） */
    MC_GPU_KIND_SOFTWARE        = 3     /* WARP / Basic Render Driver（DXGI is_software） */
} mc_gpu_kind_t;

/* ──────────────────────────────────────────────────────────────
 * 渲染 profile（ADD §5.10）— AUTO 默认，能力探测降序激活
 * ─────────────────────────────────────────────────────────────*/
typedef enum mc_render_profile_e {
    MC_RENDER_PROFILE_AUTO              = 0,
    MC_RENDER_PROFILE_COMPAT            = 1,
    MC_RENDER_PROFILE_BALANCED          = 2,    /* default after probe */
    MC_RENDER_PROFILE_EXTREME           = 3,
    MC_RENDER_PROFILE_ULTIMATE_DCOMP    = 4
} mc_render_profile_t;

/* ──────────────────────────────────────────────────────────────
 * 色彩元数据 — 三级兜底（ADD §5.12）
 * ─────────────────────────────────────────────────────────────*/
typedef enum mc_color_primaries_e {
    MC_COLOR_PRIMARIES_AUTO     = 0,    /* 走三级兜底 */
    MC_COLOR_PRIMARIES_BT709    = 1,
    MC_COLOR_PRIMARIES_BT601    = 5,    /* 对齐 ITU-T 取值 */
    MC_COLOR_PRIMARIES_BT2020   = 9
} mc_color_primaries_t;

typedef enum mc_color_range_e {
    MC_COLOR_RANGE_AUTO         = 0,
    MC_COLOR_RANGE_LIMITED      = 1,
    MC_COLOR_RANGE_FULL         = 2
} mc_color_range_t;

typedef enum mc_color_matrix_e {
    MC_COLOR_MATRIX_AUTO        = 0,
    MC_COLOR_MATRIX_BT709       = 1,
    MC_COLOR_MATRIX_BT601       = 6,
    MC_COLOR_MATRIX_BT2020_NCL  = 9
} mc_color_matrix_t;

/* ──────────────────────────────────────────────────────────────
 * 状态机（ADD §7.2 + Controller 层）
 * ─────────────────────────────────────────────────────────────*/
typedef enum mc_state_e {
    MC_STATE_IDLE               = 0,
    MC_STATE_CONNECTING         = 1,    /* RTSP DESCRIBE/SETUP/PLAY 进行中 */
    MC_STATE_PLAYING            = 2,
    MC_STATE_RECOVERING         = 3,    /* 网络断 / Device Lost 恢复中 */
    MC_STATE_FROZEN             = 4,    /* freeze last-good frame，无新帧 */
    MC_STATE_ERROR              = 5,
    MC_STATE_CLOSED             = 6
} mc_state_t;

/* ──────────────────────────────────────────────────────────────
 * 事件类型 — App 通过单一事件回调拿全部增量
 * ─────────────────────────────────────────────────────────────*/
typedef enum mc_event_type_e {
    MC_EVENT_STATE_CHANGED      = 1,
    MC_EVENT_STREAM_INFO        = 2,    /* SDP / SPS 解析后首次发布 */
    MC_EVENT_FIRST_FRAME        = 3,
    MC_EVENT_DEVICE_LOST        = 4,
    MC_EVENT_DEVICE_RECOVERED   = 5,
    MC_EVENT_ADAPTER_SWITCHED   = 6,    /* 跨屏 transition 完成 */
    MC_EVENT_NETWORK_DOWN       = 7,
    MC_EVENT_NETWORK_UP         = 8,
    MC_EVENT_FREEZE             = 9,    /* >10s 无新视频帧 */
    MC_EVENT_RESUME             = 10,
    MC_EVENT_ERROR              = 11
} mc_event_type_t;

typedef struct mc_stream_info_s {
    size_t              struct_size;
    uint32_t            struct_version;

    mc_video_codec_t    video_codec;
    mc_audio_codec_t    audio_codec;
    mc_decoder_kind_t   video_decoder_kind;     /* 实际激活的解码器 */
    mc_decoder_kind_t   audio_decoder_kind;

    uint32_t            video_width;
    uint32_t            video_height;
    uint32_t            video_fps_num;          /* fps = num / den */
    uint32_t            video_fps_den;
    uint32_t            audio_sample_rate_hz;
    uint32_t            audio_channels;

    /* B-Frame Policy（ADD §5.6.4）实际取得的 reorder 深度 */
    uint32_t            video_reorder_depth;

    /* 色彩元数据（三级兜底后锁定的最终生效值） */
    mc_color_primaries_t    color_primaries;
    mc_color_range_t        color_range;
    mc_color_matrix_t       color_matrix;

    /* 实际命中的 RTP transport */
    mc_protocol_t       active_protocol;

    /* GPU 信息（version >= 2 起）：用于诊断日志区分 集显硬解 / 独显硬解 / 软解。
     * gpu_kind 与 video_decoder_kind 正交：例如 video_decoder_kind=LIBCODEC 时 gpu_kind 仍可
     * 报告所选 adapter 的物理类型（说明为何不走硬解）；video_decoder_kind=MFT_HARDWARE 时
     * gpu_kind 指明硬解跑在 iGPU 还是 dGPU。 */
    mc_gpu_kind_t       gpu_kind;
    char                gpu_description[96];    /* UTF-8，DXGI adapter description；非 0 终止时按截断处理 */
} mc_stream_info_t;

typedef struct mc_event_s {
    size_t              struct_size;
    uint32_t            struct_version;

    mc_event_type_t     type;

    /* 仅在 type == MC_EVENT_STATE_CHANGED 时有效 */
    mc_state_t          new_state;
    mc_state_t          prev_state;

    /* 仅在 type == MC_EVENT_STREAM_INFO 时有效（值在 stream_info 中） */
    const mc_stream_info_t* stream_info;

    /* 仅在 type == MC_EVENT_ERROR 时有效 */
    mc_status_t         error_code;
    const char*         error_message;          /* UTF-8，库内字符串，回调返回后失效 */
} mc_event_t;

/* 事件回调 — 在库内部线程调用，App 须自己 marshal 到 UI 线程 */
typedef void (*mc_event_callback_fn)(void* user_data, const mc_event_t* event);

/* ──────────────────────────────────────────────────────────────
 * 不透明会话句柄
 * ─────────────────────────────────────────────────────────────*/
typedef struct mc_player_s* mc_player_t;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MC_PLAYER_TYPES_H_ */
