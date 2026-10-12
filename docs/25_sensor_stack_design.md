# 센서 기반 스택 설계 (P2)

[포팅 계획](20_windows_native_port_plan.md) P2의 상세입니다. 게임 GT 없이 `/sensing/*`와 카메라만으로 고속도로 ACC·차로 유지를 수행하는 스택의 경계·인터페이스·첫 알고리즘·평가 방법을 정합니다. 알고리즘 품질 자체는 이 문서의 범위가 아니고, "무엇을 어디에 어떤 메시지로" 두는지가 범위입니다.

## 1. 입력·출력

| 방향 | 토픽 | 타입 | 비고 |
| --- | --- | --- | --- |
| 입력 | `/ets2/camera/{C_FW,C_FN}/perception/image_raw`, `.../perception/camera_info` | Image, CameraInfo | 640×360 RGB8, 30 Hz. ✔ 보정값도 축소됨. 근거리 차로는 C_FW, 원거리·ACC 보조는 C_FN(§3.1) |
| 입력 | `/sensing/lidar/{L_F,L_PL,L_PR}/points` | PointCloud2 | ✔ degradation 경유(잡음·결측) |
| 입력 | `/sensing/imu/imu_raw`, `/sensing/wheels/state`, `/sensing/gnss/nav_sat_fix` | Imu, WheelState, NavSatFix | ✔ degradation 경유 |
| 입력 | `/ets2/vehicle/actuation`, `/ets2/drive/state` | ActuationState, DriveState | 적용 조향·페달·기어, 소유 상태(센서가 아니라 차량 상태) |
| 입력 | `/tf_static` | cabin→camera_optical, cabin→lidar, base_link→imu_link | ✔ 정적 장착 |
| 입력(TF) | `base_link→cabin` | 릴레이, 렌더 묶음과 함께 | 캐빈 서스펜션. 카메라·라이다 외참에 필요 |
| 출력 | `/ets2/drive/command` | DriveCommand | 50 Hz, `sent_steady_ns` |
| 출력 | `/ets2/overlay/path` | nav_msgs/Path (`base_link`) | 10–20 Hz, ≤ 256점 |
| 출력 | `/stack/lane`, `/stack/objects`, `/stack/trajectory`, `/diagnostics` | 자체 msg(아래), Detection3DArray, Path | Foxglove·bag·평가용 |
| 평가 전용 | `/ets2/ground_truth/ego/pose`, `/ets2/ground_truth/traffic`, provider 차로 JSON | | 스택 코드가 구독하지 않는다. 평가 노드만 |

사용하지 않는 것: `world→base_link`(GT TF, 릴레이 설정 `gt_localization_tf=false`), `/ets2/wheels/odometry`(이상 적분. 스택은 `/sensing/wheels/state`에서 자체 적분).

## 2. 패키지 배치

```text
ros2/src/ets2_stack/            C++ 하나의 실행 파일 ets2_stack
  src/lib/                      rclcpp 비의존. 단위 테스트 대상
    lane_ipm.*                  IPM 차로 추정(§3.1)
    cluster.*                   라이다 격자 군집·박스(§3.2)
    tracker.*                   객체 추적(상수 속도 KF)
    odometry.*                  바퀴+IMU 데드레코닝(§3.3)
    planner.*                   차로 중심 궤적 + 속도 프로파일(§3.4)
    controller.*                pure pursuit + 속도 PI(§3.5)
    virtual_eps.*               바퀴각 → 상대 조향 입력(W12)
  src/node/                     토픽 연결, 파라미터, watchdog
  config/stack.yaml
  test/                         lib 단위 테스트(gtest, MSVC ctest)
tools/lane_training/            학습 전용(별도 Python 환경, 저장소 밖 가능). 추론은 ets2_stack 안의 ONNX Runtime(§3.1 2단계)
ros2/src/ets2_stack_msgs/       Lane.msg, Objects는 vision_msgs 사용
ros2/src/ets2_eval/             Python. GT 대비 지표(§5). 스택과 분리
```

`ets2_stack`과 릴레이는 별도 프로세스다([01 P5](01_requirements_and_decisions.md)). 스택은 game pipe·공유메모리를 열지 않는다.

`Lane.msg`:

```text
std_msgs/Header header          # base_link, 카메라 stamp
bool valid
float64[4] left_poly            # y = c0 + c1 x + c2 x^2 + c3 x^3, base_link(m)
float64[4] right_poly
float64 lane_width_m
float64 valid_range_m           # 추정 유효 거리
float32 confidence
```

## 3. 모듈

### 3.1 차로 추정 (lane_ipm → DNN)

카메라 선택: ✔ 핀홀 계산으로 C_FN(HFOV 30°, pitch −1.5°, z 3.04 m)의 최하단 광선은 약 −10.1°라 노면을 카메라 앞 약 17 m부터 본다. C_FW(120°, −8°)는 약 2.4 m부터 본다. 따라서 **근거리 차로(0–30 m)는 C_FW, 원거리·ACC 보조는 C_FN**. △ 실제 CameraInfo로 재확인.

1단계(고전, P2 완료 기준): C_FW(120°, 960×544 perception 축소 영상)의 하단 ROI를 IPM으로 BEV에 펼친다. 카메라 내·외참은 ✔ CameraInfo + `/tf_static`(cabin→C_FW_optical) + `base_link→cabin`으로 정확히 안다. 노면 평면 가정(자차 접지면). BEV에서 밝기 경사·색(흰/노랑)으로 차선 픽셀 → 좌우 RANSAC 3차 다항식 → `Lane.msg`. 밤·비·터널은 1단계 비범위.

2단계: DNN. **라벨은 GT에서 자동 생성한다.** 지도 provider 차로(✔ 2D world 좌표)와 `world→gt/base_link`·TF·CameraInfo로 차선을 영상에 투영해 마스크를 만든다. 기록 bag에서 수만 장을 자동 라벨링할 수 있으므로 학습 데이터 비용이 없다. 학습은 별도 Python 환경(PyTorch CUDA)에서 하고 ONNX로 내보낸다. 추론은 `ets2_stack` 안에서 ONNX Runtime(CUDA → TensorRT → DirectML EP 순으로 시도)으로 돌려 같은 `Lane.msg`를 낸다. robostack 환경과 PyTorch CUDA를 한 환경에 섞지 않는다. Python 인지 노드는 선택 사항이다. 이 경로가 "GT를 학습에는 쓰되 주행에는 쓰지 않는" 경계다.

### 3.2 객체 (cluster + tracker)

3개 라이다 점군을 `base_link`로 합쳐(TF static+cabin) 지면 제거(자차 접지면 ±0.3 m, 경사는 z 기울기 적합) → 0.5 m 격자 점유 → 연결 성분 → AABB·속도(추적). 자체 구현한다(PCL 의존 없음, △ robostack win-64에 `pcl-conversions`는 있으나 빌드 단순화 우선). 추적은 상수 속도 KF, ID 유지 1 s. 출력 `vision_msgs/Detection3DArray`(`base_link`). ACC 대상 선택은 §3.4의 차로 좌표(s, d)에서 |d| < 차로폭/2 중 최소 s.

### 3.3 자차 운동 (odometry)

바퀴 각속도·반지름·조향각(✔ WheelState)과 IMU yaw rate로 `odom→base_link`를 적분한다. 차로 유지에는 자차 기준 상대 기하만 필요하므로 전역 위치는 쓰지 않는다. GNSS는 P2에서 로그만 하고, 경로 선택(목적지)이 생길 때 사용한다. ✔ robot_localization 3.8.3이 win-64에 있어 EKF가 필요해지면 교체 가능.

### 3.4 계획 (planner)

- 입력: `Lane`, 객체, 자차 속도.
- `Lane`은 그 stamp 이후 자차 운동(odometry)으로 제어 시각까지 **항상** 보상한 뒤 쓴다(무효일 때만이 아니라 매 주기).
- 차로 중심 다항식에서 0–80 m를 1 m 간격으로 샘플한 점열(`base_link`)을 궤적으로 삼는다. `Lane.valid=false`가 500 ms 넘으면 마지막 궤적을 odometry로 자차 기준으로 옮기며 2 s 유지하고, 그 뒤 LCC를 해제한다.
- 속도 프로파일: 목표 속도(파라미터)와 ACC(앞차 거리·속도, 시간 간격 1.5 s, 정지 간격 5 m, ✔ 기존 baseline과 같은 의미의 파라미터 `parameters.hpp:DrivingParameters`)의 최소. 곡률 기반 상한(횡가속 1.5 m/s²).
- 출력 `/stack/trajectory`와 `/ets2/overlay/path`(같은 점열, 폭은 차로폭).

### 3.5 제어 (controller + virtual_eps)

- 횡: pure pursuit(lookahead = 6 m + 0.7 s × v, ✔ baseline 값). 출력은 **바퀴각**(rad).
- 바퀴각 → W12 가상 EPS → 상대 조향 입력 [-1,1]. W11 식별 결과(`docs/26_steering_plant.md`, 예정)가 없는 동안은 ✔ baseline의 applied-steering 추종 PI(`gt_drive_node.cpp:main`의 조향 PI)를 `interim` 모드로 둔다.
- 종: 속도 PI(✔ baseline 이득 `throttle_gain .2`, `brake_gain .2`, `speed_integral_gain .08`).
- 명령: 50 Hz, `sequence` 단조 증가, `sent_steady_ns`, `axes`=LCC 1 | ACC 2.
- watchdog: `/ets2/drive/state`·센서 stamp가 200 ms 이상 늦으면 명령 중단(코어가 `expired`로 해제). Lane invalid 지속·객체 추적 소실 시 ACC만 유지하거나 전체 해제(파라미터).

### 3.6 노드 구조

단일 프로세스, 콜백은 데이터만 갱신하고 50 Hz 타이머가 planner→controller→publish를 순서대로 돈다. lane 추정(≈5 ms, △)과 ONNX 추론은 **별도 스레드(또는 Reentrant callback group + MultiThreadedExecutor)**에서 돌려 제어 타이머를 막지 않는다. 점군 3개는 stamp가 같으므로(✔ 라이다 stamp = 같은 묶음 카메라 stamp) `message_filters` 없이 최신 3개를 stamp 일치로 합친다.

## 4. 파라미터(`config/stack.yaml`, 초기값)

| 키 | 값 | 출처 |
| --- | --- | --- |
| `target_speed_mps` | 22.2 (80 km/h) | |
| `lookahead_m`, `lookahead_time_s` | 6, 0.7 | ✔ baseline |
| `following_time_s`, `standstill_gap_m` | 1.5, 5 | ✔ baseline |
| `lateral_acceleration_mps2` | 1.5 | ✔ baseline |
| `lane.roi_rows`, `lane.bev_range_m`, `lane.bev_cell_m` | 180–360, 60, 0.1 | |
| `cluster.cell_m`, `cluster.min_points`, `cluster.ground_z_tolerance_m` | 0.5, 6, 0.3 | |
| `watchdog_ms` | 200 | ✔ 코어 유효창과 일치 |

## 5. 평가 (`ets2_eval`)

같은 bag에서 스택 출력과 GT를 비교한다. 스택은 GT를 모르고, 평가 노드만 안다.

| 지표 | 계산 |
| --- | --- |
| 차선 추정 오차 | `Lane` 다항식 vs provider 차로(world→gt/base_link로 변환) 0–50 m 횡오차 RMS·p95 |
| 객체 | `Detection3DArray` vs `/ets2/ground_truth/traffic` 매칭(IoU 0.3): 재현율·거짓양성·거리별 |
| 추종 | 차로 중심 오차(`gt/base_link` 기준) p95, ✔ baseline 0.03–0.57 m와 비교 |
| ACC | 시간 간격 오차, 최소 거리 |
| 지연 | 카메라 stamp → DriveCommand `sent_steady_ns` |

## 6. 단계와 완료 기준

| 단계 | 내용 | 완료 기준 |
| --- | --- | --- |
| S1 | odometry + IPM lane + planner + controller(interim 조향), ACC 없이 | 주간·맑음·직선+완만 곡선 2 km, 개입 0, 중심 오차 p95 < 0.6 m |
| S2 | cluster+tracker, ACC | 앞차 1대 추종 2 km, 최소 거리 > 15 m at 80 km/h |
| S3 | W12 가상 EPS 연결 | S1 재측정, 오버슈트 감소 |
| S4 | DNN lane(GT 자동 라벨) | 야간·비에서 S1 기준 |
| S5 | 운전자 지시 차로 변경 | 비범위 표시, 트리거 시 설계 |

## 7. 리스크

| 리스크 | 대응 |
| --- | --- |
| IPM 평면 가정이 경사·언덕에서 깨짐 | 유효 거리 30 m로 제한, 라이다 지면 적합으로 경사 보정 |
| 카메라 외참의 캐빈 흔들림 | `base_link→cabin` TF가 렌더 묶음과 함께 오므로 같은 stamp로 보정(✔ [16](16_ros2_bridge.md)) |
| 트레일러 점군이 객체로 잡힘 | 자차 후방 −2 m 이하 제외. 굴절 데이터는 [legacy/08](legacy/08_open_questions.md) 미확인 |
| 인지 지연이 제어를 막음 | 인지는 별도 스레드, 제어 타이머는 최신 `Lane`+odometry 보상 사용. DNN은 30 Hz 입력을 10 Hz로 샘플 |
| ONNX Runtime CUDA EP와 게임 GPU 공유 | DirectML EP 대안, 추론 해상도 축소 |
