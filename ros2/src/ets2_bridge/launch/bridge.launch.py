"""Run the serialized bridge and the compressed-only Foxglove endpoint."""
import json
import subprocess
from pathlib import Path

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction, SetEnvironmentVariable
from launch.conditions import IfCondition
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    share = Path(get_package_share_directory("ets2_bridge"))
    def nodes(context):
        config = LaunchConfiguration("config").perform(context)
        settings = json.loads(Path(config).read_text(encoding="utf-8"))
        address = settings.get("bind_address")
        if not address:
            interfaces = json.loads(subprocess.check_output(["ip", "-j", "-4", "addr", "show", "eth0"]))
            address = next(a["local"] for i in interfaces for a in i["addr_info"] if a["family"] == "inet")
        return [
            Node(package="ets2_bridge", executable="ets2_bridge", output="screen", parameters=[{"config": config}]),
            Node(package="foxglove_bridge", executable="foxglove_bridge", output="screen",
                 condition=IfCondition(LaunchConfiguration("foxglove")),
                 parameters=[str(share / "config/foxglove.yaml"), {"address": address}]),
        ]
    return LaunchDescription([
        DeclareLaunchArgument("config", description="Absolute bridge.local.json path"),
        DeclareLaunchArgument("foxglove", default_value="true"),
        DeclareLaunchArgument("domain_id", default_value="42"),
        SetEnvironmentVariable("ROS_DOMAIN_ID", LaunchConfiguration("domain_id")),
        SetEnvironmentVariable("RMW_IMPLEMENTATION", "rmw_fastrtps_cpp"),
        SetEnvironmentVariable("FASTRTPS_DEFAULT_PROFILES_FILE", str(share / "config/fastdds.xml")),
        OpaqueFunction(function=nodes),
    ])
