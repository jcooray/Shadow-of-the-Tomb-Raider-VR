cmake_minimum_required(VERSION 3.21)

# Statically link the MSVC C++ runtime (/MT, /MTd) instead of dynamically linking
# it (/MD, the CMake/MSVC default). Must be set before project() so it applies to
# every target, including MinHook's. Without this, our DLL depends on an external
# MSVCP140.dll/VCRUNTIME140.dll at whatever version the loader finds first -- and
# games commonly ship their own older copy right next to the exe, which gets
# picked up instead of a matching one, crashing on the first CRT call (this is
# exactly what happened: SOTTR.exe crashed inside a mismatched MSVCP140.dll the
# moment our DLL touched std::string/std::ofstream/std::mutex). Static linking
# means we carry our own CRT and never touch the game's copy at all.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(SOTTR_VR_Mod LANGUAGES CXX C ASM_MASM RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This project targets 64-bit only (the game is a 64-bit executable).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(FetchContent)

# --- MinHook: ships its own CMakeLists.txt, which provides the "minhook" target ---
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        8af6b4acae5a9388fd742b56fa79ece89d96f823   # master after v1.3.4, 2026-09-08
)
FetchContent_MakeAvailable(minhook)

# --- OpenXR-SDK: provides the "openxr_loader" target + headers. Disable the
# extra build surface we don't need (tests, conformance suite, API layers). ---
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    openxr_sdk
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        release-1.1.65                             # 24a1f2b
)
FetchContent_MakeAvailable(openxr_sdk)

# OpenXR-SDK's own CMakeLists.txt predates CMP0091 and resets it to OLD via
# its own cmake_minimum_required() call, so it silently built against the
# dynamic CRT (/MD) despite our top-level static-CRT setting above -- causing
# duplicate-symbol LNK2005 errors against our /MT objects at link time (same
# root cause as the original MSVCP140.dll crash fix, just at link time
# instead of runtime). Force the target property directly since the policy
# setting alone doesn't reach into its subdirectory scope.
set_target_properties(openxr_loader PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- The proxy DLL itself ---
# Target is named vrmod_dxgi_proxy (not "dxgi") so linking against the real
# system dxgi.lib import library below doesn't resolve to this target itself.
# OUTPUT_NAME still produces dxgi.dll, which is what actually needs to be dropped
# next to the game's exe.
add_library(vrmod_dxgi_proxy SHARED
    src/dllmain.cpp
    src/dxgi_exports.cpp
    src/present_hook.cpp
    src/d3d12_cb_hook.cpp
    src/camera_probe.cpp
    src/ansel_camera_hook.cpp
    src/camera_interceptor.cpp
    camera_interceptor.asm
    src/command_list_probe.cpp
    src/stereo_mode.cpp
    src/backbuffer_capture.cpp
    src/bmp_writer.cpp
    src/eye_split.cpp
    src/openxr_integration.cpp
    src/head_tracking.cpp
    src/stall_watchdog.cpp
    src/exit_hook.cpp
    src/resolution_unlock.cpp
    src/desktop_spoof.cpp
    src/settings.cpp
    src/cinematic_watch.cpp
    src/gpu_frame_timer.cpp
    src/rt_logger.cpp
    src/stereo_scale.cpp
    src/cambone_probe.cpp
    src/bone_probe.cpp
    src/frame_log.cpp
    src/d3d12_debug.cpp
    src/ghost_test.cpp
    src/pairing_test.cpp
    src/reader_probe.cpp
    src/value_scan.cpp
    src/counter_scan.cpp
    src/bone_scan.cpp
    src/near_probe.cpp
    src/ui_probe.cpp
    src/face_track.cpp
    src/body_turn.cpp
    src/facing_probe.cpp
    src/game_addresses.cpp
    src/gpu_tier.cpp
    src/steamvr_settings.cpp
    src/cheeky_loader.cpp
    src/hud_layer.cpp
    src/aim_finder.cpp
    src/aim_watch.cpp
    src/dlss_probe.cpp
    src/dlss_mv.cpp
    src/eye_gaze.cpp
    src/openvr_gaze.cpp
    src/gear_hide.cpp
    src/cam_state_probe.cpp
    src/head_drive.cpp
    src/afw.cpp
    src/cheeky_host.cpp
    src/aer.cpp
    src/projection_probe.cpp
    src/logger.cpp
    src/overlay_pad.cpp
    src/vr_overlay.cpp
    src/version.rc
)

# Release builds (-DVRMOD_DIAGNOSTICS=OFF) leave the diagnostic probes out: their headers become inline no-ops
# (src/build_config.h), and with them their fixed game addresses and stray hotkeys (F6/F7 stereo mode, probes).
option(VRMOD_DIAGNOSTICS "Build the diagnostic probes (development builds)" ON)
if(NOT VRMOD_DIAGNOSTICS)
    foreach(diag d3d12_cb_hook camera_probe ansel_camera_hook command_list_probe stereo_mode rt_logger cambone_probe
                 bone_probe ghost_test pairing_test reader_probe value_scan counter_scan near_probe aim_finder aim_watch
                 gear_hide cam_state_probe projection_probe facing_probe)
        get_target_property(srcs vrmod_dxgi_proxy SOURCES)
        list(REMOVE_ITEM srcs src/${diag}.cpp)
        set_property(TARGET vrmod_dxgi_proxy PROPERTY SOURCES ${srcs})
    endforeach()
    target_compile_definitions(vrmod_dxgi_proxy PRIVATE VRMOD_DIAG=0)
else()
    target_compile_definitions(vrmod_dxgi_proxy PRIVATE VRMOD_DIAG=1)
endif()

target_include_directories(vrmod_dxgi_proxy PRIVATE src third_party/openvr)
target_link_libraries(vrmod_dxgi_proxy PRIVATE minhook openxr_loader d3d11.lib dxgi.lib d3d12.lib)
set_target_properties(vrmod_dxgi_proxy PROPERTIES
    OUTPUT_NAME "dxgi"
    PREFIX ""
)

# Real system DXGI functions are resolved at runtime via LoadLibraryW, not link-time
target_compile_definitions(vrmod_dxgi_proxy PRIVATE UNICODE _UNICODE)

# Eye tracking monitor (tools/gaze_monitor, development copy; released separately as
# github.com/jcooray/PimaxEyeTrackingMonitor): a desktop window showing whether gaze gets from the Pimax eye tracker
# through the headset driver to SteamVR.
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/gaze_monitor/gaze_monitor.cpp)
    add_executable(vrmod_gaze_monitor WIN32 tools/gaze_monitor/gaze_monitor.cpp tools/gaze_monitor/gaze_monitor.rc)
    target_include_directories(vrmod_gaze_monitor PRIVATE third_party/openvr)
    target_compile_definitions(vrmod_gaze_monitor PRIVATE UNICODE _UNICODE)
    target_link_libraries(vrmod_gaze_monitor PRIVATE user32 gdi32 ole32)
endif()

# Offline check of the release address patterns against the installed SOTTR.exe (tools/gameaddr_test.cpp).
# Offline preview of the in-headset overlay's pages (tools/overlay_preview.cpp): the overlay with stand-ins for the game.
add_executable(vrmod_overlay_preview tools/overlay_preview.cpp src/vr_overlay.cpp src/settings.cpp src/gpu_tier.cpp src/steamvr_settings.cpp)
target_compile_definitions(vrmod_overlay_preview PRIVATE UNICODE _UNICODE VRMOD_DIAG=0)
target_include_directories(vrmod_overlay_preview PRIVATE ${CMAKE_BINARY_DIR}/_deps/openxr_sdk-src/include)
target_link_libraries(vrmod_overlay_preview PRIVATE user32 gdi32)

add_executable(vrmod_gameaddr_test tools/gameaddr_test.cpp src/game_addresses.cpp)
target_compile_definitions(vrmod_gameaddr_test PRIVATE GAMEADDR_TEST)
