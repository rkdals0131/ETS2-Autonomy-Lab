# ROS 2 토픽 계약

Windows 릴레이가 메시지를 만들고 ROS 2 Jazzy에 발행합니다. 전송 계층은 포팅 중입니다. 현재는 WSL의 `ets2_bridge`가 TCP로 받아 GenericPublisher로 발행하고([legacy/16 전송](legacy/16_ros2_bridge.md#전송)), P1 이후에는 릴레이 자신이 노드가 되어 직접 발행합니다([포팅 계획](20_windows_native_port_plan.md)). 아래 토픽·QoS·수집·시계·복구 계약은 두 경우에 같습니다.

## 토픽

| 토픽 | 타입·의미 | QoS |
| --- | --- | --- |
| /ets2/camera/{camera}/image_raw | RGB8 Image | BE |
| /ets2/camera/{camera}/depth/image_raw | 미터 광축 깊이 32FC1, 무효 NaN | BE |
| /ets2/camera/{camera}/camera_info | 실제 projection·viewport 기반 CameraInfo | BE |
| /ets2/camera/{camera}/preview/image/compressed | 절반 크기 JPEG, 기본 10 Hz | BE |
| /ets2/camera/{camera}/preview/camera_info | JPEG 크기의 CameraInfo | BE |
| /ets2/camera/{camera}/perception/image_raw | 절반 크기 RGB8 | BE |
| /ets2/camera/{camera}/perception/camera_info | perception 보정값 | BE |
| /ets2/lidar/{L_F,L_PL,L_PR}/points | XYZ·range·status·소스 PointCloud2 | BE |
| /ets2/lidar/{name}/preview/points | 표시용, 수평 빔 4개마다 1개, 10 Hz | BE |
| /ets2/ground_truth/{camera}/objects | 차량 본체 Detection3DArray (평가 전용) | BE |
| /ets2/ground_truth/{camera}/markers | Foxglove용 MarkerArray | BE |
| /ets2/ground_truth/ego/pose | SDK 시각 물리 자세 PoseStamped (평가 전용) | R |
| /ets2/ground_truth/traffic | world 교통 GT TrafficState (baseline 전용) | R |
| /ets2/vehicle/state | SDK 시각·속도·입력·벡터 상태 VehicleState | R |
| /ets2/vehicle/actuation | 운전자 입력·적용 조향/페달·기어 ActuationState | R |
| /ets2/drive/state | owner·epoch·sequence·arm·유효창·적용값 DriveState | R |
| /ets2/drive/command | 역방향 normalized 3축 DriveCommand, `sent_steady_ns` 필수 | BE depth 1 |
| /ets2/imu/data_raw | 섀시 고정 IMU, specific force·각속도, orientation 없음 | R |
| /ets2/wheels/state | 바퀴 index·rad/s·실제 조향 rad·반지름·접지 | R |
| /ets2/wheels/odometry | 바퀴 구름 적분 Odometry, `wheel_odom` 기준, TF 미발행 | R |
| /ets2/gnss/fix | 가상 WGS84 NavSatFix, 10 Hz | R |
| /ets2/sensors/config | 센서 실행 ID·단위·모델·장착·차량 기하·지리 기준 JSON | R TL |
| /ets2/frame_info, /ets2/frame_info/exposure | 세션·render_frame_id·센서 대응, gain | BE |
| /clock, /tf, /tf_static, /diagnostics | 시뮬레이션 시각·좌표·진단 | R, R, R TL, R |

서비스: `/ets2/drive/control`(`ets2_msgs/srv/DriveControl`, arm/disarm/axes), `/ets2/capture`(`std_srvs/srv/SetBool`). 스택이 보는 `/sensing/*`는 `ets2_sensor_degradation`이 만듭니다([ros2/README](../ros2/README.md)).

TF는 `base_link → cabin → sensors`, `base_link → imu_link / gnss_link`, `world → gt/base_link`(항상), `world → base_link`(`gt_localization_tf`일 때)입니다. 동적 TF는 SDK 프레임마다 상태 경로에서 reliable로 나오고, cabin TF는 렌더 묶음과 함께 나옵니다. 각속도 단위·ActuationState 부호 등 메시지 의미는 `ros2/src/ets2_msgs`의 주석이 원본입니다. 0.22.0 이전 bag의 `angular_velocity_base_radps`는 회전/초 값이므로 읽을 때 2π를 곱합니다.

## 수집과 노출

실제 구독을 합쳐 color·depth·preview·display·pose·metadata·lidar를 선택합니다. CameraInfo 요청은 pose만 켜고, `/tf` 구독만으로는 렌더 수집을 시작하지 않습니다. objects·markers 요청은 해당 카메라의 차량 목록을 읽습니다. 변경은 묶음 경계에서 반영합니다.

평상시에는 perception·JPEG·라이다를 사용합니다. 원본 RGB·depth는 구독할 때만 pack·readback합니다. 라이다는 GPU 깊이를 사용하고 전체 depth 영상 readback을 요구하지 않습니다. 렌더 출력 요구가 모두 사라지면 hook·stream을 해제합니다. 기본 카메라 30 Hz, 라이다 10 Hz, JPEG 10 Hz이며 라이다 stamp는 같은 묶음의 카메라 stamp와 같습니다. IMU·휠은 새 SDK 표본마다, GNSS는 시뮬레이션 시각 100 ms 경계 이후 첫 표본을 씁니다.

자동 노출은 뷰별 log luminance EMA로 계산합니다(밝아질 때 0.7 s, 어두워질 때 0.3 s). gain은 프레임별 기록합니다.

## 시계·복구

`/clock`과 센서 stamp는 `paused_simulation_time_us`입니다. pause 동안 `/clock`은 정지하고 상태 전송은 유지되며 준비된 센서 묶음은 폐기합니다. 운전 명령·watchdog은 steady clock입니다. `DriveCommand.sent_steady_ns`는 발행 프로세스의 monotonic 시각이며 수신 쪽(현재 `ets2_bridge`, P1 이후 릴레이 노드)이 자기 steady clock과의 차이로 `max_command_age_ms`(기본 50)보다 오래된 명령을 버립니다. 따라서 발행자와 수신자가 **같은 원점의 시계**를 써야 합니다. Windows에서는 QPC 기반이어야 합니다: C++ `std::chrono::steady_clock`(MSVC=QPC), Python `time.perf_counter_ns()`. △ Python 3.12의 `time.monotonic`은 GetTickCount64 기반(15.6 ms 분해능)이라 쓰면 `age<0` 판정으로 버려질 수 있습니다. 원점 일치는 [22 §8](22_p0_spike_runbook.md#8-명령-시계-확인)에서 확인합니다.

lease가 만료되면 Tier 0으로 복귀합니다. F11은 lease를 취소하고 사용자 시작을 기다립니다. `/ets2/capture=false`는 렌더 hook·stream을 해제하고 Tier 1 물리 읽기·IMU·휠·GNSS를 유지합니다. 릴레이 중지·재시작은 새 `sensor_session`·오도메트리 원점·GNSS 기준점을 만들며, 게임 시각 초기화·차량 구성 변경은 명시적 재시작을 요구합니다.

## 확인 결과(요약)

Jazzy 역직렬화로 4뷰 RGB·깊이·JPEG·CameraInfo·GT, 3라이다와 stamp 정합을 확인했습니다. GPU 깊이 복원 오차 중앙값 0.00000191 m. MCAP 79묶음 전체 재생, pause 포함 기록의 clock 정합, ActuationState·VehicleState 프레임 일치, 영상 중지 중 상태 센서 유지, 재연결 후 동일 sensor_session을 확인했습니다. 세부 수치는 [legacy/16 확인 결과](legacy/16_ros2_bridge.md#확인-결과)에 있습니다. 포팅 후 Windows에서 같은 항목을 다시 확인합니다(P0 완료 기준).
