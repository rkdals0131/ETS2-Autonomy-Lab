# 추가 센서와 확장 계획

core 0.23.0과 Windows 릴레이는 독립 카메라 4뷰·깊이 라이다 3개, 이상적 IMU·바퀴 측정·휠 오도메트리·가상 GNSS를 제공합니다. `MotionSensors`가 상태 측정을 계산하고 `state_messages.cpp`가 ROS CDR로 변환합니다. 잡음·바이어스·RTK 상태는 후속 작업입니다.

## 단위와 현재 확인값

- **각속도:** SDK 회전/초에 2π를 곱해 VehicleState와 IMU의 rad/s로 발행합니다. 0.22.0까지의 VehicleState bag에는 이 변환이 빠져 있습니다.
- **속도 기준점:** SDK 선형 속도는 질량중심 기준입니다. 센서 장착점 속도에는 `omega × r`를 적용합니다.
- **정차 수신:** IMU·휠 약 38.6 Hz(시뮬레이션 stamp 기준), IMU z specific force 9.799m/s², 휠 접지 4개, 휠 오도메트리 이동 약 0.32mm. 표본 사이 운동을 보간하지 않습니다.
- **GNSS:** 수정 후 두 10초 구간에서 10.000·10.017 Hz. 기본 가상 기준에서 위도·경도는 거의 0°, 높이는 약 1m였습니다.

## 센서 구성

| 센서 | 측정 생성 | 초기 주기 | ROS 출력 |
| --- | --- | --- | --- |
| IMU | 섀시 물리 속도·회전, 장착점 보정, specific force | 새 SDK 표본마다 | sensor_msgs/Imu 구현 |
| 휠·조향 | SDK 바퀴 회전속도와 실제 바퀴 조향 | 새 SDK 표본마다 | WheelState·Odometry 구현 |
| GNSS | 섀시 고정 측정점 → 가상 ENU → WGS84 | 10 Hz | NavSatFix 구현 |
| RTK | fixed·float·일반 측위·수신 불가, 상태별 오차 | 후속 | RTK 상태·오차 모델 |
| 굴절각 | 트랙터와 트레일러 연결부 상대 자세 | 후속, 목표 20–60 Hz | 각도·각속도와 GT 계획 |
| 초음파·자기계 | 근접 거리·자기장 측정 모델 | 후속 | Range / MagneticField |

측정 stamp는 `paused_simulation_time_us`입니다. StateSource가 SDK 최신 표본을 한 번씩 계산하며 실제 발행률은 게임 프레임·수신 처리에 따라 달라집니다. pause 중에는 센서 발행·적분을 멈추고 재개 첫 표본에서 차분 이력을 다시 시작합니다. 트럭 구성·게임 시각이 초기화되면 브리지를 재시작합니다. 영상 수집 중지·TCP 재연결·구독 해제에도 센서 계산과 기준점은 유지됩니다. [실행 수명](03_system_design.md#수명과-구성).

## IMU

IMU는 섀시 고정 `imu_mount_base_m`에 둡니다. 기본값 [0,0,1]m는 후륜축 지면 중심인 base_link 위 1m입니다. PhysX 질량중심 위치와 SDK 질량중심 속도·각속도로 장착점 가속도를 계산합니다.

```text
a_sensor = d(v_COM)/dt + alpha × r + omega × (omega × r)
f_sensor = R_world_sensor^T (a_sensor - g_world)
gyro_sensor = R_world_sensor^T omega
```

월드 속도 차분과 각가속도, 접선·구심 가속도를 사용합니다. SDK의 10-step 평균 가속도 채널은 VehicleState에 유지됩니다. 캐빈 IMU는 상대 운동을 추가하는 후속 구성입니다.

`imu/data_raw`는 가속도·각속도를 제공하고 orientation covariance 첫 값을 -1로 둡니다. 수평 정차 시 위쪽 가속도는 +g입니다. 현재 인위적 잡음은 없고 가속도·각속도 covariance는 모두 0입니다. ROS Imu에서 이 표기는 공분산 미상이며, 차분 오차와 주행 정확도는 아직 측정하지 않았습니다. [ROS IMU 규약](https://github.com/ros-infrastructure/rep/blob/master/rep-0145.rst).

## 휠 오도메트리

SDK 바퀴 각속도는 회전/초, 조향은 회전 단위이며 rad/s·rad로 변환합니다. 트럭 구성의 실제 wheel count만큼 채널을 등록합니다. `WheelState`는 유효 표본의 index·각속도·조향각·반지름·접지를 보냅니다. 운전자 정규화 입력인 `truck.input.steering`은 별도 항목입니다.

물리 시뮬레이션에 참여하고 접지한 바퀴의 구름 제약 `r*omega = cos(delta)*v + (-y*cos(delta)+x*sin(delta))*yaw_rate`를 최소제곱으로 풀고 평면 이동을 적분합니다. 현재 모델은 base_link 횡속도 0을 가정합니다. 후륜 조향·크랩 주행에는 횡속도를 포함한 모델과 주행 대조가 필요합니다. SDK의 바퀴별 측정은 해당 바퀴 수대로 제공합니다.

`wheel_odom` 원점은 릴레이 시작 시 0이며 통신 재연결 때 유지됩니다. GT 위치 보정이나 TF 발행은 하지 않습니다. 현재 covariance는 0으로 고정돼 있습니다. 위치 추정에 연결할 때 슬립·반지름 편차·선회 오차를 반영한 공분산이 필요합니다. 바퀴 적분 이력은 IMU 차분 이력과 분리돼 있습니다.

## GNSS와 RTK

현재 측정점은 섀시 고정 `gnss_mount_base_m`이며 기본 [0,0,1]m입니다. 기준 `gnss_reference_lla`는 첫 base_link 위치에 놓은 가상 위도·경도·WGS84 타원체 고도입니다. 기본값은 [0°,0°,0m]이고, 게임 +X가 동쪽·-Z가 북쪽입니다. ENU 변위를 ECEF로 옮겨 위도·경도·고도로 변환합니다. 기준점은 릴레이 실행 동안 유지됩니다. 게임 도시와 실유럽 지도 정합은 후속 지리 보정 작업입니다.

현재 NavSatFix는 이상적 FIX, GPS service, 0 공분산입니다. RTK fixed/float를 발행하지 않습니다. 캐빈 지붕 안테나는 실제 부착점과 SDK 시각의 캐빈 상대 운동을 연결한 뒤 추가합니다. 현재 센서 설정은 `/ets2/sensors/config`에 기록됩니다.

이상적 위치에서 시작해 RTK fixed·float·일반 측위·수신 불가를 추가합니다. 각 상태에 시간 상관 위치 오차, 공분산, 지연, 복구 시간을 둡니다. 터널·차폐 조건을 실제 맵에서 읽기 전에는 명시한 시나리오로 상태를 전환합니다. 위성 궤도·반송파 연산은 후속 범위입니다.

NavSatFix에는 WGS84 위치와 ENU 공분산을 넣고, fixed/float·보정 정보 나이는 별도 상태로 보냅니다. [NavSatFix](https://github.com/ros2/common_interfaces/blob/jazzy/sensor_msgs/msg/NavSatFix.msg), [NavSatStatus](https://github.com/ros2/common_interfaces/blob/jazzy/sensor_msgs/msg/NavSatStatus.msg).

## 트레일러

트레일러는 실제 연결부 자세와 각도를 확보한 뒤 잡음이 있는 굴절각 센서와 GT를 함께 제공합니다. 외판·굴절 가림과 모델 제출 여부를 동일 주행에서 확인합니다.

## 노이즈 모델 추가 위치

현재 노이즈 생성 코드·파라미터·난수 seed는 없습니다. 상태 센서 잡음을 추가할 위치는 `bridge/windows/motion_sensors.cpp`의 이상적 측정 계산과 `MotionSample` 반환 사이입니다. 영상 노이즈가 필요하면 JPEG 이전 GPU 영상에 적용합니다. WSL은 수신·발행을 담당합니다.

| 센서 | 모델에 넣을 특성 |
| --- | --- |
| IMU | 백색잡음·바이어스 변화·축 정렬 오차, 측정 주기에 맞춘 공분산 |
| GNSS | ENU 시간 상관 오차·RTK 상태·차폐·지연·복구 |
| 휠 | 엔코더 양자화·반지름 보정 오차·접지와 슬립에 따른 불확실성 |
| 라이다 | 거리·각도 오차·결측, 근거가 확보된 표면별 응답 |

센서별 seed·모델 파라미터·시뮬레이션 시각으로 상태를 유지하고 구독자 수가 바뀌어도 바이어스 이력은 이어갑니다. pause 동안 적분을 멈추며 게임 시각 초기화와 센서 재설정 시점을 기록합니다. 이상적 측정·오차 적용 측정·GT·추정 결과를 구분해 오차를 비교합니다. 직진·선회·제동에서 이상적 센서를 먼저 확인한 뒤 잡음과 위치 추정을 붙입니다.
