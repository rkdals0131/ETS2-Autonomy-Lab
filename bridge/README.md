# 유로파일럿 브리지와 런처

## 실행

게임을 Steam의 **DirectX11 (64-bit)**로 실행한 뒤 [launch.cmd](launch.cmd)를 엽니다.

- **시작:** WSL Ubuntu의 ROS·Foxglove 서버를 준비하고 Windows 릴레이를 연결합니다.
- **중지:** 릴레이의 수집 소유권을 해제하고 이번 실행의 Windows·WSL 프로세스를 종료합니다.
- **재시작:** 기존 실행을 정리한 뒤 설정 파일을 다시 읽습니다.
- **창 닫기:** 이번 실행을 종료합니다.
- **다시 열기:** 기존 런처 창을 표시합니다.

화면은 SDK 연결, 프로세스 PID, ROS 상태 수신, 전송·수신 묶음 수, 큐 누락, Foxglove 주소를 표시합니다. 연결 상태는 실제 ROS 응답과 SDK 상태 메시지의 최근 수신으로 판정합니다. 센서 구독이 없으면 영상 수집을 기다립니다.

`실행 시간(초)`, `카메라 Hz`, `라이다 Hz`는 시작할 때 `config/bridge.local.json`에 저장됩니다. 기본 주기는 카메라 30 Hz·라이다 10 Hz입니다. Windows 릴레이는 BELOW_NORMAL 우선순위로 게임에 CPU를 양보합니다. 시간이 끝나면 중지합니다. F11·게임 오류·프로세스 종료 뒤에는 사용자가 시작을 눌러 새 실행을 엽니다. 게임 실행과 운전은 사용자가 맡습니다.

기존 수동 브리지가 같은 포트를 사용하면 런처가 충돌을 표시합니다. 기존 실행을 종료한 뒤 시작합니다. 다른 WSL 작업과 게임 프로세스는 유지됩니다.

## 설정

Windows와 WSL은 같은 `config/bridge.local.json`을 읽습니다. 현재 배포판은 Ubuntu, ROS domain은 42입니다. 연결 때 Ubuntu eth0 주소를 조회합니다.

| 설정 | 용도 |
| --- | --- |
| rig / lidar / slots | 센서 프리셋과 슬롯. 현재 private 프리셋·[3,4,6,7]. []는 상태 센서만 실행 |
| duration_s | 실행 제한 시간 |
| camera_hz / lidar_hz | 기본 30 / 10. 라이다는 필요한 카메라 프레임에서만 gather·readback |
| imu_mount_base_m / gnss_mount_base_m | 섀시 고정 장착점, base_link 기준 m. 기본 [0,0,1] |
| gnss_reference_lla | 시작 위치의 가상 기준 위도·경도(deg)·타원체 고도(m), 기본 [0,0,0] |
| recording_root | 차량 상태 기록 폴더. 생략하면 bridge/recordings |
| sensor_recording_root | 영상·라이다 기록 폴더. 사용할 때 명시적으로 지정 |
| token / state_port / bulk_port | 양쪽 연결 설정 |
| auto_exposure / color_gain | 자동 노출 또는 수동 gain |
| shared_gpu | 기본 true, 공유 GPU 텍스처 전달 |

설정 변경은 중지 후 적용합니다. `configure.py`는 최초 파일을 만들고 기존 설정을 보존합니다. token은 로컬 파일에만 보관합니다.

원본 RGB·depth는 기본 화면에서 구독하지 않습니다. `/perception/image_raw`는 인지용 축소 RGB, `/preview/image/compressed`는 표시용 JPEG입니다. 원본 `/image_raw`와 `/depth/image_raw`를 구독할 때만 고해상도 pack·readback·전송을 수행합니다. 라이다만 필요하면 GPU 깊이에서 빔을 샘플링하고 전체 깊이 영상은 내려받지 않습니다. `ros2 bag record -a`는 원본 토픽도 구독하므로, 평상시 기록은 필요한 토픽을 지정합니다.

`/ets2/capture` 서비스로 렌더 수집을 중지해도 IMU·휠·GNSS 계산은 계속됩니다. TCP 재연결에도 적분과 지리 기준점을 유지합니다. 런처의 중지·재시작은 센서 실행 전체를 끝내며 다음 시작에서 새 기준점을 만듭니다. `/ets2/sensors/config`의 `sensor_session`으로 이 수명을 구분합니다.

## 최초 준비

Windows에서 `ot/build.cmd`, `bridge/build.cmd`를 실행합니다. 설치된 DLL 배치는 [ot 문서](../ot/README.md)를 따릅니다.

```powershell
py -3.13 bridge/configure.py
.\bridge\build.cmd
```

WSL Ubuntu 24.04에는 ROS 2 Jazzy ros-base, vision-msgs, foxglove-bridge, rosbag2-storage-mcap, rmw-fastrtps-cpp, colcon, CMake, nlohmann-json3-dev를 설치합니다. 런처는 WSL의 systemd user manager를 사용합니다.

```bash
REPO=/mnt/c/path/to/ETS2-Autonomy-Lab  # 실제 저장소 경로
# Bash에서 실행. Zsh에서는 setup.zsh를 사용합니다.
source /opt/ros/jazzy/setup.bash
mkdir -p ~/ets2-ros
cd ~/ets2-ros
colcon build --base-paths "$REPO/bridge/ros2" --executor sequential --cmake-args -DCMAKE_BUILD_TYPE=Release
```

`bridge/ros-env.sh`는 Bash·Zsh에 맞는 ROS setup을 선택합니다. 다른 ROS 소비자도 이 환경을 불러 domain 42와 같은 DDS SHM 프로필을 사용합니다.

## WSL에서 토픽 확인

런처에서 **시작**한 뒤 해당 WSL 터미널에서 실행합니다.

```bash
source "$REPO/bridge/ros-env.sh"
echo "$ROS_DOMAIN_ID"                 # 42
ros2 topic list --no-daemon
```

`/opt/ros/jazzy/setup.*`만 불러오면 프로젝트 domain 42와 메시지 overlay가 빠집니다. `--no-daemon`은 이전 domain에서 시작된 ROS CLI daemon의 목록을 피합니다. 브리지가 중지된 상태에는 발행자가 없습니다.

## Foxglove

Windows Foxglove에서 런처의 `ws://<Ubuntu-eth0-IP>:8765`에 연결합니다.

- 영상: `/ets2/camera/{C_FN,C_FW,C_RL,C_RR}/preview/image/compressed`
- 점군: `/ets2/lidar/{L_F,L_PL,L_PR}/points`
- 박스: `/ets2/ground_truth/{camera}/markers`
- 상태: `/ets2/vehicle/state`, `/diagnostics`, `/tf`, `/clock`
- 조작과 기어: `/ets2/vehicle/actuation`의 운전자 입력·실제 적용 입력·기어
- 추가 센서: `/ets2/imu/data_raw`, `/ets2/wheels/state`, `/ets2/wheels/odometry`, `/ets2/gnss/fix`

라이다는 Color map → range → Turbo로 설정하고 가까운 장면은 0–30m 또는 0–50m 범위를 사용합니다. 원본 RGB·depth와 `/perception/image_raw`는 ROS 소비자가 직접 구독합니다. [토픽·시각 계약](../docs/16_ros2_bridge.md).

전방 원본은 1280×720, 측면 원본은 960×544입니다. JPEG·인지용 영상은 각각 640×360·480×272입니다. HD 원본을 구독하면 readback·직렬화·전송 비용이 증가합니다. 30 Hz 설정은 수집 요청 주기이며 실제 전달률은 게임과 소비자 처리량에 따라 달라집니다.

## 기록과 재생

런처에서 브리지를 **시작**하고 연결이 확인되면 기록 종류를 선택해 **기록 시작**을 누릅니다.

| 기록 종류 | 내용·저장 위치 |
| --- | --- |
| 차량 상태 (소용량) | 조작·기어·속도·자세·IMU·휠·GNSS·시계·센서 구성·진단. 기본 bridge/recordings/<run>/bag |
| 영상·라이다 포함 | 차량 상태 + 4뷰 JPEG·보정값·3라이다·차량 GT·렌더 TF·프레임 대응. sensor_recording_root 지정 필요 |

두 종류 모두 원본 RGB·depth는 구독하지 않습니다. 상태 기록만 켜면 카메라 수집을 요구하지 않습니다. 영상·점군의 실제 수신 여부는 선택한 슬롯과 센서 구성에 따릅니다.

**기록 종료**는 파일을 마무리하고 브리지를 유지합니다. 브리지 중지·창 닫기·실행 시간 만료도 기록기를 종료합니다. 중복 시작은 기존 기록을 유지합니다. 화면에는 실행 상태·파일 크기·경로, 종료 후 실제 저장 메시지 수를 표시합니다. 저장 폴더 오류와 기록기 종료는 화면에 나타나며 자동으로 새 기록을 시작하지 않습니다.

저장 경로 설정은 WSL 절대 경로 또는 Windows 드라이브 경로를 받습니다. 지정한 폴더가 존재하고 여유 공간이 있어야 시작하며, 남은 공간이 1 GiB 아래로 내려가면 기록을 마무리합니다. 기본 차량 상태 기록은 Git에서 제외됩니다. 영상·점군용 폴더는 해당 PC의 저장 장치와 용량을 확인해 선택합니다.

기록은 기본 수신 시각을 사용합니다. `--use-sim-time`을 넣지 않습니다. 재생 전 런처에서 중지하고 다음을 실행합니다.

```bash
ros2 bag info '<런처에서 복사한 bag 경로>'
ros2 bag play '<런처에서 복사한 bag 경로>'
```

기록된 `/clock`을 사용하므로 `--clock`을 넣지 않습니다. 소비자는 `use_sim_time=true`로 설정합니다. 이전 실험 bag은 기존 `~/ets2-data/bags/`에 보존돼 있습니다.

첫 수동 주행은 차량 상태 기록으로 정차·직진·일정 속도·완만한 가속·타력 주행·제동·좌우 선회를 담습니다. 이 기록으로 센서 정합과 차량 반응을 대조합니다. 영상 가림·거리 측정 누락 확인에는 영상·라이다 기록을 별도로 사용합니다. 컴퓨터가 보내는 운전 명령의 적용·해제는 M8에서 시험합니다.

## 수동 실행

런처를 중지한 상태에서 개발용으로 사용합니다.

```bash
source "$REPO/bridge/ros-env.sh"
ros2 launch ets2_bridge bridge.launch.py config:="$REPO/bridge/config/bridge.local.json"
```

```powershell
.\bridge\dist\ets2_relay.exe .\bridge\config\bridge.local.json
```

`/ets2/capture` SetBool 서비스로 렌더 수집을 중지·재개합니다. 상태 센서·SDK·clock은 유지됩니다. 서비스 응답은 요청 접수이고 `/diagnostics`의 `capture_active`가 적용 상태입니다. F11 이후에는 릴레이를 다시 시작합니다.

## 운영 구조

런처는 단일 Windows mutex로 중복 창을 막습니다. Windows Job Object는 릴레이와 WSL 연결 프로세스를, WSL transient systemd unit은 ROS·Foxglove·상태 구독 노드를 소유합니다. Windows heartbeat가 8초 끊기면 WSL 세션도 종료합니다. 중지는 릴레이 정상 종료 → WSL 그룹 종료 → PID·unit 상태 확인 순서입니다.

종료 확인이 실패하면 해당 WSL 실행을 화면에 남기고 새 시작을 막습니다. 중지를 다시 눌러 같은 실행의 정리를 확인합니다.

[센서 프리셋](../docs/14_phase1_highway_sensors.md), [성능과 운영 구성](../docs/18_performance.md), [주요 시행착오](../docs/history/lessons.md).
