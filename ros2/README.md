# ROS 소스

| 패키지 | 현재 책임 |
| --- | --- |
| src/ets2_msgs | 센서·차량 상태·운전 명령과 서비스 계약 |
| src/ets2_relay | Windows 릴레이 노드: 게임 공유메모리·GPU 출력을 읽어 직접 발행, 구독→수집 요구, `/ets2/capture`, 운전 명령 전달. 소스는 `bridge/windows` |
| src/ets2_autonomy | GT baseline: 차로 추종(pure pursuit 선형화 + 곡률 feedforward)·가상 EPS·속도 PI·앞차 속도 제한과 안전 시험용 `drive_hold`. 횡제어 오프라인 폐루프 시험 `lateral_sim_test` |
| src/ets2_sensor_degradation | `/ets2/*` 이상 센서에 seed 고정 잡음·지연·드롭을 적용해 `/sensing/*`로 재발행 |

ROS 2 Jazzy는 Windows 네이티브 RoboStack 환경(저장소 루트 `pixi.toml`)에서 빌드·실행합니다. pixi activation(`ros2/activate.bat`)이 overlay, domain 42, `rmw_fastrtps_cpp`, localhost 탐색, 공유 DDS 프로필(`bridge/config/fastdds.xml`)을 모든 프로세스에 설정합니다.

## 빌드와 테스트

실행 중인 브리지를 런처에서 정상 중지한 뒤 저장소 루트에서 빌드합니다. Visual Studio 2022 Build Tools(C++)가 필요합니다.

```powershell
pixi run build    # colcon --merge-install, Ninja, Release → ros2/build·install·log (Git 제외)
pixi run test     # ets2_relay·ets2_sensor_degradation gtest + tests/ (MSVC)
```

`PATH`의 다른 Python이 섞이지 않도록 ROS 명령은 항상 `pixi run` 또는 `pixi shell` 안에서 실행합니다.

버전 규칙: `ets2_msgs`의 메시지를 바꾸면 `ets2_msgs/package.xml` 버전과 `bridge/common/topics.hpp`의 `msgs_version`을 함께 올립니다(`tests/topics_test`가 둘을 대조). 릴레이는 이 레이아웃으로 CDR을 직접 만들므로 `pixi run build`로 릴레이와 메시지 패키지를 함께 빌드합니다. 실행 중인 버전은 `/ets2/sensors/config`의 `core_version`·`msgs_version`에서 확인합니다.

## 실행

일반 사용은 [런처](../apps/README.md)로 합니다. 수동 실행은 [브리지 문서의 수동 실행](../bridge/README.md#수동-실행)을 따릅니다.

스택이 보는 센서는 `ros2 launch ets2_sensor_degradation sensor_degradation.launch.py`가 만듭니다. IMU `/sensing/imu/imu_raw`, GNSS `/sensing/gnss/nav_sat_fix`, 바퀴 `/sensing/wheels/state`, 라이다 `/sensing/lidar/<L_F|L_PL|L_PR>/points`이며, 값은 [config/degradation.yaml](src/ets2_sensor_degradation/config/degradation.yaml)에서 바꿉니다. stamp는 측정 시각 그대로이고, 지연은 `use_sim_time`의 `/clock` 기준으로 늦게 발행해서 줍니다. 출력에 구독자가 있을 때만 해당 `/ets2` 입력을 구독하므로, 쓰지 않는 라이다의 렌더 수요를 만들지 않습니다. bag 재생에도 같은 노드를 사용합니다(bag의 `/clock`을 그대로 재생하고 `--clock`은 쓰지 않음).

GT baseline은 `ros2 run ets2_autonomy drive_speed`로 실행합니다. 노드명은 `/ets2_drive_speed`이고, 런처는 `--stop-event`로 정상 disarm시킵니다. 실제 운전은 [ACC/LCC 사용법](../bridge/README.md#gt-도로-자동-주행)을 따릅니다. 인지·위치 추정·FSM·목적지 경로 계획은 아직 없으며 [센서 스택 설계](../docs/25_sensor_stack_design.md)에 따라 확장합니다.
