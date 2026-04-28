/*
 * mc-libcodec public C ABI — 独立子项目（Apache 2.0）。
 *
 * 演进契约（ADD §5.7.2）：所有 struct 首字段 size_t struct_size，第二字段 uint32_t struct_version。
 * 字段追加不破坏旧调用方。
 *
 * 语义：「一次提交一个 Annex-B AU，一次拉取一个 NV12 帧」，与 MFT 异步模型对位。
 */

#ifndef MC_LIBCODEC_H_
#define MC_LIBCODEC_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t mclc_status_t;

#define MCLC_OK                  ((mclc_status_t)0)
#define MCLC_ERR_INVALID_ARG     ((mclc_status_t)-1)
#define MCLC_ERR_OUT_OF_MEMORY   ((mclc_status_t)-2)
#define MCLC_ERR_UNSUPPORTED     ((mclc_status_t)-3)
#define MCLC_ERR_NEED_MORE_INPUT ((mclc_status_t)-4)
#define MCLC_ERR_INTERNAL        ((mclc_status_t)-99)

typedef enum mclc_codec_e {
    MCLC_CODEC_H264 = 1,
    MCLC_CODEC_H265 = 2
} mclc_codec_t;

typedef struct mclc_decoder_s* mclc_decoder_t;

typedef struct mclc_create_options_s {
    size_t       struct_size;
    uint32_t     struct_version;
    mclc_codec_t codec;
    int32_t      single_thread;     /* 1 = 默认；ADD §5.7.6 */
    int32_t      enable_avx2;        /* 0 = 禁用 AVX2，仅 SSE2；运行期 CPUID 探测兜底 */
} mclc_create_options_t;

#define MCLC_CREATE_OPTIONS_VERSION 1

typedef struct mclc_nv12_frame_s {
    size_t       struct_size;
    uint32_t     struct_version;

    uint32_t     width;
    uint32_t     height;
    int64_t      pts_us;

    /* NV12: Y plane + UV interleaved plane */
    const uint8_t* y_plane;
    uint32_t     y_stride;
    const uint8_t* uv_plane;
    uint32_t     uv_stride;

    /* SPS VUI 提取（输出方负责填，调用方按 ADD §5.12 三级兜底使用） */
    int32_t      vui_colour_primaries;
    int32_t      vui_matrix_coefficients;
    int32_t      vui_transfer_characteristics;
    int32_t      vui_video_full_range_flag;

    int32_t      is_keyframe;
    int32_t      decode_error;       /* 1 = 错误帧，调用方应丢 */
} mclc_nv12_frame_t;

#define MCLC_NV12_FRAME_VERSION 1

mclc_status_t mclc_create(const mclc_create_options_t* options, mclc_decoder_t* out);
mclc_status_t mclc_destroy(mclc_decoder_t handle);

/* 提交一个 Annex-B AU；non-blocking。 */
mclc_status_t mclc_submit(mclc_decoder_t handle,
                           const uint8_t* annexb, size_t bytes,
                           int64_t pts_us);

/* 拉取一个 NV12 帧。out_frame->y_plane / uv_plane 内存由 decoder 拥有，下次 submit/pull 前有效。 */
mclc_status_t mclc_pull(mclc_decoder_t handle, mclc_nv12_frame_t* out_frame);

mclc_status_t mclc_flush(mclc_decoder_t handle);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* MC_LIBCODEC_H_ */
