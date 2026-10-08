# Source in each WSL shell used by the bridge, Foxglove, or rosbag2.
source /opt/ros/jazzy/setup.bash
source "$HOME/ets2-ros/install/setup.bash"
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export ROS_DOMAIN_ID=42
export FASTRTPS_DEFAULT_PROFILES_FILE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/config/fastdds.xml"
