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

GPU LiDAR, vehicle GT publication, complete cabin/base TF, MCAP replay/pause,
reconnection, full-sensor performance and ego-render correction remain open.
