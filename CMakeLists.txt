cmake_minimum_required(VERSION 3.26)

project(mc_player_v3
    VERSION 0.1.0
    DESCRIPTION "Multi-protocol Client Player — Windows ultra-low-latency media player"
    LANGUAGES C CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "mc-player targets Windows 10 1809+ / Windows 11 x64 only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Default build type" FORCE)
endif()

# Per ADD §1: Win10 1809 (build 17763) is the minimum supported OS.
set(MCP_MIN_WINNT 0x0A00)
set(MCP_MIN_NTDDI 0x0A000006) # NTDDI_WIN10_RS5

add_library(mcp_compile_options INTERFACE)
target_compile_definitions(mcp_compile_options INTERFACE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=${MCP_MIN_WINNT}
    NTDDI_VERSION=${MCP_MIN_NTDDI}
)

if(MSVC)
    target_compile_options(mcp_compile_options INTERFACE
        /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /Zc:inline
        /utf-8 /MP /Gy /Gw /EHsc
        $<$<CONFIG:RelWithDebInfo,Release>:/Oi /Ot>
    )
    target_link_options(mcp_compile_options INTERFACE
        /OPT:REF /OPT:ICF
    )
endif()

option(MCP_ENABLE_LIBCODEC "Build mc-libcodec software-decode subproject" ON)
option(MCP_ENABLE_DEMO_HOST "Build the Win32 demo host app" ON)
option(MCP_ENABLE_TESTS "Build unit tests" OFF)

add_subdirectory(mc-player)

if(MCP_ENABLE_LIBCODEC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/subprojects/mc-libcodec/CMakeLists.txt")
    add_subdirectory(subprojects/mc-libcodec)
endif()

if(MCP_ENABLE_DEMO_HOST AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/demo-host/CMakeLists.txt")
    add_subdirectory(demo-host)
endif()
