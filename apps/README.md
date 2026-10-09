# 실행 진입점

## Windows 전체 모드

`apps/launch.cmd`를 열고 **시작**을 누릅니다. 기존 UI가 Windows 릴레이와 WSL ROS/Foxglove·GT 주행·기록을 함께 관리합니다.

## Linux가 ROS를 관리하는 모드

WSL Ubuntu에서 저장소 루트 기준으로 실행합니다. ROS 환경은 진입점이 불러옵니다.

```bash
./apps/launch.sh start
```

ROS가 준비되면 Windows 터미널에서 게임 릴레이만 연결합니다.

```powershell
.\apps\launch.cmd --host-only
```

양쪽은 같은 `bridge/config/bridge.local.json`의 pairing·포트 설정을 사용합니다. 다른 파일은 Linux `--config <경로>`, Windows `--host-only <경로>`로 지정합니다. Linux 화면은 실제 VehicleState 수신과 ACC/LCC 상태를 보여줍니다. Windows의 전체 모드와 Linux 모드는 동일한 ROS 세션을 중복 생성하지 않습니다. 사용 중이라는 안내가 나오면 기존 런처에서 정상 중지한 뒤 모드를 바꿉니다. host-only의 중복 실행도 게임 lease에서 거부하며 기존 실행은 유지합니다.

Linux 실행 터미널에는 다음 명령을 입력할 수 있습니다.

| 명령 | 동작 |
| --- | --- |
| status / stop | 실제 상태 확인 / 이 Linux 세션 종료 |
| assist `<차로JSON>` `<km/h>` both/acc/lcc | 명시적으로 GT 보조 시작. 현재 위치용 차로 JSON이 필요 |
| mode `<km/h>` both/acc/lcc | 자신이 시작한 보조의 속도·모드 변경 |
| assist-stop | 자신이 시작한 보조 해제 |
| record state/sensors / record-stop | 기존 차량 상태·센서 기록 시작 / 종료 |

GT 차로 JSON은 [지도 provider](../bridge/map_lane_provider/README.md)로 현재 위치에서 생성합니다. Linux 시작 자체는 arm하지 않습니다. 소유한 축의 수동 입력·F11·만료 뒤에는 자동으로 다시 arm하지 않습니다. 센서 기록은 기존 설정에 저장 폴더를 지정해야 합니다.

다른 Linux 터미널에서도 `./apps/launch.sh status`·`./apps/launch.sh stop`을 사용할 수 있습니다. 중지할 때는 Windows host-only 터미널에서 **Ctrl+C**, Linux에서 **stop** 또는 **Ctrl+C**를 사용합니다. 각 진입점은 자신이 소유한 프로세스만 종료합니다. 실행 시간은 기존 `duration_s`를 따릅니다.

Linux 진입점은 Win32·게임 DLL·D3D 릴레이를 import하거나 실행하지 않습니다. 현재 실제 연결은 Windows 게임 + WSL Ubuntu이며, 별도 Linux PC에 대한 자동 host 연결 관리는 제공하지 않습니다. ROS listener의 `bind_address`를 명시하면 eth0 외의 IPv4 주소도 사용할 수 있고, 생략하면 기존 WSL eth0를 사용합니다.

구현은 [launcher/](launcher/README.md), ROS 빌드는 [ros2/](../ros2/README.md)에 있습니다.
