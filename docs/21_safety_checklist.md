# armed 안전 시험 체크리스트 (W02)

목적: 로직 테스트(`ot/native/drive_control_test.cpp`)만 통과한 해제 경로를 실제 게임에서 확인하고 bag으로 남긴다. 근거와 기대값은 [legacy/19 W02](legacy/19_real_stack_interface_plan.md#p0-보전과-안전)에서 가져왔다. 이 문서는 실행할 때 채우는 기록표이며, 포팅 전(WSL)과 후(Windows) 어느 쪽에서든 같은 절차로 수행한다.

## 사전 조건

- W00 완료: `ot.cmd version`의 `drive.profile_supported`가 **true**. ✔ 2026-10-12 실측은 false("Unsupported control operator", 프로필의 `no_cstm_mod`·`joy.b5`를 파서가 못 읽음)라 이 상태에서는 arm이 거절된다([20 W00](20_windows_native_port_plan.md#w00-프로필-파서입력-보완-s-안전-선행)).
- W01 완료: 설치본 백업, `exe_sha256` 대조(✔ Steam은 현재 `AutoUpdateBehavior 0`).
- 교통 없는 직선 고속도로, `uset g_traffic 0`. 트레일러 없음.
- 런처 **입력·상태만**으로 시작하고 "차량 상태" 프로필 기록을 켠 상태에서 각 시험을 수행한다(✔ `apps/launcher/bag_recording.py:STATE_TOPICS`에 `/ets2/drive/state`, `/ets2/drive/command` 포함).
- arm과 명령 발행은 `ros2 run ets2_autonomy drive_hold --ros-args -p axes:=3 -p steering:=0.0 -p throttle:=0.0 -p brake:=0.0`로 만든다(✔ W00 구현: arm 뒤 50 Hz로 `sent_steady_ns`를 채워 발행, Ctrl+C 또는 `--stop-event`로 disarm, DriveState reason 변화를 시뮬레이션 시각과 함께 출력, 종료 코드 0 disarm·1 오류·2 게임이 해제). 출력 줄을 각 행의 결과 칸에 붙인다. ✔ `ros2 topic pub`은 `sent_steady_ns`를 매번 새로 채우지 못해 `ets2_bridge`의 나이 검사에 버려지므로 쓰지 않는다. 런처 GT 주행(30 km/h, 경로 JSON 필요)도 가능하다. 명령 발행 없이 arm만 하면 200 ms 뒤 `expired`가 된다(#6).
- ✔ 현재 프로필은 `c_throt_dz 0`, `c_brake_dz 0`이라(fixture는 0.03/0.04) 트리거 노이즈가 수동 개입으로 판정될 수 있다. #0을 실제 데드존으로 먼저 확인한다.
- 발행기를 kill로 끝내면 `disarmed`가 아니라 `expired`로 끝난다(✔ GT 노드는 SIGINT로만 `control(false)`를 부름). 정상 종료 경로(stop event·Ctrl+C)와 kill을 구분해 기록한다.

## 시험표

| # | 조작 | 기대 `DriveState.reason` / axes | 근거(코드) | 일자 | core | 결과 | bag 경로 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | 3축 armed(`drive_hold`), 패드를 만지지 않고 30 s 유지, 데드존 0/0 | `armed`/`command` 유지, 해제 없음 | 휴지 노이즈 확인(W00 완료 기준) | | | | |
| 1 | LCC armed 중 패드 좌우 조작 | `manual`, axes→0, 출력 3축 0 | `drive_control.hpp:DriveControl::frame`, `manual_input.cpp:ManualInput::evaluate` | | | | |
| 2 | ACC armed 중 RT/LT 또는 W/S | `manual` | 동일 | | | | |
| 3 | ACC만 armed(axes=2) 중 조향 조작 | 유지(해제 없음) | `drive_control.hpp:DriveControl::frame`의 `manual_axes_&axes_` | | | | |
| 4 | F11 | `panic`, lease 취소, Tier 0 | `input_device.cpp:InputDevice::sample`(panic_down), `core.cpp:Runtime::panic` | | | | |
| 5 | ESC pause → resume | `paused`; resume 후 재arm 없음 | `drive_control.hpp:DriveControl::paused`, `loader.cpp`(pause 이벤트) | | | | |
| 6 | 제어 노드 정지(발행 중단, `drive_hold`를 kill) | ≤ 250 ms 내 `expired` | `drive_control.hpp:DriveControl::frame`(deadline) | | | | |
| 7 | 릴레이 종료(런처 중지) | `disconnected` | `bridge/windows/main.cpp:DriveEnd`(소멸자가 `drive disconnect`), `core.cpp:Runtime::command`의 drive disconnect | | | | |
| 8 | 패드 USB 분리 | `input_unavailable` | `input_device.cpp:InputDevice::sample`(connected 전이) | | | | |

완료 기준: 8개 모두 기대 reason으로 전이하고, `/ets2/vehicle/actuation`의 applied 값이 다음 SDK 프레임부터 사람 입력을 따른다. 각 행의 bag 경로가 채워진다. 실패 항목은 [포팅 계획 §6](20_windows_native_port_plan.md#6-의존-그래프와-이어받는-작업)에 등록한다.

## bag 확인 명령

```bash
ros2 bag info <bag>
ros2 topic echo --qos-reliability reliable /ets2/drive/state --field reason   # 재생 중
```

재생은 `ros2 bag play <bag>`이며 `--clock`을 붙이지 않는다([04 기록](04_sensors_and_data.md#기록)).
