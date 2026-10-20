#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace bridge {
// ets2_msgs/package.xml version whose layouts the relay serializes (checked by tests/topics_test).
inline constexpr std::string_view msgs_version="0.4.0";
// state: reliable, published from the SDK-frame path. latched: transient local.
struct Topic {std::string name,type;bool state=false,latched=false;};
inline std::vector<Topic> topics() {
    std::vector<Topic> result{
        {"/clock","rosgraph_msgs/msg/Clock",true},
        {"/ets2/vehicle/state","ets2_msgs/msg/VehicleState",true},
        {"/ets2/vehicle/actuation","ets2_msgs/msg/ActuationState",true},
        {"/ets2/vehicle/powertrain","ets2_msgs/msg/PowertrainState",true},
        {"/ets2/drive/state","ets2_msgs/msg/DriveState",true},
        {"/ets2/imu/data_raw","sensor_msgs/msg/Imu",true},
        {"/ets2/wheels/state","ets2_msgs/msg/WheelState",true},
        {"/ets2/wheels/odometry","nav_msgs/msg/Odometry",true},
        {"/ets2/gnss/fix","sensor_msgs/msg/NavSatFix",true},
        {"/ets2/sensors/config","std_msgs/msg/String",true,true},
        {"/ets2/ground_truth/ego/pose","geometry_msgs/msg/PoseStamped",true},
        {"/ets2/ground_truth/traffic","ets2_msgs/msg/TrafficState",true},
        {"/diagnostics","diagnostic_msgs/msg/DiagnosticArray",true},
        {"/tf_static","tf2_msgs/msg/TFMessage",true,true},
        // Reliable: the SDK-frame path publishes world TF; render bundles add the cabin frame.
        {"/tf","tf2_msgs/msg/TFMessage",true},
        {"/ets2/frame_info","ets2_msgs/msg/FrameInfo"},
        {"/ets2/frame_info/exposure","ets2_msgs/msg/FrameExposure"}};
    for(const auto* name:{"C_FN","C_FW","C_RL","C_RR"}) {
        const std::string base=std::string("/ets2/camera/")+name;
        result.push_back({base+"/image_raw","sensor_msgs/msg/Image"});
        result.push_back({base+"/depth/image_raw","sensor_msgs/msg/Image"});
        result.push_back({base+"/camera_info","sensor_msgs/msg/CameraInfo"});
        result.push_back({base+"/preview/image/compressed","sensor_msgs/msg/CompressedImage"});
        result.push_back({base+"/preview/camera_info","sensor_msgs/msg/CameraInfo"});
        result.push_back({base+"/perception/image_raw","sensor_msgs/msg/Image"});
        result.push_back({base+"/perception/camera_info","sensor_msgs/msg/CameraInfo"});
        result.push_back({std::string("/ets2/ground_truth/")+name+"/objects","vision_msgs/msg/Detection3DArray"});
        result.push_back({std::string("/ets2/ground_truth/")+name+"/markers","visualization_msgs/msg/MarkerArray"});
    }
    for(const auto* name:{"L_F","L_PL","L_PR"}) {
        result.push_back({std::string("/ets2/lidar/")+name+"/points","sensor_msgs/msg/PointCloud2"});
        result.push_back({std::string("/ets2/lidar/")+name+"/preview/points","sensor_msgs/msg/PointCloud2"});
    }
    return result;
}
}
