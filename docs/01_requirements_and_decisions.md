# 요구사항과 결정

Phase 1은 운전자 감독 아래 고속도로 ACC·차로 유지·사각지대 감시를 제공합니다. 램프·톨게이트는 운전자가 담당하고, 차로 변경은 운전자 지시 방식으로 확장합니다.

| 항목 | 결정 |
| --- | --- |
| 게임·렌더러 | ETS2 1.61.1.1, DX11 64-bit |
| 실행 | Windows 게임·DLL·C++ 릴레이, WSL2 Ubuntu ROS 2 Jazzy |
| 사용자 화면 | FHD 운전석 POV와 기본 미러 유지 |
| Phase 1 영상 | 전방 협각·광각, 좌우 포드 측후방 4뷰 |
| 시간 | 정상 게임 속도, 같은 렌더 상태의 센서 묶음 |
| 인지 | 뷰별 경량 모델과 객체 수준 융합 |
| 제어 순서 | 입력 연결 → GT ACC/LCC → 센서 인지 폐루프 |
| 후속 범위 | 시내·주차, BEV/E2E, ATS 이식 |
| 실행 관리 | Windows 런처 한 곳에서 실제 상태 확인·시작·중지 |
| 설정 | bridge/config/bridge.local.json과 연결된 센서 프리셋 |
| 저장 | 외장 SSD의 ~/Storage/ROS2_Workspace_offload/ETS2-Autonomy-Lab/ |

게임 감속과 프레임별 카메라 순차 회전은 사용하지 않습니다. 센서는 캐빈에 부착하고 운전자 고개 움직임과 독립적으로 유지합니다. 입력 소유자는 하나이며 명령 만료·수동 개입·패닉에서 해제합니다.

현재 장비는 Ryzen 7 3700X, RTX 3060 Ti 8 GiB, 메모리 약 23.95 GiB입니다. [성능 실측](18_performance.md)을 기준으로 영상 채널과 모델 예산을 정합니다. 게임과 모델 추론은 GPU를 공유합니다.

[현재 진행](13_game_operating_table.md), [센서 리그](14_phase1_highway_sensors.md), [인지·제어](07_learning_and_control.md).
