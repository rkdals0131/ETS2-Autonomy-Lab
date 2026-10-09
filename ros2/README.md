# ROS 소스

| 패키지 | 현재 책임 |
| --- | --- |
| src/ets2_msgs | 센서·차량 상태·운전 명령과 서비스 계약 |
| src/ets2_bridge | Windows 릴레이의 CDR 수신·ROS 발행, 구독·제어 요구 전달 |
| src/ets2_autonomy | GT baseline: 차로 pure-pursuit·속도 PI·앞차 속도 제한·ETS2 입력 adapter |

ROS는 WSL Ubuntu의 Jazzy에서 실행합니다. 현재 저장소는 Windows에 있고 WSL은 같은 `/mnt/c` 소스를 읽습니다. `~/ets2-ros`에는 build/install/log를 보관하며 별도 src 복사본이 없습니다. Linux ext4 checkout은 향후 개발 배치 선택이며 이번 정리에서 복제하지 않았습니다.

## 빌드

실행 중인 기존 브리지를 런처에서 정상 중지한 뒤 빌드합니다. 기존 소스 경로로 만든 build cache는 새 경로를 사용하도록 정리하고 재구성해야 합니다.

```bash
REPO=/mnt/c/path/to/ETS2-Autonomy-Lab
source /opt/ros/jazzy/setup.bash  # Zsh에서는 setup.zsh
cd ~/ets2-ros
/usr/bin/python3 /usr/bin/colcon build --base-paths "$REPO/ros2/src" --cmake-clean-cache --executor sequential --cmake-args -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/usr/bin/python3 -DPYTHON_EXECUTABLE=/usr/bin/python3
source "$REPO/bridge/ros-env.sh"
```

이후 모든 소비자는 `bridge/ros-env.sh`로 overlay·domain 42·DDS 설정을 함께 불러옵니다. ROS 전용 설정은 ets2_bridge 패키지에 설치됩니다.

Jazzy는 시스템 Python 3.12를 사용합니다. 이 머신의 기본 PATH에는 사용자 Python 3.11도 있으므로 위 명령은 시스템 interpreter를 명시합니다. 저장소의 `ros2/build`, `ros2/install`, `ros2/log`는 격리 빌드에 사용한 Git 제외 산출물이며 일반 실행 overlay는 `~/ets2-ros/install`입니다. 향후 Linux ext4 checkout을 원본으로 선택하면 그 checkout의 `ros2`에서 `src/build/install/log` workspace로 운용할 수 있습니다.

## 실행

일반 사용은 기존 [런처](../apps/launcher/README.md)로 합니다. 수동 ROS 브리지 실행은 다음 명령이며 Windows 릴레이가 별도로 연결되어야 합니다.

```bash
source "$REPO/bridge/ros-env.sh"
ros2 launch ets2_bridge bridge.launch.py config:="$REPO/bridge/config/bridge.local.json"
```

GT baseline 실행 패키지는 `ets2_autonomy`입니다. `ros2 run ets2_bridge drive_speed`도 새 실행파일로 넘기는 호환 진입점이며 노드명 `/ets2_drive_speed`, 메시지·토픽·파라미터는 유지합니다. 실제 운전은 [ACC/LCC 사용법](../bridge/README.md#gt-도로-자동-주행)을 따릅니다. 인지·위치 추정·FSM·목적지 경로 계획은 아직 없으며 [다음 모듈 경계](../docs/03_system_design.md#ros-개발과-다음-스택)에 따라 이 패키지부터 확장합니다.
