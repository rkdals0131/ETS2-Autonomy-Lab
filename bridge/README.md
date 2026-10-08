# ETS2 → ROS 2

The first connection publishes captured RGB8, metric optical-depth 32FC1,
CameraInfo, JPEG previews, render-camera TF, frame correspondence, `/clock`, SDK
state and observed ego pose. Windows uses Fast-CDR 2.2.5 with explicit XCDRv1;
Linux uses Jazzy GenericPublisher and the XML SHM transport profile. This is an
incremental implementation: LiDAR, vehicle GT,
full base_link/cabin TF and automatic reconnection are still being added.

## Build

Windows: build/install `ot_core` using `ot/build.cmd`, then run `bridge/build.cmd`.
The relay has no ROS runtime dependency. Its pinned Fast-CDR source is downloaded
by CMake; JPEG uses Windows Imaging Component. Run `python3 bridge/configure.py`
once to create the ignored local pairing/settings file. Both sides read that file.

Ubuntu 24.04 / WSL2: install ROS 2 Jazzy ros-base, vision-msgs, foxglove-bridge,
rosbag2-storage-mcap, rmw-fastrtps-cpp, colcon, CMake and nlohmann-json3-dev.
Set `REPO` to this checkout's WSL path, then:

```bash
source /opt/ros/jazzy/setup.bash
mkdir -p ~/ets2-ros
cd ~/ets2-ros
colcon build --base-paths "$REPO/bridge/ros2" --executor sequential --cmake-args -DCMAKE_BUILD_TYPE=Release
source "$REPO/bridge/ros-env.sh"
ros2 run ets2_bridge ets2_bridge --ros-args -p config:="$REPO/bridge/config/bridge.local.json"
```

The server binds eth0 only. Start ROS subscribers, then on Windows run:

```powershell
bridge/dist/ets2_relay.exe bridge/config/bridge.local.json
```

The relay queries Ubuntu's current eth0 IPv4, opens separate bulk/state TCP
connections and claims the idle game plugin. No command may reacquire the game
after a lost lease: F11 requires an explicit relay restart. Normal exit releases
its lease; a dead relay loses it after five seconds and the DLL returns to Tier 0.
The current local configuration bounds each run with `duration_s`.

In another WSL shell, source `ros-env.sh` and start Foxglove:

```bash
ip=$(ip -j -4 addr show dev eth0 | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["addr_info"][0]["local"])')
ros2 run foxglove_bridge foxglove_bridge --ros-args --params-file "$REPO/bridge/config/foxglove.yaml" -p address:="$ip"
```

Connect Foxglove to `ws://<WSL-eth0-IP>:8765`. The whitelist exposes previews,
point clouds, GT, TF and state; full-resolution RGB/depth stays on ROS topics.
Preview scales on the GPU. Subscriptions select full color, metric depth, preview
and metadata readback separately; no sensor subscriptions means no new GPU captures.
Changes apply at bundle boundaries and do not free in-flight resources.

## Record and replay

Use the same ROS environment for all participants. Initial bags on this machine
go to the user-approved WSL ext4 directory `~/ets2-data/bags/`. For example:

```bash
ros2 bag record -s mcap -o ~/ets2-data/bags/run-01 --topics /clock /tf /tf_static /ets2/frame_info /ets2/vehicle/state /ets2/camera/C_FN/image_raw /ets2/camera/C_FN/depth/image_raw /ets2/camera/C_FN/camera_info
```

Do not use `--use-sim-time` for recording. Stop the live relay and bridge before
`ros2 bag play ~/ets2-data/bags/run-01`. Do not add `--clock`; the recorded clock is
the only clock source. Playback consumers use `use_sim_time=true`.

The first live MCAP contained 249 matched image/depth/preview/info/frame/TF samples
in 24.98 s. Actual Jazzy deserialization confirmed RGB8 1280×720, 32FC1 1280×720,
JPEG 640×360 and matching sensor stamps. The depth's invalid samples were NaN.
Pause/replay, foreground performance and the remaining sensors need separate
verification before the bridge is considered complete.
