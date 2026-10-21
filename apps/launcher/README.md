# 실행 관리 앱

`launcher.py`는 Windows UI와 Job Object로 Foxglove 서버·릴레이 노드·지도 provider·GT 주행 노드·기록기를 소유합니다. pixi 환경에서 실행돼야 하며(`pixi run launcher`) 자식 프로세스는 그 환경을 물려받습니다. 상태는 릴레이의 1 Hz stdout JSON(발행 묶음, 수집, 일시정지, ACC/LCC 소유, 명령 나이)으로 표시하며 rclpy를 쓰지 않습니다. `bag_recording.py`는 런처 안에서 rosbag2 기록기를 시작·종료합니다. `cpu_layout.py`는 시작할 때 CPU 토폴로지(물리 코어·L3 묶음)를 읽어 게임 메인 스레드에 코어 하나를 주고 자식 프로세스(Job affinity)와 Foxglove 뷰어를 다른 L3 묶음으로 보내며, 중지할 때 되돌립니다. 운전 판단과 제어 알고리즘은 `ros2/src/ets2_autonomy`에 있습니다.

## 화면

**시작**을 누릅니다. 같은 버튼이 실행 중에는 **중지**입니다. 릴레이는 설정의 rig로 뜨고, 주행 보조와 센서는 함께 쓸 수 있습니다(2026-10-21 전에는 주행 보조와 센서 수집이 서로 배제되는 모드였습니다).

- **주행 보조(늘 켜짐):** 연결되면 크루즈 버튼 대기를 자동으로 켭니다. provider `--follow`(`bridge/recordings/gt-path/follow-lane.json`)와 `drive_speed engage:=cruise`를 함께 띄워 두고, 게임 크루즈(LB+RT)를 켜면 그 속도로 ACC와 LCC(트럭이 지도 차로 위일 때)를 잡고 다시 누르면 끕니다. 브레이크나 3 s 넘게 이어진 조향은 ACC와 LCC를 함께 끄고(LB+RT로 다시 켬), 짧은 조향에는 LCC가 비켜 주며, 방향지시등을 켜면 옆 차로가 빌 때 차로를 바꿉니다([브리지 문서](../../bridge/README.md#게임-크루즈-버튼으로-켜기-engagecruise)). 대기는 축을 잡지 않으므로 켜 두어도 직접 운전에 영향이 없습니다. 대기 노드가 끝나면 다시 띄우지 않으며, 중지 후 시작으로 새로 엽니다. 판단 노드 `planner`도 함께 뜹니다. 행동 트리와 차선 변경 상태기계가 지금 무엇을 하려는지 Foxglove **Behavior board** 패널에 보여 주기만 하고(그림자 모드), 운전은 하지 않습니다([docs/32](../../docs/32_behavior_planner.md)). 점유 노드 `occupancy`도 뜹니다. 라이다를 켜면 전체 점군을 받아 GT 차량으로 설명되지 않는 장애물(사고·랜덤 인카운터 등)을 찾고, Foxglove에 `/ets2/perception/occupancy`(점유 격자)와 `/ets2/perception/obstacles`(빨강 설명 안 됨, 회색 GT 차량)를 냅니다. 라이다가 꺼져 있으면 아무 일도 하지 않습니다.
- **센서(카메라·라이다 체크, 기본 꺼짐):** 비용이 드는 선택은 게임 안 렌더링과 readback뿐이라 이것만 고릅니다. 켜 둔 센서는 Foxglove 패널이나 센서 포함 기록이 구독할 때만 렌더링하고, 꺼 둔 센서는 구독이 있어도 렌더링하지 않습니다. 실행 중에 바꾸면 `ros2 param set /ets2_relay camera_enabled|lidar_enabled`로 적용하고, 릴레이 1 Hz 줄의 `sensors.enabled`가 같아질 때까지 세 번까지 다시 보냅니다. 라이다는 미러 뷰의 depth로 만들므로 카메라를 꺼도 그 뷰는 렌더링합니다. 현재 트럭이 rig 보정과 다르면 릴레이는 센서 없이 계속 돌고(`sensors.available` false), 체크를 막고 이유를 보여 줍니다.

ACC·LCC 선택 체크는 없앴습니다. 시험용으로 LCC만 끄려면 `ros2 param set /ets2_drive_speed lcc_enabled false`를 씁니다.

상태 줄은 게임 SDK 연결, 브리지 단계(운전 명령 전달·지연 폐기 수, 센서를 수집한 적이 있으면 수집 여부·발행 묶음·큐 누락·50 ms 넘은 발행 수), 게임이 보고한 실제 ACC/LCC 켜짐(`drive.axes` 1=LCC, 2=ACC)과 `drive_speed`의 마지막 알림, Foxglove 서버(`ws://127.0.0.1:8765`)를 보여 줍니다.

**기록**은 **차량 상태 기록**과 **센서 포함 기록**(카메라나 라이다를 켰을 때, 시작할 때 켜진 센서의 토픽만) 중 하나로 시작하고 **기록 종료**로 마무리합니다. bag 경로와 실행 로그 폴더는 각각 **복사**로 클립보드에 넣습니다.

실행 시간·카메라·라이다·미리보기 주기·CPU 배치·게임 미러 렌더는 `bridge/config/bridge.local.json`에서만 바꿉니다(**설정 파일 열기**). 창은 다음 시작에 읽을 값을 요약해 보여 주고, 파일을 저장하면 바로 갱신합니다.

## 실행 로그

시작할 때마다 `bridge/recordings/logs/<YYYYMMDD-HHMMSS>/`를 만들고 자식 출력을 `<source>.log`(`relay`, `foxglove`, `follow`, `drive`, `hud`, `planner`, `occupancy`, `param`)에 줄마다 시각(ms)을 붙여 남깁니다. `drive.log`에는 `drive_speed`의 1 Hz 상태 줄이 모두 남고, `relay.log`의 1 Hz 상태 JSON은 5초에 한 줄만 남깁니다. 런처 자체의 알림과 오류는 `launcher.log`에 남습니다. 창의 로그 칸은 사건만 보여 주며 1 Hz 상태 줄과 provider 진행 줄은 파일에만 있습니다. 기록기 출력은 bag 폴더의 `recorder.log`에 따로 남습니다.

## 종료

자식 종료는 모두 정상 경로를 먼저 씁니다. 릴레이와 GT 노드는 이름 있는 stop event(`--stop-event`)를 기다리고, 기록기는 숨긴 자기 콘솔에서 실행돼 CTRL_C로 MCAP을 마무리합니다. Foxglove 서버와 provider는 Job 종료로 끝납니다. 창을 닫으면 이번 실행의 프로세스를 모두 종료합니다.

공개 진입점은 [apps/launch.cmd](../README.md)입니다. `bridge/config/bridge.local.json`, `ros2/install`, `bridge/recordings` 위치를 사용합니다. 실행 중인 이전 런처는 정상 종료 후 다시 열어 새 코드를 사용합니다.

현재 GT 지도 provider는 `bridge/map_lane_provider`에 있고 `research/tools/dotnet10` 및 고정 TruckLib 소스를 사용합니다. 이 의존성과 GPL 라이선스는 [provider 문서](../../bridge/map_lane_provider/README.md)를 따릅니다.
