# mc-player main library
#
# 五层架构按 ADD §3.1 落地：
#   src/app/         应用层 / 公开 C ABI 实现
#   src/controller/  Controller — 生命周期 FSM、Adapter 选择、Device Lost 恢复
#   src/transport/   Transport Session — RTSP/RTP 聚合层
#   src/media/       Media Core — jitter / depack / codec / render / sync / gate
#   src/pal/         Platform Abstraction — Clock / Socket / MMCSS / SPSC / DXGI Probe

set(MCP_PUBLIC_HEADERS
    include/mc-player/mc_player.h
    include/mc-player/mc_player_types.h
    include/mc-player/mc_player_stats.h
)

set(MCP_PAL_SOURCES
    src/pal/error.h
    src/pal/error.cpp
    src/pal/clock.h
    src/pal/clock.cpp
    src/pal/spsc_queue.h
    src/pal/cache_line.h
    src/pal/log.h
    src/pal/log.cpp
    src/pal/thread.h
    src/pal/thread.cpp
    src/pal/com_apartment.h
    src/pal/com_apartment.cpp
    src/pal/mf_runtime.h
    src/pal/mf_runtime.cpp
    src/pal/socket_iocp.h
    src/pal/socket_iocp.cpp
    src/pal/dxgi_caps_probe.h
    src/pal/dxgi_caps_probe.cpp
    src/pal/raii.h
    # Phase 0 — 性能量度地基（性能量度规范 §9 / plan Phase 0）
    src/pal/metric.h
    src/pal/metric.cpp
    src/pal/etw_provider.h
    src/pal/etw_provider.cpp
)

set(MCP_TRANSPORT_SOURCES
    src/transport/transport_session.h
    src/transport/rtp_packet.h
    src/transport/rtp_packet.cpp
    src/transport/rtcp.h
    src/transport/rtcp.cpp
    src/transport/sdp_parser.h
    src/transport/sdp_parser.cpp
    src/transport/rtsp_digest.h
    src/transport/rtsp_digest.cpp
    src/transport/rtsp_message.h
    src/transport/rtsp_message.cpp
    src/transport/rtsp_client.h
    src/transport/rtsp_client.cpp
    src/transport/ts_rtsp_udp.h
    src/transport/ts_rtsp_udp.cpp
    src/transport/ts_rtsp_tcp.h
    src/transport/ts_rtsp_tcp.cpp
)

set(MCP_MEDIA_SOURCES
    src/media/frame.h
    src/media/jitter_buffer_video.h
    src/media/jitter_buffer_video.cpp
    src/media/jitter_buffer_audio.h
    src/media/jitter_buffer_audio.cpp
    src/media/nack_module.h
    src/media/nack_module.cpp
    src/media/depack_h264.h
    src/media/depack_h264.cpp
    src/media/depack_h265.h
    src/media/depack_h265.cpp
    src/media/depack_aac.h
    src/media/depack_aac.cpp
    src/media/codec_bridge.h
    src/media/codec_bridge.cpp
    src/media/codec_mft_video.h
    src/media/codec_mft_video.cpp
    src/media/codec_libcodec.h
    src/media/codec_libcodec.cpp
    src/media/codec_dxva_video.h
    src/media/codec_dxva_video.cpp
    src/media/codec_nvdec.h
    src/media/codec_nvdec.cpp
    src/media/codec_onevpl.h
    src/media/codec_onevpl.cpp
    src/media/codec_vendor_base.h
    src/media/dxva_h264.h
    src/media/dxva_h264.cpp
    src/media/codec_mft_audio.h
    src/media/codec_mft_audio.cpp
    src/media/codec_g711.h
    src/media/codec_g711.cpp
    src/media/color_meta.h
    src/media/color_meta.cpp
    src/media/frame_validity_gate.h
    src/media/frame_validity_gate.cpp
    src/media/av_sync.h
    src/media/av_sync.cpp
    src/media/render_d3d11.h
    src/media/render_d3d11.cpp
    src/media/render_swap_chain.h
    src/media/render_swap_chain.cpp
    src/media/render_present_epoch.h
    src/media/render_present_epoch.cpp
    src/media/render_dcomp.h
    src/media/render_dcomp.cpp
    src/media/ui_overlay.h
    src/media/ui_overlay.cpp
    src/media/audio_render_wasapi.h
    src/media/audio_render_wasapi.cpp
)

set(MCP_CONTROLLER_SOURCES
    src/controller/adapter_picker.h
    src/controller/adapter_picker.cpp
    src/controller/device_lost.h
    src/controller/device_lost.cpp
    src/controller/controller.h
    src/controller/controller.cpp
)

set(MCP_APP_SOURCES
    src/app/mc_player_session.h
    src/app/mc_player_session.cpp
    src/app/mc_player_api.cpp
)

add_library(mc_player STATIC
    ${MCP_PUBLIC_HEADERS}
    ${MCP_PAL_SOURCES}
    ${MCP_TRANSPORT_SOURCES}
    ${MCP_MEDIA_SOURCES}
    ${MCP_CONTROLLER_SOURCES}
    ${MCP_APP_SOURCES}
)
add_library(mc_player::mc_player ALIAS mc_player)

target_include_directories(mc_player
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(mc_player
    PRIVATE
        mcp_compile_options
        # OS 平台原生组件（ADD §2 #10 平台原生优先）
        winmm           # MMCSS
        avrt            # MMCSS task scheduling
        ws2_32          # Winsock2
        iphlpapi        # Adapter info
        bcrypt          # Cryptographic primitives (Schannel/MD5)
        ncrypt
        crypt32
        winhttp         # HTTP client (WHEP 信令使用，v1 暂未启用但保留链接以备扩展)
        # Media Foundation
        mfplat
        mfuuid
        mfreadwrite
        mf
        wmcodecdspuuid
        # DirectX
        d3d11
        dxgi
        dcomp           # DirectComposition
        dxguid
        # Audio
        ksuser
        avrt
        # COM utility
        runtimeobject
)

target_compile_definitions(mc_player
    PRIVATE
        MCP_BUILD_LIBRARY
)

set_target_properties(mc_player PROPERTIES
    FOLDER "mc-player"
    POSITION_INDEPENDENT_CODE ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

if(MCP_ENABLE_LIBCODEC)
    target_compile_definitions(mc_player PRIVATE MCP_HAVE_LIBCODEC=1)
    target_link_libraries(mc_player PRIVATE mc_libcodec::mc_libcodec)
endif()

# Phase 0 — stats JSON dump 工具（plan §0.1 验收）。
add_subdirectory(tools/dump_stats)
