# P0 스파이크 기록: Windows 네이티브 ROS 2

[포팅 계획](20_windows_native_port_plan.md) P0의 기록입니다. 처음 계획은 기존 릴레이를 그대로 두고 TCP 수신기만 Windows RoboStack Jazzy에서 빌드해 이전 구성과 번갈아 측정(A/B)하는 것이었습니다. 2026-10-12에 게임 없이 할 수 있는 확인을 끝냈고, 사용자가 이전 실행 환경을 이미 제거해 A/B는 할 수 없게 됐습니다. 그래서 P1(릴레이 노드 전환)을 바로 진행했고, 게임이 필요한 확인은 [P1 완료 기준](20_windows_native_port_plan.md#p1-릴레이를-ros-노드로-접기-m---코드-완료-2026-10-12-게임-시험-남음)으로 옮겼습니다(§5). 표기 ✔/△는 계획과 같습니다.

## 1. 준비

| 항목 | 확인 |
| --- | --- |
| MSVC | ✔ VS 2022 Build Tools 17.14(`winget install Microsoft.VisualStudio.2022.BuildTools`, VCTools 워크로드). 이전 v18 Build Tools는 폴더가 사라지고 설치 기록만 남아 있으나 vswhere `-requires VC.Tools`가 2022를 고른다 |
| pixi | ✔ 0.81.0(`winget install prefix-dev.pixi --scope user`, 관리자 불필요, `%LOCALAPPDATA%\pixi\bin`). 새 터미널부터 PATH에 잡힘 |
| 긴 경로 | ✔ `LongPathsEnabled=1` |
| PATH의 Python | ✔ `python.exe`가 C:\Python27. 모든 ROS 빌드·실행은 `pixi run`/`pixi shell` 안에서만 한다(△ 밖에서 CMake FindPython이 2.7을 잡을 수 있음) |
| 게임 | ✔ DX11, exe SHA 일치. core/loader 0.31.0 설치(W00) |
| 동시 로드 플러그인 | ✔ `Real_G27_ffb_x64.dll`. 측정 조건에 기록 |

## 2. pixi 환경

원본은 저장소 루트 `pixi.toml`과 `ros2/activate.bat`입니다. 채널 `robostack-jazzy`·`conda-forge`, `win-64`, 의존 `ros-base`·`rmw-fastrtps-cpp`·`vision-msgs`·`foxglove-bridge`·`rosbag2-storage-mcap`·`ament-cmake-gtest`·`colcon-common-extensions`·`compilers`·`nlohmann_json`·`cmake`·`ninja`. activation은 overlay(`ros2/install/setup.bat`, 있을 때), `FASTRTPS_DEFAULT_PROFILES_FILE`(`bridge/config/fastdds.xml`), foxglove DLL 경로, `ROS_DOMAIN_ID=42`·`RMW_IMPLEMENTATION=rmw_fastrtps_cpp`·`ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST`를 설정합니다. task는 `build`·`test`·`test-portable`·`relay`·`launcher`.

- ✔ 해석된 버전: python 3.12.15, ros-base 0.11.0, rclcpp 28.1.22, rmw-fastrtps-cpp 8.4.4, foxglove-bridge 3.6.0, rosbag2-storage-mcap 0.26.11, vision-msgs 4.1.1, ament-cmake-gtest 2.5.6, `compilers` → `vs2022_win-64` 19.44, cmake 4.4.4, ninja 1.13.2. `pixi.lock` 커밋.
- ✔ `pixi run build`가 Ninja·Release로 msgs·relay·degradation·autonomy를 빌드한다(처음 70 s, 증분 수십 초). 저장소에 남아 있던 Linux용 `ros2/build·install·log`(isolated 레이아웃)는 merge-install과 충돌해 지웠다.
- ✔ **foxglove-bridge DLL 경로**: RoboStack win-64의 foxglove-bridge(3.6.0과 3.4.3 둘 다)가 `foxglove.dll`·`foxglove_cpp_shared.dll`을 PATH에 없는 `Library\lib`에 설치해 `foxglove_bridge.exe`가 `0xC0000135`(DLL 없음)로 죽는다. `ros2/activate.bat`이 `Library\lib`를 PATH **맨 뒤**에 붙여 해결(그 폴더의 DLL은 이 둘뿐). 3.4.3 고정은 해결책이 아니다: 빌드 번호 _21이라 rclcpp 28.1.22→28.1.21처럼 배포판 전체가 이전 스냅숏으로 내려간다. 그래서 3.6.0(_25)을 유지한다.
- `FASTRTPS_DEFAULT_PROFILES_FILE`을 activation에 두는 이유: launch 파일이 아니라 런처가 직접 띄우는 릴레이·Foxglove·기록기·스택이 모두 같은 프로필(128 MiB SHM, 8 MiB 메시지)을 받아야 한다.

## 3. 게임 없이 확인한 것

| 항목 | 결과 |
| --- | --- |
| DDS 탐색 | ✔ (P0 당시 TCP 수신기) 다른 프로세스의 `ros2 topic list --no-daemon`에서 토픽 61개. launch 파일의 `domain_id` 기본값이 CLI 도메인과 달라 처음엔 2개만 보였음 |
| foxglove_bridge | ✔ 3.6.0이 `127.0.0.1`에서 대기(위 DLL 경로 우회 뒤) |
| 출력 캡처 | ✔ Windows에서 `ros2 topic list`를 출력 캡처로 부르면 새 ros2 daemon이 파이프를 물어 끝나지 않는다. 스크립트·런처는 `--no-daemon` |
| 릴레이 노드 | ✔ P1 이후 `ros_endpoint_test`가 게임 없이 발행·demand·arm·명령 나이·전달을 확인. 릴레이 실행 파일은 DLL 문제 없이 떠서 "Game command pipe unavailable"에서 멈춘다 |
| `drive_hold` | ✔ `--stop-event` 신호에 disarm, exit 0, 50.6 Hz |

### 3.1 rosbag2 종료 방식

게임 없이 `std_msgs/String` 20 Hz를 각 6 s 기록했습니다.

| 방법 | 결과 |
| --- | --- |
| 자기 콘솔로 띄운 기록기에 보조 프로세스가 `AttachConsole`+`GenerateConsoleCtrlEvent(CTRL_C_EVENT,0)` | ✔ exit 0, `metadata.yaml` 있음, 66개 |
| `CREATE_NEW_PROCESS_GROUP` + `CTRL_BREAK_EVENT` | ✔ exit 0xC000013A, metadata 없음, MCAP 547 B(헤더만) |
| `taskkill /T /F` | ✔ metadata 없음 |

런처 기록기는 첫 번째 방식을 씁니다(✔ `bag_recording.py`로 다시 확인: 저장 완료, 53개, metadata).

### 3.2 명령 시계

✔ pixi Python 3.12.15에서 `time.get_clock_info('monotonic')`은 `GetTickCount64()`·분해능 0.015625 s, `perf_counter`는 `QueryPerformanceCounter()`입니다. 같은 순간 `QPC×1e9/freq − perf_counter_ns` = −700 ns(호출 간격), `monotonic_ns − perf_counter_ns` = **−35.5 ms**였습니다. ✔ C++ 쪽도 같습니다: `drive_hold`(MSVC `steady_clock`)가 찍은 `sent_steady_ns`를 Python 모의 수신기가 `perf_counter_ns`로 빼면 최대 0.80 ms(50 Hz, 83개). Python 발행기는 `perf_counter_ns`만 씁니다. [16 시계](16_ros2_bridge.md#시계복구).

## 4. 실행 순서(현재)

```powershell
pixi run build          # 저장소 루트
pixi run test
.\apps\launch.cmd       # 런처: Foxglove 서버 + 릴레이 노드
```

## 5. 게임이 필요한 확인(P1 완료 기준으로 이동)

| # | 확인 | 방법 | 결과 |
| --- | --- | --- | --- |
| A | Foxglove 수신 | `ws://127.0.0.1:8765`. JPEG 4·표시용 점군 3·TF·`/diagnostics` 패널 | |
| B | 전체 점군 통과 | `pixi shell`에서 `ros2 topic hz /ets2/lidar/L_F/points` ≈ 10 Hz, `/diagnostics`의 `queue_dropped` 0 | |
| C | bag 기록·재생 | 런처의 차량 상태 기록 → 종료 → `ros2 bag play`. pause 중 `/clock` 정지, 재생 뒤 TF 체인 | |
| D | 성능 | §6. frame time p50/p99·CPU/GPU busy 기록, 이전 0.26.1 표([18](18_performance.md))와 비교만 | |
| E | 명령 나이 | 런처 화면·릴레이 JSON `drive_commands.command_age` p99 < 20 ms, `stale_commands` 증가 없음 | |
| F | `ets2_sensor_degradation` | `ros2 launch ets2_sensor_degradation sensor_degradation.launch.py` 후 `/sensing/imu/imu_raw` 수신 | |

## 6. 성능 측정

같은 정차 장면(FH4)에서 JPEG 10 Hz·표시용 라이다 10 Hz·GT 30 Hz, `diagnostic_sensor_stage=publish`, Foxglove 화면 종료 상태로, `pixi shell`의 `ros2 topic hz`로 JPEG 4·preview 점군 3·objects 4를 구독합니다. 게임 전경 60 s. 측정 도구는 PresentMon(frame time p50/p99, GPU busy)과 릴레이 stdout(`queue_dropped`, `drive_commands`), 작업 관리자 CPU입니다. 기록할 조건: `config.cfg`의 vsync·프레임 제한(✔ 센서 없는 주행에서 약 60/s로 포화 가능, △ 리미터), `Real_G27_ffb_x64.dll` 로드 여부, 해상도·`r_scale`. 결과는 `research/live/2026-10-XX-windows-native/`에 두고 [18](18_performance.md)에 한 줄 추가합니다. FPS 단일 수치로 판정하지 않습니다.

## 7. 예상 문제

| 증상 | 원인·대응 |
| --- | --- |
| `colcon build`가 cl.exe를 못 찾음 | VS 2022 Build Tools 미설치 또는 pixi 밖 실행. `pixi shell`에서 `where cl` |
| CMake가 Python 2.7을 잡음 | pixi 밖에서 실행함. 항상 `pixi run` |
| foxglove_bridge가 바로 종료(0xC0000135) | `ros2/activate.bat`의 `Library\lib` PATH 줄이 빠짐 |
| Fast DDS SHM 세그먼트 실패 | `fastdds.xml` 128→64 MiB. 그래도 안 되면 `useBuiltinTransports=true`로 D 재측정 |
| 명령이 "stale"로 버려짐 | 발행기가 `time.monotonic`을 씀(§3.2) |
| 스크립트의 `ros2 topic list`가 끝나지 않음 | `--no-daemon` 없이 출력 캡처 |
