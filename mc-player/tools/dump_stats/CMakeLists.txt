# dump_stats — plan Phase 0 §0.1 验收"stats JSON dump 工具可输出"。
#
# 链接 mc_player STATIC lib 的内部 metric / etw API 直接拿 Registry::snapshot()。
# 不通过公开 C ABI（Registry 不暴露给 ABI 用户；本工具是 internal CI 工具）。

add_executable(mc_player_dump_stats
    main.cpp
)

target_link_libraries(mc_player_dump_stats
    PRIVATE
        mc_player::mc_player
        mcp_compile_options
        d3d11
        dxgi
)

# 让 dump_stats 能用 internal headers (pal/metric.h, pal/etw_provider.h)。
target_include_directories(mc_player_dump_stats
    PRIVATE
        ${CMAKE_SOURCE_DIR}/mc-player/src
)

set_target_properties(mc_player_dump_stats PROPERTIES
    FOLDER "mc-player/tools"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
