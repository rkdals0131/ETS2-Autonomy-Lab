# ROS 소스

> 이 문서는 WSL Ubuntu 기준의 현재 절차입니다. [Windows 네이티브 포팅 계획](../docs/20_windows_native_port_plan.md) P1이 끝나면 pixi 환경 절차로 바뀝니다.

| 패키지 | 현재 책임 |
| --- | --- |
| src/ets2_msgs | 센서·차량 상태·운전 명령과 서비스 계약 |
| src/ets2_bridge | Windows 릴레이의 CDR 수신·ROS 발행, 구독·제어 요구 전달 |
| src/ets2_autonomy | GT baseline: 차로 pure-pursuit·속도 PI·앞차 속도 제한·ETS2 입력 adapter(연결 확인용, 동결) |
| src/ets2_sensor_degradation | `/ets2/*` 이상 센서에 seed 고정 잡음·지연·드롭을 적용해 `/sensing/*`로 재발행 |

ROS는 WSL Ubuntu의 Jazzy에서 실행합니다. 현재 저장소는 Windows에 있고 WSL은 같은 `/mnt/c` 소스를 읽습니다. `~/ets2-ros`(`ETS2_ROS_WS`로 변경 가능)에는 build/install/log를 보관하며 별도 src 복사본이 없습니다. Linux ext4 checkout은 향후 개발 배치 선택이며 이번 정리에서 복제하지 않았습니다.

## 빌드

실행 중인 기존 브리지를 런처에서 정상 중지한 뒤 빌드합니다. 기존 소스 경로로 만든 build cache는 새 경로를 사용하도록 정리하고 재구성해야 합니다.

```bash
REPO="$(git rev-parse --show-toplevel)"  # 저장소 안에서 실행
WS="${ETS2_ROS_WS:-$HOME/ets2-ros}"
source /opt/ros/jazzy/setup.bash  # Zsh에서는 setup.zsh
mkdir -p "$WS" && cd "$WS"
/usr/bin/python3 /usr/bin/colcon build --base-paths "$REPO/ros2/src" --cmake-clean-cache --executor sequential --cmake-args -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/usr/bin/python3 -DPYTHON_EXECUTABLE=/usr/bin/python3
source "$REPO/bridge/ros-env.sh"
```

이후 모든 소비자는 `bridge/ros-env.sh`로 overlay·domain 42·DDS 설정을 함께 불러옵니다.

버전 규칙: `ets2_msgs`의 메시지를 바꾸면 `ets2_msgs/package.xml` 버전과 `bridge/common/wire.hpp`의 `msgs_version`을 함께 올립니다(`tests/wire_test`가 둘을 대조). 토픽 목록·소켓 경로·패킷 meta나 릴레이가 직렬화하는 메시지 형식이 바뀌면 `wire_version`을 올립니다. 릴레이와 ets2_bridge는 서로 다른 `wire_version`이면 양쪽 버전을 보고하고 연결하지 않으므로, Windows 릴레이와 ROS 패키지는 같은 커밋에서 함께 빌드합니다. 실행 중인 버전은 `/ets2/sensors/config`의 `core_version`·`wire_version`·`msgs_version`에서 확인합니다. ROS 전용 설정은 ets2_bridge 패키지에 설치됩니다.

Jazzy는 시스템 Python 3.12를 사용합니다. 이 머신의 기본 PATH에는 사용자 Python 3.11도 있으므로 위 명령은 시스템 interpreter를 명시합니다. WSL의 일반 실행 overlay는 `~/ets2-ros/install`입니다. 저장소의 `ros2/build`, `ros2/install`, `ros2/log`는 Windows `pixi run build`(merge-install)의 Git 제외 산출물입니다([P0 실행서](../docs/22_p0_spike_runbook.md)). 향후 Linux ext4 checkout을 원본으로 선택하면 그 checkout의 `ros2`에서 `src/build/install/log` workspace로 운용할 수 있습니다.

## 실행

일반 사용은 기존 [런처](../apps/launcher/README.md)로 합니다. 수동 ROS 브리지 실행은 다음 명령이며 Windows 릴레이가 별도로 연결되어야 합니다.

```bash
source "$REPO/bridge/ros-env.sh"
ros2 launch ets2_bridge bridge.launch.py config:="$REPO/bridge/config/bridge.local.json"
```

스택이 보는 센서는 `ros2 launch ets2_sensor_degradation sensor_degradation.launch.py`가 만듭니다. IMU `/sensing/imu/imu_raw`, GNSS `/sensing/gnss/nav_sat_fix`, 바퀴 `/sensing/wheels/state`, 라이다 `/sensing/lidar/<L_F|L_PL|L_PR>/points`이며, 값은 [config/degradation.yaml](src/ets2_sensor_degradation/config/degradation.yaml)에서 바꿉니다. stamp는 측정 시각 그대로이고, 지연은 `use_sim_time`의 `/clock` 기준으로 늦게 발행해서 줍니다. 출력에 구독자가 있을 때만 해당 `/ets2` 입력을 구독하므로, 쓰지 않는 라이다의 렌더 수요를 만들지 않습니다. bag 재생에도 같은 노드를 사용합니다(bag의 `/clock`을 그대로 재생하고 `--clock`은 쓰지 않음).

GT baseline은 `ros2 run ets2_autonomy drive_speed`로 실행합니다. 노드명은 `/ets2_drive_speed`입니다. 실제 운전은 [ACC/LCC 사용법](../bridge/README.md#gt-도로-자동-주행)을 따릅니다. 인지·위치 추정·FSM·목적지 경로 계획은 아직 없으며 [다음 모듈 경계](../docs/03_system_design.md#다음-모듈-경계)에 따라 이 패키지부터 확장합니다.
