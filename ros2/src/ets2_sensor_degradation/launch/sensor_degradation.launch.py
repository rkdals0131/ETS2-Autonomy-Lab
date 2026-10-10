from pathlib import Path

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    defaults = Path(get_package_share_directory("ets2_sensor_degradation")) / "config" / "degradation.yaml"
    return LaunchDescription([
        DeclareLaunchArgument("params", default_value=str(defaults)),
        DeclareLaunchArgument("seed", default_value="1"),
        # Latency is applied on the node clock; /clock carries the game's paused simulation time.
        DeclareLaunchArgument("use_sim_time", default_value="true"),
        Node(package="ets2_sensor_degradation", executable="sensor_degradation", output="screen",
             parameters=[LaunchConfiguration("params"),
                         {"seed": LaunchConfiguration("seed"), "use_sim_time": LaunchConfiguration("use_sim_time")}]),
    ])
