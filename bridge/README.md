# 유로파일럿 브리지와 런처

## 실행

게임을 Steam의 **DirectX11 (64-bit)**로 실행한 뒤 [apps/launch.cmd](../apps/launch.cmd)를 엽니다. `launch.cmd`는 pixi 환경(`pixi run launcher`)에서 런처를 띄우므로 런처가 시작하는 모든 프로세스가 같은 ROS 2 환경과 DDS 프로필을 씁니다.

- **시작:** Foxglove 서버(`127.0.0.1:8765`)와 릴레이 노드(`ets2_relay`)를 띄웁니다.
- **중지:** 기록기·GT 주행·릴레이를 정상 종료하고 수집 소유권을 해제한 뒤 이번 실행의 프로세스를 모두 종료합니다.
- **재시작:** 기존 실행을 정리한 뒤 설정 파일을 다시 읽습니다.
- **창 닫기:** 이번 실행을 종료합니다.
- **다시 열기:** 기존 런처 창을 표시합니다.

화면은 SDK 연결, 프로세스 PID, 발행 묶음 수·큐 누락·50 ms 넘은 발행 수, 운전 명령 전달·지연 폐기 수, Foxglove 주소를 표시합니다. 50 ms 넘은 발행은 릴레이 상태 JSON의 `slow_publishes.recent`에 시각·토픽·잠금 대기·발행 시간으로 최근 16건이 남습니다. 연결 상태는 릴레이의 1 Hz 상태 보고로 판정합니다. 센서 구독이 없으면 영상 수집을 기다립니다.

`실행 시간(초)`, `카메라 Hz`, `라이다 Hz`, `미리보기 Hz`는 시작할 때 `config/bridge.local.json`에 저장됩니다. 기본 주기는 카메라 30 Hz·라이다 10 Hz·JPEG 미리보기 10 Hz입니다. 릴레이는 BELOW_NORMAL 우선순위로 게임에 CPU를 양보합니다. 시간이 끝나면 중지합니다. F11·게임 오류·프로세스 종료 뒤에는 사용자가 시작을 눌러 새 실행을 엽니다. 게임 실행·기어·주차브레이크는 사용자가 설정합니다.

**입력·상태만**을 체크하면 이번 실행만 센서 슬롯을 비워 FH4에서도 상태·운전 API를 연결합니다. 기존 센서 설정은 유지하며, 체크를 풀고 다시 시작하면 원래 슬롯을 사용합니다.

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

Windows Foxglove에서 `ws://127.0.0.1:8765`에 연결합니다.

- 영상: `/ets2/camera/{C_FN,C_FW,C_RL,C_RR}/preview/image/compressed`
- 표시용 점군: `/ets2/lidar/{L_F,L_PL,L_PR}/preview/points`
- 전체 점군: `/ets2/lidar/{L_F,L_PL,L_PR}/points`
- 박스: `/ets2/ground_truth/{camera}/markers`
- 상태: `/ets2/vehicle/state`, `/diagnostics`, `/tf`, `/clock`
- 조작과 기어: `/ets2/vehicle/actuation`의 운전자 입력·실제 적용 입력·기어
- 추가 센서: `/ets2/imu/data_raw`, `/ets2/wheels/state`, `/ets2/wheels/odometry`, `/ets2/gnss/fix`

라이다는 Color map → range → Turbo로 설정하고 가까운 장면은 0–30m 또는 0–50m 범위를 사용합니다. 원본 RGB·depth와 `/perception/image_raw`는 ROS 소비자가 직접 구독합니다. [토픽·시각 계약](../docs/16_ros2_bridge.md).

전방 원본은 1280×720, 측면 원본은 960×544입니다. JPEG·인지용 영상은 각각 640×360·480×272입니다. HD 원본을 구독하면 readback·직렬화·발행 비용이 증가합니다. 30 Hz 설정은 수집 요청 주기이며 실제 전달률은 게임과 소비자 처리량에 따라 달라집니다.

인지용 영상은 카메라 주기를 따르고 JPEG 미리보기는 `preview_hz`를 따릅니다. 표시용 점군은 XYZ·range만 전달하며 전체 점군에서 선택한 빔의 값을 그대로 사용합니다. 기존 Foxglove 레이아웃의 점군 토픽을 `/preview/points`로 바꾸면 표시 비용이 줄어듭니다. 원본 점군의 필드·10 Hz 주기는 유지됩니다.

## GT 도로 자동 주행

현재 위치의 실제 지도 차로 기준선을 생성하고 GT 위치로 추종합니다. 속도 PI, pure-pursuit 경로 추종, applied steering PI를 연결했습니다. 같은 차로 AI 차량의 GT 거리·속도로 목표 속도를 낮추며, 선택한 기준선 끝과 급한 곡선에서도 감속합니다. 차량·바퀴 pose나 사람 Xbox 설정은 쓰지 않습니다.

**입력·상태만** 체크 → **시작** → 목표 km/h와 **ACC 속도 / LCC 차로** 선택 → **선택 적용 / 켜기**를 누르고 게임 창으로 돌아갑니다. 현재 위치에서 지도 차로 생성과 단일 제어기 실행까지 연결합니다. 실행 중에도 선택·목표 속도를 바꾸고 같은 버튼으로 적용합니다(세 값을 `set_parameters_atomically` 한 번으로 바꿔 owner·epoch를 유지). 체크박스는 요청값이고 아래 상태는 게임이 보고한 실제 ACC/LCC 켜짐·꺼짐과 해제 이유입니다. 엔진·D 기어·주차브레이크는 게임에서 주행 가능한 상태로 설정합니다. **입력 해제** 또는 두 선택을 모두 끄고 적용하면 GT 노드가 stop event를 받아 disarm합니다. 차량을 계속 세워 두는 기능은 아닙니다. 브리지 중지·창 닫기도 주행 소유권과 프로세스를 정리합니다.

**ACC만** 켜면 가속·제동을 맡기고 사람이 조향합니다. **LCC만** 켜면 조향을 맡기고 사람이 가감속합니다. 소유하지 않은 축의 사람 입력은 유지하며, 자동으로 맡긴 축을 사람이 조작하면 **모든 보조가 해제**됩니다. F11·만료·릴레이 종료·pause도 전체 해제하며 실패 후 자동으로 다시 켜지지 않습니다. 독립 ACC는 매 GT 프레임의 현재 자차 위치·진행방향과 실제 차로폭으로 앞차를 선택하여, 시작 LCC 경로의 이탈·거리 끝 때문에 제동하지 않습니다. 이 진행방향 corridor 방식은 굽은 도로에서 먼 앞차를 놓칠 수 있습니다. LCC 단독은 기준선 이탈·끝에서 페달을 조작하지 않고 해제합니다. 두 기능을 함께 켰을 때는 기존 지도 경로 끝 감속을 사용합니다.

독립 축 지원은 **core/loader 0.29.0**에서 추가됐습니다. 상주 loader 교체는 정상 게임 종료 후 설치·재시작이 필요합니다. 새 ROS 메시지 타입을 쓰면 `pixi run build`로 릴레이와 ROS 패키지를 함께 다시 빌드합니다. 구 host ABI-1 prefix와 기존 JSON 클라이언트는 전체 축 계약을 유지하고, 새 core에 구 loader를 연결하면 독립 모드를 거절합니다.

현재 provider는 설치된 지도 자산의 `ger16` 단방향 차로와 같은 지원 layout에서 현재 진행방향 차로를 고르고, 공유 Node로 연결되는 도로를 최대 1 km까지 잇습니다. 실제 모델 노면·shoulder로 차로 폭을 구합니다. prefab/갈림길·layout 변경·선택 거리 끝에서는 기준선이 끝나므로 감속합니다. 내비 목적지 선택·교차로/신호 통과·차로 변경은 아직 없습니다. [지도 provider 빌드·지원 범위](map_lane_provider/README.md).

2026-10-10 FH4 전경 주행(이전 TCP 구성)에서 기준선 약 **453 m**를 진행했습니다. 제품 로그의 중심선 횡오차는 최대 **1.35 m**, 후반 **0.03–0.57 m**, 후반 속도는 **29.84–29.97 km/h**였습니다. 목표 30 km/h의 초기 PI 전이에서 **35.86 km/h** overshoot가 있었으며 개선 대상입니다. 목표0 제동·명시적 해제 후 최종 상태는 정지·disarmed·자동 3축0이었습니다. 먼 AI 차량은 723–819 m에서 관측됐지만 근접 앞차 추종 제동은 이 실행에서 검증하지 않았습니다. 이는 현재 도로의 짧은 주행 결과이며 차체 전체의 차로 내 유지나 일반 도로 완주를 입증하지 않습니다. Windows 네이티브에서의 재확인은 [포팅 계획](../docs/20_windows_native_port_plan.md) P1 완료 기준입니다.

## 운전 명령 API

상주 loader는 공식 SCS Input SDK에 `ot_drive` 가상 3축 장치를 등록합니다. 사람의 키보드/Xbox 바인딩을 유지하면서 컴퓨터가 횡방향 조향과 종방향 가속·제동을 보냅니다. 장치 등록·disarmed Xbox 좌우/페달 대조·ROS arm/disarm과 FH4 실제 가속·조향·제동을 확인했습니다. FFB나 조향 토크 제어는 구현하지 않았습니다.

최초 등록은 정상 게임 종료 → 새 loader/core 설치 → 게임 재시작 순서입니다. SDK는 input init 때만 장치를 등록하므로 기존 loader의 hot reload로 추가할 수 없습니다. 설치된 `ot_runtime/ot_config.json`에서 `allow_drive: true`, `singleplayer_research: true`, 활성 `controls.sii`의 절대 경로 `drive_controls_path`를 설정합니다. 저장소 기본 권한은 꺼져 있으며 센서 설정과 사용자 controls.sii는 바꾸지 않습니다.

| 창구 | 계약 |
| --- | --- |
| `/ets2/drive/control` | `DriveControl` 서비스: owner, arm, epoch, axes → success, message, epoch |
| `/ets2/drive/command` | `DriveCommand`: owner, epoch, 증가하는 sequence, sent_steady_ns, steering/throttle/brake, axes |
| `/ets2/drive/state` | `DriveState`: 활성/해제 이유, owner·epoch·마지막 승인 sequence, 유효창, 명령·물리입력·SDK 적용값, 실제 axes·independent_axes |

요청 `axes`는 1=조향/LCC, 2=페달/ACC, 3=전체이며 0은 기존 전체 축 요청의 기본값입니다. 실제 상태 `axes=0`은 해제 상태입니다. 모두 끄기는 `arm=false`로 해제하며 `axes=0` arm을 보내지 않습니다. 한 owner/epoch 안의 새 명령으로 축을 전환하고 소유하지 않은 출력은 0으로 만듭니다. 새로 맡길 축을 사람이 조작 중이면 해제하므로 켜기 전에 그 축을 놓습니다.

DriveCommand.steering은 **왼쪽 양수 [-1,1]인 프로필 입력**, throttle/brake는 **[0,1]**입니다. 현재 Xbox 프로필의 `c_relatsteer=1`은 조향 위치를 변화시키는 상대 입력입니다. 따라서 이 하위 명령값을 바퀴 위치나 각도 목표로 해석하지 않습니다. 현재 프로필의 steering mix가 semantical 값을 빼므로 SDK 장치에는 steering의 부호를 뒤집어 내보냅니다. 기존 `/ets2/vehicle/actuation`의 단위·부호는 유지합니다. applied 값은 게임의 effective 입력 관측이며 명령을 복사한 값이 아닙니다. SDK 입력은 실제 게임 전경에서 소비됐습니다. 비전경 실행은 native 명령이 승인돼도 게임 입력 축과 속도가 0이었으며, 전경 실행에서 가속·제동이 적용됐습니다.

arm은 단일 owner에 새 epoch를 발급하고 200 ms 동안 첫 명령을 기다립니다. Command의 `sent_steady_ns`는 명령을 계산한 시점의 **발행 프로세스 steady clock ns**(Windows에서는 QPC: C++ `std::chrono::steady_clock`, Python `time.perf_counter_ns()`; `time.monotonic` 금지)이며 ROS stamp가 아닙니다. 릴레이 노드는 나이가 `max_command_age_ms`(노드 파라미터, 기본 50)를 넘거나 음수·0인 명령을 버리고, 가장 최근 명령만 core에 넘기면서 core 시계로 200 ms 유효창을 붙입니다(실차 DBW heartbeat와 같은 형태). 이전 epoch·반복 sequence·만료된 명령은 거절합니다. DriveState의 `command_window_ms`는 진단용 Windows 시계값이며 명령에 복사하지 않습니다. owner는 식별자이며 로컬 pipe ACL을 대신하지 않습니다.

명령 만료·소유한 축의 물리 조작·F11·pause·SDK 입력 비활성·core unload·릴레이 종료는 arm을 해제합니다. resume·릴레이 재시작은 다시 arm하지 않습니다. F11은 bridge lease도 취소하므로 런처를 명시적으로 재시작합니다. SDK 한 입력 프레임의 세 축은 같은 snapshot이며 해제는 다음 입력 프레임의 세 축 0으로 반영합니다. 렌더 수집 on/off와 운전 arm은 독립입니다.

수동 해제는 현재 프로필의 A/Left·D/Right·W/Up·S/Down, `joy.x/rt/lt`와 패드 버튼(`joy.bN`)을 직접 읽습니다. controls.sii의 deadzone·축 변환·custom modifier(`!`·`&`, 예: LB를 누르는 동안 스로틀 값 유지)를 적용하고, 반대 키나 키보드/패드가 서로 상쇄돼도 각 물리 source의 활동을 보고 해제합니다. modifier를 누른 채 트리거를 당겨도 활동으로 보고 해제합니다. pad 부재는 프로필의 `?0` fallback대로 중립이며 키보드 조작은 유지합니다. pad 연결 변화는 arm을 해제하고 읽기 오류·지원하지 않는 binding(`&&`·`||` 포함)에서는 arm을 허용하지 않습니다. 프로필 변경은 권한 재읽기 또는 재시작으로 반영합니다. armed 상태의 수동 해제·F11 시험은 [안전 체크리스트](../docs/21_safety_checklist.md)에 기록합니다.

게임 안전 시험용 명령 발행기는 `ros2 run ets2_autonomy drive_hold --ros-args -p axes:=3 -p throttle:=0.0`입니다. arm 뒤 고정 명령을 50 Hz로 보내고 Ctrl+C 또는 `--stop-event`에서 disarm하며 DriveState 해제 사유를 출력합니다.

### 속도 목표와 조향 위치 목표

core 0.28.0은 카메라와 독립된 `/ets2/ground_truth/traffic`을 SDK frame_end에서 요청 시 발행합니다. AI actor 위치·진행방향·속도·차체 크기이며 카메라 가시성 GT와 별개입니다.

`drive_speed`는 GT 속도로 throttle/brake를 계산하고 SDK applied steering으로 정규화 조향 위치를 추적합니다. `steering_target`은 **왼쪽 양수 [-1,1]인 applied steering 목표**이며 실제 바퀴 각도(rad)나 토크가 아닙니다. 현재 상대 입력 프로필에서는 PI feedback으로 하위 조향 입력을 계산하므로 사람의 Xbox 설정과 게임의 복귀·차량 물리를 유지합니다. 지도 경로 모드는 ACC·LCC를 연결하며 `acc_enabled`·`lcc_enabled`를 독립 bool 파라미터로 바꿀 수 있습니다.

1. 게임에서 엔진·D 기어·주차브레이크를 주행 가능한 상태로 설정합니다.
2. `apps/launch.cmd`에서 **입력·상태만**을 체크하고 **시작**합니다. 센서 수집 없이 상태·입력만 연결하며 설정 파일의 slots를 편집할 필요가 없습니다.
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

릴레이는 Ctrl+C로 정상 종료하고 lease를 해제합니다. `/ets2/capture` SetBool 서비스로 렌더 수집을 중지·재개합니다. 상태 센서·SDK·clock은 유지됩니다. 서비스 응답은 요청 접수이고 `/diagnostics`의 `capture_active`가 적용 상태입니다. F11 이후에는 릴레이를 다시 시작합니다.

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
- 권장: 센서를 수집하는 개발·기록 세션에서는 snowymoon을 끕니다. 센서 RGB의 조명이 모드 설정에 따라 바뀌면 인지 데이터가 달라지기 때문입니다. **입력·상태만** 실행은 렌더 hook을 켜지 않으므로 해당하지 않습니다.

## 운영 구조

런처는 단일 Windows mutex로 중복 창을 막고, Windows Job Object로 Foxglove 서버·릴레이·지도 provider·GT 주행·기록기를 소유합니다. 창이 비정상 종료돼도 Job이 닫히며 모두 종료됩니다. 중지는 기록기 CTRL_C → GT 노드 stop event(disarm) → 릴레이 stop event(lease 해제·Tier 0) → Job 종료 순서입니다. 릴레이가 7초 안에 끝나지 않으면 강제 종료하고 lease 만료(5초)를 기다립니다.

[센서 프리셋](../docs/14_phase1_highway_sensors.md), [성능과 운영 구성](../docs/18_performance.md), [주요 시행착오](../docs/history/lessons.md).

현재 센서 기본은 `phase1-highway-fh4-private.json`이고 `bridge/configure.py`의 새 설정도 FH4를 선택합니다. Scania S(`vehicle.scania.s_2016`)에서는 `rig`를 `../../ot/presets/phase1-highway-scania-s-private.json`으로 바꿉니다. ACC/LCC만 쓸 때는 **입력·상태만**을 선택합니다. 다른 차량을 센서 모드로 요청하면 보정 불일치 이유를 표시하고 종료하며 자동으로 재시도하지 않습니다. [차량별 장착점과 확인 범위](../docs/14_phase1_highway_sensors.md).
