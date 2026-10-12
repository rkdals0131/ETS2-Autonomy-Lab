cmake_minimum_required(VERSION 3.25)
project(ets2_relay LANGUAGES CXX)
# The relay is the ROS node itself (docs/23). Sources stay in bridge/windows.
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_srvs REQUIRED)
find_package(ets2_msgs REQUIRED)
find_package(fastcdr REQUIRED)
find_package(nlohmann_json REQUIRED)
set(BRIDGE ${CMAKE_CURRENT_SOURCE_DIR}/../../../bridge)
add_executable(ets2_relay
  ${BRIDGE}/windows/main.cpp ${BRIDGE}/windows/ros_endpoint.cpp ${BRIDGE}/windows/game_ipc.cpp
  ${BRIDGE}/windows/vehicle_profile.cpp ${BRIDGE}/windows/motion_sensors.cpp ${BRIDGE}/windows/state_source.cpp
  ${BRIDGE}/windows/render_capture.cpp ${BRIDGE}/windows/state_messages.cpp ${BRIDGE}/windows/image_messages.cpp
  ${BRIDGE}/windows/gpu_readback.cpp)
target_compile_features(ets2_relay PRIVATE cxx_std_20)
target_include_directories(ets2_relay PRIVATE ${BRIDGE}/common)
target_compile_definitions(ets2_relay PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
# Warnings apply to this project, not to the ROS and conda headers.
target_compile_options(ets2_relay PRIVATE /W4 /EHsc /utf-8 /permissive- /external:anglebrackets /external:W0)
target_link_libraries(ets2_relay PRIVATE rclcpp::rclcpp ${std_srvs_TARGETS} ${ets2_msgs_TARGETS}
  fastcdr nlohmann_json::nlohmann_json windowscodecs ole32 d3d11 dxgi)
install(TARGETS ets2_relay DESTINATION lib/${PROJECT_NAME})
if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(std_msgs REQUIRED)
  ament_add_gtest(ros_endpoint_test test/ros_endpoint_test.cpp ${BRIDGE}/windows/ros_endpoint.cpp TIMEOUT 60)
  target_compile_features(ros_endpoint_test PRIVATE cxx_std_20)
  target_include_directories(ros_endpoint_test PRIVATE ${BRIDGE}/common ${BRIDGE}/windows)
  target_compile_definitions(ros_endpoint_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  target_compile_options(ros_endpoint_test PRIVATE /EHsc /utf-8 /permissive- /external:anglebrackets /external:W0)
  target_link_libraries(ros_endpoint_test rclcpp::rclcpp ${std_srvs_TARGETS} ${ets2_msgs_TARGETS} ${std_msgs_TARGETS}
    fastcdr nlohmann_json::nlohmann_json)
endif()
ament_package()
