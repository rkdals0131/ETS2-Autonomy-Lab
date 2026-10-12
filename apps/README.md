# 실행 진입점

## 런처

`apps/launch.cmd`를 열고 **시작**을 누릅니다. `launch.cmd`는 저장소 루트에서 `pixi run launcher`를 실행해 pixi ROS 2 환경의 창을 띄우고 바로 끝납니다. 런처는 Foxglove 서버·릴레이 노드·GT 주행·기록을 함께 관리합니다. 사용법은 [브리지와 런처](../bridge/README.md)에 있습니다.

## 릴레이만 실행

ROS 쪽을 직접 다룰 때는 게임 릴레이 노드만 띄웁니다.

```powershell
.\apps\launch.cmd --host-only                   # bridge\config\bridge.local.json
.\apps\launch.cmd --host-only <설정 경로>
```

이 콘솔에서 **Ctrl+C**로 정상 종료하고 lease를 해제합니다. 런처와 host-only를 동시에 띄우면 게임 lease에서 뒤의 것이 거부되고 기존 실행은 유지됩니다.

pixi가 없으면 `launch.cmd`가 설치 명령을 안내하고 끝납니다. 최초 빌드는 [브리지의 최초 준비](../bridge/README.md#최초-준비)를 따릅니다. 구현은 [launcher/](launcher/README.md), ROS 빌드는 [ros2/](../ros2/README.md)에 있습니다.
