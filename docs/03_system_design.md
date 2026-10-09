# 시스템 구조

```text
ETS2 / Windows
  ot_loader → ot_core
  ├─ SDK·물리 상태 → OT_State
  └─ 독립 센서 pass → GPU pack → 공유 텍스처·fence + OT_Bundles
                         ↓
Windows C++ 릴레이
  ├─ StateSource → MotionSensors → 상태 CDR
  ├─ RenderCapture → GPU readback → 영상·라이다 CDR·JPEG
  ├─ 대용량 TCP: 영상·라이다·GT·렌더 TF·frame_info
  └─ 상태 TCP: SDK·IMU·휠·GNSS·clock·진단 / 역방향 구독·수집 제어
                         ↓ Ubuntu eth0 NAT 직접 주소
WSL ROS 2 Jazzy
  GenericPublisher → Fast DDS SHM → 소비자·rosbag2
                         └─ foxglove_bridge → Windows Foxglove

Windows 런처 → WSL ROS/Foxglove 소유 그룹 + Windows 릴레이
              ← 프로세스 종료·ROS 상태 수신·왕복 응답·수집 카운터
```

## 실행과 소유권

- 메타로더가 SDK 플러그인으로 상주하고 기능 DLL을 교체합니다.
- DLL의 lease는 한 클라이언트가 내부 읽기·리그·수집을 소유하게 합니다. 소유자가 사라지면 5초 뒤 Tier 0으로 복귀합니다.
- F11은 lease를 취소합니다. 재시작은 사용자 조작으로 수행합니다.
- 런처는 Windows Job Object와 WSL systemd unit으로 자신의 프로세스를 관리합니다. 창 종료·오류·실행 시간 만료 시 해당 그룹을 정리합니다.
- 설정은 로컬 JSON 한 곳에서 읽고 센서 프리셋을 상대 경로로 참조합니다. 프로세스·응답·수신 시각이 화면 상태의 기준입니다.

## 렌더와 전송

캡처 요청 하나를 다음 센서 선택 호출이 가져갑니다. 4뷰 구성에서는 선택된 네 pass의 실제 Present ID를 대조하고 완성된 묶음만 발행합니다. 구독 요구와 라이다 주기에 따라 필요한 뷰·출력을 선택하며 비수집 프레임의 상세 관측을 건너뜁니다.

이미지는 공유 D3D11 텍스처에 pack합니다. 게임은 fence를 signal하고, 릴레이가 별도 device에서 staging으로 복사해 읽습니다. 소비 완료 전 슬롯은 재사용하지 않습니다. CPU 공유 슬롯은 소유 버퍼로 복사한 즉시 반환합니다. 혼잡 시 대기 중인 오래된 묶음을 통째로 버립니다.

상태와 영상의 송수신 worker·큐는 분리합니다. 상태 연결은 TCP_NODELAY를 사용합니다. WSL 주소는 연결 때 eth0에서 다시 조회합니다.

## ROS 소비자

Windows가 XCDRv1 메시지를 만들고 WSL은 GenericPublisher로 발행합니다. 영상·라이다는 best-effort, SDK·IMU·휠·GNSS 상태는 reliable, 정적 TF·구성은 transient-local입니다. DDS SHM participant 세그먼트는 128 MiB, 최대 메시지는 8 MiB입니다.

Foxglove는 JPEG·라이다·GT·TF·상태를 구독합니다. 원본 RGB·depth와 축소 perception 영상은 ROS 소비자가 선택합니다. 실제 구독이 GPU pack·readback 요구로 연결됩니다.

## 코드 경계

| 위치 | 책임 |
| --- | --- |
| ot/native | SDK·물리 읽기, 리그·pass 관측, GPU pack·공유 자원 |
| bridge/windows/vehicle_profile.* | SDK 바퀴 기하·기준 축·수동 장착 프리셋 해석, 라이다 소스 연결 |
| bridge/windows/state_source.* | SDK 최신 표본 읽기, 센서 계산 스레드와 실행 수명 |
| bridge/windows/motion_sensors.* | IMU·휠 오도메트리·가상 GNSS 측정과 적분 이력 |
| bridge/windows/render_capture.* | 구독 → GPU 출력 요구, 렌더 hook·stream 시작·중지 |
| bridge/windows/gpu_readback.* | 공유 D3D11 텍스처·fence 소비 |
| bridge/windows/state_messages.cpp / image_messages.cpp | 상태·영상 측정의 ROS CDR·JPEG 변환 |
| bridge/windows/game_ipc.* | 게임 명령 pipe와 공유 메모리 소유 슬롯 |
| bridge/windows/main.cpp / relay_workers.hpp | 프로세스 소유권, 두 TCP 연결, worker·큐와 재연결 |
| bridge/common/wire.hpp / bridge/ros2 | 토픽·전송 계약, GenericPublisher·구독 요구 전달 |
| bridge/launcher.py / wsl_session.py | Windows·WSL 실행 관리와 실제 연결 상태 표시 |

센서 계산은 ROS 직렬화·구독 수·소켓 상태를 참조하지 않습니다. `MotionSample`을 만든 뒤 전송 쪽이 구독된 측정만 직렬화합니다. 센서 잡음은 측정 계산 단계에 추가하고, ROS 메시지 형식과 전송 코드는 유지할 수 있습니다.

## 수명과 구성

릴레이 프로세스가 lease·Tier 1 물리 읽기·StateSource를 소유합니다. RenderCapture는 렌더 hook과 GPU stream만 소유하며, 수집 중지·구독 해제·네트워크 단절 때 해제합니다. 영상 수집을 꺼도 상태 계산과 적분은 계속됩니다. 소켓 재연결은 새 전송 세션을 만들고 기존 StateSource를 사용합니다.

`/ets2/sensors/config`의 `sensor_session`은 릴레이 실행 동안 유지됩니다. 중지·재시작은 새 오도메트리 원점·가상 GNSS 기준점을 만듭니다. 게임 시각 초기화·차량 구성 변경·F11은 명시적 재시작을 요구합니다. 센서 기준점을 바꾸는 동작과 네트워크 복구를 이 경계로 구분합니다.

`slots: []`는 카메라·라이다 없이 상태 센서만 실행합니다. 기존 뷰는 slots로 선택하고 출력은 ROS 구독으로 활성화합니다. 원본 RGB·depth 구독이 없으면 고해상도 readback·전송을 생략합니다. 라이다 소스인 깊이 렌더는 해당 라이다 요구에 따라 유지합니다.

현재 카메라 ID C_FN·C_FW·C_RL·C_RR과 라이다 ID L_F·L_PL·L_PR은 고정입니다. 새 이름·개수에는 토픽 계약과 출력 요구 매핑을 함께 확장합니다. 장착·기준 축은 차량 프리셋으로 바꾸며, FH5와 선호 차종의 수동 보정을 지원합니다. Python 연구 도구와 C++ 릴레이는 같은 프리셋과 기준 축 선택 규칙을 사용합니다. [차량 보정 절차](14_phase1_highway_sensors.md).

GPU 생성·readback은 DX11 API를 직접 사용합니다. DX12 이식 시 교체할 범위는 게임 렌더 후킹·GPU 자원 경로이며 센서 측정·ROS 메시지·전송 계약은 유지하도록 분리합니다. [개발 순서](13_game_operating_table.md), [ROS 계약](16_ros2_bridge.md).
