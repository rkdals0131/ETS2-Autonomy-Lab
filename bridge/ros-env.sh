# Source in each Bash or Zsh WSL shell used by the bridge, Foxglove, or rosbag2.
if [ -n "${ZSH_VERSION:-}" ]; then
    _ets2_ros_env_dir="$(dirname "${(%):-%x}")"
    source /opt/ros/jazzy/setup.zsh || return 1
    source "$HOME/ets2-ros/install/setup.zsh" || return 1
elif [ -n "${BASH_VERSION:-}" ]; then
    _ets2_ros_env_dir="$(dirname "${BASH_SOURCE[0]}")"
    source /opt/ros/jazzy/setup.bash || return 1
    source "$HOME/ets2-ros/install/setup.bash" || return 1
else
    printf '%s\n' 'Source ros-env.sh from Bash or Zsh.' >&2
    return 1
fi
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export ROS_DOMAIN_ID=42
export FASTRTPS_DEFAULT_PROFILES_FILE="$(cd "$_ets2_ros_env_dir" && pwd)/config/fastdds.xml"
unset _ets2_ros_env_dir
