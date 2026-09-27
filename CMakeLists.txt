cmake_minimum_required(VERSION 3.20)
project(MOHAVR LANGUAGES CXX)

# One source tree, two binaries, chosen by the toolchain's bitness (tools/build.ps1 builds both):
#   x86 -> dinput8.dll      the game-side mod (MOHA.exe is 32-bit, ENGINE-NOTES 2; D2)
#   x64 -> MOHAVR-host.exe  the OpenXR host, outside the game's 2 GB address space (D10)

# Static CRT: neither binary may depend on which VC++ runtime the player has installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# OpenXR loader, linked statically (x86-windows-static / x64-windows-static via vcpkg manifest).
find_package(OpenXR CONFIG REQUIRED)

set(MOHAVR_COMMON_DEFS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set(MOHAVR_COMMON_OPTS /W4 /permissive- /utf-8)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_library(mohavr SHARED
        src/mohavr/dllmain.cpp
        src/mohavr/log.cpp
        src/mohavr/config.cpp
        src/mohavr/build_check.cpp
        src/mohavr/proxy_dinput8.cpp
        src/mohavr/hooks_d3d9.cpp
        src/mohavr/frame_capture.cpp
        src/mohavr/bridge.cpp
        src/mohavr/patch.cpp
        src/mohavr/xr_session.cpp
        src/mohavr/vr_view.cpp
        src/mohavr/aim.cpp
        src/mohavr/viewmodel.cpp
        src/mohavr/names.cpp
        src/mohavr/xinput_hook.cpp
        src/mohavr/game_exec.cpp
        src/mohavr/render_res.cpp
        src/mohavr/window_lock.cpp
        src/mohavr/dinput8.def
    )
    # Mid-function hooks with register context (safetyhook needs C++23).
    find_package(safetyhook CONFIG REQUIRED)
    target_link_libraries(mohavr PRIVATE safetyhook::safetyhook)
    # The game loads it as dinput8.dll from its Binaries folder (D5).
    set_target_properties(mohavr PROPERTIES OUTPUT_NAME dinput8)
    target_compile_features(mohavr PRIVATE cxx_std_23)
    target_compile_definitions(mohavr PRIVATE ${MOHAVR_COMMON_DEFS})
    target_compile_options(mohavr PRIVATE ${MOHAVR_COMMON_OPTS})
    target_link_options(mohavr PRIVATE /SAFESEH:NO)
    target_link_libraries(mohavr PRIVATE OpenXR::openxr_loader OpenXR::headers d3d11 d3d12 dxgi)
else()
    add_executable(mohavr_host WIN32
        src/host/main.cpp
        src/host/menu.cpp
        src/host/mirror.cpp
        src/host/pad.cpp
        src/host/reticle.cpp
        src/mohavr/log.cpp
    )
    # In-headset menu (Dear ImGui, drawn with D3D11 into its own quad layer).
    find_package(imgui CONFIG REQUIRED)
    set_target_properties(mohavr_host PROPERTIES OUTPUT_NAME MOHAVR-host)
    target_compile_features(mohavr_host PRIVATE cxx_std_20)
    target_compile_definitions(mohavr_host PRIVATE ${MOHAVR_COMMON_DEFS})
    target_compile_options(mohavr_host PRIVATE ${MOHAVR_COMMON_OPTS})
    target_link_libraries(mohavr_host PRIVATE OpenXR::openxr_loader OpenXR::headers imgui::imgui d3d11 dxgi shell32)
endif()
