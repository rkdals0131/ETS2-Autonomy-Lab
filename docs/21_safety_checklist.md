# armed 안전 시험 체크리스트 (W02)

목적: 로직 테스트(`ot/native/drive_control_test.cpp`)만 통과한 해제 경로를 실제 게임에서 확인하고 bag으로 남긴다. 근거와 기대값은 [legacy/19 W02](legacy/19_real_stack_interface_plan.md#p0-보전과-안전)에서 가져왔다. 이 문서는 실행할 때 채우는 기록표이며 Windows 네이티브 런처·`pixi shell`에서 수행한다.

## 사전 조건

- W00 완료: `ot.cmd version`의 `drive.profile_supported`가 **true**. ✔ 2026-10-12 0.30.0에서는 false("Unsupported control operator", 프로필의 `no_cstm_mod`·`joy.b5`를 파서가 못 읽음)였고 0.31.0 loader에서 true([20 W00](20_windows_native_port_plan.md#w00-프로필-파서입력-보완-s-안전-선행)).
- W01 완료: 설치본 백업, `exe_sha256` 대조(✔ Steam은 현재 `AutoUpdateBehavior 0`).
- 교통 없는 직선 고속도로, `uset g_traffic 0`. 트레일러 없음.
- 런처 **주행 보조** 모드(입력·상태만, 2026-10-21 전 런처의 **입력·상태만** 체크)로 시작하고 "차량 상태" 프로필 기록을 켠 상태에서 각 시험을 수행한다(✔ `apps/launcher/bag_recording.py:STATE_TOPICS`에 `/ets2/drive/state`, `/ets2/drive/command` 포함).
- arm과 명령 발행은 `ros2 run ets2_autonomy drive_hold --ros-args -p axes:=3 -p steering:=0.0 -p throttle:=0.0 -p brake:=0.0`로 만든다(✔ W00 구현: arm 뒤 50 Hz로 `sent_steady_ns`를 채워 발행, Ctrl+C 또는 `--stop-event`로 disarm, DriveState reason 변화를 시뮬레이션 시각과 함께 출력, 종료 코드 0 disarm·1 오류·2 게임이 해제). 출력 줄을 각 행의 결과 칸에 붙인다. ✔ `ros2 topic pub`은 `sent_steady_ns`를 매번 새로 채우지 못해 릴레이 노드의 나이 검사에 버려지므로 쓰지 않는다. 런처 GT 주행(30 km/h, 경로 JSON 필요)도 가능하다. 명령 발행 없이 arm만 하면 200 ms 뒤 `expired`가 된다(#6).
- ✔ 현재 프로필은 `c_throt_dz 0`, `c_brake_dz 0`이라(fixture는 0.03/0.04) 트리거 노이즈가 수동 개입으로 판정될 수 있다. #0을 실제 데드존으로 먼저 확인한다.
- ✔ 키보드 WASD·방향키는 게임 창이 아니어도 수동 개입으로 잡힌다(`input_device.cpp:InputDevice::sample`의 `GetAsyncKeyState`). armed 중에는 다른 창에서도 타이핑하지 않는다.
- 발행기를 kill로 끝내면 `disarmed`가 아니라 `expired`로 끝난다(✔ GT 노드는 SIGINT로만 `control(false)`를 부름). 정상 종료 경로(stop event·Ctrl+C)와 kill을 구분해 기록한다.

## 시험표

| # | 조작 | 기대 `DriveState.reason` / axes | 근거(코드) | 일자 | core | 결과 | bag 경로 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | 3축 armed(`drive_hold`), 패드를 만지지 않고 30 s 유지, 데드존 0/0 | `armed`/`command` 유지, 해제 없음 | 휴지 노이즈 확인(W00 완료 기준) | 2026-10-12 | 0.31.0 | ✔ 30.3 s `command` 유지, 해제 없음, stop event로 `disarmed`(exit 0). 명령 1501개(50 Hz), 릴레이 명령 나이 p50 0.56·p99 0.89·최대 1.56 ms. 정차·주차브레이크, Windows 네이티브 릴레이(입력·상태만) | 기록 안 함 |
| 1 | LCC armed 중 패드 좌우 조작 | `manual`, axes→0, 출력 3축 0 | `drive_control.hpp:DriveControl::frame`, `manual_input.cpp:ManualInput::evaluate` | 2026-10-12 | 0.31.0 | ✔ axes=1 arm 약 20 s 뒤 스틱 조작에 `manual`(exit 2). 해제 프레임 `steering_manual` +0.19, 다음 프레임부터 applied +0.001→+0.003→+0.015→… 증가(게임 조향 속도 제한으로 완만) | `bridge/recordings/20261012-215141-1f6616d6/bag` |
| 2 | ACC armed 중 RT/LT 또는 W/S | `manual` | 동일 | 2026-10-12 | 0.31.0 | ✔ axes=2 arm 10.5 s 뒤 RT에 `manual`(exit 2). 해제 프레임 `throttle_manual` 0.18, 다음 프레임부터 applied 0.088→0.217→0.419→… | 동일 |
| 3 | ACC만 armed(axes=2) 중 조향 조작 | 유지(해제 없음) | `drive_control.hpp:DriveControl::frame`의 `manual_axes_&axes_` | 2026-10-12 | 0.31.0 | ✔ 33 s 동안 `steering_manual` −1.000…+1.000(페달 0)에도 `command` 유지, stop event로 `disarmed`(exit 0) | 동일 |
| 4 | F11 | `panic`, lease 취소, Tier 0 | `input_device.cpp:InputDevice::sample`(panic_down), `core.cpp:Runtime::panic` | 2026-10-12 | 0.31.0 | ✔ `panic`(exit 2), Tier 0, render hook 0. 릴레이가 lease 상실로 스스로 끝나고 런처가 기록을 정상 종료(metadata.yaml 있음). △ 상태가 `panic`→`paused`(1표본, 30 ms)→`panic`으로 깜빡임, 원인 미확인 | `bridge/recordings/20261012-220053-eaf9d6bd/bag` |
| 5 | ESC pause → resume | `paused`; resume 후 재arm 없음 | `drive_control.hpp:DriveControl::paused`, `loader.cpp`(pause 이벤트) | 2026-10-12 | 0.31.0 | ✔ `paused`(exit 2), pause부터 resume을 거쳐 36 s 동안 재arm 없음 | 동일 |
| 6 | 제어 노드 정지(발행 중단, `drive_hold`를 kill) | ≤ 250 ms 내 `expired` | `drive_control.hpp:DriveControl::frame`(deadline) | 2026-10-12 | 0.31.0 | ✔ 마지막 명령 수신 234 ms 뒤 `expired`(bag 수신 시각 기준). 첫 시도는 kill 전에 끝남: 아래 "발견" 1 | 동일 |
| 7 | 릴레이 종료(런처 중지) | `disconnected` | `bridge/windows/main.cpp:DriveEnd`(소멸자가 `drive disconnect`), `core.cpp:Runtime::command`의 drive disconnect, lease 반납 `core.cpp:Runtime::release_runtime` | 2026-10-12 | 0.31.1 | ✔ `disconnected` 뒤 165 ms에 Tier 0, 사유 유지(core 10 ms 폴링). bag에는 릴레이가 상태 발행을 멈춘 뒤라 전이가 없다. 0.31.0은 실패: 아래 "발견" 2 | `bridge/recordings/20261012-220911-19814b0c/bag` |
| 8 | 패드 USB 분리 | `input_unavailable` | `input_device.cpp:InputDevice::sample`(connected 전이) | 2026-10-12 | 0.31.0 | ✔ `input_unavailable`(exit 2). 0.47 s 뒤 게임이 스스로 일시정지해 사유가 `paused`로 바뀜(해제 상태 유지) | `bridge/recordings/20261012-220053-eaf9d6bd/bag` |

| 9 | loader 0.44 ACC armed(axes=2 또는 6) 중 가속 페달 | 해제 없음. 게임 입력은 운전자 값과 명령 중 큰 쪽(게임이 둘을 더하므로 명령은 초과분만), 브레이크 0, 리타더 0단 펄스·엔진브레이크 끔. 페달을 떼면 명령 단 다시 펄스 | `drive_control.hpp:DriveControl::frame`(throttle_override), `aux_locked(suspended)` | 2026-10-21 | 0.44.0 | ✔ `drive_speed`(axes 7, 목표 0, HOLD 브레이크 0.2) 중 운전자 가속 0.49–0.54가 약 1.2 s: 해제 없음, applied throttle = 운전자 값, applied brake 0(명령 0.15). 떼자 다음 프레임 brake 0.149 적용. △ 리타더 펄스 중지는 게임 리타더 채널로 확인하지 않음 | 로컬 `override2/drive_state.jsonl` |
| 10 | loader 0.44 ACC armed 중 브레이크 페달 | 페달·보조 브레이크 축 회수(그 epoch 동안), 남은 축이 없으면 `manual`. `drive_speed`는 "ACC cancelled by the driver's brake" 뒤 LCC만 계속 | `DriveControl::frame`의 `revoked_`, `gt_drive_node.cpp`(pedal_sequence) | 2026-10-21 | 0.44.0 | ✔ 운전자 브레이크 0.13에 같은 표본에서 axes 7→1, retarder 명령 −1(해제), 노드 "ACC cancelled by the driver's brake; LCC continues", 조향 명령은 계속. 10 s 뒤 stop event로 `disarmed`(exit 0) | 동일 |
| 11 | loader 0.44 LCC armed 중 조향 | 해제 없음. 조작하는 동안 명령 조향 0(운전자만), `drive_speed`는 놓은 뒤 1 s 지나 LCC 재개. 차로 기준을 벗어나면 ACC가 켜져 있을 때 조향만 돌려줌 | `DriveControl::frame`, `gt_drive_node.cpp`(driver_steering_until) | 2026-10-21 | 0.44.0 | ✔ 운전자 스틱 ±0.16–0.78 동안 해제 없음, 조향 명령 0, applied가 운전자를 따름(±0.4). 마지막 조작 약 1.4 s 뒤 LCC 재개(명령 −0.004, 목표 0으로 복귀). 차로 기준 이탈 경로는 게임에서 시험 안 함 | 동일 |
| 12 | loader 0.44 운전자 입력 중 arm | 요청 축에 운전자 입력이 있으면 거절(이전과 같음) | `DriveControl::request`의 `manual_owned` | 2026-10-21 | 0.44.0 | ✔ 첫 시도에서 시작 시 가속 0.47이 눌려 있어 arm 거절(사유는 `disabled`로만 보임). `drive_speed`가 이제 "Driver input on a requested axis…"로 먼저 알림 | 로컬 `override1/` |

완료 기준: 8개 모두 기대 reason으로 전이하고, `/ets2/vehicle/actuation`의 applied 값이 다음 SDK 프레임부터 사람 입력을 따른다. 각 행의 bag 경로가 채워진다. 실패 항목은 [포팅 계획 §6](20_windows_native_port_plan.md#6-의존-그래프와-이어받는-작업)에 등록한다.

✔ 2026-10-12 **W02 완료**(#7은 core 0.31.1). applied 추종은 `/ets2/drive/state`의 `*_applied`로 #1·#2에서 확인. Windows 네이티브 릴레이(입력·상태만). 런처 기록 세 개는 모두 metadata.yaml로 닫혔다.

## loader 0.44.0 운전자 개입 계약 (2026-10-20 밤)

사용자 요청으로 #1·#2의 "어느 축이든 운전자 입력이면 전부 해제"를 바꿨다. `ManualInput`의 활동 비트를 조향·가속·브레이크로 나누고(`drive_api.hpp:ot_manual_*`), 가속은 상향 오버라이드, 조향은 운전자 우선(세션 유지), 브레이크만 종방향 보조 종료로 둔다. `ot_drive_test`가 fixture와 실제 프로필로 통과했다(2026-10-20). 0.31 loader에서는 #1·#2 동작이 그대로라, `drive_speed`는 `manual` 해제를 오류가 아닌 "The driver took over"로 끝낸다. 2026-10-21 loader 0.44.0을 설치하고 #9–#12를 게임에서 확인했다(트레일러 없음, 회사 마당 정차). 같은 시험에서 ACC 결함 하나가 보였다: 가속 오버라이드로 HOLD → GO가 된 뒤 목표 0·선행차 없음의 CRUISE에서 1.08 m/s로 8 s 넘게 기어갔다(a_des −1.5, 브레이크 0.085). 선행차 없이 목표 0에서 STOP/HOLD로 가는 경로가 없고, 자동 변속기의 아이들 크리프를 할당기가 모르는 것으로 본다. ACC 질량 적응 작업에 함께 넘겼다.

## 발견

1. ✔ **발행기 publish 정지 330 ms.** #6 첫 시도에서 arm 1.5 s 뒤 kill 전에 `expired`가 났다. bag의 `sent_steady_ns`로 보면 seq 75는 seq 74보다 15 ms 뒤에 찍혔지만 342 ms 뒤에 도착했고, seq 76은 seq 75보다 328 ms 뒤에 찍혔다. 즉 `drive_hold` 안에서 seq 75의 publish 호출이 약 330 ms 막혔다. 같은 구간의 `/ets2/drive/state`·`/clock`·GNSS는 끊기지 않았다. 릴레이는 seq 75를 나이 초과로 버리고(`stale_commands` 1) core는 마지막 명령 233 ms 뒤 `expired`로 해제해 안전 경로는 설계대로 동작했다. △ 원인은 Fast DDS 쓰기 경로로 추정하며 미확인. 2026-10-12 시험 약 10회 중 1회. 자율주행 제어기가 같은 정지를 겪으면 200 ms 창에서 거짓 해제가 난다. 장시간 발행 jitter 측정이 남았다.
   - 2026-10-13 게임 없는 재현 시도(같은 `fastdds.xml`, rclpy 발행기 50 Hz·대용량 10 Hz): 구독자 강제 종료는 4회 모두 1.3–3.0 s 뒤 발행 한 번이 약 20 ms 막혔다(토픽 16개여도 한 번). 구독자 25 s 일시정지, ros2 CLI 5회, bag 기록기의 19개 토픽 구독은 최대 4.9 ms. 330 ms는 재현하지 못했다. 릴레이는 이제 토픽 하나의 발행이 50 ms를 넘으면 시각·토픽·잠금 대기·발행 시간을 상태 보고 `slow_publishes`에 남기고 런처 "발행" 줄에 횟수를 보인다. 다음 게임 세션에서 그 시각을 구독자 접속·종료와 맞춰 본다.
2. ✔ **0.31.0은 릴레이 정상 종료를 `panic`으로 기록.** `DriveEnd`가 `disconnected`로 해제한 76 ms 뒤 lease 반납이 `Runtime::panic`을 불러 사유를 `panic`으로 덮었다. 해제 자체는 맞지만 기록이 틀리다. 0.31.1에서 lease 반납은 `Runtime::release_runtime(disconnected)`로 바꾸고 F11·오류·lease 만료는 `panic`으로 남겼다. 게임 실행 중 `loader unload` → core 교체 → `loader load`로 설치(7.5 s 공백, 운전 epoch 유지). 이전 DLL은 `ot/backup/2026-10-12-before-0.31.1/`.
3. ✔ 해제 상태의 `reason`은 마지막 해제 사유가 아니라 이후 사건으로 바뀐다(#8의 `input_unavailable`→`paused`, #4의 깜빡임). 해제 사유는 armed→해제 전이 표본에서 읽는다.

## bag 확인 명령

```bash
ros2 bag info <bag>
ros2 topic echo --qos-reliability reliable /ets2/drive/state --field reason   # 재생 중
```

재생은 `ros2 bag play <bag>`이며 `--clock`을 붙이지 않는다([04 기록](04_sensors_and_data.md#기록)).
