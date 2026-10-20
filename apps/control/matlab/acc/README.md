# 고속도로 ACC 종방향 제어기 (MPC + Stateflow + Embedded Coder)

레퍼런스 트럭(Scania S 6x2, DC16 520, G33CM-R 리타더, 빈 박스 트레일러 연결 약 20.3 t, [docs/31](../../../../docs/31_plant_parameters.md))의 ACC입니다. 게임의 리얼 오토매틱이 변속하고, 제어기는 스로틀·서비스 브레이크·리타더 단만 냅니다. 상위는 선형 MPC(MPC Toolbox, 비교용 두 제어기를 Multiple MPC 블록으로 전환), 하위는 측정 맵으로 만든 액추에이터 배분기, 모드는 Stateflow이고, `ACC_Controller` 하나를 Embedded Coder로 C++ 클래스(`ets2::AccController`)로 만듭니다. 폐루프 검증용 플랜트는 [종방향 플랜트](../../../sysid/matlab/longitudinal/README.md)에 자동 변속 모델을 붙인 것입니다.

## 실행

MATLAB에서 이 폴더로 이동한 뒤:

```matlab
build_acc_plant();        % acc_plant.slx: 종방향 플랜트 + 자동 변속(Stateflow)
validate_acc_plant();     % acc-20261020-192243 구간 재생 → results/plant_validation.json, plant_*.png (약 1분)
build_acc_controller();   % ACC_Controller.slx
build_acc_closed_loop();  % acc_closed_loop.slx (제어기·플랜트 Model 블록 + 선행차)
run_acc_scenarios();      % 시나리오 10개 → results/scenario_*.json/png, scenarios_summary.json (약 2분)
generate_acc_code();      % C++ 생성·컴파일(codegen/, git 제외) → generated/에 소스 복사 (약 1.5분)
verify_acc_codegen();     % 생성 코드 SIL 대 모델 비교 → results/codegen_verification.json (약 8분)
compile_generated();      % generated/만으로 MSVC 컴파일 + example/acc_smoke_main.cpp 실행
```

`run_acc_scenarios(Rebuild=true)`는 세 .slx를 모두 다시 만들고 돌립니다. `.slx`는 모두 build 스크립트로 다시 만들어지므로 모델을 직접 고치지 않습니다. 맞춤과 식별은 `fit_auto_shift()`(변속 사건 표, 발진 클러치 맞춤), `identify_actuator_lag()`(게임 기록에서 경로별 지연), `identify_closed_loop_lag()`(시나리오 결과에서 a_des → 가속도 지연)입니다.

- MPC 블록은 제어기 객체를 기본 작업공간에서만 찾습니다. 그래서 `ACC_Controller.slx`의 InitFcn이 `acc_controller_init`를 불러 `mpcComfort`, `mpcEmergency`를 기본 작업공간에 만듭니다. 나머지 파라미터(`C`, `MODE_*`)는 모델 작업공간에 있고 `acc_controller_params.m`에서 옵니다.
- 플랜트 파라미터는 `acc_plant.slx` 모델 작업공간의 `P = acc_plant_params()`와 차트용 `AT_*`입니다. `acc_plant_params`가 `apps/sysid/matlab/longitudinal`을 경로에 넣고 `longitudinal_params("trailer")`를 읽습니다(그 폴더는 고치지 않음).
- 기록은 `acc_load_recording("<run>")`가 읽어 `cache/<run>.mat`에 둡니다(git 제외).

| 파일 | 내용 |
|---|---|
| `acc_plant_params.m` | 플랜트 파라미터: 종방향 맞춤값 + 자동 변속(`P.at`) + ACC용 보정(`P.acc`), 값마다 출처 |
| `build_acc_plant.m` → `acc_plant.slx` | 종방향 모델을 메모리에서 만들어 서브시스템으로 복사하고 `AutoGearbox` 차트를 붙임 |
| `extract_shift_events.m`, `fit_auto_shift.m` | 자동 변속 사건 추출(140개), 발진 클러치 용량 맞춤 → `results/auto_shift_fit.json` |
| `replay_segment.m`, `replay_collect.m`, `validate_acc_plant.m` | 기록 명령 재생과 구간별 검증 |
| `identify_actuator_lag.m`, `identify_closed_loop_lag.m` | System Identification Toolbox `procest`(P1D)로 지연 식별 |
| `acc_controller_params.m` | 제어기 튜닝·차량 맵 전부(측정 출처 포함) |
| `acc_design_mpc.m`, `acc_controller_init.m` | MPC 객체 두 개(평상·비상) |
| `acc_condition.m`, `acc_mpc_prep.m`, `acc_allocate.m`, `acc_coast.m`, `acc_retarder_decel.m`, `acc_inverse_throttle.m`, `acc_inverse_brake.m` | MATLAB Function 블록이 부르는 코드 생성용 함수 |
| `build_acc_controller.m` → `ACC_Controller.slx`, `acc_codegen_config.m` | 제어기 모델과 ERT/C++ 설정 |
| `acc_scenarios.m`, `acc_scenario_signals.m`, `build_acc_closed_loop.m` → `acc_closed_loop.slx` | 시나리오 정의와 폐루프 모델 |
| `run_acc_scenarios.m`, `acc_scenario_metrics.m` | 시나리오 실행·지표·그림 |
| `generate_acc_code.m`, `verify_acc_codegen.m`, `compile_generated.m` | 코드 생성, SIL 비교, 독립 컴파일 |
| `generated/` | 생성된 C++ 소스(packNGo, 추적). `example/acc_smoke_main.cpp`는 통합 예제 |
| `results/` | 검증·시나리오·코드 검증 결과(json, png) |

## 구조

### 플랜트: `acc_plant.slx`

입력 `thr_cmd`, `brk_cmd`, `ret_step`(0–5), `eb_cmd`, `brake_temp`, `grade`, 출력 `v`, `a`, `rpm`, `gear`(SDK, 0 중립), `Te`, `clutch`, `thr_app`, `F_brake`입니다. ode3 5 ms(20 ms 차트, 50 ms 제어기의 정수배).

- `Longitudinal`: [종방향 플랜트](../../../sysid/matlab/longitudinal/README.md)를 그대로 복사한 것에 두 가지를 더했습니다. 서비스 브레이크 힘에 (입력, 속도) 보정표를 곱하고(저속 경제동, 아래), 정지 중 가속도 출력을 0으로 둡니다(모델의 지연된 힘/질량이 정지 후에도 음수로 남는 것을 게임처럼 0으로).
- `AutoGearbox`(Stateflow, 20 ms): NEUTRAL → LAUNCH_ENGAGE → LAUNCH_SLIP → LAUNCH_CLOSE → DRIVE ↔ SHIFT_OPEN → SHIFT_CLOSE → THR_RETURN. 기록에서 읽은 규칙입니다(`acc_plant_params.m`에 근거 수치).
  - 올림: 현재 단 회전수 > up_rpm(스로틀, 단). 스로틀 1.0에서 약 1,900, 0.2에서 약 1,460–1,500 rpm. 목표 단은 변속 뒤 회전수가 899(목표 ≤ 6) / 910(7–9) / 935 rpm(10 이상) 이상인 가장 높은 단이라 4 → 6 → 8 → 10처럼 건너뜁니다. 기록의 올림 69개 중 66개에서 같은 단을 고르고, 나머지 3개는 리타더 5단을 푼 직후의 7 → 8을 모델이 7 → 9로 고릅니다.
  - 내림: 예측 회전수(속도 + 감속 × 0.75 s)가 955 − 24·(12 − 단) rpm(스로틀 0) 아래, 킥다운 쪽은 스로틀³로 올림. 리타더 5단·엔진브레이크 중에는 1,505 rpm 아래에서 2,500 rpm 이하가 되는 가장 낮은 단으로 내림(12 → 9 → 7, 11 → 8 → 6 → 4 재현).
  - 변속 시간: 클러치가 열리기 시작해 0.50 s 뒤 SDK 단 변경, 0.233 s 뒤 클러치 닫힘, 0.80–0.82 s 뒤 스로틀 복귀(기록의 모든 변속이 같음). 내림 중에는 적용 스로틀 0.2(회전수 맞춤).
  - 중립: 스로틀 0에서 600 rpm 아래, 또는 브레이크 0.5 이상에서 예측 900 rpm 아래.
  - 발진: SDK 단 0 → 4(기록 17번 중 11번, 나머지는 2), 클러치 용량을 스로틀별로 램프(용량 최대 200/651/647/1,557/2,113 Nm, 시간 2.31/2.31/0.44/0.69/0.50 s, 스로틀 0/0.2/0.4/0.7/1.0). 종방향 모델의 클러치 식에 게임 클러치 값을 그대로 넣으면 게임보다 훨씬 많이 전달되므로, 재생 속도로 맞춘 값입니다(`fit_auto_shift.m`).
- 엔진브레이크(5단·B) 토크는 0.82배로 씁니다. 엔진 토크 채널은 −3,000 Nm인데 감속은 그만큼 나오지 않습니다(9단 63–40 km/h에서 게임 2.45, 보정 전 2.62, 보정 뒤 2.43 m/s²; docs/31의 "엔진브레이크 8단 약 12 % 작음"과 같은 방향).
- 서비스 브레이크 보정: 원래 모델은 70 km/h 표와 50 km/h 강제동을 잘 맞추지만 저속 경제동이 셉니다(20 km/h 0.1 → 모델 2.44, 게임 1.81 m/s²). 0.02–0.2 입력만 측정값/모델 비율(저속 0.67–0.85)을 곱하고, 큰 입력은 모델 그대로 둡니다. 0.3–1.0 정지 시험(`acc-20261020-185529`)은 브레이크 94–188 °C에서 쟀으므로 쓰지 않았습니다.

### 제어기: `ACC_Controller.slx` (서브시스템 `ACC_Controller`, 0.05 s, 원자 서브시스템)

`InputConditioning` → `ModeLogic`(Stateflow) → `MPC_Prep` → `MPC`(Multiple MPC Controllers) → `MPC_Post` → `Allocator`, 마지막 적용 요청은 Unit Delay로 MPC의 `ext.mv`에 돌아갑니다.

**MPC 예측 모델**(`acc_design_mpc.m`): 상태 [간격 d, 상대속도 v_lead − v, 자차 속도 v, a1, a2], a = a1 + a2. 조작변수는 둘입니다. u1은 구동 경로(스로틀·리타더 1–4, 1차 지연 0.5 s), u2는 브레이크 경로(1차 지연 0.33 s, u2 ≤ 0)이고, 선행차 가속도는 측정 외란으로 지평선 동안 일정합니다(선행차가 2 s 안에 서는 만큼까지만 감속으로 봄). 출력은 y1 = d − 2.0·v, y2 = 상대속도, y3 = v. Ts 0.05 s, 예측 80 스텝(4 s), 이동 블로킹 [2 3 5 10 20 40](경로당 6개 이동), 상태는 측정값을 그대로 넣는 custom 추정입니다(외란 모델 없음). 출력 a_des = u1 + u2입니다.

- **브레이크를 아끼는 비용**: 선형 MPC는 음의 가속에만 다른 가중을 줄 수 없습니다. 그래서 감속을 경로 둘로 나눴습니다. u1은 타력 + 리타더 4단까지(브레이크 없이 낼 수 있는 감속, 속도별 온라인 하한)만 내려갈 수 있고 가중 0.05, u2(서비스 브레이크)는 가중 1.0으로 20배 비쌉니다. 그래서 최적해는 타력·리타더를 먼저 쓰고 그 범위를 넘을 때만 브레이크 경로를 씁니다. 변화율 가중(2.0 / 3.0)과 경로별 변화율 제약이 매끄러움을 맡습니다.
- **제약**: 총 하한 a_min − u1_min이 u2 하한(평상 −3.0, 비상 −6.0 m/s²), u1 상한은 min(0.8, 0.85·전부하 가속(v)), 선행차를 따라잡는 중(선행차가 가속하지 않음)에는 0.4. 저크: u1 −1.0/+1.5, u2 −2.0/+2.5 m/s³(평상), −3/+3, −12/+6(비상). 온라인 경계는 마지막 이동에서 저크 제약으로 닿을 수 있는 값보다 좁히지 않습니다(그렇지 않으면 QP가 불능이 되어 마지막 값을 붙잡는 것을 시나리오에서 봤음). QP가 실패하면 가속하지 않고 마지막 제동을 유지합니다.
- **간격 정책**: 목표 간격 d0 + th·v(d0 5 m, th 기본 2.0 s, 범퍼-범퍼)를 직접 추종하지 않고, 상대속도 기준을 만듭니다. 선행차 속도에서의 간격 오차 e = d − (d0 + th·v_lead)에 대해 e > 0이면 접근 속도 기준 −(√(2·a·e + c²) − c), c = a/k(a = 2.0 m/s², k = 0.4 /s, STOP에서 0.8 /s)로, 최대 a의 감속으로 목표 간격에 닿는 속도이고 목표 근처에서는 선형(시정수 2.5 s)입니다. 지금 접근 속도보다 빠르게 다가가라고는 하지 않습니다(따라잡기 1.5 m/s까지). e < 0이면 0.05 /s·|e|(최대 0.5 m/s)로 천천히 벌립니다. 이 기준은 지평선을 따라 앞으로 굴려(간격 오차가 기준 속도만큼 줄고 선행차 감속으로 목표 간격이 줄어듦) 미리보기로 넣습니다. y1은 약한 가중(0.05)으로 시간 간격을 유지합니다.
- **최소 간격(soft)**: d ≥ 3 + 0.5·th·v입니다. 과제 문구의 "d0 + th·v"를 하한으로 그대로 두면 30 m 끼어들기(80 km/h, 목표 49 m)에서 하한을 19 m 어긴 상태가 되어 가혹하게 제동합니다. 그래서 d0 + th·v는 추종 목표로, 그 절반 시간 간격을 soft 하한(ECR 1, 가중 1e5)으로 두었습니다.
- **설정 속도**: CRUISE는 y3를 설정 속도로 추종(가중 0.6), 다른 모드에서는 v ≤ 설정 + 0.3 m/s soft 상한.

**배분기**(`acc_allocate.m`, 측정 맵):
- 타력 감속(속도·단·rpm에서 구름 + 공기 + 엔진 마찰, 70 km/h 12단 0.21 m/s²)보다 0.08 m/s² 이상 큰 요청이면 구동: 리타더 즉시 해제, 스로틀 = 현재 단의 역엔진맵(부분 부하 1 − (1 − thr)^1.33) + 가속도 PI(kp 0.2, ki 0.6 /s, 오차는 max(a, 타력) 기준, 중립·클러치 미끄럼·변속 뒤 1 s 동안 적분 정지, 포화 시 적분 정지). 타력 아래로 내려가면 구동을 끝냅니다(히스테리시스 0.08 / 0, 리타더가 아직 걸려 있으면 0.25 넘어야 구동으로 가고 그 전에는 리타더를 한 단씩 내림).
- 그 아래는 스로틀 0, 리타더 1–4단 양자화: 필요 감속이 다음 단 몫의 70 %를 넘으면 한 단 올림(0.4 s 간격), 아래 단 쪽으로 70 % 내려가면 한 단 내림(1.0 s 간격), 방향을 바꿀 때는 2.5 s를 기다림. 단 몫 = 기울기(4.33/9.04/14.69/21.03 Nm/(rad/s))·v·(fd/r)²/m, 타력 포함 2·3·4단 0.43/0.53/0.61 m/s²(게임 0.42/0.52/0.60).
- 남는 감속이 0.12 m/s²(급할 때 0.02)를 넘으면 서비스 브레이크: 측정 표를 이 속도에서 역보간(연속값, 70 km/h에서 0.01당 약 0.23 m/s²), 브레이크 중 리타더 몫은 0.75로 셈(3단 + 0.05/0.1에서 1.61/2.79 대 합 1.70/2.82).
- 리타더 5단은 4 s 지평선 평균 요청 ≤ −2.2, 지금 요청 ≤ −2.0이 2 s 계속, 30 km/h 이상, 운동학적 필요 감속 ≤ 2.5(선행차에 다가가는 중이 아님)일 때만 씁니다. 3.1 s 동안 브레이크가 다리를 놓고, 그 뒤 5단 + 0.6보다 더 필요하면 4단 + 브레이크로 돌아갑니다. 비상에서는 쓰지 않습니다. `engine_brake`는 `C.alc.use_engine_brake = true`일 때만(리타더가 없는 트럭에서 5단 대신) 켭니다.
- HOLD: 브레이크 0.2, 리타더 0. STOP에서 0.5 m/s 아래는 브레이크 최소 0.05.

**모드**(`ModeLogic`, `mode` 출력): 0 OFF(enable false, 모든 출력 0), 1 CRUISE, 2 FOLLOW(선행차가 d0 + th·v + (v − v_lead)²/(2·2.0) + 20 m 안, 해제는 +30 m 또는 0.5 s 동안 선행차 없음), 3 STOP(선행차 < 0.5 m/s), 4 HOLD(정지 < 0.15 m/s), 5 GO(정지 뒤 3 s 안에 선행차가 2 m 이상 떠나면 자동, 아니면 resume 입력; 2 m/s에서 FOLLOW/CRUISE), 6 EMERGENCY(필요 감속 > 3.2 m/s² 또는 3 m/s 넘게 다가가며 TTC < 2 s; 4 s·2.0 m/s² 아래가 1 s 계속되면 해제). 필요 감속은 선행차가 속도를 유지하면 상대 정지거리, 선행차가 제동 중이면 둘 다 정지할 때의 값입니다(상수 감속 접근에서 TTC는 v/(2a)로 작아지므로 TTC만으로는 비상을 켜지 않음). enable 직후 첫 스텝은 `mode 0`, 다음 스텝부터 판정합니다. 정지 상태에서 켜면 HOLD에서 resume을 기다립니다.

### 폐루프: `acc_closed_loop.slx`

제어기(0.05 s)와 플랜트(5 ms)를 Model 블록으로 잇고, 명령에 35 ms 지연(게임 1 프레임 + 표본 하나, docs/31 "제어 루프 지연")을 둡니다. 선행차와 운전자 입력은 `scn_lead`, `scn_disc`(시나리오에서 생성)이고, `Controller` 블록을 SIL 모드로 바꾸면 생성 코드로 돕니다. 브레이크 온도는 80 °C로 고정입니다.

## 플랜트 검증 (`acc-20261020-192243` 재생, 기록 명령 그대로, 자동 변속은 모델이 선택)

| 구간 | 속도 RMS m/s | 비고 |
|---|---|---|
| 발진 0.2 / 0.4 / 0.7 / 1.0 → 50 km/h | 0.18 / 0.09 / 0.11 / 0.12 | 1.0: 0 → 49.5 km/h 10.72 s(게임 10.72), 단 순서 0-4-6-8-10 같음. 0.4는 게임이 2단에서 출발 |
| 50 km/h에서 정지(하네스 강제동) 4개 | 0.09–0.27 | 게임은 10 → 8로 한 번 내린 뒤 중립, 모델은 바로 중립 |
| 0 → 68–73 km/h 전부하, 4단 출발 3개 | 0.15 | |
| 같은 것, 게임이 2단 출발 5개 | 0.92–1.13 | 모델은 늘 4단 출발이라 50 km/h에 1 s 빠름 |
| 같은 것, U턴 직후 30 km/h에서 2개 | 1.01 | 게임 가속이 0.7 m/s²로 낮음(선회 중으로 추정), 9 → 8 킥다운 미재현 |
| 리타더 사다리 1 → 5 → 0 (73 km/h부터) | 0.83 | 단별 감속 아래 표 |
| 블렌드 9개(70 km/h, 리타더 0/3/5 × 브레이크 0/0.02/0.05/0.1) | 0.06–0.93 | 감속 아래 표 |

| 감속 m/s²(65–35 km/h 중앙값, 사다리는 단 후반 중앙값) | 게임 | 모델 |
|---|---|---|
| 브레이크 0.02 / 0.05 / 0.1 | 0.69 / 1.38 / 2.50 | 0.69 / 1.33 / 2.37 |
| 리타더 3 + 0 / 0.05 / 0.1 | 0.53 / 1.61 / 2.79 | 0.51 / 1.56 / 2.67 |
| 리타더 5 + 0 / 0.05 / 0.1 | 2.37 / 2.47 / 3.07 | 2.37 / 2.90 / 3.51 |
| 사다리 1 / 2 / 3 / 4 / 5단 | 0.26 / 0.42 / 0.52 / 0.60 / 2.46 | 0.31 / 0.40 / 0.48 / 0.56 / 2.10 |

평균 RMS: 발진 0.13, 정지 0.18, 전부하 가속 0.79, 사다리 0.83, 블렌드 0.43 m/s. 상세는 `results/plant_validation.json`, 그림 `results/plant_*.png`.

## 지연 식별

| 경로 | Tp1 s | Td s | 이득 | 자료 |
|---|---|---|---|---|
| 서비스 브레이크(게임) | 0.29–0.32 | 0.01–0.02 | 1.07–1.45(측정 표 대비) | 블렌드 0.02/0.05/0.1 계단, 적합 94–98 % |
| 스로틀(게임, 고정 단 팁인 0.8) | 0.22–0.61(중앙 0.33) | 0.15–0.31(중앙 0.24) | 0.61–0.91 | `acc-20261020-185529` tipin 7개, 적합 77–92 % |
| 리타더 2 → 3, 3 → 4(게임) | 0.06–0.15 | 0.05–0.11 | | 사다리, 적합 58–80 % |
| a_des → 가속도(폐루프, 배분기 + 플랜트) | 0.13–0.44 | 0–0.11 | 0.99–1.15 | 시나리오 5개 중 적합 82–97 %인 4개(설정 속도 시나리오는 48 %) |

MPC 모델은 구동 경로 0.5 s(스로틀 지연 + 데드타임), 브레이크 경로 0.33 s를 씁니다.

## 튜닝 값과 이유 (`acc_controller_params.m`)

| 항목 | 값 | 이유 |
|---|---|---|
| 표본 시간 / 예측 / 블로킹 | 0.05 s / 80(4 s) / [2 3 5 10 20 40] | 과제 조건. 4 s면 선행차 −4 제동이 끝까지 보임, 블로킹으로 QP 변수 13개 |
| 시간 간격 / 정지 간격 | 2.0 s(입력 1–3 s) / 5 m | 트레일러 연결 트럭 |
| 가중 y(간격항, 상대속도, 속도) | FOLLOW [0.05 0.5 0], CRUISE [0 0 0.6] | 간격은 상대속도 기준으로 다루고 시간 간격항은 보조 |
| 가중 u / Δu | [0.05 1.0] / [2.0 3.0] | 브레이크 경로 20배 → 타력·리타더 먼저 |
| 감속 한계 | −3.0(평상), −6.0(EMERGENCY만) | 과제 조건 |
| 가속 한계 | 0.8, 따라잡기 0.4, ≤ 0.85·전부하 가속 | 전부하 가속 65–75 km/h 0.7 m/s²(측정) |
| 접근 감속 a / 기울기 k | 2.0 m/s² / 0.4 /s(STOP 0.8) | 80 km/h에서 150 m 앞 정지차 최소 1.7 m/s²; k가 작으면 지수 꼬리가 길고(초기 −2.9 피크, 25 s 정지), 크면 끝에서 감속이 급해짐 |
| 따라잡기 / 벌리기 | 1.5 m/s / 0.05 /s·e, 최대 0.5 m/s | 선행차 제동 뒤 재가속 크기, 끼어들기 제동 크기 |
| soft 최소 간격 | 3 m + 0.5·th·v | 위 설명 |
| 리타더 히스테리시스 / 체류 | 70 % / 0.4 s 올림, 1.0 s 내림, 2.5 s 방향 전환 | 60 %·방향 전환 없음에서 0-1-2단을 오가는 것을 봄 |
| 브레이크 진입 / 해제 | 잔여 0.12(급할 때 0.02) / 0.03 m/s² | 작은 잔여는 MPC 피드백에 맡김 |

## 시나리오 결과 (`run_acc_scenarios`, 충돌 없음, 리타더 5단 사용 없음)

| 시나리오 | 최소 간격 m | 최소 TTC s | 최대 감속 m/s² | 최대 저크 m/s³ (1 s 변화) | 서비스 브레이크 s / kJ / 최대 입력 | 리타더 단 변경 | 정지 간격 m | 모드 |
|---|---|---|---|---|---|---|---|---|
| 설정 속도 60 → 80 km/h | – | – | 0.19 | 1.15 (0.85) | 0 / 0 / 0 | 0 | – | CRUISE, 80 km/h 오차 0.00 |
| 선행 80 km/h, −2로 40 km/h | 27.2 | 22.8 | 1.99 | 2.78 (2.22) | 5.8 / 2,563 / 0.064 | 6 | – | FOLLOW, 최종 간격 오차 −0.02 m |
| 선행 80 km/h, −4로 40 km/h | 27.2 | 8.6 | 3.13 | 3.00 (2.63) | 3.3 / 2,421 / 0.120 | 8 | – | FOLLOW(비상 아님) |
| 선행 80 km/h, −6으로 정지 | 5.0 | 3.1 | 6.58 | 10.8 (5.84) | 11.2 / 4,680 / 0.463 | 5 | 5.0 | FOLLOW → EMERGENCY → STOP → HOLD |
| 30 m 끼어들기, 10 km/h 느림 | 26.1 | 10.7 | 1.48 | 2.72 (1.47) | 2.1 / 608 / 0.050 | 5 | – | CRUISE → FOLLOW |
| 스톱앤고(50 km/h, 5 s 정지, 1 m/s² 출발) | 7.7 | 4.8 | 1.97 | 2.72 (2.54) | 9.1 / 1,556 / 0.097 | 6 | 정지 전 재출발 | FOLLOW → STOP → FOLLOW |
| 스톱앤고, 15 s 정지 + resume | 5.6 | 4.8 | 1.97 | 3.47 (1.79) | 10.2 / 1,561 / 0.200* | 6 | 5.6 | … HOLD → GO(resume 28.95 s) |
| 스톱앤고, 정지 직후 출발 | 5.8 | 4.8 | 1.97 | 3.47 (1.79) | 10.1 / 1,561 / 0.200* | 6 | 5.9 | … HOLD → 자동 GO(22.65 s) |
| 150 m 앞 정지차, 80 km/h | 4.7 | 3.0 | 2.39 | 2.99 (2.29) | 15.2 / 3,753 / 0.200* | 5 | 4.7 | CRUISE → FOLLOW → STOP → HOLD |
| 선행 60 km/h가 1 m/s²로 멀어짐 | 38.3 | – | 0.21 | 1.15 (0.85) | 0 / 0 / 0 | 0 | – | FOLLOW → CRUISE(80 km/h) |

\* 0.200은 HOLD 유지 브레이크입니다. 브레이크 시간·에너지는 움직이는 동안만 셉니다(에너지 = 브레이크 힘 × 속도의 적분, 트레일러 포함). 저크는 0.25 s 이동평균 가속도의 미분이고, 변속 ±0.6 s와 정지 순간(v < 0.3 m/s)은 뺐습니다(변속 공백은 제어기가 못 없앰). 비상 외에는 1 s 가속도 변화가 2.7 m/s³ 이하입니다. 시나리오별 시계열은 `results/scenario_*.json`, 그림은 `results/scenario_*.png`, 지표는 `results/scenarios_summary.json`입니다.

스톱앤고(5 s 정지)에서 트럭은 서지 않고 4–12 km/h로 다가가다가 선행차가 다시 출발합니다. 50 km/h에서 32.8 m 간격으로 따라가다 선행차와 같은 −2로 제동하면 선행차가 멈출 때 25 m가 남는데, 간격 기준이 이 여유를 저속 접근으로 씁니다. 정지·HOLD·GO는 15 s 정지(resume)와 정지 직후 출발(자동 GO) 두 변형으로 확인했습니다.

## 코드 생성 (`generate_acc_code`, `verify_acc_codegen`, `compile_generated`)

- ERT, C++ 클래스 `ets2::AccController`, 고정 스텝 0.05 s 단일 레이트, 동적 메모리 없음, 파라미터 인라인, SIMD 확장 없음, 하드웨어 x86-64(Windows64, SIL 호스트와 같게; 코드는 `<stdint.h>` 고정폭 형을 써서 Linux x86-64 g++에서도 그대로 빌드되도록 함).
- `generated/`(packNGo 결과): `ACC_Controller.h/.cpp`, `ACC_Controller_data.cpp`, `ACC_Controller_private.h`, `ACC_Controller_types.h`, `rtGetInf.*`, `rtGetNaN.*`, `rt_nonfinite.*`, `complex_types.h`, `coder_posix_time.c/.h`(QP 해법기의 시간 함수, MathWorks 런타임 파일), `ert_main.cpp`(예제 main, 노드에는 넣지 않음).
- 검증: 폐루프 3개(선행 −4, 15 s 스톱앤고, 비상 정지)에서 제어기 입력을 기록해 `ACC_Controller.slx`를 Normal과 SIL로 다시 돌림. 출력 6개 최대 차이 6.4e-13 이하(폐루프 기록 대 Normal 재생 1.8e-14 이하). SIL 스텝 시간(/Od 디버그 빌드, 이 PC) 평균 3.5 ms, 최대 5.3 ms(주기 50 ms의 11 %).
- Embedded Coder 빌드가 `ert_main.cpp`까지 MSVC 2022로 링크하고(`codegen/ACC_Controller.exe`), `compile_generated()`가 `generated/`만으로 `example/acc_smoke_main.cpp`를 다시 컴파일해 돌립니다.

ROS 2 노드에서 쓰는 법(50 ms마다 `step()`):

```cpp
#include "ACC_Controller.h"
ets2::AccController acc;
acc.initialize();
ets2::AccController::ExtU_ACC_Controller_T in{};   // enable, set_speed_mps, time_gap_s, ego_speed_mps,
in.enable = true; /* ... */                          // ego_accel_mps2, lead_valid, lead_gap_m, lead_speed_mps,
acc.setExternalInputs(&in);                          // lead_accel_mps2, resume, engine_rpm, gear(int32_t)
acc.step();
const auto &out = acc.getExternalOutputs();         // throttle, brake, retarder_step(int32_t),
                                                     // engine_brake(bool), a_des_mps2, mode(uint8_t)
ets2::AccController::terminate();
```

컴파일할 소스는 `ACC_Controller.cpp`, `ACC_Controller_data.cpp`, `rtGetInf.cpp`, `rtGetNaN.cpp`, `rt_nonfinite.cpp`, `coder_posix_time.c`입니다. 입력 규약: `lead_gap_m`은 범퍼-범퍼, `gear`는 SDK 엔진 단(0 중립), `ego_accel_mps2`는 정지 중 0이어야 하고 안에서 0.1 s 1차 필터를 거칩니다. `resume`은 누르는 동안 true(한 스텝 이상). `retarder_step`은 정상 동작에서 0.4 s에 한 단씩 바뀌고, 하네스가 레버 키로 그 단까지 움직여야 합니다.

제품 노드 연결: `drive_speed`(`ros2/src/ets2_autonomy`)가 `longitudinal:=mpc`(기본)에서 이 코드를 씁니다. CMake가 `generated/`를 `acc_controller` 정적 라이브러리로 묶고, `src/control/acc_mpc.hpp`가 50 Hz 루프 안에서 50 ms 일정으로 `step()`을 부릅니다.
- 입력: 선행차는 GT 교통의 가장 가까운 차(`LeadTracker`가 가속도 추정), 간격은 앞범퍼 기준(앞 오버행 `front_overhang_m` 1.5 m, 미측정 추정). `gear`는 `/ets2/vehicle/actuation`의 엔진 단, `ego_accel_mps2`는 `VehicleState.acceleration_base_mps2[0]`, `resume`은 운전자가 가속 페달을 0.2 넘게 밟을 때입니다.
- 차로 끝: LCC 경로가 끝나는 곳에 정지한 가상 선행차를 두어 MPC가 세우고, HOLD가 되면 제어를 놓습니다.
- 리타더: 로더 0.42+이면 axes 4로 단과 엔진브레이크를 직접 냅니다(semantical `retarderN`). 아니면 단별 같은 감속의 브레이크 입력으로 접습니다(`AccMpc::folded_brake`).
- `longitudinal:=pid`는 예전 속도 PID와 간격 규칙입니다.

## 알려진 한계 / 남은 일

- 게임에서 아직 돌려 보지 않았습니다. 플랜트와 시나리오 결과는 모두 시뮬레이션입니다.
- 자동 변속: 게임은 발진 단을 2와 4 중에서 고르는데 규칙을 모릅니다(모델은 4). 스로틀을 갑자기 밟을 때의 킥다운(9 → 8, 1,126 rpm)은 재현하지 않습니다(정상 상태 규칙을 거기 맞추면 8 → 10 직후 바로 내림). 강제동 중 10 → 8 한 번 내림도 없습니다.
- 리타더 5단: 사다리 2.10 대 게임 2.46 m/s², 5단 + 브레이크 0.05/0.1은 2.90/3.51 대 2.47/3.07로 과대. 5단 해제 직후 저속(9 km/h) 감속 0.08 대 0.39. 배분기는 5단을 거의 쓰지 않으므로(시나리오에서 0회) 폐루프 영향은 작지만, 5단 경로는 폐루프로 검증되지 않았습니다. 평지 ACC에서 5단 감속(2.3–2.5 m/s²)은 대부분의 상황에 너무 크고, 쓸모는 내리막(플랜트 `grade` 입력은 있으나 미시험)에 있습니다.
- 브레이크: 0.02/0.05의 저속 값, 저속 ↔ 고속 전환 속도(20 → 43 km/h)는 측정이 없어 가정했습니다. 온도 페이드는 폐루프에서 80 °C 고정입니다. 9 km/h 아래 제동 곡선 1.6배(docs/31)는 넣지 않았습니다.
- 질량 20.3 t 고정(빈 트레일러). 화물을 실으면 MPC 모델의 경로 이득과 역스로틀 맵이 맞지 않습니다(PI 적분과 MPC 피드백이 일부 보상). 질량 추정은 없습니다.
- 변속 공백(0.73 s, 첫 변속은 터보 지연까지 1 s 남짓)은 MPC 모델에 없고 외란으로 보입니다. 설정 속도 가속 중 게임 변속기의 11 → 13 → 12 왕복(플랜트에서도 재현)이 가속을 2 s쯤 끊습니다.
- 선행차 가속도 입력의 잡음, 선행차 인식 끊김(0.5 s 뒤 CRUISE), 센서 지연은 시험하지 않았습니다.
- 생성 코드가 큽니다(`ACC_Controller.cpp` 2.3 MB, 약 33,000줄). 온라인 출력 가중(`y.wt`)과 예측 80 스텝 때문에 QP 행렬을 매 스텝 만들고, 두 MPC가 각각 코드를 가집니다. 고정 가중 제어기를 모드별로 따로 두거나 예측을 줄이면 작아지고 빨라집니다.
- 과제의 "최소 간격 soft = d0 + th·v"는 추종 목표로 쓰고 soft 하한은 3 m + 0.5·th·v로 두었습니다(위 설명). 하한을 바꾸려면 `C.fol.safe_frac`, `C.fol.d0_safe`입니다.
- `fit_auto_shift.m`의 발진 클러치 맞춤은 변속 규칙을 다듬기 전의 모델로 돌린 값이고, `acc_plant_params.m`에는 그 값을 옮겨 두었습니다.
