# Demo Host — Win32 native 单窗口承载 mc_player，用于联调与无内存泄露验收。
# 不属于发布产物。

add_executable(mc_player_demo WIN32
    src/main.cpp
)

target_link_libraries(mc_player_demo
    PRIVATE
        mcp_compile_options
        mc_player::mc_player
)

set_target_properties(mc_player_demo PROPERTIES
    FOLDER "demo-host"
    OUTPUT_NAME "mc-player"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
