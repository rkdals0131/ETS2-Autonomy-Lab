# 시스템 구조

```text
ETS2 / Windows
  ot_loader → ot_core
  ├─ SDK·물리 상태 → OT_State
  └─ 독립 센서 pass → GPU pack → 공유 텍스처·fence + OT_Bundles
                         ↓
Windows C++ 릴레이
  ├─ GPU readback·센서 측정 계산·CDR·JPEG
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

## 코드 경계와 다음 정리

| 위치 | 현재 책임 | 필요한 정리 |
| --- | --- | --- |
| ot/native | 로더·SDK·물리 읽기·리그·pass·GPU pack·capture stream을 분리 | 상태 센서의 내부 읽기와 영상 수집 활성 상태 분리 |
| bridge/windows/main.cpp | 설정·리그 좌표 계산·소유권·구독·큐·TCP worker | 차량 프로필·캘리브레이션을 세션 관리에서 분리 |
| bridge/windows/messages.cpp | 좌표 수학·IMU·오도메트리·GNSS·점군·JPEG·CDR | 센서 측정 생성과 ROS 직렬화 분리 |
| bridge/common/wire.hpp | 고정 토픽 목록과 전송 계약 | 센서 구성 목록에서 토픽·TF·의존성 생성 |
| bridge/ros2 | CDR 발행·ROS 구독 수 전달·메시지 정의 | Windows가 보낸 센서 구성과 발행 목록 연결 |
| bridge/launcher.py, wsl_session.py | 실제 상태를 확인하는 Windows·WSL 실행 관리 | 설정 적용 후 센서별 수신 상태 표시 |

현재 영상 수집을 끄면 Tier 0으로 내려가 IMU·GNSS가 중단되고 휠 오도메트리의 적분도 진행되지 않습니다. 리그 설정은 카메라를 하나 이상 요구합니다. 상태 센서만 켜는 운용을 먼저 지원해야 합니다.

카메라는 C_FN·C_FW·C_RL·C_RR, 라이다는 L_F·L_PL·L_PR로 고정돼 있습니다. 기존 뷰 선택과 구독 해제는 가능하지만 새 이름·개수에는 코드 변경이 필요합니다. 센서 목록 하나가 장착점·주기·출력 토픽·소스 의존성을 결정하도록 바꿉니다. 첫 적용 방식은 중지 → 설정 변경 → 재시작입니다. 라이다만 켜도 깊이 소스인 렌더 뷰는 유지합니다.

차량 기하 계산은 Python `otpy/rig_layout.py`와 C++ `resolve_rig()`에 중복돼 있으며 바퀴 선택 조건도 다릅니다. SDK 구성에서 차량 좌표와 장착점을 해석하는 경로를 통일합니다. 상세 차량 변경 절차는 [FH5 리그와 캘리브레이션](14_phase1_highway_sensors.md)에 있습니다.

TCP 재연결은 새 MotionSensors를 만들어 휠 오도메트리와 가상 GNSS 기준점을 초기화합니다. 위치 추정을 붙이기 전에 통신 재연결과 물리 센서 초기화를 분리하고 좌표 초기화 시점을 명시해야 합니다.

GPU 생성·readback은 DX11 API를 직접 사용합니다. DX12 이식 시 교체할 범위는 게임 렌더 후킹·GPU 자원 경로이며 센서 측정·ROS 메시지·전송 계약은 유지하도록 분리합니다. [개발 순서](13_game_operating_table.md), [ROS 계약](16_ros2_bridge.md).
