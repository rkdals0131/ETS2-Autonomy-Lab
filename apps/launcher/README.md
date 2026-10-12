# 실행 관리 앱

`launcher.py`는 Windows UI와 Job Object로 Foxglove 서버·릴레이 노드·지도 provider·GT 주행 노드·기록기를 소유합니다. pixi 환경에서 실행돼야 하며(`pixi run launcher`) 자식 프로세스는 그 환경을 물려받습니다. 상태는 릴레이의 1 Hz stdout JSON(발행 묶음, 수집, 일시정지, ACC/LCC 소유, 명령 나이)으로 표시하며 rclpy를 쓰지 않습니다. `bag_recording.py`는 런처 안에서 rosbag2 기록기를 시작·종료합니다. 운전 판단과 제어 알고리즘은 `ros2/src/ets2_autonomy`에 있습니다.

자식 종료는 모두 정상 경로를 먼저 씁니다. 릴레이와 GT 노드는 이름 있는 stop event(`--stop-event`)를 기다리고, 기록기는 숨긴 자기 콘솔에서 실행돼 CTRL_C로 MCAP을 마무리합니다. Foxglove 서버와 provider는 Job 종료로 끝납니다.

공개 진입점은 [apps/launch.cmd](../README.md)입니다. `bridge/config/bridge.local.json`, `ros2/install`, `bridge/recordings` 위치를 사용합니다. 실행 중인 이전 런처는 정상 종료 후 다시 열어 새 코드를 사용합니다.

현재 GT 지도 provider는 `bridge/map_lane_provider`에 있고 `research/tools/dotnet10` 및 고정 TruckLib 소스를 사용합니다. 이 의존성과 GPL 라이선스는 [provider 문서](../../bridge/map_lane_provider/README.md)를 따릅니다.
