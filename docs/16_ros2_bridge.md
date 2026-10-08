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

Complete cabin/base TF, MCAP replay/pause, reconnection, full-sensor foreground
performance and ego-render correction remain open.
