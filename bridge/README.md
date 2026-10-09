# 유로파일럿 브리지와 런처

## 실행

게임을 Steam의 **DirectX11 (64-bit)**로 실행한 뒤 [launch.cmd](launch.cmd)를 엽니다.

- **시작:** WSL Ubuntu의 ROS·Foxglove 서버를 준비하고 Windows 릴레이를 연결합니다.
- **중지:** 릴레이의 수집 소유권을 해제하고 이번 실행의 Windows·WSL 프로세스를 종료합니다.
- **재시작:** 기존 실행을 정리한 뒤 설정 파일을 다시 읽습니다.
- **창 닫기:** 이번 실행을 종료합니다.
- **다시 열기:** 기존 런처 창을 표시합니다.

화면은 SDK 연결, 프로세스 PID, ROS 상태 수신, 전송·수신 묶음 수, 큐 누락, Foxglove 주소를 표시합니다. 연결 상태는 실제 ROS 응답과 SDK 상태 메시지의 최근 수신으로 판정합니다. 센서 구독이 없으면 영상 수집을 기다립니다.

`실행 시간(초)`, `카메라 Hz`, `라이다 Hz`, `미리보기 Hz`는 시작할 때 `config/bridge.local.json`에 저장됩니다. 기본 주기는 카메라 30 Hz·라이다 10 Hz·JPEG 미리보기 10 Hz입니다. 기존 설정에 미리보기 주기가 없어도 다음 시작부터 10 Hz를 사용합니다. Windows 릴레이는 BELOW_NORMAL 우선순위로 게임에 CPU를 양보합니다. 시간이 끝나면 중지합니다. F11·게임 오류·프로세스 종료 뒤에는 사용자가 시작을 눌러 새 실행을 엽니다. 게임 실행과 운전은 사용자가 맡습니다.

기존 수동 브리지가 같은 포트를 사용하면 런처가 충돌을 표시합니다. 기존 실행을 종료한 뒤 시작합니다. 다른 WSL 작업과 게임 프로세스는 유지됩니다.

## 설정

Windows와 WSL은 같은 `config/bridge.local.json`을 읽습니다. 현재 배포판은 Ubuntu, ROS domain은 42입니다. 연결 때 Ubuntu eth0 주소를 조회합니다.

| 설정 | 용도 |
| --- | --- |
| rig / lidar / slots | 센서 프리셋과 슬롯. 현재 private 프리셋·[3,4,6,7]. []는 SDK 바퀴 기준의 상태·입력만 실행하며 카메라 장착 보정을 사용하지 않음 |
| duration_s | 실행 제한 시간 |
| camera_hz / lidar_hz | 기본 30 / 10. 라이다는 필요한 카메라 프레임에서만 gather·readback |
| preview_hz / lidar_preview_stride | 표시용 JPEG 기본 10 Hz. 표시용 점군은 수평 빔 4개마다 1개, 모든 수직 링 유지 |
| capture_warmup | 기본 true. private 센서는 유휴 뒤 한 프레임 준비하고 다음 프레임에서 수집. false는 연속 렌더 비교용 |
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
- 표시용 점군: `/ets2/lidar/{L_F,L_PL,L_PR}/preview/points`
- 전체 점군: `/ets2/lidar/{L_F,L_PL,L_PR}/points`
- 박스: `/ets2/ground_truth/{camera}/markers`
- 상태: `/ets2/vehicle/state`, `/diagnostics`, `/tf`, `/clock`
- 조작과 기어: `/ets2/vehicle/actuation`의 운전자 입력·실제 적용 입력·기어
- 추가 센서: `/ets2/imu/data_raw`, `/ets2/wheels/state`, `/ets2/wheels/odometry`, `/ets2/gnss/fix`

라이다는 Color map → range → Turbo로 설정하고 가까운 장면은 0–30m 또는 0–50m 범위를 사용합니다. 원본 RGB·depth와 `/perception/image_raw`는 ROS 소비자가 직접 구독합니다. [토픽·시각 계약](../docs/16_ros2_bridge.md).

전방 원본은 1280×720, 측면 원본은 960×544입니다. JPEG·인지용 영상은 각각 640×360·480×272입니다. HD 원본을 구독하면 readback·직렬화·전송 비용이 증가합니다. 30 Hz 설정은 수집 요청 주기이며 실제 전달률은 게임과 소비자 처리량에 따라 달라집니다.

인지용 영상은 카메라 주기를 따르고 JPEG 미리보기는 `preview_hz`를 따릅니다. 표시용 점군은 XYZ·range만 전달하며 전체 점군에서 선택한 빔의 값을 그대로 사용합니다. 기존 Foxglove 레이아웃의 점군 토픽을 `/preview/points`로 바꾸면 표시 비용이 줄어듭니다. 원본 점군의 필드·10 Hz 주기는 유지됩니다.

## 운전 명령 API

0.27.0은 공식 SCS Input SDK에 `ot_drive` 가상 3축 장치를 등록합니다. 사람의 키보드/Xbox 바인딩을 유지하면서 컴퓨터가 횡방향 조향과 종방향 가속·제동을 보냅니다. 조향은 정규화 위치 입력이며 FFB나 조향 토크 제어는 구현하지 않았습니다. 설치와 장치 등록, disarmed Xbox 좌우·RT/LT 대조, 실제 ROS arm/disarm을 확인했습니다. 자동 명령 적용·만료의 게임 시험은 진행 중입니다.

최초 등록은 정상 게임 종료 → 새 loader/core 설치 → 게임 재시작 순서입니다. SDK는 input init 때만 장치를 등록하므로 기존 loader의 hot reload로 추가할 수 없습니다. 설치된 `ot_runtime/ot_config.json`에서 `allow_drive: true`, `singleplayer_research: true`, 활성 `controls.sii`의 절대 경로 `drive_controls_path`를 설정합니다. 저장소 기본 권한은 꺼져 있으며 센서 설정과 사용자 controls.sii는 바꾸지 않습니다.

| 창구 | 계약 |
| --- | --- |
| `/ets2/drive/control` | `DriveControl` 서비스: owner, arm, epoch → success, message, epoch |
| `/ets2/drive/command` | `DriveCommand`: owner, epoch, 증가하는 sequence, command_window_ms, steering/throttle/brake |
| `/ets2/drive/state` | `DriveState`: 활성/해제 이유, owner·epoch·마지막 승인 sequence, 유효창, 명령·물리입력·SDK 적용값 |

steering은 **왼쪽 양수 [-1,1]**, throttle/brake는 **[0,1]**입니다. 현재 프로필의 steering mix가 semantical 값을 빼므로 SDK 장치에는 steering의 부호를 뒤집어 내보냅니다. 기존 `/ets2/vehicle/actuation`의 단위·부호는 유지합니다. applied 값은 게임의 effective 입력 관측이며 명령을 복사한 값이 아닙니다. 게임 입력 처리 순서에 따른 적용 지연은 게임 시험에서 확인합니다.

arm은 단일 owner에 새 epoch를 발급하고 200 ms 동안 첫 명령을 기다립니다. Command의 `command_window_ms`는 **Windows monotonic clock의 절대 만료 시각(ms)**이며 duration·ROS stamp가 아닙니다. 최신 DriveState에서 받은 값을 그대로 복사합니다. DDS/TCP/pipe에서 지연된 명령에 새 수명을 붙이지 않으며, 이전 epoch·반복 sequence·만료된 명령은 거절합니다. owner는 식별자이며 기존 TCP pairing token과 로컬 pipe ACL을 대신하지 않습니다.

명령 만료·물리 조작·F11·pause·SDK 입력 비활성·core unload·통신 단절은 arm을 해제합니다. resume/reconnect는 다시 arm하지 않습니다. F11은 bridge lease도 취소하므로 런처를 명시적으로 재시작합니다. SDK 한 입력 프레임의 세 축은 같은 snapshot이며 해제는 다음 입력 프레임의 세 축 0으로 반영합니다. 렌더 수집 on/off와 운전 arm은 독립입니다.

수동 해제는 현재 프로필의 A/Left·D/Right·W/Up·S/Down 및 `joy.x/rt/lt`를 직접 읽습니다. controls.sii의 deadzone·축 변환을 적용하고, 반대 키나 키보드/패드가 서로 상쇄돼도 각 물리 source의 활동을 보고 해제합니다. pad 부재는 프로필의 `?0` fallback대로 중립이며 키보드 조작은 유지합니다. pad 연결 변화는 arm을 해제하고 읽기 오류·지원하지 않는 binding에서는 arm을 허용하지 않습니다. 프로필 변경은 권한 재읽기 또는 재시작으로 반영합니다. 현재 프로필의 `xinput_gamepad_1`과 Windows index 0은 disarmed 좌우 stick·RT/LT 대조에서 대응했습니다. deadzone 경계의 세밀한 대조와 armed 상태의 수동 해제·F11 시험은 남아 있습니다.

2026-10-10 입력 시험 차량은 FH4였고 기존 카메라 보정은 FH5입니다. `slots: []`에서는 현재 SDK 바퀴로 base_link를 계산해 상태·입력을 연결합니다. 센서 슬롯을 선택하면 기존 차량·장착 검사로 잘못된 보정을 거절합니다. FH5 보정 파일은 변경하지 않았으며 이 시험은 IMU/GNSS 보정 검증이 아닙니다. 자동 시동 옵션이 켜져 있어 수동 RT 조작 때 엔진이 시작됐으므로, 현재 정차 자동 입력 시험의 throttle은 항상 0입니다.

### 속도 목표와 직접 조향

`drive_speed`는 GT 속도와 목표 속도의 차이로 throttle/brake를 계산하고, `steering`을 그대로 보냅니다. 첫 동작용 P 제어이며 ACC·차로 유지 제어기는 아닙니다. 실행 중 ROS parameter로 목표를 바꿀 수 있습니다. 속도 단위는 m/s, 조향은 양수 왼쪽 `[-1,1]`입니다. 엔진·D 기어·주차브레이크는 게임에서 설정합니다.

ROS 브리지가 실행 중인 WSL에서 같은 domain을 사용합니다:

```bash
source /path/to/ETS2-Autonomy-Lab/bridge/ros-env.sh
ros2 run ets2_bridge drive_speed --ros-args -p arm:=true -p target_speed_mps:=2.0 -p steering:=0.0
```

다른 터미널에서 목표를 바꿉니다:

```bash
ros2 param set /ets2_drive_speed target_speed_mps 3.0
ros2 param set /ets2_drive_speed steering 0.1
```

`arm` 기본값은 false이고, `arm:=true`를 명시한 실행만 시작합니다. Ctrl+C는 자신의 epoch를 해제합니다. 수동 조작·F11·만료·통신 단절 뒤 노드는 종료하며 자동으로 재arm하지 않습니다. `throttle_gain`·`brake_gain` 기본값 0.2는 속도 오차 1 m/s당 정규화 페달 0.2를 의미합니다. 두 페달은 동시에 적용하지 않으며 출력은 `[0,1]`입니다. gain은 차량 반응에 맞춰 조정할 수 있습니다.

CLI `topic pub --once`는 새 노드 발견 지연으로 200 ms 창을 놓칠 수 있습니다. 이미 연결된 노드에서 상태의 창을 받아 명령을 발행합니다. 다음 예는 **3초 동안 중립값만** 전송하며 실행 전에 새 장치 설치·권한·물리입력 대조가 필요합니다. 게임에 이 예를 실행한 검증은 아직 하지 않았습니다.

```python
import time
import rclpy
from ets2_msgs.msg import DriveCommand, DriveState
from ets2_msgs.srv import DriveControl

rclpy.init()
node = rclpy.create_node('drive_neutral_example')
latest = [None]
sub = node.create_subscription(DriveState, '/ets2/drive/state',
                               lambda state: latest.__setitem__(0, state), 1)
pub = node.create_publisher(DriveCommand, '/ets2/drive/command', 1)
service = node.create_client(DriveControl, '/ets2/drive/control')
epoch = 0
try:
    if not service.wait_for_service(timeout_sec=5):
        raise RuntimeError('Driving service unavailable')
    ready_until = time.monotonic() + 5
    while (latest[0] is None or pub.get_subscription_count() == 0) and time.monotonic() < ready_until:
        rclpy.spin_once(node, timeout_sec=0.05)
    if latest[0] is None or pub.get_subscription_count() == 0:
        raise RuntimeError('Driving state/command connection unavailable')
    future = service.call_async(DriveControl.Request(owner='neutral_example', arm=True))
    rclpy.spin_until_future_complete(node, future, timeout_sec=1)
    result = future.result()
    if result is None or not result.success:
        raise RuntimeError(result.message if result else 'Arm timed out')
    epoch = result.epoch
    sequence = 0
    end = time.monotonic() + 3
    while time.monotonic() < end:
        rclpy.spin_once(node, timeout_sec=0.02)
        state = latest[0]
        if state.epoch != epoch:
            continue  # Wait for the first state of this explicit arm.
        if not state.armed:
            break
        sequence += 1
        pub.publish(DriveCommand(owner='neutral_example', epoch=epoch,
                    sequence=sequence, command_window_ms=state.command_window_ms,
                    steering=0.0, throttle=0.0, brake=0.0))
finally:
    if epoch:
        future = service.call_async(DriveControl.Request(owner='neutral_example', arm=False, epoch=epoch))
        rclpy.spin_until_future_complete(node, future, timeout_sec=1)
    node.destroy_node()
    rclpy.shutdown()
```

## 기록과 재생

런처에서 브리지를 **시작**하고 연결이 확인되면 기록 종류를 선택해 **기록 시작**을 누릅니다.

| 기록 종류 | 내용·저장 위치 |
| --- | --- |
| 차량 상태 (소용량) | 운전 명령·승인/해제 상태·조작·기어·속도·자세·IMU·휠·GNSS·시계·센서 구성·진단. 기본 bridge/recordings/<run>/bag |
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

### 처리 단계별 성능 진단

별도 설정 복사본의 `diagnostic_sensor_stage`로 센서 묶음을 처리할 마지막 단계를 선택합니다. 기본값은 `publish`입니다. 모든 단계에서 같은 ROS 구독을 유지하며 상태 전송·소유권·패닉 처리는 계속 동작합니다.

| 값 | 마지막 처리 |
| --- | --- |
| gpu_copy | 센서 생성·pack 후 릴레이 GPU staging 복사와 fence 반환. 영상 Map·CPU 복사 생략 |
| readback | 영상 Map·CPU 복사까지 하고 결과 폐기 |
| encode | JPEG·ROS CDR 메시지까지 생성하고 결과 폐기 |
| publish | 실제 TCP 전송·ROS 발행까지 수행하는 일반 실행 |

`publish` 외에는 센서 토픽을 발행하지 않습니다. DLL의 작은 라이다·노출 버퍼 읽기는 모든 수집 단계에서 유지됩니다. 로그의 `consumed_bundles`, `encoded_bundles`, `sent_bundles`, `ros_received_bundles`로 실제 처리 진행을 확인합니다. 종료 뒤 일반 설정으로 실행합니다. [측정 결과](../docs/18_performance.md).

## 운영 구조

런처는 단일 Windows mutex로 중복 창을 막습니다. Windows Job Object는 릴레이와 WSL 연결 프로세스를, WSL transient systemd unit은 ROS·Foxglove·상태 구독 노드를 소유합니다. Windows heartbeat가 8초 끊기면 WSL 세션도 종료합니다. 중지는 릴레이 정상 종료 → WSL 그룹 종료 → PID·unit 상태 확인 순서입니다.

종료 확인이 실패하면 해당 WSL 실행을 화면에 남기고 새 시작을 막습니다. 중지를 다시 눌러 같은 실행의 정리를 확인합니다.

[센서 프리셋](../docs/14_phase1_highway_sensors.md), [성능과 운영 구성](../docs/18_performance.md), [주요 시행착오](../docs/history/lessons.md).
