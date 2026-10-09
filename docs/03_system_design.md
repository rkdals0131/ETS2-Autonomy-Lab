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
  └─ 상태 TCP: SDK·IMU·휠·GNSS·clock·진단 / 역방향 구독·수집·운전 제어
                         ↓ Ubuntu eth0 NAT 직접 주소
WSL ROS 2 Jazzy
  GenericPublisher → Fast DDS SHM → 소비자·rosbag2
                         └─ foxglove_bridge → Windows Foxglove

  ets2_autonomy/drive_speed
    GT 자차·교통 상태 + 지도 차로 JSON → 앞차 속도 제한·경로 추종
    → 속도 PI·pure-pursuit → applied 조향 PI → /ets2/drive/command
    → 상태 TCP → core → 상주 loader의 SDK 가상 입력

Windows 지도 provider → 설치 지도 Road·현재 SDK 위치 → 차로 JSON

Windows 런처 → WSL ROS/Foxglove 소유 그룹 + Windows 릴레이
              ← 프로세스 종료·ROS 상태 수신·왕복 응답·수집 카운터
```

## 실행과 소유권

- 메타로더가 SDK 플러그인으로 상주하고 기능 DLL을 교체합니다.
- DLL의 lease는 한 클라이언트가 내부 읽기·리그·수집을 소유하게 합니다. 소유자가 사라지면 5초 뒤 Tier 0으로 복귀합니다.
- F11은 lease를 취소합니다. 재시작은 사용자 조작으로 수행합니다.
- 운전은 별도 단일 owner/arm epoch를 사용합니다. ROS → 기존 상태 TCP → core의 bridge lease → loader의 공식 SDK 가상 3축으로 전달합니다. owner 이름은 인증 토큰이 아니며 기존 TCP pairing과 로컬 pipe ACL을 유지합니다. 입력 콜백·상태는 상주 loader가 소유하고 core unload 시 해제합니다.
- 운전 명령은 Windows monotonic clock의 200 ms 유효창을 그대로 전달합니다. ACC는 페달, LCC는 조향을 소유하며 소유하지 않는 축의 사람 입력은 유지합니다. 소유한 축의 수동 입력·pause·F11·연결 단절은 전체 owner를 해제하며 resume/reconnect가 다시 arm하지 않습니다. SDK 한 프레임은 불변 3축 snapshot을 사용하고, 해제는 다음 입력 프레임에 반영합니다.
- 런처는 Windows Job Object와 WSL systemd unit으로 자신의 프로세스를 관리합니다. 창 종료·오류·실행 시간 만료 시 해당 그룹을 정리합니다.
- 설정은 로컬 JSON 한 곳에서 읽고 센서 프리셋을 상대 경로로 참조합니다. 프로세스·응답·수신 시각이 화면 상태의 기준입니다.

## 렌더와 전송

캡처 요청 하나를 다음 센서 선택 호출이 가져갑니다. 4뷰 구성에서는 선택된 네 pass의 실제 Present ID를 대조하고 완성된 묶음만 발행합니다. 구독 요구와 라이다 주기에 따라 필요한 뷰·출력을 선택하며 비수집 프레임의 상세 관측을 건너뜁니다.

게임의 pass 관측은 카메라·차량의 실제 렌더 자세를 숫자 구조체에 저장합니다. 같은 SDK 표본은 불변 참조로 공유하고, 엔진 주소표는 한 번 해석해 재사용합니다. 명령 구간과 캡처 슬롯은 같은 관측 표본을 참조하며 슬롯 완료 뒤 전송 작업자가 JSON을 만듭니다. 다른 시점의 카메라·차량 자세는 각각 유지합니다.

실시간 ROS 수집은 카메라 슬롯과 출력 종류로 pass를 식별합니다. 상세 pass 이름·입출력 이미지 설명·전체 SDK 채널 복사는 원시 캡처와 수동 진단에서 생성합니다. 카메라 가시성 GT는 해당 카메라에서 요청한 묶음에만 읽습니다. 자동주행의 world 교통 GT는 렌더와 독립된 상태 snapshot으로 제공합니다. 재사용되는 명령 버퍼의 빈 경계에서 이전 표본을 해제합니다.

이미지는 공유 D3D11 텍스처에 pack합니다. 게임은 fence를 signal하고, 릴레이가 별도 device에서 staging으로 복사해 읽습니다. 소비 완료 전 슬롯은 재사용하지 않습니다. CPU 공유 슬롯은 소유 버퍼로 복사한 즉시 반환합니다. 혼잡 시 대기 중인 오래된 묶음을 통째로 버립니다.

상태와 영상의 송수신 worker·큐는 분리합니다. 상태 연결은 TCP_NODELAY를 사용합니다. WSL 주소는 연결 때 eth0에서 다시 조회합니다.

## ROS 소비자

Windows가 XCDRv1 메시지를 만들고 WSL은 GenericPublisher로 발행합니다. 영상·라이다는 best-effort, SDK·IMU·휠·GNSS 상태는 reliable, 정적 TF·구성은 transient-local입니다. DDS SHM participant 세그먼트는 128 MiB, 최대 메시지는 8 MiB입니다.

Foxglove는 JPEG·라이다·GT·TF·상태를 구독합니다. 원본 RGB·depth와 축소 perception 영상은 ROS 소비자가 선택합니다. 실제 구독이 GPU pack·readback 요구로 연결됩니다.

## 코드 경계

| 위치 | 책임 |
| --- | --- |
| ot/native | SDK·물리 읽기, 리그, GPU pack·공유 자원 |
| ot/include | core·릴레이·Python IPC DLL이 공유하는 슬롯 소유권·좌표 연산·SDK 장착 원점 계산 |
| ot/native/pass_commands.* / render_sample.* | 엔진에서 렌더 자세·차량 관측, 불변 표본과 진단 직렬화 |
| bridge/windows/vehicle_profile.* | SDK 바퀴 기하·기준 축·수동 장착 프리셋 해석, 라이다 소스 연결 |
| bridge/windows/state_source.* | SDK 최신 표본 읽기, 센서 계산 스레드와 실행 수명 |
| bridge/windows/motion_sensors.* | IMU·휠 오도메트리·가상 GNSS 측정과 적분 이력 |
| bridge/windows/render_capture.* | 구독 → GPU 출력 요구, 렌더 hook·stream 시작·중지 |
| bridge/windows/gpu_readback.* | 공유 D3D11 텍스처·fence 소비 |
| bridge/windows/state_messages.cpp / image_messages.cpp | 상태·영상 측정의 ROS CDR·JPEG 변환 |
| bridge/windows/game_ipc.* | 게임 명령 pipe와 공유 메모리 소유 슬롯 |
| bridge/windows/main.cpp / relay_workers.hpp | 프로세스 소유권, 두 TCP 연결, worker·큐와 재연결 |
| bridge/common/wire.hpp / ros2/src/ets2_msgs | 토픽·전송·센서·운전 메시지 계약 |
| ros2/src/ets2_bridge | ROS GenericPublisher·구독 요구·제어 요청 전달 |
| ros2/src/ets2_autonomy | 현재 GT baseline: 앞차 선택·속도 PI·pure-pursuit·입력 adapter |
| bridge/map_lane_provider | 설치 지도 곡선에서 현재 차로 JSON 생성, Windows SDK 위치 읽기 |
| apps/launcher/launcher.py / linux_launcher.py | Windows UI·Linux 터미널 조작 |
| apps/launcher/session.py / ros_session.py | 공통 세션 명령·실제 상태 해석, ROS/Foxglove·보조·기록 수명 |
| apps/launcher/bag_recording.py | WSL 세션이 소유한 rosbag2 기록기 하나, 토픽 선택·저장 위치·종료와 결과 표시 |
| research/ | 원본·분석·연구 도구. 현재 provider용 .NET runtime·TruckLib 고정 소스도 보존 |

센서 계산은 ROS 직렬화·구독 수·소켓 상태를 참조하지 않습니다. `MotionSample`을 만든 뒤 전송 쪽이 구독된 측정만 직렬화합니다. 센서 잡음은 측정 계산 단계에 추가하고, ROS 메시지 형식과 전송 코드는 유지할 수 있습니다.

## ROS 개발과 다음 스택

편집 원본은 Windows 저장소 하나입니다. WSL은 `/mnt/c/.../ETS2-Autonomy-Lab/ros2/src`를 colcon source로 읽고 `~/ets2-ros/{build,install,log}`에 결과를 보관합니다. 별도 WSL 코드 복제본은 사용하지 않습니다. WSL의 `~/ETS2-Autonomy-Lab`에는 이전 문서 사본만 있으므로 현재 소스처럼 편집하거나 덮어쓰지 않습니다. Linux 개발 규모가 커지면 ext4 checkout을 편집 원본으로 삼는 배치를 권장하지만, 그 전환과 Windows 빌드 소스 동기화는 별도로 결정합니다.

이번 분리는 기존 GT baseline의 책임을 옮긴 것입니다. 인지·위치 추정·목적지 route planner·FSM·Stanley·MPC는 아직 구현하지 않았습니다. 다음 기능은 `ets2_autonomy` 안에서 실제 구현과 함께 모듈을 나눕니다.

```text
ROS 센서 ─┬─ perception → 주변 물체·차로
          └─ localization → 자차 위치·자세
지도·목적지 + 물체·자차 → behavior/FSM → local trajectory(위치·목표속도)
→ controller(PP/Stanley/MPC, speed PID) → ETS2 Drive adapter → 운전 API
```

현재 GT 입력은 인지·위치 추정 결과와 비교할 기준 어댑터로 유지합니다. `world`(SCS X,-Z)와 세션 원점의 `wheel_odom`은 별도 좌표계이므로 위치 추정 연결 시 변환을 명시합니다. 현재 profile-relative 입력과 applied 조향 PI는 마지막 ETS2 adapter 책임이며, 물리 바퀴 각도·조향 토크 API가 아닙니다. 제어 알고리즘을 교체해도 DLL·릴레이 계약은 유지합니다.

`apps/launch.cmd`는 Windows 전체 UI이며 `--host-only`는 기존 native 릴레이만 실행합니다. `apps/launch.sh`는 Linux의 전경 터미널에서 공통 ROS 세션·자신의 보조·기록을 관리합니다. Linux 먼저 시작하고 Windows host-only를 연결하면 동일한 paired TCP를 사용합니다. 공통 session lock이 Windows 전체 모드와 Linux 모드의 ROS 중복 시작을 막으며, 각 관리자와 입력 owner의 종료 범위는 유지합니다. 별도 Linux PC의 Windows host까지 원격 관리하는 API는 제공하지 않습니다. [두 진입점](../apps/README.md), [ROS 빌드와 실행](../ros2/README.md).

## 수명과 구성

릴레이 프로세스가 lease·Tier 1 물리 읽기·StateSource를 소유합니다. RenderCapture는 렌더 hook과 GPU stream만 소유하며, 수집 중지·구독 해제·네트워크 단절 때 해제합니다. 영상 수집을 꺼도 상태 계산과 적분은 계속됩니다. 소켓 재연결은 새 전송 세션을 만들고 기존 StateSource를 사용합니다.

`/ets2/sensors/config`의 `sensor_session`은 릴레이 실행 동안 유지됩니다. 중지·재시작은 새 오도메트리 원점·가상 GNSS 기준점을 만듭니다. 게임 시각 초기화·차량 구성 변경·F11은 명시적 재시작을 요구합니다. 센서 기준점을 바꾸는 동작과 네트워크 복구를 이 경계로 구분합니다.

`slots: []`는 카메라·라이다 없이 상태 센서만 실행합니다. 기존 뷰는 slots로 선택하고 출력은 ROS 구독으로 활성화합니다. 원본 RGB·depth 구독이 없으면 고해상도 readback·전송을 생략합니다. 라이다 소스인 깊이 렌더는 해당 라이다 요구에 따라 유지합니다.

현재 카메라 ID C_FN·C_FW·C_RL·C_RR과 라이다 ID L_F·L_PL·L_PR은 고정입니다. 새 이름·개수에는 토픽 계약과 출력 요구 매핑을 함께 확장합니다. 장착·기준 축은 차량 프리셋으로 바꾸며 현재 센서 기본은 FH4입니다. Python 연구 도구와 C++ 릴레이는 같은 프리셋과 기준 축 선택 규칙을 사용합니다. [차량 보정 절차](14_phase1_highway_sensors.md).

GPU 생성·readback은 DX11 API를 직접 사용합니다. DX12 이식 시 교체할 범위는 게임 렌더 후킹·GPU 자원 경로이며 센서 측정·ROS 메시지·전송 계약은 유지하도록 분리합니다. [개발 순서](13_game_operating_table.md), [ROS 계약](16_ros2_bridge.md).
