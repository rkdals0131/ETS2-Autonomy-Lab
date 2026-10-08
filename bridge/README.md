# ETS2 → ROS 2

The first connection publishes captured RGB8, metric optical-depth 32FC1,
CameraInfo, JPEG previews, render-camera TF, frame correspondence, `/clock`, SDK
state and observed ego pose. Windows uses Fast-CDR 2.2.5 with explicit XCDRv1;
Linux uses Jazzy GenericPublisher and the XML SHM transport profile. The
implementation connects GPU LiDAR and per-pass vehicle GT, a cabin/base TF tree,
subscription control and reconnection. The sensor views now submit the full ego
body. Foreground frame times and attached-trailer validation still limit driving use.

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
ros2 launch ets2_bridge bridge.launch.py config:="$REPO/bridge/config/bridge.local.json"
```

The server binds eth0 only. Start ROS subscribers, then on Windows run:

```powershell
bridge/dist/ets2_relay.exe bridge/config/bridge.local.json
```

The relay queries Ubuntu's current eth0 IPv4, opens separate bulk/state TCP
connections and claims the idle game plugin. No command may reacquire the game
after a lost lease: F11 requires an explicit relay restart. Normal exit releases
its lease; a dead relay loses it after five seconds and the DLL returns to Tier 0.
The local configuration bounds each run with `duration_s`. Network loss stops the
capture and rig, closes old queues, resolves eth0 again and opens a new session.
The same game lease must still be alive; F11 cancels it and ends the relay.
SDK timer restart also ends the relay so queued data cannot cross clock epochs.
For DLL replacement, stop the relay first, reload the core, then restart the relay.
During game pause, SDK state and the frozen simulation clock continue on the
state connection. Sensor work compiled while paused is omitted; unpaused sensor
frames resume without restarting the relay.

The launch file starts both ROS and Foxglove with the shared-memory profile. Add
`foxglove:=false` for a receive-only run. Other ROS consumers must source
`ros-env.sh` to join domain 42 with the same DDS profile.

Collection can be stopped and resumed without disconnecting SDK state:

```bash
ros2 service call /ets2/capture std_srvs/srv/SetBool '{data: false}'
ros2 service call /ets2/capture std_srvs/srv/SetBool '{data: true}'
```

The service acknowledges a request; `/diagnostics` reports applied capture state.
It cannot clear F11. `/diagnostics` also reports queue drops, pipeline elapsed times
and Windows-send → ROS-publish → Windows-ack round trips (not one-way latency).

To run Foxglove separately instead of through the launch file:

```bash
ip=$(ip -j -4 addr show dev eth0 | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["addr_info"][0]["local"])')
ros2 run foxglove_bridge foxglove_bridge --ros-args --params-file "$REPO/bridge/config/foxglove.yaml" -p address:="$ip"
```

Connect Foxglove to `ws://<WSL-eth0-IP>:8765`. The whitelist exposes previews,
point clouds, GT, TF and state; full-resolution RGB/depth stays on ROS topics.
Preview scales on the GPU. Subscriptions select full color, metric depth, preview
and metadata readback separately; no sensor subscriptions means no new GPU captures.
Changes apply at bundle boundaries and do not free in-flight resources.
Use `/ets2/ground_truth/{camera}/markers` in a Foxglove 3D panel for boxes; the
canonical objects remain `vision_msgs/Detection3DArray`. The verified 3.6 bridge
uses the `foxglove.sdk.v1` WebSocket subprotocol. No desktop UI automation is used.

The TF tree is `world → base_link → cabin → sensors`. `base_link` uses the SDK-time
physics pose and the nominal powered-axle ground reference. `cabin` incorporates
the rendered camera parent relative to that pose, including interpolation and
suspension. Camera and LiDAR mounts are fixed under `cabin`; head motion is excluded.

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
The subsequent four-camera run delivered 161 of every RGB/depth/preview/info/GT
message and each of three PointCloud2 topics, with matching frame stamps and no
relay queue drops. Point clouds contain XYZ, range, beam index, status (0 return,
1 outside sources, 2 invalid depth, 3 beyond range), source camera slot/pixel and
angular sampling error. Invalid beams have NaN range/XYZ. These are ideal samples
of rendered depth, not ray casts; engine visibility omissions still apply.

A full MCAP subsequently contained 79 aligned bundles and was played through
Jazzy with all 79 image/depth/point/GT/frame/TF messages and all 147 original clock
messages received. Foxglove WebSocket reception and ROS decoding of preview,
point cloud, MarkerArray, TF and diagnostics also passed. Attached/articulated
trailer rendering remains unverified; the current body result is for the FH5
without a trailer.

A pause-containing MCAP was also replayed at normal speed without a generated
clock. All 7,214 recorded clocks and 2,129 previews arrived; the 13.26-second pause
and subsequent 61 sensor frames were preserved. Sensor stamps matched frame_info,
and the simulation-time consumer's final clock matched the recording.

Core 0.20.4 fixes missing ego body panels by using the engine's full body-part list
for this rig's views, instead of the subset prepared for the original mirror.
Both side views show the body and its actual metric depth. This changes submission,
not the truck model or camera memory, and does not synthesize an occlusion mask.
Other research callers can opt in with `camera_rig.ego_full_model=true`.

## Capture and relay optimizations (core 0.21.1)

The relay defaults to `shared_gpu: true`. Packed images stay in shared D3D11
textures; the game signals a fence without waiting. A relay-owned device on the
same adapter copies them into its staging buffers and reads them on its own
worker. The fence releases a ring slot only after those GPU copies finish. The
game still reads the small LiDAR buffers and 16-byte exposure samples. Setting
`shared_gpu: false` retains the CPU image transport for comparison and diagnosis.
The relay requires D3D11 Device5/Context4 and shared-fence support; failures end
the run instead of silently changing its transport. Stop the relay before a DLL
reload as before. Do not attach a generic OT_Bundles reader to a shared-GPU stream;
the relay owns its GPU acknowledgement protocol.

Continuous capture selects sensor views in a two-Present submission window for
each requested frame. This accommodates the observed engine submission/execution
overlap: one-window selection lost frames. At 10 Hz, sensors render about 20 Hz
instead of every display frame. Compilation metadata is limited to those windows;
GPU completion is polled at most once per Present. Non-stream rig use retains
continuous rendering. This still borrows slots 0/1/2/5; private slots 6–8 and native
HUD mirror preservation are not implemented by this optimization.

`auto_exposure: true` is the default. Each camera samples log luminance on the GPU
and adapts gain in log space (0.7 s brightening, 0.3 s darkening); its three capture
slots share exposure history. Raw RGB and JPEG use the same gain. Set
`auto_exposure: false` to use `color_gain` directly. The companion topic
`/ets2/frame_info/exposure` (`ets2_msgs/FrameExposure`) carries camera names, gain
and automatic/manual flags with the same stamp and render-frame number. Gain is
NaN when that camera produced no color output. The original `FrameInfo` message
is unchanged, preserving previously recorded bag schemas. Rebuild `ets2_msgs`
and `ets2_bridge` before starting the new relay.

For an uncompressed smaller input, subscribe to each camera's
`/perception/image_raw` and `/perception/camera_info`. They use half the full
image width/height and the corresponding scaled calibration. The GPU output is
shared with JPEG preview, and a perception-only subscriber does not request full
RGB/depth readback. These raw topics remain outside Foxglove's whitelist.

The ROS receiver reads directly into reusable SerializedMessage buffers. CPU
RGB conversion uses SSSE3 when available (with a scalar fallback), and LiDAR beam
directions are reused until their angular configuration changes.

Buffer reuse and direct packet serialization improved full-resolution foreground
performance from **9.73 to 23.26 FPS**, with zero relay queue drops in the repeat.
Frame p95 was still 61.50 ms; driving performance needs further improvement.
See [measurements](../docs/16_ros2_bridge.md) for the current limits.
