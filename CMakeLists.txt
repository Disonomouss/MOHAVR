cmake_minimum_required(VERSION 3.20)
project(MOHAVR LANGUAGES CXX)

# MOHA.exe is 32-bit (ENGINE-NOTES 2); everything loaded into it must be x86 (D2).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "MOHAVR must be built for x86 -- use tools/build.ps1 (VS dev shell -arch=x86)")
endif()

# Static CRT: the proxy must not depend on which VC++ runtime the player has installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(mohavr SHARED
    src/mohavr/dllmain.cpp
    src/mohavr/log.cpp
    src/mohavr/config.cpp
    src/mohavr/build_check.cpp
    src/mohavr/proxy_dinput8.cpp
    src/mohavr/hooks_d3d9.cpp
    src/mohavr/frame_capture.cpp
    src/mohavr/patch.cpp
    src/mohavr/xr_session.cpp
    src/mohavr/dinput8.def
)

# OpenXR loader, linked statically (triplet x86-windows-static via vcpkg manifest mode), so the
# mod stays a single DLL in the game folder.
find_package(OpenXR CONFIG REQUIRED)
target_link_libraries(mohavr PRIVATE OpenXR::openxr_loader OpenXR::headers d3d11 dxgi)

# The game loads it as dinput8.dll from its Binaries folder (D5).
set_target_properties(mohavr PROPERTIES OUTPUT_NAME dinput8)
target_compile_features(mohavr PRIVATE cxx_std_20)
target_compile_definitions(mohavr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(mohavr PRIVATE /W4 /permissive- /utf-8)
target_link_options(mohavr PRIVATE /SAFESEH:NO)
