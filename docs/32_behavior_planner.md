# 32. 판단 계층(행동 트리 + 상태기계)

제어(ACC·LCC) 위에 올리는 판단 계층의 구조와 지금 상태입니다. 2026-10-22에 정한 계획의 2단계이고, 그날 만든 첫 뼈대를 설명합니다.

## 계층

```
Mission (전역 경로)        게임 GPS 경로(map_lane_provider --game-route) 또는 SD 지도 경로(--plan-route)
  ↓  <out>.route.json, walker의 route_turn / route_lane_mismatch event
Behavior: 행동 트리 (10 Hz) 무엇을 할지: 긴급, 대기, 운전자 의도, 경로 출구, 추월, 차로 유지
  ↓
Maneuver: 상태기계          어떻게 할지: 차선 변경 Idle → Signal → WaitGap → Execute → Done / Abort
  ↓
Control: drive_speed (50 Hz) ACC·LCC MPC
```

- **세계 모델(`planning/world.hpp`):** 트리와 상태기계가 보는 세계입니다.
  - 내용은 ego, 어시스트 상태, 운전자 깜빡이, 객체, 따라가는 차로와 옆 차로, 경로 event입니다.
  - 지금은 planner의 GT 어댑터가 채웁니다(`drive_speed`의 `AssistState`, `PowertrainState`, GT ego·교통, provider 차로 파일).
  - 인지로 바꿀 때도 이 구조체를 채우는 쪽만 바꾸면 됩니다. 트리와 상태기계는 어디서 온 값인지 모릅니다.
- **행동 트리:** BehaviorTree.CPP 4.10(robostack-jazzy `ros-jazzy-behaviortree-cpp`)을 씁니다.
  - 트리 모양은 `ros2/src/ets2_autonomy/config/behavior.xml`에 있고, 다시 빌드하지 않고 바꿀 수 있습니다(planner만 다시 시작).
  - node 구현은 `planning/behavior.hpp`에 있습니다.
- **상태기계(`planning/lane_change_fsm.hpp`):** tick마다 한 번, 트리가 이번 tick에 원한 쪽과 이유를 받아 한 걸음 갑니다.
  - 트리의 `ChangeLane` 행동은 이번 tick에 바꾸고 싶은 쪽과 이유만 남깁니다.
  - 상태기계가 진행 중이면 실행 중, 끝나면 성공, 중단되면 실패를 돌려줍니다.

## 트리 (`behavior.xml`)

`ReactiveFallback`이 tick마다 위에서부터 다시 봅니다. 처음 성립하는 가지가 이기고, 뒤에서 실행 중이던 가지는 멈춥니다. 가지는 `ReactiveSequence`라 조건을 매 tick 다시 봅니다. 그냥 `Sequence`는 실행 중인 자식부터 이어서 tick하기 때문에, 깜빡이를 꺼도 차선 변경이 계속되는 것을 단위 테스트에서 확인했습니다.

| 가지 | 조건 | 행동 |
|---|---|---|
| Emergency | 앞차 TTC < 2 s | EmergencyBrake (지금은 표시만) |
| Assistance off | ACC·LCC 둘 다 꺼짐 | Standby |
| Driver intent | 운전자 깜빡이 | ChangeLane(그쪽, driver) |
| Route exit | 1.5 km 안의 `route_lane_mismatch` | ChangeLane(출구로 가는 쪽, route) |
| Overtake | 설정 속도보다 10 km/h 넘게 느린 앞차(100 m 안)가 8 s 이어짐, 왼쪽 차로 간격 비었음 | ChangeLane(왼쪽, overtake) |
| (나머지) | — | KeepLane |

## 차선 변경 상태기계

| 상태 | 들어가는 조건 | 나가는 조건 |
|---|---|---|
| Idle | 시작, 또는 Done·Abort 뒤 이유가 사라짐 | 트리가 차선 변경을 원함 → Signal |
| Signal | 이유가 생김 | 깜빡이가 3 s 켜져 있었음 → WaitGap(운전자 깜빡이는 켠 시각부터 셈) |
| WaitGap | 깜빡이 시간이 참 | 실행기가 그쪽으로 옮기기 시작 → Execute |
| Execute | 실행기의 차선 변경 진행 중 | 진행이 끝남 → Done |
| Done / Abort | 끝남 / 이유가 사라졌거나, 그쪽 차로가 없거나, 다른 쪽을 원함 | 이유가 사라질 때까지 유지 → Idle |

- Done과 Abort는 이유가 남아 있는 동안 유지합니다. 차선 변경 뒤 운전자 깜빡이가 켜진 채로 남아 있어도 같은 깜빡이로 두 번 바꾸지 않습니다.
- 실행기(`drive_speed`)가 차선을 옮기기 시작하면, 계획이 무엇이었든 Execute로 따라갑니다.
- 간격 판정(`gap_problem`)은 `drive_speed`와 같은 규칙입니다(앞: 5 m + 0.5 s + 3 s 접근 속도, 뒤: 5 m + 3 s 접근 속도). 다른 층의 차(입체교차)는 뺍니다.

## 그림자 모드

지금 planner는 **보여 주기만** 합니다. 실제 차선 변경은 여전히 `drive_speed`의 깜빡이 차선 변경이 판단하고 실행합니다.
- 운전자 깜빡이 가지는 그 실행을 그대로 따라 보여 줍니다.
- 경로·추월 가지는 간격이 비면 `gap free, would start`로 멈춰 있습니다.
- 깜빡이 출력 채널(계획 3단계)이 생기면 planner가 깜빡이를 켜고 `drive_speed`에 차선 변경을 지시하는 쪽으로 넘어갑니다.
- 그다음 `drive_speed`의 깜빡이 판단은 지웁니다.

## 상황판

- 메시지는 `ets2_msgs/BehaviorStatus`이고 `/ets2/planning/behavior`로 10 Hz입니다.
  - 트리 node(이름, 종류, 부모)를 매번 통째로 보냅니다(지금 19개).
  - 이번 tick에 각 node가 돌려준 값, 상태기계 상태 목록, 활성 상태, 이유, 판단 문장, `shadow`가 들어 있습니다.
- control node는 끝난 자식을 tick이 끝나기 전에 idle로 되돌립니다. 그래서 planner는 상태 변화 콜백(`StatusChangeLogger`)으로 tick 중의 값을 잡습니다.
- Foxglove 패널은 [apps/foxglove/behavior-board](../apps/foxglove/behavior-board/README.md)입니다. 활성 경로를 녹색으로 그립니다.
- 런처가 주행 보조와 함께 planner를 띄웁니다(로그 `planner.log`, 판단이 바뀔 때마다 한 줄).

## 검증

- ✔ `planning_test` 5개(2026-10-22):
  - 상태기계: 운전자 깜빡이 → 실행 → 완료 → 유지 → Idle, 중단 세 경우, 실행기 따라가기
  - 간격, 입체교차, 경로 쪽, TTC 계산
  - 실제 `behavior.xml`을 손으로 만든 세계로 tick: 대기, 차로 유지, 운전자 차선 변경 전 과정, 긴급, 8 s 뒤 추월 제안
- ✔ planner를 실행하니 10 Hz로 발행했고(1.1 s에 12개), 어시스트가 꺼진 상태의 트리 값이 맞았습니다(Assistance off 가지 성공, Emergency 조건 실패, 뒤 가지 tick 안 됨).
- △ 게임 주행 중의 판단과 Foxglove 화면은 아직 보지 않았습니다.

## 깜빡이 출력과 지휘 모드 (3단계, 2026-10-22)

- **깜빡이 출력:** core·loader 0.48.0의 레버 누르기 위에 `drive_speed`의 `control/turn_signal.hpp`를 얹었습니다([브리지 문서](../bridge/README.md#운전-명령-api)). planner가 원하는 상태를 `/ets2/planning/turn_signal`로 내면 `drive_speed`가 레버를 눌러 맞춥니다.
- **누가 켰는지:** `AssistState.turn_signal_by`(1 운전자, 2 어시스트)와 `turn_signal_cancelled`(운전자가 어시스트 깜빡이를 끔)로 구분합니다.
  - planner의 "운전자 의도" 가지는 어시스트가 켠 깜빡이에서는 성립하지 않습니다.
  - 운전자가 어시스트 깜빡이를 끄면 경로·추월 차선 변경은 중단합니다.
- **지휘 모드(`command:=true`, 기본 꺼짐):** planner가 자기 차선 변경(경로·추월)의 깜빡이를 Signal부터 Execute까지 켜 두고, 그 밖에는 끄라고 냅니다. 운전자 깜빡이는 `drive_speed`가 건드리지 않으므로 끄라는 값을 늘 보내도 됩니다.
  - 어시스트 깜빡이는 운전자 깜빡이와 같은 `drive_speed`의 차선 변경을 시작하되, 3 s 켜 둔 뒤에야 옮깁니다.
  - 그래서 별도의 실행 경로 없이 planner가 차선 변경을 지휘합니다.
- ✔ 단위 테스트를 모두 통과했습니다.
  - `ot_drive_test`: 소유자·epoch, 100 ms 누르기와 간격, 대기열, 해제
  - 릴레이 `ros_endpoint_test`: 누적값이 한 번씩만 넘어감
  - `planning_test`: 누르기, 운전자 깜빡이 보호, 취소, 비상등, 지휘 모드 깜빡이
- △ 게임 시험 전입니다. 새 loader로 게임을 재시작해야 합니다. 런처는 아직 그림자 모드로 띄웁니다.

## 다음

1. **게임에서 깜빡이 채널 확인:** loader 0.48.0 설치와 게임 재시작이 필요합니다. 이어서 지휘 모드로 경로 출구 차선 변경과 추월을 시험합니다.
2. **추월 정책:** `auto`, 그리고 `confirm`(어시스트가 제안하면 운전자가 같은 쪽 깜빡이로 승인) 두 가지입니다.
3. **WorldModel 메시지:** 세계 모델을 토픽으로 내서 인지 노드가 채울 수 있게 합니다.
4. **AEB와 최소위험 정지:** 라이다와 GT를 합친 OGM으로 경로 위 장애물을 봅니다(5단계).
