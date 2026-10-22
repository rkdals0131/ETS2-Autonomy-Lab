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
| Emergency | 앞차 TTC < 2 s, 또는 GT로 설명되지 않는 라이다 장애물이 멈출 거리(1 s + 4 m/s² 제동 + 8 m) 안 | EmergencyBrake (지금은 표시만) |
| Assistance off | ACC·LCC 둘 다 꺼짐 | Standby |
| Driver intent | 운전자 깜빡이 | ChangeLane(그쪽, driver) |
| Route exit | 1.5 km 안의 `route_lane_mismatch` | ChangeLane(출구로 가는 쪽, route) |
| Overtake | 설정 속도보다 10 km/h 넘게 느린 앞차(100 m 안)가 8 s 이어짐, 왼쪽 차로 간격 비었음 | 정책 `auto`: ChangeLane(왼쪽, overtake). `confirm`(기본): ProposeOvertake("signal left to confirm"), 운전자가 왼쪽 깜빡이를 켜면 Driver intent 가지가 받음 |
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

## 라이다 점유와 GT 합치기 (5단계 시작, 2026-10-22)

`occupancy` 노드(`perception/occupancy.hpp`, 10 Hz)가 라이다 세 대의 전체 점군을 tf2로 world에 옮겨 GT 교통과 합칩니다. 변환은 GT ego, base_link→cabin(동적), cabin→라이다(정적)를 잇습니다.

- **지면:** 2 m 칸마다 가장 낮은 반사점을 잡고, 주변 ±1칸(±2 m)의 최솟값을 그 칸의 지면으로 씁니다. 평면을 가정하지 않으므로 경사·고갯마루도 지면입니다. 점에서 창 끝까지 최대 4 m라 10% 경사에서 0.4 m입니다.
- **점 분류:** 지면 위 0.5~4.2 m는 장애물 점입니다. 트럭 높이보다 위인 다리·표지판은 뺍니다. 0.3 m 안은 관측된 노면입니다. 내 트럭과 트레일러 위 점은 뺍니다.
- **GT와 합치기:** GT 차량 상자(+0.6 m) 안의 장애물 점은 그 차가 설명합니다. 설명되지 않는 점이 GT가 모르는 장애물입니다(사고, 랜덤 인카운터, 떨어진 물체).
- **출력:**
  - `/ets2/perception/occupancy`: OccupancyGrid. world 기준이고 트럭 방향으로 뒤 20 m·앞 100 m·좌우 20 m, 0.5 m 칸입니다. 값은 −1 모름, 0 관측된 노면, 60 설명된 장애물, 100 설명 안 된 장애물입니다.
  - `/ets2/perception/obstacles`: Foxglove 점과 막힌 곳 표시입니다.
  - `/ets2/perception/blocked_m`: 따라가는 차로 위 첫 막힌 곳까지 거리입니다. 경로 중심 ±1.6 m 안에 설명 안 된 점 3개가 2 m 안에 몰려야 막힌 것으로 칩니다. 막힌 곳이 없으면 inf, 라이다가 없으면 NaN입니다.
- **planner:** Emergency 가지의 `ObstacleAhead`가 이 거리를 멈출 거리와 비교합니다(지금은 표시만).
- 런처가 띄웁니다. 라이다 체크가 꺼져 있으면 점군이 렌더되지 않아 NaN만 냅니다.
- ✔ `occupancy_test` 4개(합성 반사점): 차로 위 블록 40 m에서 막힘, GT 차량이 설명, ±8%·10% 경사는 지면(10% 경사 위 블록은 찾음), 트레일러·가드레일·흩어진 점 몇 개는 막힘 아님. 노드는 라이다 없이 NaN을 10 Hz로 냈습니다.
- 시각 맞추기: 2026-10-22 실측(80 km/h 앞차 추종)에서 교통 GT 시각 − 점군 시각은 중앙값 133 ms·p95 517 ms였고, 앞차 뒷면 점이 GT 상자 뒤끝보다 2–3 m 뒤에 찍혀 설명 안 된 장애물이 되었습니다. planner는 그림자 모드에서 20초 넘게 "앞 24–33 m 장애물" 긴급을 냈습니다. 그래서 모두 점군 시각(세 라이다 시각의 평균)에 맞춥니다: GT 차량은 속도로 그 시각까지 되돌리고 라이다 시각 차이만큼 길게 잡으며(`perception::moved`), 트럭과 트레일러 상자는 2 s ego 자세 기록에서 그 시각으로 보간합니다.
  - ✔ 같은 주행을 기존·수정 노드가 동시에 받아 100 s 비교(33–41 km/h 앞차 추종): 기존은 막힘 5번(앞차 4 m 뒤 22–24 m, 55–58 m), 수정은 0번이고 앞차 주변 설명 안 된 점 0개. `occupancy_test` 5개(시각 차이 테스트 추가) 통과.
- △ 확인할 것: 게임 라이다의 지면 반사, 터널·다리 아래, 차로 밖 물체, 트레일러가 꺾일 때 자기 점 제외. 실제 막힌 차로(사고, 랜덤 인카운터)는 아직 보지 않았습니다.

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
2. **추월 정책:** planner 파라미터 `overtake_policy`(`confirm` 기본, `auto`)로 넣었습니다(✔ 단위 테스트). 게임에서는 지휘 모드와 함께 시험합니다.
3. **WorldModel 메시지:** 세계 모델을 토픽으로 내서 인지 노드가 채울 수 있게 합니다.
4. **AEB와 최소위험 정지:** 라이다와 GT를 합친 OGM으로 경로 위 장애물을 봅니다(5단계).
