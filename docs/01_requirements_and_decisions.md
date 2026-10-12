# 요구사항과 결정

2026-10-12 기준. 이전 판은 [legacy/01](legacy/01_requirements_and_decisions.md)입니다.

## 목표

ETS2 안에서 트럭이 **게임 ground truth 없이** 센서만으로 고속도로를 스스로 달리면 끝입니다. 실차 적용·제품화는 없습니다. "실차 스택"이라는 표현은 실차에서 쓰는 종류의 코드(인지·위치 추정·계획·제어가 GT를 소비하지 않음)를 뜻하며, 실차 이식성을 뜻하지 않습니다.

Phase 1 범위는 운전자 감독 아래 고속도로 ACC·차로 유지·사각지대 감시입니다. 램프·톨게이트는 운전자가 담당합니다. 운전 중 유일한 추가 화면 요소는 메인 시점의 도로 위에 반투명하게 그리는 계획 궤적입니다.

## 결정

| ID | 항목 | 결정 | 이유 |
| --- | --- | --- | --- |
| G1 | 목표 | 게임 내 자율주행만. 실차·창업 없음 | 사용자 확인 2026-10-12 |
| G2 | GT 사용 | `/ets2/ground_truth/*`, `world→base_link`(GT)는 평가·baseline 전용. 스택 입력은 `/sensing/*`와 `/ets2/camera|lidar|imu|wheels|gnss` | 목표 G1 |
| P1 | 실행 환경 | **Windows 네이티브 ROS 2 Jazzy(RoboStack/pixi)**. WSL은 포팅 P1 완료 시 폐기 | Ubuntu가 필요했던 이유(Autoware 패키지 그대로 의존, 실차 이식)가 G1으로 사라짐. [포팅 계획](20_windows_native_port_plan.md) |
| P2 | ROS 범위 | 메시지 버스·rosbag2·Foxglove·Python 인지와의 경계만. 알고리즘은 rclcpp 비의존 라이브러리 + 얇은 노드 래퍼 | 기록·재생·시각화를 다시 만들지 않기 위해 ROS를 남기고, 그 밖의 결합은 피함 |
| P3 | Autoware | 메시지 패키지는 선택(win-64 바이너리 있음). 알고리즘 패키지 의존 없음. 필요하면 알고리즘 소스를 복사해 라이브러리로 감쌈 | ✔ 알고리즘 패키지(예: autoware_simple_pure_pursuit)는 win-64 빌드가 없음 |
| P4 | 릴레이 | Windows 릴레이가 단일 ROS 노드가 됨. TCP 전송·세션 pairing·wire_version 삭제 | 같은 프로세스에서 CDR을 바로 publish. ✔ 릴레이는 이미 XCDRv1 바이트를 만듦 |
| P5 | 프로세스 경계 | 자율주행 스택은 릴레이와 **별도 프로세스**. 게임 공유메모리 소비자는 릴레이 하나 | 릴레이가 lease·GPU를 소유하므로 알고리즘 크래시와 수집 복구를 분리. ✔ 번들 reader mutex는 단일 소유 |
| P6 | HUD | 스택은 궤적을 `base_link`+센서 stamp로 내고, **릴레이**가 자기 SDK 자세 이력으로 `world`로 바꿔 공유메모리 블록으로 core에 넘긴다. core는 world 폴리라인을 그 프레임의 렌더 카메라로 투영해 톤매핑 결과(`main0/composition_out`)에 블릿 직전 데칼로 그린다. 가림은 외부 pass 깊이 범위 비교로. pipe는 enable/disable만 | 스택은 GT를 모르고(G2), 게임 DLL 변경은 최소화. ✔ 실내는 별도 pass·깊이 [0.9,1.0]이라 범위 비교로 가려짐([20 §0 A4](20_windows_native_port_plan.md#0-2026-10-12-읽기-전용-실측)). ✔ pipe는 단일 인스턴스라 표시 데이터를 안전 명령 채널에 태우지 않음 |
| P7 | 후퇴 경로 | 포팅 P0 스파이크가 실패하면 ROS 없이 공유메모리 + C++ 단일 프로세스 + Foxglove SDK | P0 전에는 아무것도 지우지 않으므로 후퇴 비용 0 |
| S1 | 조향 명령 | 프로필 상대 조향 유지. 바퀴각 명령은 가상 EPS(L2)로 회전 속도 입력에 변환. 절대 조향 비채택 | ✔ `c_relatsteer`는 프로필 전역, 토크 입력·텔레메트리 없음. [legacy/19 D2](legacy/19_real_stack_interface_plan.md) |
| S2 | TF | `world→gt/base_link` 항상, `world→base_link`는 `gt_localization_tf`(기본 true)일 때. 동적 TF는 상태 경로 | 스택 localization이 생기면 false로 전환 |
| S3 | 최우선 | 프로필 파서 보완(W00), 게임 업데이트 대비(W01), armed 안전 시험(W02) | ✔ 현재 프로필(`no_cstm_mod`, `joy.b5`)을 파서가 못 읽어 `profile_supported=false`, arm 거절. ✔ Steam은 `AutoUpdateBehavior 0`이며 exe SHA가 바뀌면 core가 SDK 전용으로 닫히므로 업데이트는 안전 문제가 아니라 **기능 상실** 문제. 대비는 설치본 전체 복사 + SHA 게이트 + (△) beta 브랜치 고정 |
| L1 | 라이선스 | 루트 Apache-2.0, `map_lane_provider`는 GPL-2.0 별도 프로세스 | [legacy/19 D5·D7](legacy/19_real_stack_interface_plan.md) |

## 유지하는 전제

| 항목 | 결정 |
| --- | --- |
| 게임·렌더러 | ETS2 1.61.1.1, DX11 64-bit |
| 사용자 화면 | FHD 운전석 POV와 기본 미러 유지 |
| Phase 1 영상 | 전방 협각·광각, 좌우 포드 측후방 4뷰 |
| 시간 | 정상 게임 속도, 같은 렌더 상태의 센서 묶음 |
| 인지 | 카메라·라이다, 뷰별 경량 모델과 객체 수준 융합. 추론은 게임과 같은 GPU |
| 원본 RGB-D | 데이터셋·검증 때만 구독, 평상시 축소 인지 영상·JPEG |
| 차량 | FH4 4x2 기본, 다른 차량은 프리셋 보정 |
| 입력 API | 단일 소유권·200 ms 만료·수동 개입·패닉 해제 |
| 상태 센서 | 영상 수집·연결과 독립적으로 측정 이력 유지. 가상 레이더 비범위 |
| 설정·기록 | `bridge/config/bridge.local.json`, 차량 상태 기록 `bridge/recordings`, 영상·라이다는 지정 폴더. 기록은 Git 제외 |
| 후속 범위 | 시내·주차, BEV/E2E, ATS 이식 |

장비는 Ryzen 7 3700X, RTX 3060 Ti 8 GiB, 메모리 약 23.95 GiB입니다. [성능 실측](18_performance.md)으로 영상 채널과 모델 예산을 정합니다.
