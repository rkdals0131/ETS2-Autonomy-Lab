cmake_minimum_required(VERSION 3.25)
project(ot_core VERSION 0.26.0 LANGUAGES C CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "ot_core requires MSVC x64 on Windows")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(ETS2_EXE "D:/SteamLibrary/steamapps/common/Euro Truck Simulator 2/bin/win_x64/eurotrucks2.exe" CACHE FILEPATH "Inspected ETS2 executable")
set(SCS_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../research/sdk/include" CACHE PATH "SCS SDK include directory")
if(NOT EXISTS "${SCS_SDK}/scssdk_telemetry.h")
  message(FATAL_ERROR "Set SCS_SDK to the official telemetry SDK include directory")
endif()
# Pin this build to the inspected executable, not to an editable runtime setting.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/schema/1.61.1.1/game.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/schema/1.61.1.1/fields.json")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/schema/1.61.1.1/game.json" OT_GAME_IDENTITY)
string(JSON OT_GAME_SHA256 GET "${OT_GAME_IDENTITY}" exe_sha256)
file(SHA256 "${ETS2_EXE}" OT_BUILD_EXE_SHA256)
if(NOT OT_BUILD_EXE_SHA256 STREQUAL OT_GAME_SHA256)
  message(FATAL_ERROR "The executable differs from the inspected build. Re-analyze offsets before updating game.json.")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/schema/1.61.1.1/fields.json" OT_SCHEMA)
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
add_library(ot_core SHARED native/core.cpp native/platform.cpp native/transport.cpp native/vehicle.cpp native/render_probe.cpp native/camera_rig.cpp native/gpu_capture.cpp native/gpu_pack.cpp native/capture_stream.cpp native/pass_commands.cpp native/render_sample.cpp native/ot_core.def
  "${CMAKE_CURRENT_BINARY_DIR}/generated/pack_depth.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/pack_color.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/gather_lidar.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/auto_exposure.hpp")
target_include_directories(ot_core PRIVATE "${SCS_SDK}" vendor "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(ot_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(ot_core PRIVATE /W4 /EHsc /utf-8 /permissive- /guard:cf)
target_link_options(ot_core PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
target_link_libraries(ot_core PRIVATE safetyhook bcrypt advapi32 user32 dbghelp)
set_target_properties(ot_core PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist")
add_library(ot_loader SHARED native/loader.cpp native/transport.cpp native/platform.cpp native/ot_loader.def)
target_include_directories(ot_loader PRIVATE "${SCS_SDK}" vendor "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(ot_loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 OT_RESIDENT_LOADER)
target_compile_options(ot_loader PRIVATE /W4 /EHsc /utf-8 /permissive- /guard:cf)
target_link_options(ot_loader PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
target_link_libraries(ot_loader PRIVATE bcrypt advapi32 user32 dbghelp)
set_target_properties(ot_loader PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist")
add_custom_command(TARGET ot_core POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/ot_config.json" "$<TARGET_FILE_DIR:ot_core>/ot_config.json"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/schema/1.61.1.1/fields.json" "$<TARGET_FILE_DIR:ot_core>/ot_schema.json")
message(STATUS "Pinned game SHA256: ${OT_GAME_SHA256}")
add_library(ot_ipc SHARED native/ipc_reader.cpp)
target_compile_definitions(ot_ipc PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ot_ipc PRIVATE /W4 /EHsc /utf-8 /guard:cf)
target_link_options(ot_ipc PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
set_target_properties(ot_ipc PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist")
