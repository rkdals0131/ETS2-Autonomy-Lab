# 시스템 구조

2026-10-12 기준. 포팅 전 구조(WSL ROS·TCP 전송)는 [legacy/03](legacy/03_system_design.md)에 있으며, P1이 끝나기 전까지는 그 구조가 실제로 실행되는 코드입니다. 단계별 변경은 [포팅 계획](20_windows_native_port_plan.md)을 따릅니다.

## 목표 구조

```text
ETS2 / Windows (게임 프로세스)
  ot_loader → ot_core
  ├─ SDK·물리 상태 → OT_State 링
  ├─ 독립 센서 pass → GPU pack → 공유 텍스처·fence + OT_Bundles
  ├─ 운전 명령 pipe → 상주 loader의 SDK 가상 3축
  └─ HUD 궤적 데칼 (main0/composition_out, 블릿 직전)  ← P3

ets2_relay (Windows, ROS 2 노드)                        ← P1
  ├─ StateSource → MotionSensors → 상태 CDR → GenericPublisher
  ├─ RenderCapture → GPU readback → 영상·라이다 CDR·JPEG → GenericPublisher
  ├─ 구독 수 → 수집 요구 / /ets2/capture 서비스
  ├─ /ets2/drive/* 구독·서비스 → core pipe
  └─ /ets2/overlay/path 구독 → SDK 자세 이력으로 world 변환 → 공유메모리 OT_Overlay

Fast DDS SHM (같은 PC)
  ├─ foxglove_bridge → Foxglove
  ├─ rosbag2 (MCAP)
  ├─ ets2_sensor_degradation: /ets2/* → /sensing/*
  ├─ 인지 노드 (Python, 같은 pixi 환경)
  └─ 자율주행 스택 프로세스 (C++): localization → planning → control
       → /ets2/drive/command, /ets2/overlay/path

Windows 지도 provider → 설치 지도 Road·현재 SDK 위치 → 차로 JSON (평가·baseline)
Windows 런처 → Job Object로 릴레이·foxglove_bridge·bag·스택 소유
```

## 실행과 소유권

- 메타로더가 SDK 플러그인으로 상주하고 기능 DLL을 교체합니다. DLL의 lease는 한 클라이언트가 내부 읽기·리그·수집을 소유하게 하며, 소유자가 사라지면 5초 뒤 Tier 0으로 복귀합니다. F11은 lease를 취소합니다. lease 없는 mutating 명령은 거절합니다.
- 운전은 별도 단일 owner/arm epoch를 씁니다. 명령은 발행 노드의 steady 시각을 싣고, 수신 쪽이 50 ms보다 오래된 명령을 버린 뒤 200 ms 유효창을 부여합니다. ACC는 페달, LCC는 조향을 소유하며 소유하지 않는 축의 사람 입력은 유지합니다. 소유 축의 수동 입력·pause·F11·연결 단절은 owner를 해제하고 자동으로 다시 arm하지 않습니다.
- 공유메모리 번들 reader는 한 프로세스만 소유합니다(`Local\OT_Bundles_Reader`). 릴레이가 그 하나이며, 다른 소비자는 ROS 토픽으로 받습니다.
- 게임 pipe는 단일 인스턴스이며 lease heartbeat·운전 명령 같은 제어 채널입니다. 표시용 궤적은 별도 공유메모리 블록(`Local\OT_Overlay`)으로 전달하고 pipe에는 태우지 않습니다.
- 런처는 Windows Job Object로 자신이 띄운 프로세스를 소유합니다. 창 종료·오류·실행 시간 만료 시 그룹을 정리합니다.
- 설정은 `bridge/config/bridge.local.json` 한 곳에서 읽고 센서 프리셋을 상대 경로로 참조합니다.

## 렌더와 전송

- 캡처 요청 하나를 다음 센서 선택 호출이 가져갑니다. 4뷰 구성에서는 선택된 네 pass의 실제 Present ID를 대조하고 완성된 묶음만 발행합니다. 구독 요구와 라이다 주기에 따라 필요한 뷰·출력을 선택합니다.
- private 센서는 유휴 뒤 준비 프레임 하나와 바로 다음 수집 프레임을 사용합니다. 준비 없이 요청 프레임만 렌더하면 나무·가로등이 사라집니다([lessons](history/lessons.md#센서-렌더-생략과-정적-물체-깜빡임)).
- 이미지는 공유 D3D11 텍스처에 pack합니다. 게임이 fence를 signal하고 릴레이가 별도 device에서 staging으로 복사해 읽습니다. 소비 완료 전 슬롯은 재사용하지 않고, 혼잡 시 오래된 묶음을 통째로 버립니다.
- 릴레이는 XCDRv1 little-endian 바이트를 직접 만듭니다. 영상·라이다는 best-effort, 상태는 reliable, 정적 구성은 transient-local입니다. Fast DDS SHM participant 세그먼트 128 MiB, 최대 메시지 8 MiB 설정을 유지합니다(Windows 재검증은 P0).
- 센서 계산은 ROS 직렬화·구독 수·소켓 상태를 참조하지 않습니다. `MotionSample`을 만든 뒤 전송 쪽이 구독된 측정만 직렬화합니다.

## 코드 경계

| 위치 | 책임 | 포팅 영향 |
| --- | --- | --- |
| ot/native | SDK·물리 읽기, 리그, GPU pack·공유 자원, 운전 입력 | 유지. P3에서 HUD 데칼 추가 |
| ot/include | core·릴레이·Python IPC가 공유하는 슬롯 소유권·좌표·장착 원점 | 유지 |
| ot/native/pass_commands.* / render_sample.* | 렌더 자세·차량 관측, 불변 표본 | 유지. P3에서 메인 pass 식별 추가 |
| bridge/windows/vehicle_profile.*, state_source.*, motion_sensors.* | 바퀴 기하·프리셋, SDK 표본 읽기, IMU·휠 오도메트리·GNSS | 유지 |
| bridge/windows/render_capture.*, gpu_readback.* | 구독 → GPU 출력 요구, 공유 텍스처 소비 | 유지 |
| bridge/windows/state_messages.cpp / image_messages.cpp | 상태·영상의 CDR·JPEG 변환 | 유지 |
| bridge/windows/game_ipc.* | 명령 pipe와 공유메모리 슬롯 | 유지 |
| bridge/windows/main.cpp / relay_workers.hpp | 프로세스 소유권, worker·큐 | P1: TCP·세션·WSL 조회 제거, rclcpp 노드 추가 |
| bridge/common/wire.hpp | 토픽 목록·QoS 분류 | P1: 전송 프레이밍 제거, 토픽 표만 유지 |
| ros2/src/ets2_msgs | 메시지·서비스 계약 | 유지 |
| ros2/src/ets2_bridge | TCP 수신·GenericPublisher | P1: 릴레이에 흡수 후 삭제 |
| ros2/src/ets2_sensor_degradation | `/ets2/*` → `/sensing/*` 잡음·지연·드롭 | 유지 |
| ros2/src/ets2_autonomy | GT baseline(동결) | 유지(평가 기준). 알고리즘 개선 없음 |
| bridge/map_lane_provider | 설치 지도 차로 JSON | 유지(평가·baseline·HUD 시험 입력) |
| apps/launcher/launcher.py | Windows UI·Job Object | P1: WSL 분기 제거, pixi 환경 프로세스 소유 |
| apps/launcher/session.py, ros_session.py, processes.py, linux_launcher.py, apps/launch.sh | WSL systemd 세션·Linux 진입점 | P1: 삭제 |
| apps/launcher/bag_recording.py | rosbag2 기록기 | P1: Windows 프로세스 제어로 이식 |
| tests/ | 플랫폼 독립 단위 테스트 | P1: Windows에서도 구성, wire_test 축소 |
| research/ | 분석 도구·원본, provider용 .NET/TruckLib | 유지 |

## 수명과 구성

릴레이가 lease·Tier 1 물리 읽기·StateSource를 소유합니다. RenderCapture는 렌더 hook과 GPU stream만 소유하며 수집 중지·구독 해제 때 해제합니다. 영상 수집을 꺼도 상태 계산·적분은 계속됩니다.

`/ets2/sensors/config`의 `sensor_session`은 릴레이 실행 동안 유지됩니다. 중지·재시작은 새 오도메트리 원점·가상 GNSS 기준점을 만듭니다. 게임 시각 초기화·차량 구성 변경·F11은 명시적 재시작을 요구합니다.

`slots: []`는 카메라·라이다 없이 상태 센서만 실행합니다. 카메라 ID C_FN·C_FW·C_RL·C_RR과 라이다 ID L_F·L_PL·L_PR은 고정이며, 장착·기준 축은 차량 프리셋으로 바꿉니다. 센서 기본은 FH4입니다. [차량 보정](14_phase1_highway_sensors.md).

GPU 생성·readback은 DX11 API를 직접 사용합니다. DX12 이식 시 교체할 범위는 렌더 후킹·GPU 자원 경로이며 센서 측정·메시지는 유지합니다.

## 다음 모듈 경계

```text
/sensing/* (카메라·라이다·IMU·휠·GNSS, 잡음 적용)
  ├─ perception (C++ IPM → ONNX Runtime) → 주변 물체·차로
  └─ localization (C++) → odom→base_link
지도 차로 + 물체·자차 → behavior/FSM → local trajectory
→ controller (PP/Stanley/MPC + 속도 PID) → 가상 EPS → /ets2/drive/command
                                           └→ /ets2/overlay/path → HUD
```

`world`(SCS X,-Z)와 세션 원점의 `wheel_odom`은 별도 좌표계이므로 위치 추정 연결 때 변환을 명시합니다. GT 입력(`ets2_autonomy`, `world→base_link`)은 센서 기반 결과와 비교할 기준으로만 유지합니다. 제어 알고리즘을 바꿔도 DLL·릴레이 계약은 유지합니다.
