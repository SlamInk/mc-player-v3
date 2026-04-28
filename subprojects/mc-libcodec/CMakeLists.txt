# mc-libcodec — 自研 H.264 / H.265 主流 profile 软解兜底（ADR-006 / ADR-010）。
#
# 独立 target / 独立 API / 独立 license（Apache 2.0）/ 独立测试套件。
# 不反向依赖 mc-player；可独立用于 mc-encoder、转码工具、SaaS 后端。
#
# v1 范围：API 头 + 基础架构 + H.264 Main / H.265 Main 的入口骨架；
# 完整 SIMD CABAC / IDCT / MC / deblocking / SAO / DPB 实现按 ADD §5.7 各小节后续填充。

set(MCLC_PUBLIC_HEADERS
    include/mc-libcodec/mc_libcodec.h
)

set(MCLC_SOURCES
    src/api.cpp
    src/decoder_h264.h
    src/decoder_h264.cpp
    src/decoder_h265.h
    src/decoder_h265.cpp
    src/dpb.h
    src/dpb.cpp
    src/cpu_features.h
    src/cpu_features.cpp
    src/bitstream.h
    src/bitstream.cpp
    src/nal.h
    src/nal.cpp
    src/hevc_syntax.h
    src/hevc_parser.h
    src/hevc_parser.cpp
)

add_library(mc_libcodec STATIC
    ${MCLC_PUBLIC_HEADERS}
    ${MCLC_SOURCES}
)
add_library(mc_libcodec::mc_libcodec ALIAS mc_libcodec)

target_include_directories(mc_libcodec
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(mc_libcodec PRIVATE mcp_compile_options)

set_target_properties(mc_libcodec PROPERTIES
    FOLDER "subprojects"
    POSITION_INDEPENDENT_CODE ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
