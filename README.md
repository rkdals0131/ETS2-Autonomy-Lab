# 유로파일럿 — ETS2 자율주행 연구

ETS2의 FH5에 독립 카메라 4개와 깊이 기반 라이다 3개를 배치하고 ROS 2 Jazzy로 실시간 전달합니다. Phase 1 목표는 고속도로 ACC·차로 유지·사각지대 감시입니다. 현재는 센서와 데이터 전달이 구현됐고, 위치 추정·인지·차량 제어가 다음 단계입니다.

## 실행

1. Steam에서 **DirectX11 (64-bit)**로 게임을 실행합니다.
2. [bridge/launch.cmd](bridge/launch.cmd)를 열고 **시작**을 누릅니다.
3. Windows Foxglove에서 런처에 표시된 WebSocket 주소로 연결합니다.

런처가 Windows 릴레이와 WSL Ubuntu의 ROS·Foxglove 서버를 함께 관리합니다. 중복 실행은 기존 창으로 돌아가며, 중지·창 닫기는 해당 실행의 프로세스를 정리합니다. 실행 시간과 센서 설정은 `bridge/config/bridge.local.json`을 공유합니다. 최초 설치·빌드는 [브리지 사용법](bridge/README.md)에 있습니다.

## 현재 상태 — 2026-10-09

| 구성 | 구현 |
| --- | --- |
| 게임 | ETS2 1.61.1.1, DX11, core 0.23.0, 상주 메타로더 |
| 카메라 | C_FN·C_FW 1280×720, C_RL·C_RR 960×544, 기본 30 Hz 요청 |
| 배치 | FH5 4x2 / l2h1 / LHD / mirror_01 / sunshield_01, 캐빈 부착 |
| 출력 | RGB8, 미터 광축 깊이, CameraInfo, JPEG, TF, 차량 GT |
| 라이다 | 전방 1개·좌우 2개, 10 Hz GPU 깊이 샘플링, XYZ·range·결측 상태 |
| 추가 센서 | 이상적 IMU·바퀴 측정·휠 오도메트리·가상 GNSS, ROS 실제 수신 확인 |
| 전달 | 공유 GPU 텍스처·펜스 → Windows C++ → WSL NAT 직접 TCP 2개 → ROS DDS SHM |
| 기록 | ROS MCAP 기록·재생, 일시정지 시계 정합 확인 |
| 복구 | 수집 소유권·lease, F11, 기능 DLL 교체, 연결 단절 복구 |

센서는 슬롯 3·4·6·7의 전용 camera/drawable과 출력을 사용합니다. 현재 FH5의 기본 미러 0·1·2·5는 유지됩니다. 전방 선바이저와 측면 미러 테두리 가림은 실제 모델에 맞춰 장착점을 옮겨 해결했습니다.

![측면 센서 장착 수정 전후](docs/images/side-mirror-rim-fix.png)

같은 정차 장면에서 60초 전경 측정: 리그 끔 **77.69 FPS**, 고해상도 전체 RGB-D·라이다·GT **46.35 FPS**, 고해상도 센서의 JPEG·라이다·GT·TF 구독 **64.12 FPS**. 마지막 수치는 ROS 소비자 기준이며 Foxglove 화면 표시 부하는 별도입니다. [성능 표와 다음 진단](docs/18_performance.md).

남은 작업은 추가 센서의 주행 정합·잡음·RTK 상태, 트레일러·주행 가시성, 입력 연결과 GT 기반 ACC/LCC입니다. [진행 상태](docs/13_game_operating_table.md), [추가 센서](docs/17_sensor_expansion.md).

## 문서

| 목적 | 문서 |
| --- | --- |
| 사용·설치 | [브리지와 런처](bridge/README.md), [DLL 개발 도구](ot/README.md) |
| 범위·진행 | [요구사항](docs/01_requirements_and_decisions.md), [마일스톤](docs/13_game_operating_table.md), [남은 작업](docs/08_open_questions.md) |
| 설계 | [구조](docs/03_system_design.md), [좌표·시각·데이터](docs/04_sensors_and_data.md), [FH5 센서 리그](docs/14_phase1_highway_sensors.md) |
| 센서·ROS | [라이다](docs/15_virtual_sensors.md), [ROS 계약](docs/16_ros2_bridge.md), [센서 확장](docs/17_sensor_expansion.md) |
| 다음 개발 | [성능 진단](docs/18_performance.md), [인지·제어](docs/07_learning_and_control.md) |
| 엔진 참조 | [설치 분석](docs/10_installed_game_static_analysis.md), [SDK·물리 상태](docs/11_idle_memory_and_telemetry.md), [DX11·가시성](docs/12_dx11_mirror_render_path.md) |
| 시행착오 | [해결 과정과 재발 조건](docs/history/lessons.md) |

게임 파일·SDK 원본·추출 자산·대용량 캡처·로컬 설정은 Git에서 제외합니다. 의존 코드와 라이선스는 [THIRD_PARTY](ot/THIRD_PARTY.md), 연구 도구는 [research](research/README.md)에 있습니다. 신규 ROS 대용량 결과는 마운트된 외장 SSD의 `~/Storage/ROS2_Workspace_offload/ETS2-Autonomy-Lab/`에 저장합니다.
