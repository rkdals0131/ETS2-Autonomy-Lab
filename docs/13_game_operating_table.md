# 진행 상태와 개발 순서

2026-10-09 기준: 독립 센서 생성·ROS 전달·짧은 MCAP 재생을 확보했습니다. 다음 중심 작업은 센서 의미·단위 정리, 위치 추정, 차량 입력과 고속도로 폐루프입니다.

| 단계 | 현재 상태 | 남은 완료 조건 |
| --- | --- | --- |
| M0 실행 기반 | SDK·메타로더·패닉·lease·재로딩·통합 런처 구현 | 30분 주행 안정성, 실제 F11 키 확인 |
| M1 렌더 경로 | pass 명령 구간으로 미러 식별·복사 | 완료 |
| M2 픽셀 확보 | 같은 렌더 프레임의 RGB·깊이 수집 | 완료 |
| M3 정합 | CPU/GPU 투영 대조, 이동 AI 박스·DSV 정합 | 트레일러·주행 장면 확대 |
| M4 자유 배치 | FH5 4뷰, 기본 미러 보존, 1요청 1제출 | 큰 캐빈 운동·고개 조작·트레일러 시야 |
| M5 가시성 | 제외·LOD·도로 visibility 경로 분석, 자차 외판 복구 | 거리·뷰별 실제 누락률 |
| M6 전달·기록 | 카메라 30 Hz·라이다 10 Hz, ROS, Foxglove, MCAP pause 재생 | 10분 주행 기록·30분 운용 |
| M6′ 센서 세트 | 라이다·IMU·바퀴·휠 오도메트리·가상 GNSS | 주행 정합·잡음·RTK·레이더 |
| M7 센서 검증 | 같은 DSV의 GPU/CPU 라이다 대조 | 동적 장면·노이즈·거리별 오차 |
| M8 입력 | 미구현 | 조향·가감속, 0.2초 명령 만료·수동 해제 |
| M9 GT 폐루프 | 미구현 | ACC → LCC → 운전자 지시 차로 변경 |
| M10 인지 폐루프 | 미구현 | 같은 경로에서 GT 대비 성능 비교 |

## 다음 순서

1. 라이다 range의 Foxglove 색상 범위를 주변 장면에 맞춥니다.
2. [IMU·휠·GNSS](17_sensor_expansion.md)를 짧은 주행에서 대조하고 위치 추정을 연결합니다.
3. 같은 주행에 트레일러 가림·고개 회전·객체 누락 관측을 묶습니다.
4. M8 입력 연결 후 GT 차량·차로 정보로 ACC/LCC를 만듭니다.
5. 경량 인지와 레이더를 연결해 GT 기준과 비교합니다.

성능 진단은 [A–G 비용 분해](18_performance.md)에 따릅니다. 센서의 세부 현실성 개선은 폐루프 개발과 함께 진행합니다.

## 구현 위치

- 게임: `ot/native`, 주소·필드: `ot/schema`
- Windows 릴레이: `bridge/windows`
- ROS 수신기·메시지: `bridge/ros2`
- 실행 관리: `bridge/launcher.py`, `bridge/wsl_session.py`
- 현재 리그: `ot/presets/phase1-highway-private.json`
- 라이다 패턴: `ot/presets/phase1-lidar-private.json`

현재 사용법은 [브리지](../bridge/README.md)와 [DLL](../ot/README.md), 중요한 실패와 해결은 [시행착오](history/lessons.md)에 있습니다.
