# 실행 관리 앱

`launcher.py`는 Windows UI·Job Object·릴레이·지도 provider를, `wsl_session.py`는 WSL systemd unit 안의 ROS/Foxglove·자동주행·기록 프로세스를 관리합니다. `bag_recording.py`는 해당 세션의 기록기입니다. 운전 판단과 제어 알고리즘은 `ros2/src/ets2_autonomy`에 있습니다.

기존 `bridge/launch.cmd` 또는 `bridge/launcher.py`로 실행합니다. `ot/.venv`, `bridge/config/bridge.local.json`, `bridge/dist`, `bridge/recordings` 위치는 그대로입니다. 실행 중인 이전 런처는 정상 종료 후 다시 열어 새 코드를 사용합니다.

WSL 실행 환경은 `bridge/ros-env.sh`를 사용합니다. Linux에서는 같은 ROS launch·자동주행 명령·rosbag2를 사용할 수 있습니다. Windows UI는 Win32 Job·게임 파이프·Windows 실행파일을 사용하므로 Linux에서 그대로 실행할 수 없습니다. Linux용 전체 관리 UI와 게임 host 관리 API는 아직 구현하지 않았습니다.

현재 GT 지도 provider는 `bridge/map_lane_provider`에 있고 `research/tools/dotnet10` 및 고정 TruckLib 소스를 사용합니다. 이 의존성과 GPL 라이선스는 [provider 문서](../../bridge/map_lane_provider/README.md)를 따릅니다.
