"""Run the GT baseline against an existing bridge; arm:=true starts assistance.

The default arm:=false exits without driving. It does not wait for dynamic arm.
"""
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue


def generate_launch_description():
    options = {
        "arm": ("false", bool, "true explicitly starts assistance; false exits without driving"),
        "acc_enabled": ("true", bool, "Own the throttle/brake axes"),
        "lcc_enabled": ("true", bool, "Own the steering axis"),
        "path_file": ("", str, "World-frame lane JSON from map_lane_provider"),
        "target_speed_mps": ("0.0", float, "Target speed in metres per second"),
        "steering_target": ("0.0", float, "Applied steering target when path_file is empty"),
    }
    return LaunchDescription([
        *[DeclareLaunchArgument(name, default_value=default, description=description)
          for name, (default, _, description) in options.items()],
        Node(package="ets2_autonomy", executable="drive_speed", output="screen",
             parameters=[{name: ParameterValue(LaunchConfiguration(name), value_type=kind)
                          for name, (_, kind, _) in options.items()}]),
    ])
