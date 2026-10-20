# 레퍼런스 트럭 식별 주행

[시험장 맵](../../bridge/testground_map/README.md) 위에서 조향과 페달을 함께 잡고 정해진 시험 단계를 차례로 실행합니다. 모든 SDK 프레임은 `bridge/recordings/sysid-<시각>/frames.jsonl`에 기록하고, 진행 상태는 같은 폴더의 `status.json`에 씁니다.

```powershell
py -3 apps/sysid/sysid_drive.py --view               # 20,40,60,80 km/h 전체
py -3 apps/sysid/sysid_drive.py --speeds 60 --view   # 일부 속도만
py -3 apps/sysid/sysid_drive.py --tag down --view    # 리프트 액슬을 내리고(정지 상태) 전체
py -3 apps/sysid/analyze_sysid.py bridge/recordings/sysid-<시각>
```

`--view`를 주면 새 PowerShell 창에서 `sysid_view.py`(rich 사용)가 단계 목록, 현재 단계와 전체 진행률, 속도·조향·페달·패드 가장자리까지 거리를 보여 줍니다. 끝나면 5초 뒤(중단이면 30초 뒤, 기록이 20초 멈추면 그때) 창이 저절로 닫힙니다. `py -3 apps/sysid/sysid_view.py <run>/status.json`으로 따로 열 수도 있습니다.

## 단계

- 속도별(기본 20/40/60/80 km/h): 가속·정속 → 조향 속도 펄스 ±0.3(약 2 m/s² 횡가속 크기로 시간 조절) 후 명령 0 유지 2.5 s → 조향 위치 사인 0.2/0.5/1.0 Hz 3주기(약 1.5 m/s²) → 정상 선회 8 s(약 2 m/s²).
- 종방향: 타력 15 s, 60 km/h에서 브레이크 0.2/0.5/1.0 계단, 정지에서 스로틀 0.3/0.6/1.0 계단.
- 직진은 출발 방향의 heading 유지입니다. 앞쪽 여유가 부족하면 30 km/h, 반경 60 m로 유턴합니다.

`analyze_sysid.py`는 속도대별 조향 플랜트(a, c, 지연), 사인·정상 선회의 요레이트 이득과 유효 축간거리, 종방향 계단을 출력하고 `summary.json`을 씁니다. 축간거리는 실행 폴더의 `truck_config.json`(실행 시작 때 기록)과 리프트 액슬 접지 채널로 정합니다.

## 브레이크 분해

```powershell
py -3 apps/sysid/brake_test.py --view                 # A(프로필 설정) + B(자동 엔진브레이크·리타더 끔) + R(리타더 단별)
py -3 apps/sysid/brake_test.py --phases B --view      # B만
py -3 apps/sysid/analyze_brake.py bridge/recordings/brake-<시각>
```

80 km/h에서 10 km/h까지 타력, 서비스 브레이크 계단, 엔진브레이크(B 키 유지), 둘의 조합을 잽니다. R 단계는 리타더 1–N단(`;`/`'` 키로 단 조정, `truck.brake.retarder`로 확인)과 최고 단 + 엔진브레이크, 최고 단 + 서비스 브레이크입니다. 리타더가 없는 트럭(`retarder.steps 0`)이면 R을 거부합니다. B·R 단계 전에 트럭을 세우고 제어를 푼 뒤 게임 콘솔(`` ` ``)에 `g_motor_brake_auto 0`, `g_retarder_auto 0`을 입력하고, 끝나거나 중단되면 1로 되돌립니다. W/A/S/D가 운전 키라 콘솔 입력은 정지·해제 상태에서만 합니다. 브레이크·엔진브레이크·공기압·브레이크 온도·변속 채널은 프레임의 `x`에 기록됩니다. 결과는 [docs/29](../../docs/29_reference_truck_dynamics.md#브레이크엔진브레이크리타더-brake-20261020-105939-태그-올림-brake-20261020-120355-태그-내림)(6x2 리타더 트럭)와 [4x2 당시 분해](../../docs/29_reference_truck_dynamics.md#브레이크-분해-2026-10-20-core-0380-채널)에 있습니다.

## 리프트 액슬

```powershell
py -3 apps/sysid/lift_test.py --view
```

정지 상태와 30/60/85 km/h 직진에서 U 키로 리프트 액슬을 내리고 올리며, 걸린 시간과 접지 시점을 기록합니다. 키가 먹지 않으면 기록하고 계속합니다. 끝나면 올린 상태로 둡니다. `sysid_drive.py`·`brake_test.py`의 `--tag up|down`은 시작 전에 정지 상태로 맞춥니다. 게임 키(엔진브레이크 B, 리타더 `;`/`'`, 리프트 U, 콘솔)는 [game_keys.py](game_keys.py)에 있습니다(ETS_SysID 프로필 `controls.sii` 기준).

## 실행 조건과 해제

- `ot_config.json`의 drive 권한과 `drive_controls_path`가 현재 프로필을 가리켜야 arm됩니다. 시동이 켜져 있고 주차 브레이크가 풀려 있어야 하며 자동 변속을 가정합니다.
- 실행 동안 Tier 1을 켜서(허용될 때) 프레임마다 `dt`에 실제 엔진 토크·변속기 출력 토크·축 회전·바퀴별 브레이크 토크를 기록하고, 끝나면 Tier 0으로 돌립니다([docs/30](../../docs/30_can_like_signals.md)).
- 게임 창이 백그라운드로 가면 게임이 우리 조향·페달을 무시합니다. 하네스는 멈추지 않고 이유를 기록한 채(상태 메시지, 프레임의 `focus`, 끝나면 `focus_losses.json`) 리스와 명령 창을 유지하며 최대 10분 기다립니다. 창이 돌아오면 그때 속도로 다시 맞춘 뒤 진행 중이던 단계를 처음부터 다시 돌립니다. 다시 돈 단계의 앞 시도는 프레임의 `attempt`로 구분되고, 분석 스크립트는 마지막 시도와 전경 프레임만 씁니다. 콘솔 입력도 키마다 게임 창이 앞에 있을 때까지 기다리므로 다른 창에 글자가 들어가지 않습니다.
- 게임 창이 2 s 이상 전경일 때 arm합니다. 수동 입력, 일시정지, F11, 패드 가장자리 250 m 여백 안에서 바깥쪽으로 움직임, 가장자리 100 m 이내 진입 중 하나라도 생기면 멈추고, 아직 제어 중이면 브레이크로 세운 뒤 해제합니다. 여백 안에서 안쪽으로 돌아오는 것은 허용하고, 출발할 때 앞쪽 직선 여유가 300 m 미만이면 10 km/h로 패드 중심 쪽으로 먼저 돕니다.
- 패드 좌표(`PAD`)와 `ANGLE_PER_UNIT`은 스크립트 머리에 있습니다. 축간거리(`WHEELBASE`, 앞축에서 뒤 기준점까지)와 뒤 기준점의 z(`REAR_Z`)는 SDK 트럭 구성에서 읽고, 리프트 액슬이 오르내리면 프레임마다 다시 정합니다. 뒤 기준점은 접지한 뒤축들의 가중 평균이며, 내린 리프트 액슬은 측정값 `LIFT_AXLE_WEIGHT` 0.675로 셉니다([docs/29](../../docs/29_reference_truck_dynamics.md)). `analyze_sysid.py`의 L도 이 기준이라, 태그 내림에서는 기하값 3.805 대신 3.68 m를 출력합니다.
