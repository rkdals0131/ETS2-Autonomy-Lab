# 릴레이 ROS 노드 설계 (P1)

[포팅 계획](20_windows_native_port_plan.md) P1의 상세입니다. 센서 계산·수집·CDR 생성은 건드리지 않고 전송 계층을 rclcpp로 바꿉니다. ✔ 2026-10-12 구현(게임 시험 전). 아래는 설계 기록이며 달라진 점은 [20 P1](20_windows_native_port_plan.md#p1-릴레이를-ros-노드로-접기-m---코드-완료-2026-10-12-게임-시험-남음)에 있습니다. 코드 인용은 `파일:심볼`입니다.

## 1. 현재 릴레이의 스레드와 바꿀 지점

✔ `bridge/windows/main.cpp:run`의 worker 5개:

| worker | 현재 | P1 |
| --- | --- | --- |
| control rx(첫 `workers.start`) | 상태 TCP 수신: `demand`, `capture`, `drive_request`, `received_bundles`, `echo_us` | 삭제. demand·capture·drive는 노드가 제공. 자리는 drive worker |
| state tx(`state_messages`+`append_motion_messages` → `state_send`) | SDK 프레임마다 | 유지. `state_send` → `endpoint.publish`. 자세 이력 링 갱신 추가(P3용, §7) |
| shm reader(`Mapping`·`gpu.read` → `read_queue`) | | 유지 |
| encoder(`sensor_messages` → `send_queue`) | | 유지 |
| bulk sender(`send_packet(bulk,…)` + 250 ms heartbeat) | | `endpoint.publish`. heartbeat 삭제 |

메인 루프의 `demand.load()`·`capture_wanted`는 노드 값으로. `run()` 바깥의 재연결 루프("Network disconnected…")는 삭제. `Packet{meta,data}`의 `messages[]`(✔ `ros_cdr.hpp:append_cdr`가 topic/offset/length를 채움)가 publish 단위다.

## 2. 새 모듈 `bridge/windows/ros_endpoint.{hpp,cpp}`

```cpp
namespace bridge {
class RosEndpoint {
public:
    RosEndpoint(int argc,char** argv,std::string session);       // rclcpp::init(SignalHandlerOptions::None)
    ~RosEndpoint();                                               // executor cancel → join → publishers reset → rclcpp::shutdown
    void publish(const Packet& packet);                           // 어느 스레드든. Packet.data 범위를 SerializedMessage로 한 번 복사해 publish
    std::shared_ptr<const Demand> demand() const;                 // 200 ms마다 갱신된 구독 토픽 집합
    bool capture_wanted() const;                                  // /ets2/capture
    bool pop_drive_request(json& request);                        // 최신 명령 하나(LatestQueue 의미)
    void set_drive_control(std::function<json(json)> owned);      // /ets2/drive/control 콜백 → owned({{"cmd","drive"},...})
    void set_overlay(std::function<void(const nav_msgs::msg::Path&)> forward);  // P3
    json diagnostics() const;                                     // 명령 나이 p50/p99, publish 소요, 구독 수
};
}
```

- publisher: ✔ `ets2_bridge/src/main.cpp:main`의 QoS 규칙(latched→R TL K1, state→R K3, 그 외 BE K3)을 `topics()`에 적용. `wire.hpp`에서 `Topic`·`topics()`·`msgs_version`만 남긴 `topics.hpp`.
- `publish`: 토픽별 publisher와 재사용 `rclcpp::SerializedMessage`를 한 쌍씩. `Packet.data`의 [offset, offset+length)를 `reserve` 후 복사(rclcpp는 외부 버퍼를 빌리지 않으므로 복사 한 번이 맞다). ✔ 수신 쪽 검사(`ets2_bridge:receive_publish`의 길이·경계·헤더)는 생성 쪽 버그 방어용 assert만 남긴다. publisher마다 mutex.
- demand: 타이머 스레드가 200 ms마다 `get_subscription_count()>0` 이름을 모아 `atomic<shared_ptr<const Demand>>` 교체(✔ `ets2_bridge:main`의 demand 루프).
- 구독·서비스: `SingleThreadedExecutor` 전용 스레드. `/ets2/drive/command` 콜백은 ✔ `ets2_bridge:command_subscription`의 검증(owner, 유한, 범위, `sent_steady_ns` 나이 ≤ `max_command_age_ms`)을 그대로 하고 최신 슬롯에 저장. `/ets2/drive/control` 콜백은 `owned()`를 동기 호출(pipe 3 s 타임아웃이 executor를 막지만 arm/disarm은 드묾).
- drive worker: `pop_drive_request` → `request["deadline_ms"]=ticks()+200`(✔ `main.cpp:run` control rx의 `drive_request` 처리) → `owned()` → 수락 시 `now_steady - sent_steady_ns`를 명령 나이 히스토그램에 기록(진단).
- 종료: 콘솔 핸들러·stop event(`main.cpp:stop_watch`)는 기존 유지. 소멸 순서 `workers.stop()` → `RenderCapture` → `Lease` 해제 → `RosEndpoint` → `rclcpp::shutdown`. ✔ `gt_drive_node.cpp:main`처럼 `SignalHandlerOptions::None`.

## 3. 삭제·축소

| 대상 | 처리 |
| --- | --- |
| `main.cpp`의 ROS 호스트 주소 조회 | 삭제 |
| `main.cpp:run`의 connect·hello·`wire_version`·welcome, `state_tx`·`state_send`, control rx worker, bulk heartbeat, 재연결 루프 | 삭제·교체(§1) |
| `relay_workers.hpp:Workers`의 소켓 인자·`TransportError`·`network_failed` | 제거 |
| `wire.hpp`의 `Socket`·`connect_to`·`transfer`·`send_packet`·`receive_*` | 삭제 |
| `state_messages.cpp:add_diagnostics`의 `status_publish_roundtrip`, 하드웨어 문자열 | 명령 나이 p50/p99·publish 소요로 교체, 문자열 "Windows relay" |
| `/ets2/sensors/config`의 `wire_version`(`state_source.cpp`) | 삭제, `msgs_version` 유지 |
| `ros2/src/ets2_bridge` | 삭제. `config/fastdds.xml`·`config/foxglove.yaml`은 `bridge/config/`로. launch 파일은 CLI 편의용으로 `bridge/config/foxglove.launch.py`에 선택 보존 |
| `bridge.local.json`의 `token`·`state_port`·`bulk_port`·배포판 이름·`bind_address`, `configure.py` 기본값 | 제거 |
| `tests/wire_test.cpp` | `msgs_version` 대조만 `topics_test.cpp`로. `tests/CMakeLists.txt`의 WIN32 차단 제거, MSVC는 sanitizer 생략 |

## 4. 빌드 배치: 릴레이를 colcon 패키지로

`ros2/src/ets2_relay/`를 ament_cmake 패키지로 두고 소스는 `bridge/windows`에 그대로 둔다(✔ `ets2_bridge/CMakeLists.txt`가 `../../../bridge/common`을 참조하는 방식). `ets2_msgs` 의존을 colcon이 순서대로 해결하고 빌드가 `pixi run build` 하나가 된다. `bridge/CMakeLists.txt`·`bridge/build.cmd`는 삭제. Linux 분기는 두지 않는다(Linux 폐기).

```cmake
cmake_minimum_required(VERSION 3.25)
project(ets2_relay LANGUAGES CXX)
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_srvs REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(ets2_msgs REQUIRED)
find_package(fastcdr REQUIRED)
find_package(nlohmann_json REQUIRED)
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../bridge/windows)
add_executable(ets2_relay ${SRC}/main.cpp ${SRC}/ros_endpoint.cpp ${SRC}/overlay_writer.cpp ${SRC}/game_ipc.cpp ${SRC}/vehicle_profile.cpp
  ${SRC}/motion_sensors.cpp ${SRC}/state_source.cpp ${SRC}/render_capture.cpp
  ${SRC}/state_messages.cpp ${SRC}/image_messages.cpp ${SRC}/gpu_readback.cpp)
target_compile_features(ets2_relay PRIVATE cxx_std_20)
target_include_directories(ets2_relay PRIVATE ${SRC}/../common ${SRC}/../../ot/vendor ${SRC}/../../ot/include)
target_compile_definitions(ets2_relay PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(ets2_relay PRIVATE /W4 /EHsc /utf-8 /permissive-)
target_link_libraries(ets2_relay PRIVATE rclcpp::rclcpp ${std_srvs_TARGETS} ${nav_msgs_TARGETS} ${ets2_msgs_TARGETS}
  fastcdr nlohmann_json::nlohmann_json ws2_32 windowscodecs ole32 d3d11 dxgi)
install(TARGETS ets2_relay DESTINATION lib/${PROJECT_NAME})
ament_package()
```

- △ `fastcdr` 타깃 이름은 환경의 Fast-CDR 2.x config로 확인. 현재 `bridge/CMakeLists.txt`의 FetchContent 고정 커밋은 제거. ✔ `ros_cdr.hpp`는 2.x API만 쓴다.
- MSVC 런타임: ✔ 현재 `MultiThreaded`(정적)인데 rclcpp DLL과 섞이면 CRT 불일치. **동적 CRT(기본값)**. 게임 DLL(`ot/`)은 별도 빌드라 무관.
- 실행 파일 위치가 `ros2/install/lib/ets2_relay/ets2_relay.exe`로 바뀌므로 런처·`launch.cmd`의 `bridge/dist` 참조를 바꾼다.
- `ets2_autonomy/src/gt_drive_node.cpp:main`: `<unistd.h>`·`getpid()` → `#ifdef _WIN32 <process.h> _getpid()`. `--stop-event <name>` 인자 추가(§6).

## 5. 런처(`apps/launcher/launcher.py`) 변경

실행 Python은 pixi 환경(3.12). `launch.cmd`는 `pixi run launcher`, `--host-only`는 `pixi run relay`. `ot/.venv` 분기 삭제. `otpy.client`는 ✔ ctypes만 쓰므로 그대로. **rclpy를 쓰지 않는다.**

| 현재 | 변경 |
| --- | --- |
| 이전 세션 실행·경로 변환 헬퍼 | 삭제 |
| `Controller._start`: 이전 세션 spawn | `ros2 run foxglove_bridge foxglove_bridge --ros-args --params-file bridge/config/foxglove.yaml -p address:=127.0.0.1`을 Job에 spawn → 즉시 `_start_relay()`. 준비 판정은 `127.0.0.1:8765` 소켓 연결 성공 |
| 세션 신호·heartbeat | 삭제 |
| `_drive`: 이전 세션에 위임 | `ros2 run ets2_autonomy drive_speed --ros-args -p arm:=true -p path_file:=… -p acc_enabled:=… -p lcc_enabled:=… -p target_speed_mps:=…`(✔ `ros_session.py:run_session`의 인자)를 Job에 spawn, `--stop-event` 전달. 모드 변경은 `ros2 service call /ets2_drive_speed/set_parameters_atomically rcl_interfaces/srv/SetParametersAtomically` 한 번으로 세 값을 원자적으로(✔ 기존 `ros_session.py`의 `SetParametersAtomically`와 같은 의미; `param set` 3회는 중간 상태가 생기고 Windows에서 CLI 시작이 3번 든다), 실패는 로그 |
| `_record` | `BagRecording`을 런처 안에서 직접 사용(§7) |
| `_stop`: systemd 정지·확인 | 삭제. stop event set → 각 프로세스 wait → Job close |
| `_observe`: `session_status` 해석, `observed_drive` | 릴레이 stdout JSON만. 릴레이 1 Hz JSON에 `drive.armed`·`axes`·`reason`을 추가해(✔ telemetry의 `drive`, `state_messages.cpp:state_messages`가 이미 읽음) 그 값으로 ACC/LCC 실제 상태 표시. "연결됨" 판정은 `now-last_relay<3` |
| 이전 ROS 세션 상태 행 | "Foxglove 서버"(PID·포트) |

삭제 파일: 이전 세션 관리(`session.py`·`ros_session.py`·`processes.py`), Linux 진입점(`linux_launcher.py`·`apps/launch.sh`), 셸 환경 스크립트.

`launch.cmd` 최종 형태:

```bat
@echo off
pushd "%~dp0.."
if "%~1"=="--host-only" (pixi run relay %2) else (pixi run launcher %*)
popd
```

## 6. 종료 프로토콜

런처가 띄우는 자체 프로세스는 모두 이름 있는 stop event로 끝낸다(✔ 릴레이는 이미 `main.cpp:stop_watch`가 `OpenEventA`로 기다림).

| 프로세스 | 종료 | 비고 |
| --- | --- | --- |
| `ets2_relay` | stop event(기존) | lease 해제·Tier 0 |
| `drive_speed`, `drive_hold`, `ets2_stack` | `--stop-event <name>`: 이벤트 신호 → `stopped=true` → 루프 종료 → `control(false)`(disarm) | ✔ 지금 GT 노드는 SIGINT만 보므로 Windows에서 kill되면 `disarmed`가 아니라 `expired`로 끝난다. [21](21_safety_checklist.md) 참고 |
| `foxglove_bridge` | Job close(kill) | 상태 없음 |
| `ros2 bag record` | 자기 콘솔(`CREATE_NEW_CONSOLE`, 창 숨김)로 띄우고, 종료 때 보조 프로세스가 `AttachConsole(pid)`+`GenerateConsoleCtrlEvent(CTRL_C_EVENT,0)` | ✔ P0 §6: 이 방식만 `metadata.yaml`을 남김(CTRL_BREAK·kill은 실패) |

## 7. P3를 위한 릴레이 몫

- 자세 이력 링: state tx worker가 `source.latest()` 표본마다 (`paused_simulation_time_us`, `engine.vehicle.pose_physics`)를 64개 링에 넣는다(✔ 표본은 tier 1에서 `engine`을 포함).
- `/ets2/overlay/path`(`base_link`+stamp) 구독 → stamp 이하 가장 가까운 자세로 world 변환(✔ `geometry.hpp:world_base_pose`의 역변환) → `overlay_writer.cpp`가 공유메모리 `Local\OT_Overlay`에 generation 카운터로 기록([24 §4](24_hud_overlay_design.md)). 500 ms보다 오래된 stamp면 `valid=false`.
- overlay 활성 중에는 `render_capture.cpp:RenderCapture::reconcile`이 `render_probe` `overlay` 모드를 요청한다(센서 출력이 없어도).

## 8. `bag_recording.py` 이식

- POSIX 그룹 신호(`BagRecording.stop`) → 자기 콘솔로 띄운 기록기에 `AttachConsole`+`CTRL_C_EVENT`(✔ P0 §6). 보조 프로세스는 `FreeConsole`→`AttachConsole`→`SetConsoleCtrlHandler(NULL,TRUE)`→`GenerateConsoleCtrlEvent(CTRL_C_EVENT,0)` 순서로 자기 자신은 끝나지 않게 한다. 경로 변환 삭제, 경로는 Windows만. 8 s 뒤 `SIGKILL`(`BagRecording.poll`) → `process.kill()`.

## 9. 완료 기준과 회귀 시험

1. `pixi run build` 한 번으로 `ets2_msgs`·`ets2_relay`·`ets2_sensor_degradation`·`ets2_autonomy` 빌드.
2. 런처 시작 → Foxglove 수신(JPEG 4·표시용 점군 3·TF·`/diagnostics`)·전체 점군 약 10 Hz.
3. GT baseline 회귀(W00 선행): 입력·상태만 → GT 주행 30 km/h → 약 453 m, 중심 오차 0.03–0.57 m.
4. [21](21_safety_checklist.md) #6·#7 재수행.
5. 중지·창 닫기·릴레이 강제 종료에서 lease 해제·Tier 0(✔ 런처 `_stop`의 5.1 s 대기 유지).
6. 진단에 명령 나이 p50/p99가 나오고 p99 < 20 ms(△ 목표치, BELOW_NORMAL 우선순위에서 측정).
7. ✔ 코드에서 `git grep -n -E "wsl|eth0|systemd|OTR1|wire_version"` 0건(문서는 기록용 언급만).
