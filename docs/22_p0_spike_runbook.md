# P0 스파이크 실행서: Windows ROS 2 + 기존 릴레이

[포팅 계획](20_windows_native_port_plan.md) P0의 실행 절차입니다. 아무것도 지우지 않고, 기존 WSL 경로와 같은 코드를 Windows의 RoboStack Jazzy에서 빌드해 loopback TCP로 기존 릴레이와 연결하고, 같은 세션에서 WSL과 Windows를 번갈아 측정합니다. 표기 ✔/△는 계획과 같고 코드 인용은 `파일:심볼`입니다.

## 1. 준비

| 항목 | 확인 |
| --- | --- |
| VS2022 x64 C++ 빌드 도구 | ✔ 사용 중(`ot/build.cmd`가 vswhere로 찾음) |
| pixi | ✔ 미설치(2026-10-12). `winget install prefix-dev.pixi` 후 터미널 재시작 |
| 긴 경로 | ✔ `LongPathsEnabled=1` |
| PATH의 Python | ✔ `python.exe`가 C:\Python27. 모든 빌드·실행은 `pixi shell`/`pixi run` 안에서만 한다(△ 밖에서 CMake FindPython이 2.7을 잡을 수 있음) |
| 게임 | ✔ DX11, core 0.30.0, exe SHA 일치 |
| 동시 로드 플러그인 | ✔ `Real_G27_ffb_x64.dll`. 측정 조건에 기록 |

## 2. `pixi.toml`(저장소 루트, RoboStack 템플릿 형태)

```toml
[workspace]
name = "ets2-autonomy-lab"
channels = ["https://prefix.dev/robostack-jazzy", "conda-forge"]
platforms = ["win-64"]

[dependencies]
ros-jazzy-ros-base = "*"
ros-jazzy-rmw-fastrtps-cpp = "*"
ros-jazzy-vision-msgs = "*"
ros-jazzy-foxglove-bridge = "*"
ros-jazzy-rosbag2-storage-mcap = "*"
ros-jazzy-ament-cmake-gtest = "*"
colcon-common-extensions = "*"
compilers = "*"
nlohmann_json = "*"
cmake = "*"
ninja = "*"
pkg-config = "*"

[activation]
scripts = ["ros2/activate.bat"]
env = { ROS_DOMAIN_ID = "42", RMW_IMPLEMENTATION = "rmw_fastrtps_cpp", ROS_AUTOMATIC_DISCOVERY_RANGE = "LOCALHOST" }

[tasks]
build = { cmd = "colcon build --merge-install --packages-select ets2_msgs ets2_bridge ets2_sensor_degradation --cmake-args -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF", cwd = "ros2" }
bridge = { cmd = "ros2 launch ets2_bridge bridge.launch.py config:=%PIXI_PROJECT_ROOT%\\bridge\\config\\bridge.local.json" }
relay = { cmd = "bridge\\dist\\ets2_relay.exe bridge\\config\\bridge.local.json" }
```

`ros2/activate.bat`(저장소에 포함, 빌드 전에도 존재해야 하므로 overlay는 있을 때만 호출):

```bat
@echo off
if exist "%~dp0install\setup.bat" call "%~dp0install\setup.bat"
set "FASTRTPS_DEFAULT_PROFILES_FILE=%~dp0src\ets2_bridge\config\fastdds.xml"
```

- ✔ 위 ROS 패키지는 모두 robostack-jazzy win-64에 있다. `compilers`는 RoboStack 안내의 방식이다(VS2022 설치는 별도). △ `ros-jazzy-ament-cmake-gtest` 이름은 첫 `pixi install`에서 확인.
- `FASTRTPS_DEFAULT_PROFILES_FILE`을 activation에 두는 이유: ✔ 지금은 `bridge.launch.py`만 DDS 프로필(128 MiB SHM, 8 MiB 메시지)을 설정한다. P1부터 런처가 직접 띄우는 릴레이·스택·기록기도 같은 프로필을 받아야 한다. P1에서 경로를 `bridge/config/fastdds.xml`로 바꾼다.
- `pixi.lock`을 커밋한다. `ros2/build,install,log`는 ✔ Git 제외.

## 3. 코드 변경(모두 `#ifdef _WIN32` 보호, 아무것도 삭제하지 않음)

### 3.1 `ros2/src/ets2_bridge/src/main.cpp`

| 심볼 | 변경 |
| --- | --- |
| `#include <ifaddrs.h>`, `eth0()` | `#ifndef _WIN32`로 감쌈. Windows 분기는 `throw std::runtime_error("bind_address is required on Windows")` |
| `listener()`의 `setsockopt(...,&yes,...)` | ✔ MSVC는 4번째 인자가 `const char*`. `reinterpret_cast<const char*>(&yes)` |
| `accept_client()`의 `select(server.id+1,...)` | `static_cast<int>(server.id+1)` |
| `main()` 진입·종료 | `WSAStartup(MAKEWORD(2,2))`/`WSACleanup`(✔ 릴레이 `bridge/windows/main.cpp:main`의 `WinsockEnd` 패턴) |
| `main()`의 `if(ip.empty()) ip=eth0();` | 위 분기 |

`wire.hpp`는 ✔ `_WIN32` 분기를 이미 갖는다.

### 3.2 `ros2/src/ets2_bridge/CMakeLists.txt`

`if(WIN32) target_link_libraries(ets2_bridge ws2_32) endif()`. ✔ 지금은 `nlohmann_json`만 링크해 `WSAStartup`·`socket` 심볼이 미해결이 된다.

### 3.3 `ros2/src/ets2_bridge/launch/bridge.launch.py`

`bind_address`가 비어 있고 `os.name=="nt"`면 `ip -j`를 부르지 않고 `RuntimeError("Set bind_address in bridge.local.json on Windows")`.

### 3.4 `bridge/config/bridge.local.json`

`"bind_address": "127.0.0.1"`. ✔ 릴레이 `main.cpp:ros_address`가 읽어 WSL 조회를 건너뛰고, foxglove_bridge의 `address`도 같은 값이 된다.

제외: `ets2_autonomy`는 P0 빌드 대상에서 뺀다(✔ `gt_drive_node.cpp`의 `<unistd.h>`·`getpid()`는 Linux 전용, P1에서 `_getpid`).

## 4. 실행 순서

```powershell
pixi install
pixi run build                     # ets2_msgs → ets2_bridge → ets2_sensor_degradation
pixi run bridge                    # 터미널 A: "listening at 127.0.0.1"
pixi run relay                     # 터미널 B: "ROS host IP 127.0.0.1"
```

게임은 미리 DX11로 실행해 둔다. 런처 UI는 쓰지 않는다(WSL 세션을 띄우려 하기 때문). WSL A/B를 위해 WSL 쪽 빌드는 그대로 둔다.

## 5. 확인 항목과 go/no-go

| # | 확인 | 방법 | 결과 |
| --- | --- | --- | --- |
| A | Foxglove 수신 | `ws://127.0.0.1:8765`. JPEG 4·표시용 점군 3·TF·`/diagnostics` | |
| B | 전체 점군 통과 | `ros2 topic hz /ets2/lidar/L_F/points` ≈ 10 Hz, `queue_dropped` 0 | |
| C | bag 기록·재생·종료 방식 | §6. pause 중 `/clock` 정지, 재생 뒤 TF 체인. Ctrl+C·CTRL_C_EVENT·CTRL_BREAK 각각으로 MCAP이 닫히는지 | |
| D | A/B 성능 | §7. frame time p50/p99·CPU/GPU busy가 WSL 대비 나쁘지 않음(p99 +1 ms 이내) | |
| E | 명령 나이 | §8. `age<0` 드롭 없음 | |
| F | `ets2_sensor_degradation` | `/sensing/imu/imu_raw` 수신 | |

go 조건: A·B·C·D 통과. no-go면 사유를 적고 [01 P7](01_requirements_and_decisions.md)로 간다.

## 6. bag 명령(런처 없이)

```powershell
ros2 bag record -s mcap -o bridge\recordings\p0-state --disable-keyboard-controls --max-cache-size 8388608 --topics /clock /tf /tf_static /ets2/vehicle/state /ets2/vehicle/actuation /ets2/drive/state /ets2/drive/command /ets2/ground_truth/ego/pose /ets2/imu/data_raw /ets2/wheels/state /ets2/wheels/odometry /ets2/gnss/fix /ets2/sensors/config /diagnostics
```

종료 시험 세 가지(각각 새 기록으로): (1) 같은 콘솔에서 Ctrl+C, (2) 다른 프로세스에서 `AttachConsole(pid)`+`GenerateConsoleCtrlEvent(CTRL_C_EVENT,0)`, (3) `CREATE_NEW_PROCESS_GROUP`으로 띄운 뒤 `CTRL_BREAK_EVENT`. 각각 `metadata.yaml`이 생기는지 기록한다. △ (3)은 SIGBREAK로 전달돼 SIGINT 핸들러를 안 탈 가능성이 크다. (2)가 되면 P1 기록기는 (2), 안 되면 `rosbag2_transport`를 감싼 소형 C++ 기록기(stop event 대기)로 간다.

재생은 `ros2 bag play bridge\recordings\p0-state`, `--clock` 없이, 소비자는 `use_sim_time:=true`.

## 7. A/B 성능 측정

같은 게임 세션, 같은 정차 장면(FH4, core 0.30.0), 같은 설정(JPEG 10 Hz·표시용 라이다 10 Hz·GT 30 Hz, `diagnostic_sensor_stage=publish`, Foxglove 종료, `ros2 topic hz`로 JPEG 4·preview 점군 3·objects 4 구독)에서 다음을 번갈아 2회씩:

1. WSL: 런처 전체 모드 또는 `launch.sh` + `launch.cmd --host-only`.
2. Windows: §4.

측정 도구는 PresentMon(frame time p50/p99, GPU busy)과 릴레이 stdout(`status_publish_roundtrip`, `queue_dropped`), 작업 관리자 CPU. 기록할 조건: `config.cfg`의 vsync·프레임 제한 설정(✔ A5: 센서 없는 주행에서 약 60/s로 포화 가능, △ 리미터), `Real_G27_ffb_x64.dll` 로드 여부, 해상도·`r_scale`. 결과는 `research/live/2026-10-XX-p0-ab/`에 두고 [18](18_performance.md)에 한 줄 추가한다. FPS 단일 수치로 판정하지 않는다.

## 8. 명령 시계 확인

Windows에서 `sent_steady_ns`의 원점이 C++(`std::chrono::steady_clock`, MSVC=QPC)과 Python(`time.perf_counter_ns()`)에서 같은지 본다: 같은 순간에 두 값을 찍어 차이가 수 ms 이내인지 기록. △ 다르면 ROS 쪽 발행기를 C++로 고정한다(`drive_hold`). `time.monotonic`은 쓰지 않는다. [16 시계](16_ros2_bridge.md#시계복구).

## 9. 예상 문제

| 증상 | 원인·대응 |
| --- | --- |
| `colcon build`가 cl.exe를 못 찾음 | pixi 활성화 실패. `pixi shell` 안에서 `cl` 확인 |
| CMake가 Python 2.7을 잡음 | pixi 밖에서 실행함. 항상 `pixi run` |
| Fast DDS SHM 세그먼트 실패 | `fastdds.xml` 128→64 MiB. 그래도 안 되면 `useBuiltinTransports=true`로 D 재측정 |
| foxglove_bridge 점군 끊김 | `ros-jazzy-foxglove-bridge==3.4.3` 고정 |
| 릴레이 "Cannot connect to WSL 127.0.0.1" | ets2_bridge가 먼저 떠 있어야 함. 문구는 P1에서 바뀜 |
| 명령이 "stale"로 버려짐 | §8 원점 불일치 또는 `time.monotonic` 사용 |
