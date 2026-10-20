# 30. 실차 CAN에 해당하는 신호

2026-10-20, 게임 1.61.1.9, core 0.39.0, Scania S 6x2 태그리프트(DC16 520, G33CM-R, [docs/29](29_reference_truck_dynamics.md)). 실제 트럭 CAN(J1939)이 내보내는 엔진·변속·제동 신호를 게임에서 어디서 얻는지 정리합니다. 세 출처가 있습니다.

1. SDK 채널(core 0.37.0부터, Tier 0): 회전수·기어·페달·온도·압력·연료·리타더 단·엔진브레이크 플래그 등.
2. 차량 물리 메모리(core 0.39.0, Tier 1): SDK에 없는 실제 엔진 토크, 변속기 출력 토크·회전, 바퀴별 브레이크 토크.
3. def 파일(정적 사양, EC1에 해당): 토크 곡선, 기어비, 리타더 단별 구성, 축하중, 타이어, 전역 물리 상수. [research/truck_spec.py](../research/truck_spec.py)가 세이브의 현재 트럭 부품으로 골라 줍니다.

ROS에는 `/ets2/vehicle/powertrain`(ets2_msgs/PowertrainState, 0.3.0)으로 1과 2를 함께 냅니다. 2는 릴레이가 Tier 1일 때만 채워지고(`drivetrain_available`), 없으면 NaN입니다. sysid 하네스는 실행 동안 Tier 1을 켜고 프레임마다 `dt`에 2를 기록합니다.

## 신호 대응

| J1939 | 신호 | 출처 | 단위·비고 |
|---|---|---|---|
| EEC1 | 엔진 회전수 | SDK `truck.engine.rpm`, Tier 1 `engine_speed_radps` | 같은 값(r 1.000) |
| EEC1 | 실제 엔진 토크 | Tier 1 `engine_torque_nm` | Nm. 음수는 마찰(약 −270 Nm)·엔진브레이크(약 −3,000 Nm) |
| EEC1/EEC2 | 운전자 요구 토크·가속 페달 | SDK `truck.input.throttle`·`truck.effective.throttle` × def 토크 곡선 | 요구 토크는 계산값 |
| ETC1 | 변속기 출력축 회전 | Tier 1 `output_shaft_speed_radps` | 구동 바퀴 각속도 × 종감속비 2.59와 일치(r 1.000) |
| ETC1 | 변속기 출력 토크 | Tier 1 `output_torque_nm` | 리타더 포함. 아래 물리 검증 |
| ETC2 | 현재·선택 기어, 기어비 | SDK `truck.engine.gear`·`truck.displayed.gear`, Tier 1 `gear_ratio` | 기어비는 def와 정확히 일치, 중립 0 |
| ETC1 | 클러치 | SDK `truck.effective.clutch`, Tier 1 `clutch` | 같은 값 |
| ERC1(엔진) | 엔진브레이크 작동·단 | SDK `truck.brake.motor`, Tier 1 `engine_brake` | 0/1 |
| ERC1(드라이브라인) | 리타더 단·비율 | SDK `truck.brake.retarder`, Tier 1 `retarder_level` | 단 / 0–1(def `auxiliary_brakes`) |
| ERC1 | 리타더 실제 토크 | `output_torque_nm − engine_torque_nm × gear_ratio × 0.905` | 계산값(아래) |
| EBC1 | 브레이크 페달·적용 | SDK `truck.input.brake`·`truck.effective.brake` | |
| EBC2/EBC5 | 바퀴별 브레이크 토크 | Tier 1 `wheel_brake_torque_nm[]` | Nm, SDK 바퀴 순서. 들린 태그 바퀴에도 값이 있으므로 접지 플래그와 같이 씀 |
| EBC2 | 바퀴 속도 | SDK `truck.wheel.angular_velocity[i]` | rad/s |
| CCVS | 차속·크루즈·주차 브레이크 | SDK `truck.speed`·`truck.cruise_control`·`truck.brake.parking` | |
| AIR1 | 공기압 | SDK `truck.brake.air.pressure` | psi |
| ET1 | 냉각수·오일 온도 | SDK `truck.water.temperature`·`truck.oil.temperature` | °C |
| EFL/P1 | 오일 압력 | SDK `truck.oil.pressure` | psi |
| VEP1 | 배터리 전압 | SDK `truck.battery.voltage` | V |
| LFE | 연료량·평균 소비 | SDK `truck.fuel.amount`·`truck.fuel.consumption.average` | 순간 연료율은 메모리에 후보만 있음(미확정) |
| CVW | 차량 중량 | def 축 kerb 하중 + 연료 + 캐빈 | 정적. 아래 물리 검증에서 약 12.0 t |
| — | 브레이크 온도, 리프트 액슬 | SDK `truck.brake.temperature`·`truck.lift_axle`·`truck.wheel.lift[i]` | |

ABS·ESC 작동 플래그는 찾지 못했습니다. 바퀴 각속도와 차속으로 슬립을 계산해 대신합니다.

## 메모리 위치와 찾은 방법

플레이어 vehicle(스키마 controller → player actor → +0x18)에서 시작합니다. 오프셋은 `ot/schema/1.61.1.9/fields.json`의 `vehicle_drivetrain`입니다.

| 값 | 위치 |
|---|---|
| 구동계 물리 객체 P | vehicle+0x150 → |
| 엔진 각속도, 출력축 각속도 | P+0x2e0, P+0x2ec (rad/s) |
| 엔진 토크, 변속기 출력 토크 | P+0x2f4, P+0x2d4 (Nm) |
| 기어비, 클러치 | P+0x2c4, P+0x330 |
| 바퀴별 브레이크 토크 | P+0x260 → float[바퀴 수] |
| 계기 객체 Q | vehicle+0x1820 → |
| 리타더 비율, 엔진브레이크 | Q+0x3e8, Q+0x3f0 |

[research/can_scan.py](../research/can_scan.py)가 vehicle과 그 포인터 대상 2단계(2,279개 객체, 2.3 MB)를 5 Hz로 읽으면서 SDK 채널과의 상관을 누적하고, [research/can_probe.py](../research/can_probe.py)가 후보 객체를 30 Hz로 다시 읽어 모든 SDK 채널과 대조했습니다. 둘 다 `PROCESS_VM_READ | PROCESS_QUERY_INFORMATION`만 씁니다. 원자료는 `research/live/2026-10-20-can-scan`, `-can-probe`, `-can-probe-brake`(git 제외)입니다. 게임이 업데이트되면 이 오프셋은 `rebase_rvas.py`가 옮기지 않으므로 두 스크립트로 다시 확인합니다.

## 물리 검증

- ✔ 출력 토크: 변속·클러치 구간을 뺀 4,054개 표본에서 `output_torque × 2.59 / 0.506 − 3.0·v² = m·a + c·m·g`를 맞추면 R² 0.946, m 11,967 kg, 구름 저항 c 0.0072입니다. def 타이어 구름 저항(앞 6.5, 뒤 7.5 kg/t)과 맞고, 질량은 축 kerb 9,820 kg + 캐빈 700 kg + 연료 약 900 L(약 760 kg) + 회전 관성 등가와 맞습니다. 같은 식을 엔진 토크 × 기어비로 하면 R² 0.918입니다.
- ✔ 엔진 토크: 전부하에서 1,600–2,000 rpm은 def `2700 × torque_curve`와 1 % 안에서 같습니다(2,129 대 2,137, 1,985 대 1,988 Nm). 1,000–1,600 rpm은 변속 직후 과도 구간이 섞여 더 낮게 나옵니다.
- ✔ 마찰 토크: 스로틀 0, 엔진브레이크 꺼짐, 클러치 연결에서 800–1,600 rpm에 −265 ~ −287 Nm로 거의 일정합니다.
- ✔ 엔진브레이크: −3,024 Nm(2,000–2,200 rpm). 자동 변속기가 엔진브레이크 동안 이 회전수를 유지해 다른 회전수 자료는 없습니다. 기어별 엔진브레이크 감속은 이 토크 × 기어비 × 2.59 / 0.506 / m로 나옵니다.
- ✔ 리타더: def `auxiliary_brakes`가 단별 (비율, 엔진브레이크 여부)를 정합니다. G33CM-R은 0.2, 0.4, 0.6, 0.8, 그리고 5단은 1.0과 엔진브레이크입니다(docs/29의 "5단 = 리타더 + 엔진브레이크" 측정과 일치). 출력축에서 본 리타더 토크(Nm)는 출력축 회전에 거의 비례합니다.

| 출력축 rad/s | 40–60 | 60–80 | 80–100 | 100–120 |
|---|---|---|---|---|
| 1단 | −115 | −231 | −313 | −400 |
| 2단 | −351 | −564 | −786 | −952 |
| 3단 | −574 | −931 | −1,291 | −1,577 |
| 4단 | −895 | −1,329 | −1,842 | — |

  계산식의 0.905는 타력에서 잰 `output_torque / (engine_torque × gear_ratio)` 중앙값입니다.
- ✔ 브레이크 토크: 브레이크 1.0에서 앞바퀴 28.3 kNm, 구동축 24.0 kNm, 태그축 9.4 kNm까지(태그 올림 상태). def 물리의 `brake_balance 1.1`(앞이 더 셈)과 방향이 같습니다.

## 정적 사양(EC1에 해당)

`py -3 research/truck_spec.py`는 프로필의 최신 세이브를 복호하고(research/savedec, TruckLib), SDK 트럭 구성과 맞는 소유 트럭 중 주행거리가 가장 긴 트럭을 골라 def를 모드 우선 순서로 읽습니다. 현재 레퍼런스 트럭 결과:

- 엔진 DC16 520: 2,700 Nm, 곡선 (300, 0.1) (440, 0.4) (1000–1300, 1) (1600, 0.82) (1900, 0.72) (2300, 0.4) (2500, 0.1).
- 변속기 G33CM-R: 14+8단, 종감속 2.59, 변속 시간 0.7 s, 리타더 5단.
- 섀시 6x2 롱 태그리프트: 축 kerb 5,650 / 2,383 / 1,787 kg(합 9,820), 구동 2축, 리프트 3축. Realistic Truck Physics 9.2가 섀시 서스펜션(residual travel 0.17 / 0.088 / 0.088)을 바꿉니다.
- 타이어: 구름 저항 앞 6.5, 뒤 7.5 kg/t, 젖은 노면 접지 0.9(비가 오면 마찰이 바뀌는 근거).
- 전역 물리(RTP 9.2가 덮어씀): 공기 저항 3.0·v², 브레이크 최대 온도 200 °C, 브레이크 균형 1.1, 횡 bristle 강성 ×1.15. 엔진·브레이크·리타더 관련 값은 기본 게임 그대로입니다.

## 트레일러와 구성 (core 0.40.0)

- SDK 채널: `trailer.0.connected`, `trailer.0.world.placement`, `trailer.0.velocity.linear`·`angular`, `trailer.0.acceleration.linear`, `trailer.0.cargo.damage`, 바퀴별 `trailer.0.wheel.on_ground`·`angular_velocity`·`suspension.deflection`·`lift`. 트럭 위치와 트레일러 위치로 히치 각을 직접 구할 수 있습니다(실차라면 추정해야 하는 값).
- `ot.cmd configs`(파이프 명령 `configs`): 트럭 외 SDK 구성을 id별로 돌려줍니다. `trailer.0`(바퀴 위치·반경·리프트 가능 여부, 히치 위치, 차체 종류), `job`(화물 id·질량, 작업이 없으면 빈 값), `controls`(`shifter.type`: 자동은 `automatic`, 시퀀셜은 `manual`), `hshifter`, `substances`.
- 2026-10-20 확인: 개인 트레일러 `vehicle.scs.box`(냉동 박스, 3축 6바퀴, 첫 축만 리프트 가능). 트레일러 좌표에서 히치 z −5.07 m, 축 z 1.54·2.86·4.16 m라 히치에서 가운데 축까지 약 7.9 m입니다. 작업이 없어 빈 트레일러이고 화물 질량도 없습니다.

## 남은 것

- 순간 연료율, ABS·ESC 플래그, 엔진 부하율(EEC2)은 확정하지 못했습니다.
- 엔진브레이크 토크는 2,000–2,200 rpm 한 점입니다. 수동 변속이나 내리막에서 다른 회전수를 잽니다.
- 트레일러를 달면 질량과 브레이크(트레일러 바퀴)가 바뀝니다. 같은 스캔으로 트레일러 쪽 값을 찾습니다.
