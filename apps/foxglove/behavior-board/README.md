# Behavior board (Foxglove 패널)

planner(`ros2/src/ets2_autonomy/src/planner_node.cpp`)의 상황판입니다. `/ets2/planning/behavior`(`ets2_msgs/BehaviorStatus`, 10 Hz)를 받아 다음을 보여 줍니다.

- **맨 위 줄:** 지금의 판단 문장입니다. `shadow`는 planner가 보여 주기만 하고 운전은 `drive_speed`가 한다는 표시입니다.
- **트리:** 행동 트리를 깊이 우선으로 펼친 것입니다.
  - 노드마다 이번 tick에 돌려준 값을 기호로 붙입니다: `●` 실행 중, `✓` 성공, `–` 실패, `·` tick 안 됨.
  - 실행 중이거나 성공한 노드는 녹색입니다. 이것이 지금 고른 경로입니다.
  - 실패한 조건은 회색이라, 왜 앞 가지가 아니었는지 보입니다.
  - 판단이 `emergency`이면 녹색 대신 빨강입니다.
- **아래 줄:** 차선 변경 상태기계의 상태들이고, 활성 상태만 녹색입니다. 그 아래에 이유(`waiting: vehicle behind` 등)를 적습니다.
- 1초 넘게 메시지가 없으면 전체가 흐려지고 `no data`가 붙습니다.

bag을 재생해도 같은 토픽이라 판단이 바뀌는 과정을 다시 볼 수 있습니다.

## 빌드와 설치

```powershell
cd apps/foxglove/behavior-board
npm install          # 처음 한 번
npm run local-install  # 빌드 후 %USERPROFILE%\.foxglove-studio\extensions 에 설치
```

Foxglove 데스크톱을 다시 띄우고 패널 추가에서 **Behavior board**를 고릅니다. `npm run package`는 다른 PC에 끌어다 놓을 `.foxe`를 만듭니다. `npx eslint src`로 lint합니다.

✔ 2026-10-22: 빌드와 lint를 통과했고, 로컬 설치까지 했습니다. △ Foxglove 화면에서 그려지는 모습은 아직 확인하지 않았습니다.
