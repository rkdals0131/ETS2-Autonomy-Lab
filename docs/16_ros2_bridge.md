# ROS 2 bridge

Implementation follows the approved Windows C++ relay → WSL Ubuntu/Jazzy design.
Sensor and state traffic use separate TCP connections to the WSL eth0 IPv4 address.
The relay produces little-endian XCDRv1 with Fast-CDR; Linux publishes the serialized
messages using installed ROS type support. The game DLL stays independent of ROS.

## Foreground baseline — 2026-10-09, core 0.17.0

All three conditions used the same stationary scene and Phase 1 FH5 configuration.
Each window lasted 60 seconds; every recorded Present was foreground. The common
measurement instrumentation includes the rig hooks and Present timing, including
the rig-disabled condition. CPU percentages below use one logical core = 100%.

| Condition | FPS | Frame ms p50 / p95 / p99 | Game CPU | Whole-system CPU |
|---|---:|---|---:|---:|
| Rig disabled | 53.06 | 18.65 / 20.99 / 23.13 | 156.1% | 20.6% |
| Rig only | 44.99 | 21.99 / 24.50 / 26.90 | 158.5% | 20.2% |
| Existing RGB-D + LiDAR recorder | 21.18 | 45.63 / 59.00 / 68.29 | 170.9% | 48.6% |

The 63-second recorder run completed 625 GPU bundles, published/saved 587 and
dropped 38 at the shared-memory queue. It also recorded 4 repeated-frame skips and
1 busy GPU ring event. Compressed archives totalled 8,533,418,000 bytes. This path
does not sustain lossless 10 Hz on this scene. These costs include Python LiDAR
generation and archive compression, which the live bridge will not perform.

Recorder CPU is **unavailable**: the measurement sampled the Windows Python
launcher rather than its working child. Its reported zero is invalid. Future
measurements must include the worker process tree. FPS and game CPU measurements
are unaffected. No Present records were lost. Cleanup returned to Tier 0 with
zero active hooks and the recorder exited.

Local raw results: `research/live/2026-10-09-ros2/baseline-original/` (not in Git).
`record_bundles --start-delay SECONDS` allows returning to the game before capture.

## Implementation status

One-camera ROS transport, actual Jazzy CDR decoding and short MCAP recording work.
249 camera samples in 24.98 seconds had matching RGB/depth/preview/info/frame/TF
counts and stamps. The later 65-second receive-only run delivered 645 of each
sensor message with no relay queue drops. Both Fast DDS participants mapped SHM
segments; the 128 MiB XML profile was applied.

Immediately after that connection, a 60-second foreground measurement gave
34.42 FPS and 28.04 / 36.02 / 40.49 ms frame p50 / p95 / p99. Game CPU averaged
155.1% and the native relay 24.0% (one logical core = 100%). Whole-system CPU was
25.8%. All 2060 Present samples were foreground. This uses **one** rig camera and
a ROS consumer without recording, so it is not directly comparable to the earlier
four-camera recorder. Raw frames/CPU samples: `perf-one.json` in the local run directory.

The GPU metric-depth path was compared against the existing Python reconstruction
of the **same frame's original DSV**: 780,858 valid pixels, zero validity mismatches,
absolute depth error median 0.00000191 m, p95 0.00001144 m, maximum 0.00006104 m.

Core 0.19 adds per-camera color/depth/preview/metadata selection at bundle boundaries.
Preview downsampling runs on the GPU. A live preview → RGB-D → no subscriptions →
depth-only → no subscriptions sequence completed without capture errors; the arm
counter stayed at 82 and later 122 during the unsubscribed intervals. Killing only
the owned relay let the five-second lease expire and restored Tier 0 / zero hooks.

Core 0.20 gathers ideal LiDAR returns on the GPU. LiDAR-only demand does not copy
the full depth image back. The relay combines co-located front sources by geometric
coverage priority, preserves unknown depth, and publishes organized PointCloud2.
Vehicle Detection3DArray boxes use the actual pass's prepared body models. An actor
can be occluded in the final image; submission is not a visibility guarantee.

The first full ROS run delivered 161 messages on every four-camera RGB, depth,
preview, CameraInfo and GT topic, plus all three LiDAR topics and frame metadata.
Actual Jazzy deserialization succeeded; all sensor stamps matched frame_info and
the relay reported zero queue drops. Total payload was about 24.2 MB per bundle.
This was a functional run in the background, not the foreground performance result.

The 106,799 GPU beams were compared with Python reconstruction of the same raw DSV.
For identical chosen pixels, maximum range error was 0.0000763 m (front) and
0.0000305 m (each side). FP32 projection selected different pixels for 79 / 100 /
100 beams, including 67 / 36 / 36 coverage-boundary differences; the largest range
difference where both returned a value was 0.046 m. Boundary beams are not bit-exact
with the double-precision CPU path. Source pixel and angular error remain in the
message so this sampling limitation is inspectable.

An interrupted bundle reader is recovered under its exclusive mutex, and ready
slots from an earlier native stream are discarded before attaching a new session.
Runtime cleanup returned to Tier 0 with zero hooks and both owned processes exited.

## Full bridge and recovery — core 0.20.2

The first full-resolution foreground window had 589 / 589 foreground Presents,
9.73 FPS, frame p50 / p95 / p99 102.44 / 126.51 / 142.92 ms, game CPU 210.4%,
relay CPU 174.0%, whole-system CPU 60.6%. There were no missing Present records.
During the whole 70-second run the relay dropped 38 pending bundles; ROS received
599 on each sensor topic. Status publish/ack round trip p95 was 3.27 ms, p99
19.95 ms, maximum 60.24 ms. This is **not a usable driving performance result**.

Pipeline timing was then added to diagnose the regression. A separate functional
recovery run showed about 14 / 67 / 57 ms median shared copy / encode / TCP send;
these stages have separate workers and overlap. Those timings are not a replacement
foreground benchmark. Game GPU memory was 2.79 GB against a 7.61 GB budget.

The cabin/base TF tree, static camera/LiDAR mounts, diagnostics and Foxglove
MarkerArray presentation are implemented. Foxglove bridge 3.6 received 228 previews,
point clouds, markers and dynamic TFs through its `foxglove.sdk.v1` endpoint. The
whitelist advertised no raw RGB or depth topics. Saved WebSocket payloads were
deserialized with actual Jazzy types. The desktop visualization UI itself was not
automated.

`bridge-full-v2` in the local WSL bag directory contains 79 complete sensor bundles.
With live publishers stopped, ordinary `ros2 bag play` (no `--clock`) delivered all
79 checked image/depth/LiDAR/GT/frame/TF messages and 147 recorded clock messages.
Sensor stamps matched frame_info and the consumer used simulation time.

`/ets2/capture` SetBool stop/resume restored Tier 0 and then resumed capture.
Stopping only the owned ROS server restored Tier 0; restarting it caused a fresh
eth0 lookup, new session and resumed delivery. A later panic canceled the game
lease; a ROS resume request did not reactivate it. The relay exited, both servers
were reaped, and final state was Tier 0 / zero hooks. This exercised the same native
panic operation as F11, not a physical keyboard press. WSL's IP was not changed in
this experiment, and WSL was not globally shut down.

At this stage, performance remediation, pause-containing replay, actual
WSL-address-change recovery and ego/trailer rendering were still open. Subsequent
results are recorded below.

Core 0.20.3 retains packed CPU storage across ring-slot reuse. Output descriptions,
not retained buffer contents, select the current sample's channels. The relay
recycles shared-read buffers and writes large Image/PointCloud2 CDR messages into
the final packet, retaining zeroed alignment padding. The bridge disables detailed
draw-binding observation while retaining per-pass vehicle model poses; research
callers keep the previous `draw_metadata=true` default.

Actual Jazzy decoding passed for 82 full four-camera/three-LiDAR bundles after
this change. A separate LiDAR-only → depth-only transition received 89 of each
LiDAR and 111 depth messages. The first mixed functional run exhausted its capture
duration before its final two subscription phases; those phases were repeated in
the separate run. All owned processes exited and the plugin returned to Tier 0.

The repeated 60.18-second foreground window with the same full-resolution outputs
gave **23.26 FPS**, frame p50 / p95 / p99 **41.33 / 61.50 / 75.67 ms**. All 1,400
Present records were foreground; none were missing. Game / relay / whole-system
CPU averaged 167.3% / 77.9% / 41.4%. All sensor topics delivered 606 bundles, with
zero relay queue drops and zero GPU failures. Compared with 0.20.2's 9.73 FPS,
the repeated copies were a substantial cost, but the remaining frame-time tail
still limits driving use. Raw results are `perf-opt.json` and `perf-opt-relay.log`.

## Ego body submission — core 0.20.4

The missing body panels came from cached per-mirror geometry subsets. The live
player's two cached body models at vehicle +0x1020/+0x1028 had 32/30 full-list
geometry entries. Vehicle submission `0x646C00` uses `0xA3CAD0` for mirror views;
that function chooses a subset keyed by the original mirror mask. Moving the
camera does not rebuild that subset.

A scoped hook at `0xA3CADE` selects the engine's existing full-list path by clearing
ZF after its full-list test. It applies only to the two verified body submission
callers, the current player's vehicle, and view masks owned by the configured rig.
No cached model flags or camera fields are modified. The normal hook drain,
panic and unload lifecycle includes this hook. The bridge enables it explicitly;
the general camera-rig API keeps it opt-in.

Same-scene off/on/off captures restored the cabin/body in both RGB side views.
Metric depth gained 52,401 / 51,623 near-occluder pixels in the left/right views,
with median optical depth 0.901 / 0.897 m. On restoration, those pixels returned
to their original depths with median absolute differences 0.000019 / 0.000028 m.
This is a render submission fix, not a mask. Attached/articulated trailers remain
untested. Local outputs are under `ego-parts/` in the ROS run directory.

The earlier fixed night gain (841.55) overexposed the current daylight scene.
The body comparison was also captured with gain 1 for inspection. Automatic
exposure is not implemented; local gain must still match the lighting conditions.

## Pause transition and replay

The first pause experiment exposed a real relay failure: menu rendering could
finish a sensor bundle after the SDK paused, when `ego_at_compile` was no longer
present. The relay now omits sensor bundles whose associated SDK sample is paused.
State and clock publication remain independent; the missing pose is not fabricated.

The corrected `bridge-pause-v3` MCAP contains 2,131 frame messages and 7,214 clock
and vehicle-state messages. Its 409 paused states span 13.259 seconds of recorded
receive time; all 408 clock messages within that interval have the same value.
There are 61 frames after resume, no backwards clock steps and no sensor stamps
without a matching frame message. Subscription startup/shutdown gives slightly
different per-topic totals, so this is not a claim of equal topic counts.

With all live publishers stopped, normal-speed `ros2 bag play` without `--clock`
replayed the complete bag. A `use_sim_time=true` consumer received all 7,214 clocks,
2,131 frame messages, 2,129 previews and preview calibrations, 2,130 GT messages,
2,132 dynamic TF messages and the one static TF. The replayed pause lasted 13.25897
wall-clock seconds with one clock value, followed by all 61 post-resume frames.
Clock regressions and sensor stamps without frame correspondence were both zero.
The consumer's final ROS clock equalled the last recorded clock. Local results are
`pause3-mcap-decode.json` and `pause3-replay-result.json` in the ROS run directory.

Actual trailer attachment is deferred because the user cannot connect one now.
The tested body submission fix covers the current FH5 without a trailer. Actual
WSL eth0 address changes and a foreground measurement with core 0.20.4 also remain
unmeasured; the 23.26 FPS result above was obtained with core 0.20.3.
