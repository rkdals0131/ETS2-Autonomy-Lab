# Source in each Bash or Zsh WSL shell used by the bridge, Foxglove, or rosbag2.
# ETS2_ROS_WS selects the colcon build/install workspace (default ~/ets2-ros).
ETS2_ROS_WS="${ETS2_ROS_WS:-$HOME/ets2-ros}"
if [ -n "${ZSH_VERSION:-}" ]; then
    source /opt/ros/jazzy/setup.zsh || return 1
    source "$ETS2_ROS_WS/install/setup.zsh" || return 1
elif [ -n "${BASH_VERSION:-}" ]; then
    source /opt/ros/jazzy/setup.bash || return 1
    source "$ETS2_ROS_WS/install/setup.bash" || return 1
else
    printf '%s\n' 'Source ros-env.sh from Bash or Zsh.' >&2
    return 1
fi
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export ROS_DOMAIN_ID=42
export FASTRTPS_DEFAULT_PROFILES_FILE="$(ros2 pkg prefix ets2_bridge)/share/ets2_bridge/config/fastdds.xml"
