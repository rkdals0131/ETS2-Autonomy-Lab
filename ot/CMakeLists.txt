cmake_minimum_required(VERSION 3.25)
project(ot_core VERSION 0.50.2 LANGUAGES C CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "ot_core requires MSVC x64 on Windows")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# No machine path: -DETS2_EXE, then the ETS2_DIR game directory, then Steam libraries.
if(NOT ETS2_EXE)
  set(ot_game_dirs "")
  if(DEFINED ENV{ETS2_DIR})
    list(APPEND ot_game_dirs "$ENV{ETS2_DIR}")
  endif()
  cmake_host_system_information(RESULT ot_steam QUERY WINDOWS_REGISTRY "HKCU/Software/Valve/Steam" VALUE "SteamPath" ERROR_VARIABLE ot_steam_error)
  if(ot_steam AND EXISTS "${ot_steam}/steamapps/libraryfolders.vdf")
    file(STRINGS "${ot_steam}/steamapps/libraryfolders.vdf" ot_steam_paths REGEX "\"path\"")
    foreach(line IN LISTS ot_steam_paths)
      string(REGEX REPLACE "^.*\"path\"[ \t]+\"([^\"]+)\".*$" "\\1" library "${line}")
      string(REPLACE "\\\\" "/" library "${library}")
      list(APPEND ot_game_dirs "${library}/steamapps/common/Euro Truck Simulator 2")
    endforeach()
  endif()
  foreach(dir IN LISTS ot_game_dirs)
    if(EXISTS "${dir}/bin/win_x64/eurotrucks2.exe")
      set(ETS2_EXE "${dir}/bin/win_x64/eurotrucks2.exe" CACHE FILEPATH "Inspected ETS2 executable" FORCE)
      break()
    endif()
  endforeach()
endif()
if(NOT EXISTS "${ETS2_EXE}")
  message(FATAL_ERROR "ETS2 executable not found. Set ETS2_DIR to the game directory or pass -DETS2_EXE=.../bin/win_x64/eurotrucks2.exe")
endif()
set(SCS_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../research/sdk/include" CACHE PATH "SCS SDK include directory")
if(NOT EXISTS "${SCS_SDK}/scssdk_telemetry.h")
  message(FATAL_ERROR "Set SCS_SDK to the official telemetry SDK include directory")
endif()
# Pin this build to the inspected executable, not to an editable runtime setting.
# schema/<version>/ holds one analyzed game build; a game update gets a new directory
# from research/rebase_rvas.py (which also carries the RVA literals in native/).
set(OT_GAME_VERSION "1.61.1.9" CACHE STRING "Analyzed game build in schema/")
set(OT_SCHEMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/schema/${OT_GAME_VERSION}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${OT_SCHEMA_DIR}/game.json"
  "${OT_SCHEMA_DIR}/fields.json")
file(READ "${OT_SCHEMA_DIR}/game.json" OT_GAME_IDENTITY)
string(JSON OT_GAME_SHA256 GET "${OT_GAME_IDENTITY}" exe_sha256)
file(SHA256 "${ETS2_EXE}" OT_BUILD_EXE_SHA256)
# The runtime gate compares the running executable with game.json and keeps internal
# (Tier 1) access off on any other build. OT_SDK_ONLY_BUILD lets the SDK-only parts be
# rebuilt after a game update, before the offsets are re-analyzed; game.json stays as is.
option(OT_SDK_ONLY_BUILD "Build although the installed executable differs from game.json (runtime stays SDK-only)" OFF)
if(NOT OT_BUILD_EXE_SHA256 STREQUAL OT_GAME_SHA256)
  if(OT_SDK_ONLY_BUILD)
    message(WARNING "Installed executable ${OT_BUILD_EXE_SHA256} differs from game.json; building for SDK-only use.")
  else()
    message(FATAL_ERROR "The executable differs from the inspected build. Re-analyze offsets before updating game.json, or pass -DOT_SDK_ONLY_BUILD=ON for an SDK-only build.")
  endif()
endif()
file(READ "${OT_SCHEMA_DIR}/fields.json" OT_SCHEMA)
configure_file(native/build_identity.hpp.in generated/build_identity.hpp @ONLY)
add_library(safetyhook STATIC vendor/safetyhook/safetyhook.cpp vendor/safetyhook/Zydis.c)
target_include_directories(safetyhook PUBLIC vendor/safetyhook)
target_compile_options(safetyhook PRIVATE /utf-8 /guard:cf)
find_program(OT_FXC fxc REQUIRED)
foreach(kind IN ITEMS depth color)
  set(definition "")
  if(kind STREQUAL "depth")
    set(definition /D PACK_DEPTH=1)
  endif()
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/pack_${kind}.hpp"
    COMMAND "${OT_FXC}" /nologo /Ges /O3 /T cs_5_0 /E main ${definition}
      /Vn ot_pack_${kind} /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/pack_${kind}.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/native/pack_rgbd.hlsl"
    DEPENDS native/pack_rgbd.hlsl VERBATIM)
endforeach()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/auto_exposure.hpp"
  COMMAND "${OT_FXC}" /nologo /Ges /O3 /T cs_5_0 /E main /Vn ot_auto_exposure
    /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/auto_exposure.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/native/auto_exposure.hlsl"
  DEPENDS native/auto_exposure.hlsl VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/gather_lidar.hpp"
  COMMAND "${OT_FXC}" /nologo /Ges /O3 /T cs_5_0 /E main /Vn ot_gather_lidar
    /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/gather_lidar.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/native/gather_lidar.hlsl"
  DEPENDS native/gather_lidar.hlsl VERBATIM)
# HUD decal (docs/24 §5): vertex and pixel stage of one source.
foreach(stage IN ITEMS vs ps)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/overlay_${stage}.hpp"
    COMMAND "${OT_FXC}" /nologo /Ges /O3 /T ${stage}_5_0 /E ${stage}_main /Vn ot_overlay_${stage}
      /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/overlay_${stage}.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/native/overlay_decal.hlsl"
    DEPENDS native/overlay_decal.hlsl VERBATIM)
endforeach()
add_library(ot_core SHARED native/core.cpp native/platform.cpp native/transport.cpp native/vehicle.cpp native/render_probe.cpp native/camera_rig.cpp native/gpu_capture.cpp native/gpu_pack.cpp native/capture_stream.cpp native/pass_commands.cpp native/render_sample.cpp native/thread_sampler.cpp native/stack_unwind.cpp native/hook_support.cpp native/parallel_passes.cpp native/proxy_gate.cpp native/overlay_decal.cpp native/ot_core.def
  "${CMAKE_CURRENT_BINARY_DIR}/generated/overlay_vs.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/overlay_ps.hpp"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/pack_depth.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/pack_color.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/gather_lidar.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/auto_exposure.hpp")
target_include_directories(ot_core PRIVATE "${SCS_SDK}" vendor "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(ot_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(ot_core PRIVATE /W4 /EHsc /utf-8 /permissive- /guard:cf)
target_link_options(ot_core PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
target_link_libraries(ot_core PRIVATE safetyhook bcrypt advapi32 user32 dbghelp psapi)
set_target_properties(ot_core PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist")
add_library(ot_loader SHARED native/loader.cpp native/input_device.cpp native/manual_input.cpp native/transport.cpp native/platform.cpp native/ot_loader.def)
target_include_directories(ot_loader PRIVATE "${SCS_SDK}" vendor "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(ot_loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 OT_RESIDENT_LOADER)
target_compile_options(ot_loader PRIVATE /W4 /EHsc /utf-8 /permissive- /guard:cf)
target_link_options(ot_loader PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
target_link_libraries(ot_loader PRIVATE bcrypt advapi32 user32 dbghelp xinput)
set_target_properties(ot_loader PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist")
add_custom_command(TARGET ot_core POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/ot_config.json" "$<TARGET_FILE_DIR:ot_core>/ot_config.json"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OT_SCHEMA_DIR}/fields.json" "$<TARGET_FILE_DIR:ot_core>/ot_schema.json")
message(STATUS "Pinned game SHA256: ${OT_GAME_SHA256}")
add_library(ot_ipc SHARED native/ipc_reader.cpp)
target_compile_definitions(ot_ipc PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ot_ipc PRIVATE /W4 /EHsc /utf-8 /guard:cf)
target_link_options(ot_ipc PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
set_target_properties(ot_ipc PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist")
add_executable(ot_sampler_test EXCLUDE_FROM_ALL native/thread_sampler_test.cpp native/thread_sampler.cpp native/stack_unwind.cpp native/platform.cpp)
target_compile_definitions(ot_sampler_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(ot_sampler_test PRIVATE /W4 /EHsc /utf-8 /permissive-)
target_include_directories(ot_sampler_test PRIVATE vendor)
target_link_libraries(ot_sampler_test PRIVATE bcrypt advapi32 dbghelp psapi)
add_executable(ot_unwind_test EXCLUDE_FROM_ALL native/stack_unwind_test.cpp native/stack_unwind.cpp native/platform.cpp)
target_compile_definitions(ot_unwind_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(ot_unwind_test PRIVATE /W4 /EHsc /utf-8 /permissive-)
target_include_directories(ot_unwind_test PRIVATE vendor)
target_link_libraries(ot_unwind_test PRIVATE bcrypt advapi32 dbghelp psapi)
add_executable(ot_parallel_test EXCLUDE_FROM_ALL native/parallel_passes_test.cpp)
target_compile_definitions(ot_parallel_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(ot_parallel_test PRIVATE /W4 /EHsc /utf-8 /permissive-)
add_executable(ot_drive_test EXCLUDE_FROM_ALL native/drive_control_test.cpp native/manual_input.cpp)
target_compile_definitions(ot_drive_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ot_drive_test PRIVATE /W4 /EHsc /utf-8 /permissive-)
add_executable(ot_overlay_test EXCLUDE_FROM_ALL native/overlay_test.cpp)
target_compile_definitions(ot_overlay_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ot_overlay_test PRIVATE /W4 /EHsc /utf-8 /permissive-)
add_executable(ot_proxy_gate_test EXCLUDE_FROM_ALL native/proxy_gate_test.cpp native/proxy_gate.cpp)
target_compile_definitions(ot_proxy_gate_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ot_proxy_gate_test PRIVATE /W4 /EHsc /utf-8 /permissive-)
