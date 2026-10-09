# ROS 2 센서 계약

Windows C++ 릴레이가 메시지를 생성하고, WSL Jazzy의 GenericPublisher가 수신 CDR을 발행합니다. 실행은 [런처](../bridge/README.md), 성능은 [전경 실측](18_performance.md)을 따릅니다.

## 전송

- Ubuntu eth0 NAT IPv4로 직접 연결합니다. 연결 때 주소를 다시 조회합니다.
- 대용량 TCP: 영상·깊이·라이다·차량 GT·렌더 TF·frame_info.
- 상태 TCP: `/clock`, SDK 상태·IMU·휠·GNSS·자차 물리 pose·진단·정적 TF. 역방향은 구독 요구·수집 제어·응답입니다.
- 두 연결의 worker·큐는 분리하고 상태 TCP에 NODELAY를 적용합니다.
- Fast-CDR는 XCDRv1·little endian·PLAIN_CDR·serialize_encapsulation을 사용합니다. 헤더는 `00 01 00 00`입니다.
- Fast DDS SHM은 participant당 128 MiB, 최대 메시지 8 MiB입니다. 브리지·Foxglove·rosbag2에 같은 XML을 적용합니다.

상태 경로는 reliable, 영상·라이다 경로는 best-effort, 정적 구성은 transient-local입니다. 최신 완성 묶음을 우선하고 큐 누락 수를 진단에 제공합니다.

## 토픽

| 토픽 | 타입·의미 |
| --- | --- |
| /ets2/camera/{camera}/image_raw | RGB8 Image |
| /ets2/camera/{camera}/depth/image_raw | 미터 광축 깊이 32FC1 Image, 무효 NaN |
| /ets2/camera/{camera}/camera_info | 실제 projection·viewport 기반 CameraInfo |
| /ets2/camera/{camera}/preview/image/compressed | 절반 크기 JPEG |
| /ets2/camera/{camera}/preview/camera_info | JPEG 크기의 CameraInfo |
| /ets2/camera/{camera}/perception/image_raw | 절반 크기 RGB8 Image |
| /ets2/camera/{camera}/perception/camera_info | perception 보정값 |
| /ets2/lidar/{L_F,L_PL,L_PR}/points | XYZ·range·status·소스 정보 PointCloud2 |
| /ets2/ground_truth/{camera}/objects | 차량 본체 Detection3DArray |
| /ets2/ground_truth/{camera}/markers | Foxglove용 MarkerArray |
| /ets2/ground_truth/ego/pose | SDK 시각의 물리 자세 PoseStamped |
| /ets2/vehicle/state | SDK 시각·속도·입력·벡터 상태 VehicleState |
| /ets2/vehicle/actuation | 같은 SDK 프레임의 운전자 입력·실제 적용 조향/페달·기어 ActuationState |
| /ets2/imu/data_raw | 섀시 고정 IMU, specific force m/s²·각속도 rad/s, orientation 미제공 |
| /ets2/wheels/state | WheelState: 바퀴 index·rad/s·실제 조향 rad·반지름 m·접지 |
| /ets2/wheels/odometry | 바퀴 구름 제약으로 적분한 Odometry, wheel_odom 기준, TF 미발행 |
| /ets2/gnss/fix | 가상 WGS84 기준의 이상적 NavSatFix, 10 Hz |
| /ets2/sensors/config | 센서 실행 ID·단위·모델·장착점·차량 기하·가상 지리 기준 JSON String, transient-local |
| /ets2/frame_info | 세션·render_frame_id·센서 대응 FrameInfo |
| /ets2/frame_info/exposure | 같은 프레임의 gain·자동 노출 여부 FrameExposure |
| /clock, /tf, /tf_static, /diagnostics | 시뮬레이션 시각·좌표·성능 |

Camera ID는 C_FN/C_FW/C_RL/C_RR이며 슬롯과 독립적으로 지정합니다. 현재 발행 목록은 고정돼 있어 미선택 카메라의 토픽도 보입니다. 카메라·라이다 TF는 `world → base_link → cabin → sensors`, IMU·GNSS 장착 TF는 `base_link → imu_link / gnss_link`입니다. 기본 미러 0·1·2·5와 센서 출력 3·4·6·7은 분리돼 있습니다.

0.23.0부터 VehicleState 각속도는 SDK 회전/초에 2π를 곱한 rad/s입니다. 0.22.0까지 저장한 bag의 `angular_velocity_base_radps`에는 회전/초 값이 들어 있으므로 읽을 때 2π를 곱합니다. 새 bag에 이 보정을 중복 적용하지 않습니다. 센서 계산과 장착 기준은 [추가 센서](17_sensor_expansion.md)에 있습니다.

0.24.0의 ActuationState는 기존 VehicleState와 별도 메시지입니다. 조향은 좌측 양수의 정규화 [-1,1], 페달은 [0,1]이며 누락 값은 NaN입니다. 기어는 양수 전진·0 중립·음수 후진이고 각 available 필드로 미수신을 구분합니다. 적용 브레이크는 페달 제동이며 리타더·엔진·주차브레이크를 포함하지 않습니다. [런처 기록·재생](../bridge/README.md#기록과-재생).

## 수집과 노출

실제 구독 요구를 합쳐 color·depth·preview·metadata·lidar를 선택합니다. 변경은 묶음 경계에서 반영합니다. 처리 중인 GPU 자원은 소비 완료까지 유지합니다. preview와 perception은 같은 축소 출력을 공유합니다.

평상시에는 perception·JPEG·라이다를 사용합니다. 원본 RGB·depth는 구독할 때만 pack·readback·전송합니다. 라이다 구독은 GPU 깊이를 사용하고 전체 depth 영상 readback을 요구하지 않습니다. 렌더 출력 요구가 모두 사라지면 hook·stream을 해제합니다. 상태 토픽만의 구독 변경은 GPU stream을 재설정하지 않습니다.

기본 카메라 요청은 30 Hz, 라이다는 10 Hz입니다. 라이다가 예정되지 않은 묶음은 GPU gather·라이다 readback을 생략합니다. 라이다 단독 구독의 카메라 선택도 10 Hz 기회에 맞추도록 구현돼 있습니다. 현재 실측은 JPEG·라이다 동시 구독이며 다른 소비자가 없는 단독 구독 주기 확인이 남아 있습니다. 라이다 stamp는 같은 묶음의 카메라 stamp와 같습니다. IMU·휠은 새 SDK 표본마다 발행하고 보간하지 않습니다. GNSS는 시뮬레이션 시각의 100 ms 경계 이후 첫 새 표본을 사용합니다.

게임은 공유 D3D11 텍스처와 fence를 발행하고 릴레이가 자신의 device에서 staging copy·Map을 수행합니다. 라이다 작은 버퍼와 노출 표본은 현재 DLL readback 경로를 사용합니다. 공유 텍스처 소비 완료 신호는 `released = ready + 1`입니다.

자동 노출은 뷰별 log luminance 표본의 EMA로 계산합니다. 밝아질 때 0.7초, 어두워질 때 0.3초 시정수를 사용합니다. gain은 프레임별 기록합니다. 저장 sRGB를 선형값 s로 변환한 뒤 비포화 채널은 `s / ((1-s)*gain)`으로 톤매핑 이전 값을 근사할 수 있습니다. 포화와 양자화 오차는 남습니다.

## 시계·복구

SDK와 센서 stamp의 출처는 [시각 계약](04_sensors_and_data.md)에 따릅니다. 게임 pause 동안 `/clock`은 정지하고 상태 전송은 유지됩니다. pause 중 준비된 센서 묶음은 폐기합니다.

lease가 만료되면 Tier 0으로 복귀합니다. F11은 lease를 취소하고 사용자 시작을 기다립니다. 네트워크 단절은 캡처를 해제하고 eth0를 다시 조회합니다. DLL 교체는 릴레이 중지 후 메타로더에서 수행합니다.

`/ets2/capture=false`는 렌더 hook·stream을 해제하고 Tier 1 물리 읽기를 유지합니다. IMU·휠 오도메트리·GNSS는 구독과 통신에 독립적으로 계산됩니다. TCP 재연결 때 전송 세션은 바뀌고 `wheel_odom` 원점·가상 GNSS 기준점·`sensor_session`은 유지됩니다. 연결이 없던 구간의 토픽은 재전송하지 않고 최신 측정부터 발행합니다.

릴레이 중지·재시작은 새 센서 실행을 만듭니다. 게임 시각 초기화·차량 구성 변경은 릴레이를 종료하며 명시적 재시작을 요구합니다. `/ets2/sensors/config`의 `sensor_session`과 FrameInfo의 전송 세션 ID를 각각 사용합니다. ROS 메시지 스키마는 유지됩니다.

## 확인 결과

- 실제 Jazzy 역직렬화로 4뷰 RGB·깊이·JPEG·CameraInfo·GT, 3라이다와 프레임 stamp 정합을 확인했습니다.
- 같은 DSV의 GPU 깊이 복원 오차 중앙값 0.00000191m, 최대 0.00006104m였습니다.
- 원본·라이다·깊이 구독 전환과 구독 해제, relay 종료 후 lease 복구를 확인했습니다.
- Foxglove WebSocket에서 미리보기·점군·MarkerArray·TF·진단을 수신했습니다.
- MCAP의 79묶음 전체를 재생했습니다. pause 포함 기록은 clock 7,214개·preview 2,129개와 13.26초 pause를 보존했습니다.
- 런처의 중복 기록 시작·정상 종료·기록 중 창 종료를 확인했습니다. 정차 MCAP 2개의 4,775개 메시지를 ROS에서 역직렬화했고, ActuationState 670개는 VehicleState와 SDK 프레임·시각·운전자 입력이 일치했습니다. 첫 bag의 ActuationState 435개를 ROS로 재생했습니다.
- 수명 분리 후 실제 ROS에서 영상 중지 5초 동안 IMU 310·휠 오도메트리 311·GNSS 52개와 영상·라이다 0개를 수신했습니다. TCP 단절·재연결에도 동일 sensor_session과 연속 오도메트리 좌표를 확인했습니다.
- 빈 슬롯 구성의 상태 센서 수신, 원본 RGB·depth 1280×720 각각 180장 수신, 패닉 후 Tier 0·자동 재시작 중단을 확인했습니다. 정차 기능 확인이며 주행 정확도·FPS는 별도 측정합니다.
- 실제 WSL 주소 변경, 트레일러 연결·굴절, 장시간 주행은 [남은 작업](08_open_questions.md)입니다.
