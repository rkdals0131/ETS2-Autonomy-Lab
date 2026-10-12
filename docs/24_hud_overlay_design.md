# HUD 궤적 데칼 설계 (P3)

[포팅 계획](20_windows_native_port_plan.md) P3의 상세입니다. 메인 시점의 도로 위에 계획 궤적을 반투명하게 그립니다. 표기 ✔/△는 계획과 같고, 메인 그래프 사실은 2026-10-12 읽기 전용 실측([20 §0 A4](20_windows_native_port_plan.md#0-2026-10-12-읽기-전용-실측))입니다. 코드 인용은 `파일:심볼`입니다.

## 1. 메인 프레임 구조(실측)

| 항목 | 값 |
| --- | --- |
| 메인 namespace | ✔ `main0`. 이미지 `main0/attributes_0..3`, `main0/depth_stencil`, `main0/composition_raw`, `main0/composition_out`. 백버퍼는 namespace 없는 `backbuffer` |
| 외부 pass | ✔ `scene defattr`, `scene defattrcu`, `scene lightsrc`, `scene defpost`, `scene decalpost`, `forward_scene`. viewport_depth [0.01, 0.9], P[10],P[11] = −1.00025, −0.40005 |
| 실내 pass | ✔ `interior defattr`, `interior defpost`, `interior decalpost`, `forward_interior`. viewport_depth [0.9, 1.0], P[10],P[11] = −1.01342, −0.20134 |
| 하늘 | ✔ `skybox` [0.0, 0.009] |
| 실행 순서 | ✔ … → `def.tonemap.drr.rfx`(→ `main0/composition_out`) → `blit_to_target`(→ `backbuffer`) → `content`/`ui` → `imgui` → present |
| 해상도 | ✔ 현재 viewport 1920×1080(`r_scale 1`). RenderDoc 캡처(`research/live/2026-10-08-renderdoc`)의 3840×2160은 당시 설정 |
| 그래프 manager | ✔ 3개가 번갈아 채워짐. △ 구축과 실행이 겹칠 수 있음 |

가림: reversed-Z이므로 실내 픽셀의 DSV 값(≥ 0.9)은 외부(≤ 0.9)보다 항상 크다. **이번 프레임 외부 pass의 `viewport_depth` [near, far] 밖이면 버리는 비교 하나**로 대시보드·A필러·실내 미러 가림이 해결된다. 하늘은 0이 아니라 [0, 0.009]이므로 하한도 pass 값으로 판정한다.

## 2. 삽입 지점

**`blit_to_target` 직전, 대상은 그 앞에 바인드된 `main0/composition_out`(LDR, 톤매핑 결과).** 데칼은 UI 아래, 노출·TAA 뒤에 놓인다.

판정(둘 다 구현해 비교, 기본은 (b)):
- (a) OM 훅(`render_probe.cpp:RenderProbe::observe`)에서 다음 RTV 리소스의 `IDXGIResource::GetUsage() & DXGI_USAGE_BACK_BUFFER`(리소스 포인터별 캐시).
- (b) compiled-span lookup으로 지금 실행 중인 pass 이름이 `blit_to_target`인지.
- 탈락: "깊이 없는 단일 RTV 첫 바인드" 휴리스틱(장면에 따라 깨짐), composition 전 삽입(노출·TAA 영향), Present 훅(✔ Present 반환 지점이라 다음 프레임에 그려지고 UI 위에 올라감).

색: (1,1,1), 알파 0.18–0.25, 가장자리 smoothstep 0.15 m.

## 3. 메인 카메라: 실행 중인 프레임의 것

- ✔ `pass_commands.cpp:PassCommands::describe`는 출력 namespace가 `mirrorN`일 때만 pass를 표본화한다. `main0`(외부 `scene defattr`)와 `backbuffer`(`blit_to_target`)도 이름을 붙이되, main 경로는 **camera만** 읽는다(`camera_at_compile`만; vehicles·진단 문자열·blocks 없음).
- ✔ 현재 gating: `observe()`는 stream이 돌고 pending이 아니면 첫 줄에서 반환하고, `compile_begin_callback`은 mask=0이면 건너뛰며, lookup은 stream이 없거나 compiling일 때만 한다. 데칼은 매 프레임 main0 카메라가 필요하므로 **`overlay` 모드 전용 gating**을 둔다(§7).
- 카메라 선택: "마지막으로 compile된 main0 카메라"가 아니라, 실행 중 `blit_to_target` span의 graph index(manager 0–2)와 같은 index에서 compile된 `scene defattr` 표본. `PassCommands`에 `main_camera_[3]`(graph별 최신 `RenderCameraSample`+`viewport_depth`)을 둔다. △ 파이프라이닝이 없으면 둘은 같다.
- 비용: ✔ [18](18_performance.md) compile begin 누적 2.43 s/60 s(GT 차량 관측 포함)가 기준. main 경로는 namespace 필터 뒤 camera 읽기만이라 그보다 훨씬 작아야 한다. 완료 기준에 **렌더 스레드 CPU 0.1 ms/프레임** 예산(✔ 게임이 CPU 바운드: CPU busy 15.06 ms, GPU 6.29 ms).

## 4. 데이터 경로

```text
스택: /ets2/overlay/path (nav_msgs/Path, frame_id=base_link, header.stamp=센서 stamp, 10–20 Hz, ≤256점)
  → 릴레이 노드 구독([23 §7](23_relay_node_design.md)): 자기 SDK 자세 이력으로 base_link→world 변환
  → 공유메모리 Local\OT_Overlay (generation 카운터)
  → core 렌더 스레드: generation이 바뀌면 복사 → BEV 마스크 래스터 → 이번 프레임 카메라로 데칼
pipe {"cmd":"overlay","action":"enable|disable|status"}   ← 제어만
```

- 좌표 변환은 **릴레이**가 한다. ✔ 릴레이 표본은 `engine.vehicle.pose_physics`를 가지며(`state_messages.cpp:state_messages`가 GT TF를 이것으로 만든다) 스택은 GT를 모른다(G2). core에는 자세 이력을 두지 않는다(게임 DLL 변경 최소화, [01 P6](01_requirements_and_decisions.md)). `frame_id=world`도 받는다(평가용 provider JSON 주입).
- 전달이 pipe가 아닌 이유: ✔ pipe는 단일 인스턴스(`transport.cpp:Transport::serve`의 `CreateNamedPipeW` nMaxInstances=1)이고 요청마다 새로 연결하며(`game_ipc.cpp:command`) 50 Hz 운전 명령·lease heartbeat와 같이 쓴다. 표시 데이터가 안전 명령 채널을 공유하면 안 된다.

공유메모리 블록(`ot/native/ipc_layout.hpp`에 추가, ABI 1):

```cpp
struct alignas(64) OverlayBlock {
    char magic[8];                 // "OTOVL01\0"
    uint32_t abi, point_count;     // ≤ 256
    volatile LONG64 generation;    // 쓰는 동안 홀수, 완료 후 짝수(seqlock)
    uint64_t stamp_us, ttl_ms;     // 센서 stamp, 이 시간 지나면 숨김
    float width_m, rgba[4];
    uint8_t valid, reserved[31];
    double points[256][2];         // SCS world X, Z (double: 100 km에서도 mm 정밀도)
};
```

core가 overlay enable 때 매핑을 만들고 릴레이가 연다(✔ `OT_State`와 같은 방식, `transport.cpp`). 읽기는 generation을 앞뒤로 비교해 일관 복사.

## 5. 데칼 렌더링

**기본안: BEV 마스크 텍스처 한 번 샘플.** 경로가 바뀔 때(≤ 20 Hz) 폴리라인을 자차 주변 128 m×128 m, 0.25 m 격자(512×512, R8G8: 알파·정규화 호 길이)로 CPU에서 래스터라이즈해 `UpdateSubresource`. 데칼 픽셀 셰이더는 복원한 world 위치로 그 텍스처를 한 번 샘플한다(O(1)). 모양·색·페이드는 마스크에 구워 넣으므로 셰이더를 고칠 일이 없다. 비교안(픽셀당 세그먼트 루프)은 구현하지 않는다.

상수 버퍼(프레임마다): `inv_projection`(외부 pass projection 역행렬), `view_to_world_rotation`, 카메라 world 위치(double로 마스크 원점과 빼서 float), `depth_range`(외부 pass `viewport_depth`), `mask_origin_xy`, `mask_scale`, `ground_z`, `z_tolerance` 1.0 m, `slope_tolerance` tan 8°, `rgba`.

픽셀 셰이더:
1. `d = depth.Load(pixel)`; `d < near || d > far`(외부 범위 밖: 실내·하늘)면 discard.
2. NDC(x,y,d) → `inv_projection` → 뷰 → world 상대 위치 `p`(✔ 깊이 복원 식은 `gpu_pack.cpp`의 라이다 깊이 셰이더와 같다).
3. 노면 판정: `|p.z - ground_z| < z_tolerance + |p.xy|·slope_tolerance`이고 `ddx/ddy` 법선의 z > 0.7. 실패면 discard. **앞차 지붕·벽·미러 하우징은 이 판정이 막는다(셰이더 책임, 스택은 경로를 자르지 않음).**
4. `mask.Sample(p.xy)` → 알파·호 길이 → 거리 페이드 → `float4(rgb, alpha)`, 블렌드 SRC_ALPHA/INV_SRC_ALPHA.

깊이 SRV: ✔ `gpu_capture.cpp:GpuCapture::geometry_constants`가 같은 DSV 리소스를 얻는다. `BindFlags`에 `SHADER_RESOURCE`가 있으면 `R32_FLOAT_X8X24_TYPELESS` SRV 직접, 없으면 ✔ `GpuPack::depth`처럼 `CopyResource`(1920×1080×8 B). 그리는 동안 DSV는 바인드하지 않는다.

드로우 범위: 폴리라인 화면 투영 AABB로 시저 제한.

**GPU 상태 보존(하나로 통일): deferred context + `ExecuteCommandList(list, RestoreContextState=TRUE)`.** 명령 리스트는 리소스(RTV·DSV·마스크·상수 버퍼) 포인터나 크기가 바뀔 때만 재기록하고, 매 프레임은 즉시 컨텍스트에서 `UpdateSubresource`(상수·마스크, 파이프라인 상태를 바꾸지 않음) 뒤 같은 리스트를 재실행한다. 기존 `gpu_pack.cpp:ComputeState`(compute 상태 저장·복원)는 draw 상태를 덮지 않으므로 쓰지 않는다.

셰이더 소스는 ✔ 기존 compute 셰이더처럼 문자열 내장 `D3DCompile`.

## 6. 변경 파일

| 파일 | 내용 |
| --- | --- |
| `ot/schema/1.61.1.1/fields.json` | `render_graph.main_namespace="main0"`, `backbuffer_namespace=""`, pass 이름 `scene defattr`·`blit_to_target` |
| `ot/native/pass_commands.*` | main0·backbuffer pass 이름 짓기(camera만), `main_camera_[3]` |
| `ot/native/ipc_layout.hpp` | `OverlayBlock` |
| `ot/native/overlay_decal.{hpp,cpp}`(새) | 셰이더·상수·마스크 래스터·deferred context·SRV, `draw(context, rtv, camera, depth_resource)` |
| `ot/native/render_probe.*` | `overlay` 모드(OM+compile 훅만, rig·stream 없음), `observe()`의 블릿 직전 판정 → `overlay_->draw` |
| `ot/native/core.cpp` | 명령 `overlay`(enable/disable/status), 매핑 생성, `version.overlay=true`(✔ 지금 `Runtime::command`의 version 응답은 false) |
| `bridge/windows/ros_endpoint.cpp`, `overlay_writer.cpp` | 구독, 자세 이력 변환, 공유메모리 쓰기 |
| `bridge/windows/render_capture.cpp` | overlay 활성 중 `overlay` 모드 요청 |
| `ot/py/otpy/__main__.py` | `overlay` 서브커맨드(파일 주입: lease·overlay 모드·공유메모리 쓰기, clear, status) |
| `docs/12` | 메인 그래프 사실 추가 |

## 7. `overlay` 모드 gating

- `RenderProbe::enable(mode="overlay")`: `hook_`(OM)·`compile_begin_hook_`·`compile_end_hook_`만 켠다. rig·stream·draw_batch 훅은 끄고 `vehicle_metadata=false`.
- `compile_begin_callback`: overlay 모드에서는 stream mask와 무관하게 `pass_commands_.begin(..., camera_only=true)`로 main0만 표본화.
- `observe()`: overlay 모드에서는 stream 조기 반환 없이 블릿 직전 판정만 수행.
- observe 모드(센서 수집)와 동시 사용: 릴레이가 센서 출력과 overlay를 모두 원하면 observe 모드에 `overlay=true` 옵션을 더해 같은 훅을 공유한다. 두 모드는 `camera_only` main 표본화와 블릿 판정만 공통.

## 8. 시험 계획

| 단계 | 방법 | 통과 기준 |
| --- | --- | --- |
| T1 pass 이름·카메라 | `ot.cmd hooks`에서 main0 `scene defattr` 카메라와 `viewport_depth`, `blit_to_target` span 관측 | 매 프레임 갱신, 값이 §1과 일치 |
| T2 정적 데칼 | `ot.cmd overlay --file bridge/recordings/gt-path/current-lane.json`(world) | 정차에서 차로 중심에 붙음. 대시보드·A필러·미러에 가려짐. 센서 pass 영상에 없음 |
| T3 주행 안정성 | 80 km/h 직선, core 자체 백버퍼 캡처 진단 명령(새 `overlay capture`)으로 연속 2프레임 저장 | 궤적 끝점 화면 위치 차 ≤ 1 px. ✔ RenderDoc은 `RenderProbe::enable`이 거절하므로 쓰지 않음 |
| T4 비용 | [18](18_performance.md) `display` 조건에서 데칼 on/off 60 s, PresentMon | frame time p99 증가 ≤ 0.5 ms, 렌더 스레드 콜백 timing(`hooks` 응답) 0.1 ms/프레임 이내 |
| T5 base_link 프레임 | `ot.cmd overlay --frame base_link`로 자차 기준 직선 주입(릴레이 경유) | 커브에서 자차 뒤에 끌리지 않음 |
| T6 해상도·창 전환 | 창/전체화면, `r_scale` 변경 | 다음 프레임부터 정상, 크래시 없음 |

## 9. 리스크

| 리스크 | 대응 |
| --- | --- |
| 블릿 판정 오탐 | (a)·(b) 둘 다 구현해 비교. 판정 실패 프레임은 그리지 않음 |
| DSV에 SHADER_RESOURCE 없음 | CopyResource 경로 |
| manager 파이프라이닝으로 카메라 한 프레임 어긋남 | graph index 매칭(§3). T3에서 확인 |
| 렌더 스레드 CPU 초과 | camera만 읽기, 마스크 래스터는 CPU worker, 상수 갱신만 렌더 스레드 |
| 게임 업데이트 | 모든 RVA와 함께 무효. W01 |
