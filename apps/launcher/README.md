# 실행 관리 앱

`launcher.py`는 Windows UI·Job Object·릴레이·지도 provider를, `wsl_session.py`는 WSL systemd unit 안의 ROS/Foxglove·자동주행·기록 프로세스를 관리합니다. `bag_recording.py`는 해당 세션의 기록기입니다. 운전 판단과 제어 알고리즘은 `ros2/src/ets2_autonomy`에 있습니다.

공개 진입점은 [apps/launch.cmd·launch.sh](../README.md)입니다. 기존 `bridge/launch.cmd`와 `bridge/launcher.py`는 호환 경로입니다. `ot/.venv`, `bridge/config/bridge.local.json`, `bridge/dist`, `bridge/recordings` 위치는 그대로입니다. 실행 중인 이전 런처는 정상 종료 후 다시 열어 새 코드를 사용합니다.

`linux_launcher.py`는 native Linux 터미널에서 실행·상태·중지·보조·기록 명령을 받아 같은 `wsl_session.py`를 관리합니다. 공통 세션은 systemd unit·flock으로 ROS 중복 실행을 막고 실제 상태를 stdout과 자신의 runtime directory에 발행합니다. Linux 중지는 고정 Linux unit에만, Windows 중지는 해당 UUID unit에만 적용됩니다. Windows UI의 Win32 Job·게임 파이프는 Linux 코드에서 사용하지 않습니다. 환경은 양쪽 모두 `bridge/ros-env.sh`를 사용합니다.

현재 GT 지도 provider는 `bridge/map_lane_provider`에 있고 `research/tools/dotnet10` 및 고정 TruckLib 소스를 사용합니다. 이 의존성과 GPL 라이선스는 [provider 문서](../../bridge/map_lane_provider/README.md)를 따릅니다.
