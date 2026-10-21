# 유로파일럿 브리지와 런처

## 실행

게임을 Steam의 **DirectX11 (64-bit)**로 실행한 뒤 [apps/launch.cmd](../apps/launch.cmd)를 엽니다. `launch.cmd`는 pixi 환경(`pixi run launcher`)에서 런처를 띄우므로 런처가 시작하는 모든 프로세스가 같은 ROS 2 환경과 DDS 프로필을 씁니다.

런처 화면과 버튼은 [런처 문서](../apps/launcher/README.md)에 있습니다(2026-10-21 재설계). 모드는 **주행 보조**(센서 슬롯을 비운 입력·상태 실행 + 크루즈 버튼 대기 자동 시작)와 **센서 수집** 둘이고, 버튼은 시작/중지 하나입니다. 실행 시간·카메라·라이다·미리보기 주기, CPU 배치, 게임 미러는 `config/bridge.local.json`에서 바꾸고 런처는 다음 시작에 그 값을 읽습니다. 기본 주기는 카메라 30 Hz·라이다 10 Hz·JPEG 미리보기 10 Hz입니다. 릴레이는 BELOW_NORMAL 우선순위로 게임에 CPU를 양보합니다. F11·게임 오류·프로세스 종료 뒤에는 사용자가 시작을 눌러 새 실행을 엽니다. 게임 세션이 바뀐 경우(세이브를 불러와 SDK 시계가 되돌아감, 다른 트럭)에는 릴레이가 코드 3으로 끝나고, 런처가 게임이 `truck_config`에 답하는 대로 릴레이만 다시 띄웁니다(2026-10-21). 크루즈 대기·provider·기록은 그대로 이어집니다. 같은 트럭의 구성이 다시 오는 경우(배송·작업·정비 화면)는 릴레이가 끝나지 않습니다([docs/17](../docs/17_sensor_expansion.md)). 50 ms 넘은 발행은 릴레이 상태 JSON의 `slow_publishes.recent`에 시각·토픽·잠금 대기·발행 시간으로 최근 16건이 남습니다. 자식 프로세스 출력은 실행마다 `bridge/recordings/logs/<시각>/`에 남습니다.

**CPU 배치**(기본 켬, `cpu_layout`)는 게임을 CPU 0이 속한 L3 묶음에 두고 게임 메인 스레드를 그 묶음의 두 번째 물리 코어에 혼자 둡니다. 릴레이·Foxglove 서버·기록기와 Foxglove 뷰어 앱(`Foxglove.exe`, BELOW_NORMAL)은 다른 L3 묶음으로 보냅니다. ETS2는 메인 스레드 하나가 병목이라, 같은 코어의 SMT 짝과 L3를 다른 일과 나누지 않게 하는 설정입니다. 이 머신(Ryzen 7 3700X)에서는 게임 CPU 0–7·메인 CPU 2, 나머지 CPU 8–15이며 정차 연속 A/B에서 프레임 CPU 시간이 5.9 ms 줄었습니다. 새 게임 스레드와 뒤늦게 연 뷰어 창은 5초마다 반영하고, **중지**하면 모두 되돌립니다. 런처가 비정상 종료되면 게임 재시작 전까지 배치가 남습니다. [성능](../docs/18_performance.md#cpu-병목-분석).

**게임 미러 렌더 끄기**(기본 끔, `native_mirrors:false`)는 센서를 수집하는 동안 화면에 보이는 게임 미러의 렌더를 건너뜁니다. 게임 미러는 하나마다 장면 전체를 다시 그리므로 메인 스레드를 아낍니다. 화면의 미러는 마지막 장면에서 멈추니 직접 운전할 때는 켜 두지 않습니다. core 0.33.0이 필요하고 효과는 아직 게임에서 재지 않았습니다([계획 C10](../docs/26_cpu_optimization_plan.md#c10-게임-미러-렌더-끄기-s-구현-완료-게임-확인-남음)).

다른 프로세스가 `127.0.0.1:8765`를 쓰고 있으면 런처가 시작을 거절합니다. 기존 Foxglove 서버를 종료한 뒤 시작합니다.

## 설정

릴레이와 런처는 같은 `config/bridge.local.json`을 읽습니다. ROS domain 42, `rmw_fastrtps_cpp`, localhost 탐색, `config/fastdds.xml`(공유메모리 128 MiB·최대 메시지 8 MiB)은 pixi activation(`pixi.toml`, `ros2/activate.bat`)이 모든 프로세스에 설정합니다.

| 설정 | 용도 |
| --- | --- |
| rig / lidar / slots | 센서 프리셋과 슬롯. 현재 private 프리셋·[3,4,6,7]. []는 SDK 바퀴 기준의 상태·입력만 실행하며 카메라 장착 보정을 사용하지 않음 |
| duration_s | 실행 제한 시간 |
| camera_hz / lidar_hz | 기본 30 / 10. 라이다는 필요한 카메라 프레임에서만 gather·readback |
| preview_hz / lidar_preview_stride | 표시용 JPEG 기본 10 Hz. 표시용 점군은 수평 빔 4개마다 1개, 모든 수직 링 유지. 자세(`camera_info`·TF)만 구독한 카메라도 이 주기로만 렌더 |
| cpu_layout | 런처 CPU 배치, 기본 true. 다음 시작부터 적용 |
| native_mirrors | 기본 true. false면 센서 실행 중 게임 미러(우리 슬롯이 아닌 미러) 렌더를 건너뜀. 화면 미러는 마지막 장면에서 멈춤. core 0.33.0 필요 |
| capture_warmup | 기본 true. private 센서는 유휴 뒤 한 프레임 준비하고 다음 프레임에서 수집. false는 연속 렌더 비교용 |
| imu_mount_base_m / gnss_mount_base_m | 섀시 고정 장착점, base_link 기준 m. 기본 [0,0,1] |
| gnss_reference_lla | 시작 위치의 가상 기준 위도·경도(deg)·타원체 고도(m), 기본 [0,0,0] |
| gt_localization_tf | 기본 true. 스택 localization이 `odom→base_link`를 내면 false로 GT `world→base_link`를 끔 |
| recording_root | 차량 상태 기록 폴더. 생략하면 bridge/recordings |
| sensor_recording_root | 영상·라이다 기록 폴더. 사용할 때 명시적으로 지정 |
| auto_exposure / color_gain | 자동 노출 또는 수동 gain |
| shared_gpu | 기본 true, 공유 GPU 텍스처 전달 |

설정 변경은 중지 후 적용합니다. `configure.py`는 최초 파일을 만들고 기존 설정을 보존합니다.

원본 RGB·depth는 기본 화면에서 구독하지 않습니다. `/perception/image_raw`는 인지용 축소 RGB, `/preview/image/compressed`는 표시용 JPEG입니다. 원본 `/image_raw`와 `/depth/image_raw`를 구독할 때만 고해상도 pack·readback·발행을 수행합니다. 라이다만 필요하면 GPU 깊이에서 빔을 샘플링하고 전체 깊이 영상은 내려받지 않습니다. `ros2 bag record -a`는 원본 토픽도 구독하므로, 평상시 기록은 필요한 토픽을 지정합니다.

`/ets2/capture` 서비스로 렌더 수집을 중지해도 IMU·휠·GNSS 계산과 적분·지리 기준점은 계속됩니다. 런처의 중지·재시작은 센서 실행 전체를 끝내며 다음 시작에서 새 기준점을 만듭니다. `/ets2/sensors/config`의 `sensor_session`으로 이 수명을 구분합니다.

## 최초 준비

1. Visual Studio 2022 Build Tools(C++ 워크로드)와 pixi를 설치합니다.

   ```powershell
   winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   winget install --id prefix-dev.pixi -e --scope user
   ```

2. 새 터미널의 저장소 루트에서 게임 DLL·로컬 설정·ROS 패키지를 만듭니다. DLL 설치는 [ot 문서](../ot/README.md)를 따릅니다.

   ```powershell
   .\ot\build.cmd
   pixi run python bridge/configure.py
   pixi run build      # ets2_msgs, ets2_relay, ets2_sensor_degradation, ets2_autonomy
   pixi run test       # 게임 없이 도는 단위 테스트
   ```

`PATH`의 `python.exe`가 다른 버전일 수 있으므로 ROS 빌드·명령은 항상 `pixi run` 또는 `pixi shell` 안에서 실행합니다. ROS 소스는 [ros2/src](../ros2/README.md), 실행 관리 코드는 [apps/launcher](../apps/launcher/README.md)에 있습니다.

## 토픽 확인

런처에서 **시작**한 뒤 저장소 루트에서 실행합니다.

```powershell
pixi shell
ros2 topic list --no-daemon      # domain 42는 activation이 설정
```

Windows에서 출력을 캡처하는 스크립트는 `--no-daemon`을 씁니다. ros2 daemon이 출력 파이프를 물고 있으면 명령이 끝나지 않습니다.

## Foxglove

Windows Foxglove에서 `ws://127.0.0.1:8765`에 연결합니다. 다른 PC(같은 로컬망)에서 보려면 `config/bridge.local.json`에 `"foxglove_address": "0.0.0.0"`(기본 `127.0.0.1`)을 두고, 이 PC에서 관리자 PowerShell로 TCP 8765 인바운드를 엽니다: `New-NetFirewallRule -DisplayName "Foxglove bridge 8765" -Direction Inbound -Protocol TCP -LocalPort 8765 -Action Allow -Profile Private`. 그 뒤 다른 PC에서 `ws://<이 PC IP>:8765`로 연결합니다(서버는 읽기 전용: 클라이언트 발행 기능 없음). 레이아웃은 Foxglove의 Layouts → Import로 불러옵니다. 패널 하나가 질문 하나에 답하고 같은 답을 주는 패널은 두지 않습니다. `config/foxglove-assist.json`(주행 보조): 주변(3D: 노면 표시·차로·주변 차량·앞차), ACC·LCC·차로 변경(지금 상태 표시등), 보조 상태 기록(그 상태가 언제 바뀌었나), 왼쪽 열 종방향 속도(내·설정·커브와 앞차를 반영한 목표·앞차)·앞차 간격·가감속(ACC 요구 대 실제), 오른쪽 열 횡방향 차로 중심 오차(트럭 위치와 합류·차로 변경 경로)·조향(LCC 명령·게임 적용값·운전자 스틱). 트럭 위 상태 글자(`scene/status`)는 표시등과 겹쳐 꺼 둡니다. `config/foxglove-sensors.json`(센서 수집): 카메라 4개, 라이다 3D, 속도(차량 대 휠 오도메트리), IMU. 그래프는 모두 30 s 창에 시간축이 맞춰져 있습니다. 서버 허용 목록(`config/foxglove.yaml`)에 `/ets2/hud/markers`, `/ets2/drive/*`, `/ets2/vehicle/powertrain`을 더했습니다(2026-10-21 저녁 전에는 HUD·보조 상태가 Foxglove에 오지 않았음). 런처는 서버가 떠 있는지 TCP 표의 LISTEN 소켓으로 봅니다(연결 시도는 서버에 "handshake failed" ERROR를 남겼음).

- 영상: `/ets2/camera/{C_FN,C_FW,C_RL,C_RR}/preview/image/compressed`
- 표시용 점군: `/ets2/lidar/{L_F,L_PL,L_PR}/preview/points`
- 전체 점군: `/ets2/lidar/{L_F,L_PL,L_PR}/points`
- 박스: `/ets2/ground_truth/{camera}/markers`
- 상태: `/ets2/vehicle/state`, `/diagnostics`, `/tf`, `/clock`
- 조작과 기어: `/ets2/vehicle/actuation`의 운전자 입력·실제 적용 입력·기어
- 파워트레인(실차 CAN에 해당): `/ets2/vehicle/powertrain`의 온도·압력·연료·리타더·엔진브레이크와, Tier 1일 때 실제 엔진 토크·변속기 출력 토크·축 회전·바퀴별 브레이크 토크([docs/30](../docs/30_can_like_signals.md))
- 추가 센서: `/ets2/imu/data_raw`, `/ets2/wheels/state`, `/ets2/wheels/odometry`, `/ets2/gnss/fix`

라이다는 Color map → range → Turbo로 설정하고 가까운 장면은 0–30m 또는 0–50m 범위를 사용합니다. 원본 RGB·depth와 `/perception/image_raw`는 ROS 소비자가 직접 구독합니다. [토픽·시각 계약](../docs/16_ros2_bridge.md).

전방 원본은 1280×720, 측면 원본은 960×544입니다. JPEG·인지용 영상은 각각 640×360·480×272입니다. HD 원본을 구독하면 readback·직렬화·발행 비용이 증가합니다. 30 Hz 설정은 수집 요청 주기이며 실제 전달률은 게임과 소비자 처리량에 따라 달라집니다.

인지용 영상은 카메라 주기를 따르고 JPEG 미리보기는 `preview_hz`를 따릅니다. 표시용 점군은 XYZ·range만 전달하며 전체 점군에서 선택한 빔의 값을 그대로 사용합니다. 기존 Foxglove 레이아웃의 점군 토픽을 `/preview/points`로 바꾸면 표시 비용이 줄어듭니다. 원본 점군의 필드·10 Hz 주기는 유지됩니다.

## GT 도로 자동 주행

지도 provider가 트럭 앞의 실제 지도 차로 기준선을 만들고(`--follow`, 1 km 앞까지 이어 붙임), `drive_speed`가 GT 위치로 ACC(생성 MPC, 질량 적응)와 LCC(생성 MPC)를 돌립니다. 런처 **주행 보조** 모드로 시작하면 둘이 함께 떠서 게임 크루즈 버튼(LB+RT)을 기다립니다([아래](#게임-크루즈-버튼으로-켜기-engagecruise)). 차량·바퀴 pose나 사람 Xbox 설정은 쓰지 않습니다.

- **ACC**: 앞차는 트럭이 차로 위에 있으면 차로를 따라, 아니면 현재 진행방향 직선으로 고릅니다(굽은 도로에서 먼 앞차를 놓칠 수 있음). LCC가 조향하는 동안에는 차로 앞 700 m의 곡률로 커브 속도(`lateral_acceleration_mps2` 2.0 m/s², `curve_deceleration_mps2` 0.45 m/s²로 미리 감속)를 설정 속도에 겁니다. 2026-10-21 이전에는 23 m 앞 곡률 하나만 봐서 R130 커브에 87 km/h로 들어갔습니다(횡가속 4.2 m/s²). 가속 페달은 가속만 잠시 덮어씁니다.
- **해제(같은 날 사용자 규칙)**: 브레이크는 ACC와 LCC를 함께 끄고 대기로 돌아갑니다. 운전자 조향이 `steering_takeover_s`(3 s, 0.3 s 미만 끊김은 이어진 것으로 봄) 이어져도 둘 다 끕니다. 그보다 짧은 조향에는 LCC가 양보합니다. 다시 켜는 것은 크루즈 버튼(LB+RT)입니다.
- **LCC**: 조향 중 / 운전자에게 양보 / 차로 찾기 세 상태입니다. 운전자가 스틱을 잡으면 양보하고 놓은 뒤 0.5 s에 돌아옵니다. 차로를 벗어나면(운전자의 차로 변경 등) 조향 입력 0(스틱을 놓은 것과 같음)으로 차로를 찾습니다. provider의 좌우 차로 파일(아래)에서 트럭이 들어간 차로를 0.25 s 안에 고르므로 provider reset을 기다리지 않습니다. 다시 잡는 조건은 차로 가장자리에서 0.4 m 안쪽이고, 헤딩이 중심 반대쪽이면 되돌리는 데 드는 횡이동(v²h²/2, 1 m/s² 기준)까지 그 안이며, 헤딩 오차 0.1 rad 이하입니다. 다시 잡을 때마다 지금 자세에서 차로 중심으로 가는 합류 기준(`lane_merge.hpp`, 5차 곡선이라 양 끝에서 횡가속 계단이 없음, 오프셋 항 0.5 m/s²)을 따라갑니다.
- **방향지시등 차로 변경**: LCC가 조향 중일 때 방향지시등 스위치를 켜면(켜지는 순간 한 번, 비상등은 제외) 그쪽 옆 차로로 옮깁니다. 옆 차로에 트럭 옆 차량이 없고, 앞 차량이 앞범퍼보다 5 m + 0.5 s + 접근 속도 3 s 이상 앞, 뒤 차량이 트럭(트레일러 포함) 뒤로 5 m + 접근 속도 3 s 이상일 때 시작합니다. 아니면 "waits"를 출력하고 켜져 있는 동안 기다리며, 끄면 요청을 거둡니다. 경로는 같은 5차 합류(`lane_change_lateral_acceleration_mps2` 0.9 m/s², 4.5 m 차로 약 5.4 s)이고 HUD 띠도 그 경로를 그립니다. 도중에 운전자가 조향하면 중단되고 위 규칙대로 다시 잡습니다. △ 게임 미확인.
- 지도 차로는 float32 점(원점에서 30 km면 2–4 mm 계단)이라 `LanePath`가 국소 2차 맞춤(±8 m)으로 매끈하게 한 위치·헤딩·곡률을 씁니다. 2026-10-21 첫 게임 LCC 주행에서 이 잡음이 3 Hz 조향 떨림을 만들었습니다([LCC 문서](../apps/control/matlab/lcc/README.md#게임-주행-1-2026-10-21)).
- `/ets2/drive/assist`(`ets2_msgs/AssistState`, 50 Hz)에 LCC 상태·차로 오차·합류 기준·MPC 출력·ACC 모드·선행차·질량이, `/ets2/drive/lane`(1 Hz)에 쓰는 차로가 나옵니다. 런처 기록에 둘 다 들어갑니다.

F11·만료·릴레이 종료·pause는 전체를 해제하고, 크루즈 대기 노드는 다시 대기로 돌아갑니다.

독립 축 지원은 **core/loader 0.29.0**에서 추가됐습니다. 상주 loader 교체는 정상 게임 종료 후 설치·재시작이 필요합니다. 새 ROS 메시지 타입을 쓰면 `pixi run build`로 릴레이와 ROS 패키지를 함께 다시 빌드합니다. 구 host ABI-1 prefix와 기존 JSON 클라이언트는 전체 축 계약을 유지하고, 새 core에 구 loader를 연결하면 독립 모드를 거절합니다.

현재 provider는 설치된 지도 자산의 단방향(`ger16` 등)·중앙분리 양방향(`ger11`/`ger14` 등) 고속도로 차로에서 현재 진행방향 차로를 고르고, 공유 Node로 연결되는 도로와 prefab 내비 곡선의 본선 경로를 따라 런처 기준 최대 1 km까지 잇습니다. 실제 모델 노면·shoulder로 차로 폭을 구합니다. 본선을 정할 수 없는 갈림길·차로 끝·신호·지원하지 않는 layout·선택 거리 끝에서는 기준선이 끝나므로 감속합니다. 장거리용 상주 `--follow` 모드와 소비자 측 이어 붙이기(`LanePath::extend`, `lat_extend`)는 있지만 런처·ROS 노드에는 아직 연결하지 않았습니다. 내비 목적지 선택·교차로/신호 통과·차로 변경은 아직 없습니다. [지도 provider 빌드·지원 범위](map_lane_provider/README.md).

2026-10-10 FH4 전경 주행(이전 TCP 구성)에서 기준선 약 **453 m**를 진행했습니다. 제품 로그의 중심선 횡오차는 최대 **1.35 m**, 후반 **0.03–0.57 m**, 후반 속도는 **29.84–29.97 km/h**였습니다. 목표 30 km/h의 초기 PI 전이에서 **35.86 km/h** overshoot가 있었으며 개선 대상입니다. 목표0 제동·명시적 해제 후 최종 상태는 정지·disarmed·자동 3축0이었습니다. 먼 AI 차량은 723–819 m에서 관측됐지만 근접 앞차 추종 제동은 이 실행에서 검증하지 않았습니다. 이는 현재 도로의 짧은 주행 결과이며 차체 전체의 차로 내 유지나 일반 도로 완주를 입증하지 않습니다. Windows 네이티브에서의 재확인은 [포팅 계획](../docs/20_windows_native_port_plan.md) P1 완료 기준입니다.

## 운전 명령 API

상주 loader는 공식 SCS Input SDK에 `ot_drive` 가상 3축 장치를 등록합니다. 사람의 키보드/Xbox 바인딩을 유지하면서 컴퓨터가 횡방향 조향과 종방향 가속·제동을 보냅니다. 장치 등록·disarmed Xbox 좌우/페달 대조·ROS arm/disarm과 FH4 실제 가속·조향·제동을 확인했습니다. FFB나 조향 토크 제어는 구현하지 않았습니다.

최초 등록은 정상 게임 종료 → 새 loader/core 설치 → 게임 재시작 순서입니다. SDK는 input init 때만 장치를 등록하므로 기존 loader의 hot reload로 추가할 수 없습니다. 설치된 `ot_runtime/ot_config.json`에서 `allow_drive: true`, `singleplayer_research: true`, 활성 `controls.sii`의 절대 경로 `drive_controls_path`를 설정합니다. 저장소 기본 권한은 꺼져 있으며 센서 설정과 사용자 controls.sii는 바꾸지 않습니다.

| 창구 | 계약 |
| --- | --- |
| `/ets2/drive/control` | `DriveControl` 서비스: owner, arm, epoch, axes → success, message, epoch |
| `/ets2/drive/command` | `DriveCommand`: owner, epoch, 증가하는 sequence, sent_steady_ns, steering/throttle/brake, axes |
| `/ets2/drive/state` | `DriveState`: 활성/해제 이유, owner·epoch·마지막 승인 sequence, 유효창, 명령·물리입력·SDK 적용값, 실제 axes·independent_axes |

요청 `axes`는 1=조향/LCC, 2=페달/ACC, 3=전체이며 0은 기존 전체 축 요청의 기본값입니다. 4는 보조 제동(리타더 단·엔진브레이크)이고 페달과 함께만 씁니다(6=ACC+보조 제동, 7=전체). 보조 제동을 가지면 `DriveCommand`의 `retarder_step`(0–5, 0은 끔)과 `engine_brake`(누르고 있음)가 적용됩니다. loader 0.42.0 이상이 `ot_drive` 장치에 semantical `retarder0`–`retarder5`·`motorbrake` 입력을 더 등록하며, 프로필의 `semantical.retarderN?0`·`semantical.motorbrake?0` mix로 들어가므로 바인딩을 바꾸지 않습니다. 단은 바뀔 때와 0.5 s마다 50 ms 펄스로 고르고, 소유가 끝나면(해제·만료·수동 개입) 리타더가 켜져 있었으면 0단을 한 번 펄스합니다. 엔진브레이크는 소유하는 동안만 누릅니다. 리타더 키·B의 사람 조작은 해제 조건이 아니며, 소유 중에는 0.5 s 안에 명령 단으로 돌아갑니다. `DriveState`의 `auxiliary_brakes`·`retarder_command`(−1은 미소유)·`engine_brake_command`가 실제 명령을 보여 줍니다. △ 게임에서 semantical `retarderN`이 "N단 선택"으로 동작하는지, 같은 단을 다시 고를 때 부작용이 없는지는 아직 확인하지 않았습니다(loader 교체 후 게임 재시작 필요). 실제 상태 `axes=0`은 해제 상태입니다. 모두 끄기는 `arm=false`로 해제하며 `axes=0` arm을 보내지 않습니다. 한 owner/epoch 안의 새 명령으로 축을 전환하고 소유하지 않은 출력은 0으로 만듭니다. loader 0.31은 새로 맡길 축을 사람이 조작 중이면 해제하므로 켜기 전에 그 축을 놓습니다.

DriveCommand.steering은 **왼쪽 양수 [-1,1]인 프로필 입력**, throttle/brake는 **[0,1]**입니다. 현재 Xbox 프로필의 `c_relatsteer=1`은 조향 위치를 변화시키는 상대 입력입니다. 따라서 이 하위 명령값을 바퀴 위치나 각도 목표로 해석하지 않습니다. 현재 프로필의 steering mix가 semantical 값을 빼므로 SDK 장치에는 steering의 부호를 뒤집어 내보냅니다. 기존 `/ets2/vehicle/actuation`의 단위·부호는 유지합니다. applied 값은 게임의 effective 입력 관측이며 명령을 복사한 값이 아닙니다. SDK 입력은 실제 게임 전경에서 소비됐습니다. 비전경 실행은 native 명령이 승인돼도 게임 입력 축과 속도가 0이었으며, 전경 실행에서 가속·제동이 적용됐습니다.

arm은 단일 owner에 새 epoch를 발급하고 200 ms 동안 첫 명령을 기다립니다. Command의 `sent_steady_ns`는 명령을 계산한 시점의 **발행 프로세스 steady clock ns**(Windows에서는 QPC: C++ `std::chrono::steady_clock`, Python `time.perf_counter_ns()`; `time.monotonic` 금지)이며 ROS stamp가 아닙니다. 릴레이 노드는 나이가 `max_command_age_ms`(노드 파라미터, 기본 50)를 넘거나 음수·0인 명령을 버리고, 가장 최근 명령만 core에 넘기면서 core 시계로 200 ms 유효창을 붙입니다(실차 DBW heartbeat와 같은 형태). 이전 epoch·반복 sequence·만료된 명령은 거절합니다. DriveState의 `command_window_ms`는 진단용 Windows 시계값이며 명령에 복사하지 않습니다. owner는 식별자이며 로컬 pipe ACL을 대신하지 않습니다.

명령 만료·소유한 축의 물리 조작·F11·pause·SDK 입력 비활성·core unload·릴레이 종료는 arm을 해제합니다. resume·릴레이 재시작은 다시 arm하지 않습니다. F11은 bridge lease도 취소하므로 런처를 명시적으로 재시작합니다. SDK 한 입력 프레임의 세 축은 같은 snapshot이며 해제는 다음 입력 프레임의 세 축 0으로 반영합니다. 렌더 수집 on/off와 운전 arm은 독립입니다.

수동 해제는 현재 프로필의 A/Left·D/Right·W/Up·S/Down, `joy.x/rt/lt`와 패드 버튼(`joy.bN`)을 직접 읽습니다. controls.sii의 deadzone·축 변환·custom modifier(`!`·`&`, 예: LB를 누르는 동안 스로틀 값 유지)를 적용하고, 반대 키나 키보드/패드가 서로 상쇄돼도 각 물리 source의 활동을 보고 해제합니다. modifier를 누른 채 트리거를 당겨도 활동으로 봅니다. loader 0.31까지는 소유한 축에 활동이 있으면 전부 해제합니다. loader 0.44.0부터는 축별로 다릅니다. 조향 활동 중에는 명령 조향을 더하지 않아 운전자만 조향하고 세션은 유지됩니다. 가속 페달은 상향 오버라이드입니다. 게임이 두 입력을 더하므로 명령은 운전자 값을 넘는 만큼만 보내 결과가 둘 중 큰 쪽이 되고, 그동안 브레이크는 0, 리타더는 0단, 엔진브레이크는 끕니다. 브레이크 페달은 그 epoch 동안 페달·보조 제동 축을 회수하고 남은 축(LCC)은 계속하며, 남는 축이 없으면 `manual`로 해제합니다. arm은 이전처럼 요청 축에 운전자 입력이 있으면 거절합니다. `drive_speed`는 브레이크 회수 뒤 "The driver's brake: ACC and LCC off"를 출력하고 세션을 해제하며(2026-10-21 사용자 규칙), 운전자 조향 중과 놓은 뒤 0.5 s 동안 LCC를 쉬고 3 s 이어지면 해제하고, 차로를 벗어나면 차로를 찾다가 새 차로에서 다시 잡습니다(위 GT 도로 자동 주행). ✔ 0.44.0의 축별 동작은 2026-10-20 게임에서 확인했습니다([안전 체크리스트](../docs/21_safety_checklist.md)). pad 부재는 프로필의 `?0` fallback대로 중립이며 키보드 조작은 유지합니다. pad 연결 변화는 arm을 해제하고 읽기 오류·지원하지 않는 binding(`&&`·`||` 포함)에서는 arm을 허용하지 않습니다. 프로필 변경은 권한 재읽기 또는 재시작으로 반영합니다. armed 상태의 수동 해제·F11 시험은 [안전 체크리스트](../docs/21_safety_checklist.md)에 기록합니다.

게임 안전 시험용 명령 발행기는 `ros2 run ets2_autonomy drive_hold --ros-args -p axes:=3 -p throttle:=0.0`입니다. arm 뒤 고정 명령을 50 Hz로 보내고 Ctrl+C 또는 `--stop-event`에서 disarm하며 DriveState 해제 사유를 출력합니다.

### 속도 목표와 조향 위치 목표

core 0.28.0은 카메라와 독립된 `/ets2/ground_truth/traffic`을 SDK frame_end에서 요청 시 발행합니다. AI actor 위치·진행방향·속도·차체 크기이며 카메라 가시성 GT와 별개입니다. 릴레이는 트레일러가 연결된 동안 `/ets2/ground_truth/trailer/pose`(world, SDK `trailer.0.world.placement`의 위치와 yaw만, yaw = 2π·heading + π/2)도 발행합니다. LCC MPC의 히치각(트럭 yaw − 트레일러 yaw)에 씁니다. 같은 변환을 트럭 SDK heading에 적용한 값은 GT ego 자세 yaw와 1e-6 rad 안에서 같았습니다(2026-10-21). △ 트레일러를 연결한 상태의 토픽은 아직 게임에서 보지 않았습니다.

`drive_speed`는 GT 속도로 throttle/brake를 계산하고 SDK applied steering으로 정규화 조향 위치를 추적합니다. `steering_target`은 **왼쪽 양수 [-1,1]인 applied steering 목표**이며 실제 바퀴 각도(rad)나 토크가 아닙니다. 현재 상대 입력 프로필에서는 PI feedback으로 하위 조향 입력을 계산하므로 사람의 Xbox 설정과 게임의 복귀·차량 물리를 유지합니다. 지도 경로 모드는 ACC·LCC를 연결하며 `acc_enabled`·`lcc_enabled`를 독립 bool 파라미터로 바꿀 수 있습니다.

1. 게임에서 엔진·D 기어·주차브레이크를 주행 가능한 상태로 설정합니다.
2. 릴레이를 띄웁니다(런처 **시작** — 크루즈 대기 노드도 함께 뜹니다, 또는 `pixi run relay bridge/config/bridge.local.json --input-only`로 센서 없이 상태·입력만). rig 보정과 다른 트럭이면 릴레이가 센서 없이 상태·입력만 연결하므로 설정 파일의 slots를 편집할 필요가 없습니다.
3. 저장소 루트의 `pixi shell`에서 명시적으로 노드를 실행하고 게임 창으로 돌아갑니다.

```powershell
pixi shell
ros2 run ets2_autonomy drive_speed --ros-args -p arm:=true -p target_speed_mps:=8.333333 -p steering_target:=0.0
```

런처의 주행 보조 대신 직접 실행하려면 provider로 만든 파일을 위 노드에 `-p path_file:=<current-lane.json 절대 경로>`로 추가합니다. 경로 모드의 조향은 경로 추종기가 계산합니다. 목표 위치 직접 지정은 path_file이 없는 실행에 사용합니다.

다른 `pixi shell`에서 실행 중 목표를 바꿉니다. 속도는 m/s이며 8.333333은 30 km/h입니다.

```powershell
ros2 param set /ets2_drive_speed steering_target 0.02
ros2 param set /ets2_drive_speed steering_target 0.0
ros2 param set /ets2_drive_speed target_speed_mps 0.0
```

목표 속도 0은 제동으로 감속합니다. 멈춘 뒤 Ctrl+C로 자신의 epoch를 해제하고 런처의 **중지**로 브리지를 끝냅니다. `arm` 기본값은 false이며 수동 조작·F11·만료·릴레이 종료 뒤 자동 재arm하지 않습니다. `relative_steering` 기본 true는 현재 `c_relatsteer=1` 프로필에 대응합니다. absolute 프로필(`c_relatsteer=0`)에서는 시작할 때 `-p relative_steering:=false`를 지정합니다.

속도 PI의 `throttle_gain`·`brake_gain` 기본값은 0.2, `speed_integral_gain`은 0.08입니다. 선택적 D gain `speed_derivative_gain`은 기본0입니다. 포화 같은 방향의 적분을 누적하지 않으며, 목표0은 적분기를 리셋합니다. 두 페달을 동시에 보내지 않습니다. 상대 조향은 가상 EPS(`control/virtual_eps.hpp`)가 바퀴각 목표를 따라갑니다: 실측 조향 plant `d(applied)/dt = a(v)·u − c(v)·applied`의 역모델로 `u = (목표 변화율 + c(v)·applied + steering_bandwidth·(목표 − applied))/a(v)`(적분 없음). a(v)·c(v)는 2026-10-20 Scania S(트레일러 없음) 실측 속도 테이블(0–80 km/h, a 0.65→0.32/s, c 0→2.11/s, 80 km/h 초과는 끝값 유지)입니다. 기본값 `steering_bandwidth=6.5`(rad/s), `steering_angle_per_unit_rad=0.70`(주행 속도 실측, 정지 0.7087), `steering_rate_limit=0`(끔)입니다. 차로 추종은 pure pursuit 선형화 피드백 + 차로 곡률 feedforward(`control/lateral_controller.hpp`, `curvature_window_m=6`, `curvature_preview_s=0`)이며, 곡선 속도 제한은 이전처럼 pure pursuit 곡률을 씁니다. 트레일러·다른 차종의 조향 plant는 미측정입니다. 목표·gain은 실행 중 변경할 수 있고 arm·입력 모드는 시작 옵션입니다. 원시 API 소비자는 명령마다 `sent_steady_ns`를 채워 연결된 노드에서 반복 발행해야 합니다(`ros2 topic pub`은 이 값을 채울 수 없음). 새 `topic pub --once` 노드의 discovery 지연은 200 ms 창을 놓칠 수 있습니다.

2026-10-10 FH4 실제 제품 실행은 894/894 표본이 전경이었고, 목표 30 km/h에 최대 **28.62 km/h**였습니다. 조향 목표 .02에 applied는 약 **.0180**까지 따라갔으며, 0 복귀·제동 후 **.00228**이었습니다. 속도 목표 0으로 **.01253 m/s**까지 감속한 뒤 종료했고 최종 관측은 정지·disarmed·출력 3축 0이었습니다. 이 수치는 이전 P 속도·직접 위치 목표 실행이며, 현재 GT 도로 실행은 위 결과에 따릅니다.

## HUD 궤적 오버레이

스택의 계획 궤적과 보조 상태를 게임 메인 화면의 도로 위에 칠한 표시로 그립니다([설계](../docs/24_hud_overlay_design.md)). core 0.45.0부터 색은 장면에 곱해지는 칠(조명·그늘·밤을 따름)입니다. core 0.47.0과 이 릴레이는 `Local\OT_Overlay` ABI 2(획 8개까지, 획마다 폭·색·화면 최소 두께)를 씁니다. ABI가 다른 core에서는 릴레이가 HUD만 끄고 계속 실행합니다(상태 JSON `overlay.writer.abi_mismatch`). 2026-10-21에는 ABI 2 릴레이를 core 0.46(ABI 1)에 붙여 예외가 ROS 실행 스레드에서 새어 릴레이가 멈추거나 죽었습니다.

| 토픽·파라미터 | 계약 |
| --- | --- |
| `/ets2/hud/markers` (구독) | `visualization_msgs/MarkerArray`, HUD의 마커 토픽 하나(`hud` 노드). 노면에는 namespace가 `road/`로 시작하는 `LINE_STRIP`만 칠하고(나머지 `scene/*`는 Foxglove용), 그 마커 하나가 획 하나(8개까지): `scale.x` 노면 폭(m), `scale.y` 화면 최소 폭(1080줄 기준 px, 0이면 없음), `color`(a는 칠의 세기), 0이 아닌 `lifetime`은 TTL. 마커들은 한 `frame_id`(`base_link`/`world`)와 첫 마커의 stamp를 씁니다. 다른 마커 종류는 무시하고, `DELETEALL`은 그 앞의 획을 지우며(rviz와 같음), 획 없이 끝나면 지웁니다. 획마다 200 m에서 자르고 합쳐서 256점으로 솎되 획마다 2점 이상 남깁니다. Foxglove 3D 패널은 같은 토픽을 namespace별로 켜고 끕니다 |
| `/ets2/overlay/path` (구독) | 획 하나짜리 예전 입력(호환). `nav_msgs/Path`. `frame_id`는 `base_link`(header.stamp는 그 경로를 만든 센서 stamp) 또는 `world`. 10–20 Hz, 256점 이내 권장(길면 200 m에서 자르고 256점으로 솎음). best effort 구독이라 reliable 발행자도 받음 |
| `overlay_enabled` | 기본 false: 발행자가 있는 동안만 켬. true면 발행자가 없어도 켬 |
| `overlay_width_m` / `overlay_alpha` / `overlay_ttl_ms` | 기본 3.5 / 0.22 / 300. TTL 동안 새 경로가 없으면 core가 숨김(시뮬레이션 시각 기준이라 일시정지 중에는 유지). 점이 없는 경로는 바로 지움 |
| `/ets2/overlay/style` (구독) | `ets2_msgs/OverlayStyle`(폭 m, RGBA; a는 칠의 세기). 최근 1 s 안에 받은 스타일을 이후 경로에 적용하고, 없으면 위 파라미터와 흰색 |

릴레이는 `base_link` 경로를 자기 SDK 자세 이력(`engine.vehicle.pose_physics`, 최근 128 SDK 프레임)에서 stamp 이하 가장 가까운 자세로 world로 바꿔 `Local\OT_Overlay`에 씁니다. 스택은 GT 자세를 보지 않습니다. 최신 자세보다 500 ms 넘게 오래된 stamp는 숨김으로 씁니다. stamp 0은 최신 자세로 바꿉니다(수동 시험용). 발행자가 생기면 릴레이가 core에 `overlay enable`을 보내, 센서 수집 중이면 같은 훅을 함께 쓰고 아니면 overlay 전용 훅(OM·compile)만 켭니다. 주행 보조(입력·상태만) 실행에서도 그리며 이때도 이 두 훅은 켜집니다. 1 Hz 상태 줄의 `overlay`에 활성 여부·기록 수·마지막 거절 이유가, `/diagnostics`의 `drive_commands`에 받은·거절한 경로 수가 나옵니다.

표시는 2026-10-21 저녁부터 별도 노드 `hud`(`ets2_autonomy`, 런처 주행 보조 모드가 띄움)가 만듭니다. `drive_speed`는 LCC가 조향하는 경로(`/ets2/drive/plan`, 20 Hz, 앞범퍼 1 m 앞부터 ACC 표시 지점 또는 3 s 앞까지, 합류·차로 변경 경로 포함)와 ACC 표시 지점(`AssistState.acc_mark`·`acc_mark_x/y/heading`, 차로 폭, 운전자 가속 개입)만 내고, `hud`가 30 Hz로 `/ets2/hud/markers`를 냅니다(`hud_marks.hpp`). namespace: 노면 `road/lcc_path`·`road/acc`·`road/lane_change`, Foxglove 전용은 질문 하나씩입니다: `scene/truck`(트럭과 트레일러 상자, 꺾임; 앞범퍼·트레일러 킹핀·길이는 추정치), `scene/lane`(쓰는 차로 중심·가장자리, 차로 변경으로 그 차로에 가는 중이면 초록), `scene/lanes`(provider의 옆 차로, 방향지시등 차로 변경이 기다리는 쪽은 호박색), `scene/traffic`(150 m 안 차량, 앞차 초록·차로 변경을 막는 차 호박색·나머지 회색), `scene/acc`(앞차 뒤 ACC 목표 간격 띠와 앞범퍼가 있어야 할 선), `scene/curve`(설정 속도보다 낮은 커브 속도 구간 호박색과 가장 낮은 제한 속도 글자), `scene/status`(트럭 위 상태 글자, 레이아웃에서는 꺼 둠), 그리고 provider의 주변 지도 파일(`follow-lane.scene.json`, 트럭 주변 120 m, 파일이 바뀌면 읽고(0.1 s마다 확인) 매 프레임 트럭 주변 `scene_view_m` 60 m만 그림; 형식은 [map_lane_provider/README.md](map_lane_provider/README.md))에서 `scene/markings`(차선·도로 경계: 같은 방향 차로 사이 3 m 점선, 반대 방향 사이 두 줄 실선, 도로 가장자리·중앙분리대 실선), `scene/junctions`(교차로·램프 prefab의 내비 곡선, 흐리게), `scene/signals`(신호등 위치: 기둥과 등 상자, 등에서 그 신호를 보는 교통 쪽으로 1 m 짧은 선; 게임 SDK가 신호 상태를 내지 않아 회색 고정). `debug/mpc`는 횡방향 MPC가 이번 주기에 계획한 뒷기준점 경로 1.5 s(0.1 s마다 점, drive_speed의 `/ets2/drive/mpc_plan`)로, 계획과 실제 궤적을 비교할 때 켭니다(레이아웃 기본 꺼짐). 세 가지 모두 회색 배경이며 트럭이 파일이 덮는 반경 밖이면(멈춘 provider의 낡은 파일) 그리지 않습니다. 색은 셋입니다: 초록 따르는 것, 호박색 막거나 제한하는 것, 회색 배경. 규칙: LCC 조향 중 0.2 m 흰 띠(화면 최소 3 px), ACC가 앞차 없이 켜져 있으면 지킬 간격 위치의 차로 양 끝 괄호 두 개(가운데가 열림, 흐리게), 앞차가 있으면 그 뒤범퍼의 가로선 하나(괄호가 0.25 s에 걸쳐 미끄러져 와 닫힘), 감속 1 m/s² 넘으면 호박색·2.5 넘거나 비상이면 빨강, 운전자 가속 개입 중 흐리게, 방향지시등 차로 변경이 빈 틈을 기다리면 옆 차로 8–26 m에 점선, 꺼지면 없음. 띠와 가로선은 서로 다른 획이라 겹쳐도 두께가 섞이지 않습니다(0.46의 한 폴리라인 T자는 가로선의 화면 최소 두께가 띠 쪽 픽셀에 번졌습니다). △ 게임 화면 확인 전입니다. 아래는 그 전의 규칙입니다. `drive_speed`는 보조가 잡혀 있는 동안 20 Hz로 world 경로와 스타일을 냈습니다(2026-10-21 낮 규칙). LCC가 조향하는 동안만 앞범퍼 1 m 앞부터 차로 중심 띠를 그리고(운전자 조향·차로 찾기 중에는 없음), ACC가 켜져 있으면 선행차 뒤범퍼 아래 가로줄을 그립니다. 선행차가 없으면 ACC가 지킬 간격(앞범퍼 + 5 m + 시간 간격 × 속도) 위치에 흐린 청록 가로줄을 둡니다. 둘이 함께면 띠가 가로줄에서 끝나 T자가 됩니다. 폭 0.5 m, 색은 추종 초록·간격 흐린 청록·강한 감속 빨강·가속 페달 개입 흰색·ACC 없는 LCC 회색입니다. 그릴 것이 없거나 세션이 끝나면 빈 경로를 보내 바로 지웁니다.

### 게임 크루즈 버튼으로 켜기 (`engage:=cruise`)

`drive_speed --ros-args -p arm:=true -p engage:=cruise -p path_file:=<provider --follow 출력>`은 상주하며 기다립니다. 게임 크루즈(프로필의 LB+RT)가 켜지면 그 속도를 설정 속도로 ACC와 LCC(차로가 아직 없으면 찾는 상태로)를 잡고, 0.05 브레이크 펄스로 게임 크루즈를 끕니다(✔ 2026-10-21 게임: 펄스 뒤 30 ms에 크루즈 0). LB가 스로틀 값을 0으로 고정한 채 RT 조작은 loader가 운전자 입력으로 보므로, 거절되면 게임 크루즈가 켜진 동안 0.1 s 간격으로 10 s까지 다시 arm합니다. 잡힌 뒤 버튼을 다시 누르면(게임 크루즈가 다시 켜짐) 같은 펄스로 끄고 해제합니다. 브레이크나 3 s 조향으로 해제된 뒤 누르면 그 속도로 다시 잡습니다. 게임 상태가 늦게 오면(끊김) 1 s까지는 마지막 명령을 다시 보내며 기다리고(그 사이 계산 없음), 게임이 일시정지되거나 일시정지로 해제되면 3 s 안에 다시 돌 때 같은 설정 속도로 스스로 다시 잡습니다(2026-10-21 저녁: 200 ms 늦음이나 짧은 일시정지 이벤트에 바로 꺼졌음). 더 길면 일시정지·해제 사유가 있어도 끝나지 않고 대기로 돌아가며, 게임이 멈춰 있으면 탐색을 기한 없이 기다립니다. 차로 파일은 잡을 때마다 새로 읽고 주행 중 0.25 s마다 쓰고 있는 차로의 파일(따라가는 차로 또는 좌우 차로)에서 revision을 이어 붙입니다. 그 파일이 다른 차로로 바뀌면(차로 변경 뒤 provider reset) 같은 차로를 가진 파일로 옮기며 진행 중인 합류를 유지합니다.

릴레이 실행 중 수동 시험([설계 T5](../docs/24_hud_overlay_design.md#8-시험-계획)):

```powershell
pixi run -- ros2 topic pub -r 10 /ets2/overlay/path nav_msgs/msg/Path "{header: {frame_id: base_link}, poses: [{pose: {position: {x: 0.0}}}, {pose: {position: {x: 40.0}}}, {pose: {position: {x: 80.0}}}]}"
```

릴레이 없이 world 경로 파일(지도 provider JSON)만 그릴 때는 `.\ot\ot.cmd overlay --file <path.json>`을 씁니다. 이 명령은 lease를 직접 잡으므로 런처·릴레이를 중지한 상태에서 실행합니다([ot 문서](../ot/README.md#hud-궤적-데칼)).

## 기록과 재생

런처에서 브리지를 **시작**하고 연결이 확인되면 기록 종류를 선택해 **기록 시작**을 누릅니다.

| 기록 종류 | 내용·저장 위치 |
| --- | --- |
| 차량 상태 (소용량) | 운전 명령·승인/해제 상태·조작·기어·속도·자세·IMU·휠·GNSS·시계·센서 구성·진단. 기본 bridge/recordings/<run>/bag |
| 영상·라이다 포함 | 차량 상태 + 4뷰 JPEG·보정값·3라이다·차량 GT·렌더 TF·프레임 대응. sensor_recording_root 지정 필요 |

두 종류 모두 원본 RGB·depth는 구독하지 않습니다. 상태 기록만 켜면 카메라 수집을 요구하지 않습니다. 영상·점군의 실제 수신 여부는 선택한 슬롯과 센서 구성에 따릅니다.

**기록 종료**는 파일을 마무리하고 브리지를 유지합니다. 브리지 중지·창 닫기·실행 시간 만료도 기록기를 종료합니다. 기록기는 숨긴 자기 콘솔에서 실행되고, 종료는 그 콘솔에 CTRL_C를 보내는 방식입니다(rosbag2가 MCAP·`metadata.yaml`을 마무리하는 유일한 경로, [P0 실행서 §6](../docs/22_p0_spike_runbook.md)). 8초 안에 끝나지 않으면 강제 종료하고 복구가 필요할 수 있다고 표시합니다. 중복 시작은 기존 기록을 유지합니다. 화면에는 실행 상태·파일 크기·경로, 종료 후 실제 저장 메시지 수를 표시합니다.

저장 경로는 Windows 경로입니다. 지정한 폴더가 존재하고 여유 공간이 있어야 시작하며, 남은 공간이 1 GiB 아래로 내려가면 기록을 마무리합니다. 기본 차량 상태 기록은 Git에서 제외됩니다. 영상·점군용 폴더는 해당 PC의 저장 장치와 용량을 확인해 선택합니다.

기록은 기본 수신 시각을 사용합니다. `--use-sim-time`을 넣지 않습니다. 재생 전 런처에서 중지하고 `pixi shell`에서 실행합니다.

```powershell
ros2 bag info "<런처에서 복사한 bag 경로>"
ros2 bag play "<런처에서 복사한 bag 경로>"
```

기록된 `/clock`을 사용하므로 `--clock`을 넣지 않습니다. 소비자는 `use_sim_time=true`로 설정합니다.

첫 수동 주행은 차량 상태 기록으로 정차·직진·일정 속도·완만한 가속·타력 주행·제동·좌우 선회를 담습니다. 이 기록으로 센서 정합과 차량 반응을 대조합니다. 영상 가림·거리 측정 누락 확인에는 영상·라이다 기록을 별도로 사용합니다.

## 수동 실행

런처를 중지한 상태에서 개발용으로 사용합니다. 각각 저장소 루트의 별도 터미널에서 실행합니다.

```powershell
pixi run ros2 run foxglove_bridge foxglove_bridge --ros-args --params-file bridge/config/foxglove.yaml -p address:=127.0.0.1
pixi run relay bridge/config/bridge.local.json            # 또는 apps\launch.cmd --host-only
```

릴레이는 Ctrl+C로 정상 종료하고 lease를 해제합니다. `/ets2/capture` SetBool 서비스로 렌더 수집을 중지·재개합니다. 파라미터 `camera_enabled`·`lidar_enabled`(기본 true, 실행 중 변경 가능)를 끄면 카메라(`/ets2/camera/*`, `/ets2/frame_info*`, 카메라 GT 객체·마커)나 라이다(`/ets2/lidar/*`) 토픽의 구독이 렌더 요구가 되지 않습니다. 런처는 둘을 끈 채 시작해 체크로 켭니다. 현재 트럭이 rig 보정과 다르면 릴레이는 끝나지 않고 센서 없이 계속 돌며 1 Hz 줄의 `sensors`(`available`, `reason`, `enabled`)에 알립니다. 상태 센서·SDK·clock은 유지됩니다. 서비스 응답은 요청 접수이고 `/diagnostics`의 `capture_active`가 적용 상태입니다. F11 이후에는 릴레이를 다시 시작합니다.

### 처리 단계별 성능 진단

별도 설정 복사본의 `diagnostic_sensor_stage`로 센서 묶음을 처리할 마지막 단계를 선택합니다. 기본값은 `publish`입니다. 모든 단계에서 같은 ROS 구독을 유지하며 상태 발행·소유권·패닉 처리는 계속 동작합니다.

| 값 | 마지막 처리 |
| --- | --- |
| gpu_copy | 센서 생성·pack 후 릴레이 GPU staging 복사와 fence 반환. 영상 Map·CPU 복사 생략 |
| readback | 영상 Map·CPU 복사까지 하고 결과 폐기 |
| encode | JPEG·ROS CDR 메시지까지 생성하고 결과 폐기 |
| publish | ROS 발행까지 수행하는 일반 실행 |

`publish` 외에는 센서 토픽을 발행하지 않습니다. DLL의 작은 라이다·노출 버퍼 읽기는 모든 수집 단계에서 유지됩니다. 릴레이 로그의 `consumed_bundles`, `encoded_bundles`, `sent_bundles`로 실제 처리 진행을 확인합니다. 종료 뒤 일반 설정으로 실행합니다. [측정 결과](../docs/18_performance.md).

## 렌더 모드와의 충돌

센서 카메라는 게임의 미러 슬롯으로 메인 화면과 같은 deferred 파이프라인을 한 번 더 렌더합니다. 게임 렌더러를 가로채는 모드는 이 추가 pass를 메인 화면으로 볼 수 있습니다.

- **snowymoon.io 조명 모드**(`bin/win_x64/dxgi.dll` 프록시): ✔ 2026-10-12, 센서 수집 중 안티앨리어싱(TAA/DLSS)을 켜면 메인 화면이 센서 렌더 주기로 한 프레임씩 깨집니다. 안티앨리어싱을 끄거나 브리지를 끄면 정상입니다. △ 시간 누적 AA 히스토리가 센서 카메라 프레임으로 오염되는 것으로 보이며, AA를 끄면 snowymoon 효과 대부분도 함께 빠집니다.
- **`r_mirror_view_distance`**: 게임 미러와 센서 뷰 모두의 시야거리라 메인 스레드 비용을 크게 좌우합니다. ✔ 2026-10-12 정차·뷰어 켬에서 400 → 250 m로 CPU busy 57.4 → 43.2 ms. 전방 라이다 최대 거리에 맞춰 **250**을 권장합니다(콘솔 `uset r_mirror_view_distance 250`, 종료 시 `config.cfg`에 저장).
- 권장: 센서를 수집하는 개발·기록 세션에서는 snowymoon을 끕니다. 센서 RGB의 조명이 모드 설정에 따라 바뀌면 인지 데이터가 달라지기 때문입니다. 주행 보조(입력·상태만) 실행은 HUD 오버레이를 켜지 않는 한 렌더 hook을 켜지 않으므로 해당하지 않습니다. HUD 데칼도 순정 렌더의 메인 그래프 구조를 전제로 하므로 이런 모드가 켜져 있으면 지원 범위 밖입니다.

## 운영 구조

런처는 단일 Windows mutex로 중복 창을 막고, Windows Job Object로 Foxglove 서버·릴레이·지도 provider·GT 주행·기록기를 소유합니다. 창이 비정상 종료돼도 Job이 닫히며 모두 종료됩니다. 중지는 기록기 CTRL_C → GT 노드 stop event(disarm) → 릴레이 stop event(lease 해제·Tier 0) → Job 종료 순서입니다. 릴레이가 7초 안에 끝나지 않으면 강제 종료하고 lease 만료(5초)를 기다립니다.

[센서 프리셋](../docs/14_phase1_highway_sensors.md), [성능과 운영 구성](../docs/18_performance.md), [주요 시행착오](../docs/history/lessons.md).

현재 센서 기본은 `phase1-highway-fh4-private.json`이고 `bridge/configure.py`의 새 설정도 FH4를 선택합니다. Scania S(`vehicle.scania.s_2016`)에서는 `rig`를 `../../ot/presets/phase1-highway-scania-s-private.json`으로 바꿉니다. ACC/LCC만 쓸 때는 런처의 **주행 보조** 모드를 고릅니다. 다른 차량을 센서 모드로 요청하면 보정 불일치 이유를 표시하고 종료하며 자동으로 재시도하지 않습니다. [차량별 장착점과 확인 범위](../docs/14_phase1_highway_sensors.md).
