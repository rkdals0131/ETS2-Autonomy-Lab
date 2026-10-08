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

Foreground baseline complete. Bridge, ROS decoding, MCAP replay, GPU selective
readback and ego-render correction remain to be implemented and verified.
