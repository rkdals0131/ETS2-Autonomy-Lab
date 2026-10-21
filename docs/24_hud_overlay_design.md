# HUD 궤적 데칼 설계 (P3)

[포팅 계획](20_windows_native_port_plan.md) P3의 상세입니다. 메인 시점의 도로 위에 계획 궤적을 반투명하게 그립니다. 표기 ✔/△는 계획과 같고, 메인 그래프 사실은 2026-10-12 읽기 전용 실측([20 §0 A4](20_windows_native_port_plan.md#0-2026-10-12-읽기-전용-실측))입니다. 코드 인용은 `파일:심볼`입니다.

## 0. 구현 상태

✔ 2026-10-20 구현(core 0.43.0·릴레이·otpy). 같은 날 밤 게임 첫 시험에서 데칼이 그려지지 않아 core 0.44.0에서 깊이 버퍼를 잡는 방법을 바꿨습니다(아래 "게임 첫 시험"). 게임 1.61.1.9 schema(`ot/schema/1.61.1.9/fields.json`)에 맞췄습니다. 아래 본문의 ✔는 설계 근거이고, 구현이 게임에서 맞게 동작하는지는 §8 T1–T6이 끝날 때까지 △입니다.

| 부분 | 구현 | 확인 |
| --- | --- | --- |
| core 명령 | `overlay` `enable`(lease, Tier 1, `allow_render_probe`)·`disable`·`status`·`capture`(진단, lease 불필요). `render_probe`의 `mode: "overlay"`와 observe `overlay: true`. `version.overlay=true`, `overlay_allowed`, capability `overlay` | 빌드·경고 없음. △ 게임 |
| 공유메모리 | `ipc_layout.hpp:OverlayBlock`·`overlay_write`·`overlay_read`(seqlock). core가 enable 때 `Transport::overlay_block`으로 만들고, 쓰는 쪽이 열어 둔 기존 매핑은 내용째 다시 씀 | ✔ `ot_overlay_test`: 동시 writer 2만 번에서 찢어진 경로 0건 |
| pass 이름 | `pass_commands.cpp:PassCommands::describe`가 main 경로에서 `main0/attributes_0`(이름 `scene defattr`), `main0/composition_out`, 무 namespace `backbuffer`(이름 `blit_to_target`)를 camera만 읽어 표본화. graph별 최신 scene 표본 `main_scene_[3]`, 블릿 표본은 같은 graph의 scene 표본을 품음. main span만 따로 `main_at`(shared lock) | △ T1 |
| 블릿 판정 | composition_out pass의 bind 다음 첫 단일 RT bind에서 (b) compiled span과 (a) `DXGI_USAGE_BACK_BUFFER`를 함께 계산해 `overlay.blit_detection`에 집계. 기본 (b), `detector: "backbuffer_usage"`로 (a) | △ T1·T2 |
| 데칼 | `overlay_decal.{hpp,cpp,hlsl}`: 마스크 worker(200 Hz generation 확인, 바뀔 때만 래스터), 깊이 SRV 또는 CopyResource, deferred context 명령 리스트를 `ExecuteCommandList(list, TRUE)`로 재실행 | ✔ 마스크·affine·투영 단위 시험. △ T2–T6 |
| 릴레이 | `ros_endpoint.cpp` `/ets2/overlay/path` 구독(best effort), `overlay_writer.cpp` 자세 이력·변환·기록, `render_capture.cpp` overlay 요청 | ✔ `overlay_writer_test` 5건, `ros_endpoint_test` 구독 경로. △ T5 |
| otpy | `ot.cmd overlay show --file …`, `clear`, `status`, `capture` | ✔ ot_ipc.dll 기록을 별도 이름 매핑으로 확인. △ T2 |

설계와 다르게 정한 점:
- 셰이더는 `D3DCompile`이 아니라 기존 compute 셰이더처럼 빌드 때 fxc(`vs_5_0`·`ps_5_0`)로 헤더에 넣습니다. 기존 셰이더(`pack_rgbd.hlsl` 등)가 실제로 CMake fxc 방식이라 §5의 "문자열 내장 `D3DCompile`" ✔는 사실과 달랐습니다. 게임 안 런타임 컴파일 비용과 d3dcompiler 의존이 없습니다.
- 시저: 명령 리스트를 고정하려고 RS 시저 대신 VS가 상수 버퍼의 NDC 사각형만 덮는 4정점 strip을 그립니다. 래스터 범위는 같습니다. 사각형은 경로 점(최대 64개 표본)의 폭·높이 허용 상자를 투영하고, 카메라 뒤 점은 near plane에서 자릅니다.
- `OverlayBlock`의 reserved 일부를 `double ground_y`(경로 시작점 노면의 SCS Y, NaN이면 높이 판정 생략)로 씁니다. §5의 `ground_z`를 core가 자세 없이 얻는 방법입니다. 끝에 명시 padding 32 B를 두어 크기는 4224 B입니다.
- 마스크 축은 시작점에서 마스크 길이 안의 가장 먼 점으로 향하는 chord, 횡 중심은 그 범위 경로의 중앙, 뒤쪽 여유 4 m입니다. 반경 500 m 곡선이 32 m 폭에 들어갑니다(반경 60 m U턴은 `clipped`). 호 길이 페이드(기본 80→120 m)는 R에 굽고 G는 호 길이/160 m입니다.
- composition_out RTV는 그 pass의 bind에서 graph별로 잡습니다. DSV는 처음에 `scene defattr` bind에서 잡았으나 1.61.1.9에서는 그 span에 bind가 하나도 들어오지 않아(아래 "게임 첫 시험"), 0.44.0부터 composition_out bind 직전에 마지막으로 bind된 composition 크기의 DSV를 씁니다. 블릿 bind 때 바인드된 RTV를 묻지 않습니다(tonemap 끝에 unbind되면 비어 있음).
- 메인 카메라에 projection modifier가 있어도 그립니다(해석하지 않은 값이 sub-pixel jitter일 것으로 봄, `last_draw.projection_modifier_flag`로 보고).
- 릴레이 자세 이력은 64개가 아니라 128개입니다(SDK 프레임율이 렌더보다 높을 때 500 ms를 덮도록). stamp가 0인 경로는 최신 자세로 바꿉니다(수동 `ros2 topic pub` 시험용).

### 게임 첫 시험 (2026-10-20 밤, core 0.43.0 → 0.44.0)

장소는 회사 마당 생울타리 앞 정차(지도 provider가 지원하지 않는 도로), 실내 시점, 1920×1080입니다. provider 경로를 만들 수 없어 트럭 진행 방향 직선 경로와 왼쪽으로 꺾는 경로를 임시로 만들어 썼습니다.

- T1: ✔ `scene defattr`·`def.tonemap.drr.rfx`·`blit_to_target` 모두 매 프레임 compile되고, `scene_cameras`의 `viewport_depth` [0.01, 0.9]·P[10],P[11] = −1.00025, −0.40005가 §1과 같습니다. `blit_detection`은 618회 모두 `both`(`neither`·`*_only` 0)입니다.
- 0.43.0에서 데칼이 그려지지 않았습니다: `skips.depth`가 매 프레임. `binds.scene` 0. 진단 core로 세어 보니 overlay hook을 지나는 bind는 프레임당 타깃 0개 5·1개 약 41·2개 5·4개 5·6개 1회이고 DSV는 약 30회(1920×1080, `DXGI_FORMAT_D32_FLOAT_S8X24_UINT`)였습니다. 토큰 인덱스와 무관하게 `scene defattr` span 안의 bind는 0회였습니다. 원인(실행 경로가 달라졌는지, span 토큰이 다른지)은 확인하지 않았습니다.
- 0.44.0: composition_out bind 직전 마지막 composition 크기 DSV를 메인 깊이로 씁니다(외부·실내 pass가 viewport 깊이를 나눠 같은 깊이를 씀). 6,514프레임 모두 그려졌고 `skips.depth` 0입니다. 캡처(`overlay capture --png`)에서 트럭 앞 왼쪽 포장면에 띠가 깔리고 A필러·핸들·대시보드에 가려집니다.
- △ 디더링 투명(나뭇잎·생울타리) 픽셀에 점처럼 묻습니다. 그 픽셀의 색은 잎인데 깊이는 뒤 지면이라 마스크 안으로 판정되는 것으로 봅니다(추정).
- △ T4 렌더 스레드 콜백은 프레임당 약 0.22 ms로 기준(0.1 ms)을 넘습니다. 히스토그램상 거의 전부 프레임당 한 번의 데칼 그리기(깊이 CopyResource와 `ExecuteCommandList(list, TRUE)`의 상태 복원)이고, bind 처리 자체는 1 µs 안팎입니다.
- ✘ 0.44.0 첫 빌드는 진단을 넓히면서 타깃 수 상한을 8로 올리고 타깃 배열은 4칸으로 둬서 스택을 덮어썼고, 데칼을 켠 직후 게임이 종료됐습니다(19:45 `NONCONTINUABLE_EXCEPTION`). 배열을 8칸으로 고친 빌드부터 정상입니다.
- 남은 시험: 실제 차로에서 T2(차로 중심 정렬), T3(주행 중 1 px), T5, T6.

### 주행 중 사용 (2026-10-21, `drive_speed` HUD)

- 0.6 m 띠는 25 cm 마스크 텍셀로 계단처럼 보였습니다(사용자: "저해상도"). 0.44.0 셰이더는 마스크를 샘플하지 않고 경로 점(최대 256, 마스크 원점 기준 상대 좌표와 호 길이)을 상수 버퍼 b1로 받아 픽셀마다 폴리라인까지의 거리를 계산하고, 가장자리를 `fwidth` 한 픽셀로 부드럽게 합니다. 마스크는 원점·축·화면 범위 계산에만 남습니다. 사용자 확인: "이제 매끈함".
- 반투명(0.55) 띠가 젖은 노면 반사와 조명을 비쳐 빛나 보였습니다. 불투명도 0.85와 12 cm 어두운 테두리로 바꿨으나 사용자는 "그래도 빛나 보인다"고 했습니다. △ 원인 후보: composition_out 뒤 단계(블릿·업스케일 등)의 처리, 또는 장면 밝기 대비 LDR 색 자체. 확인하지 않았습니다.
- 2026-10-21 두 번째 주행 뒤 사용자: "여전히 너무 밝고 블렌딩되지 못함", "꺼질 때 1–2초 찌꺼기". 데칼은 tonemap 뒤 LDR 이미지(composition_out)에 그리므로, 평평한 색을 알파로 덮으면 그늘·밤·역광에서도 혼자 같은 밝기라 스티커처럼 뜹니다. core 0.45.0은 dual-source 블렌드(`target = add + target × multiply`)로 바꿨습니다. 칠의 세기 a, 색 c, 테두리를 뺀 안쪽에서 `multiply = 1 − a + a·2.2·c`, `add = a·0.06·c`입니다. 표시가 아래 노면색에 곱해지므로 장면의 조명·그늘을 따르고, 작은 발광분(0.06)이 불 없는 도로에서도 보이게 합니다. 테두리는 색 × 0.35로 어둡게 곱합니다. 잔상은 릴레이 TTL 1 s 때문이었습니다. 이제 TTL은 300 ms이고, 점 없는 경로는 바로 지웁니다(`drive_speed`는 그릴 것이 없을 때와 세션 끝에 보냄). △ 0.45.0은 게임에 교체만 했고(SDK 0.45.0 load 확인), 게임이 메인 메뉴라 화면 확인은 아직입니다.
- 표시 규칙(같은 날): LCC가 조향할 때만 차로 띠, ACC가 켜져 있을 때 선행차 뒤 가로줄, 선행차가 없으면 ACC가 지킬 간격 위치에 흐린 가로줄([브리지 문서](../bridge/README.md#hud-궤적-오버레이)). 예전의 "LCC 없으면 요레이트 예측 호" 띠는 없앴습니다(사용자: 의미 없음).

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

공유메모리 블록(`ot/native/ipc_layout.hpp`, ABI 1, 구현):

```cpp
struct alignas(64) OverlayBlock {
    char magic[8];                 // "OTOVL01\0"
    uint32_t abi, point_count;     // ≤ 256
    volatile LONG64 generation;    // 쓰는 동안 홀수, 완료 후 짝수(seqlock)
    uint64_t stamp_us, ttl_ms;     // 센서 stamp(SDK paused simulation time), 이 시간 지나면 숨김. ttl 0은 숨기지 않음
    float width_m, rgba[4];
    uint8_t valid, reserved0[3];
    double ground_y;               // 경로 시작점 노면의 SCS Y(릴레이: base_link 원점). NaN이면 높이 판정 생략
    uint8_t reserved[24];
    double points[256][2];         // SCS world X, Z (double: 100 km에서도 mm 정밀도)
    uint8_t tail[32];              // 64 B 정렬 padding, sizeof 4224
};
```

core가 overlay enable 때 매핑을 만들고 릴레이가 연다(✔ `OT_State`와 같은 방식, `transport.cpp`). 읽기는 generation을 앞뒤로 비교해 일관 복사. ROS world (x, y, z)는 SCS (X, −Z, Y)이므로 릴레이는 X = x, Z = −y로 적는다. TTL은 core가 SDK frame_end의 paused simulation time과 비교한다(정지 중에는 시간이 흐르지 않아 그대로 남음).

## 5. 데칼 렌더링

**기본안: BEV 마스크 텍스처 한 번 샘플.** 경로가 바뀔 때(≤ 20 Hz) 폴리라인을 **경로 방향으로 정렬한 직사각형**(경로 시작점 원점, 길이 160 m × 폭 32 m, 0.25 m 격자 = 640×128, R8G8: 알파·정규화 호 길이)에 CPU에서 래스터라이즈해 `UpdateSubresource`. world→마스크 uv는 2×3 affine으로 상수 버퍼에 싣는다. 자차 중심 정사각형(128 m×128 m)은 앞쪽이 64 m뿐이라 0–80 m 궤적과 120 m 페이드를 담지 못하고, 같은 범위를 정사각형으로 덮으면 텍셀이 6배다. 데칼 픽셀 셰이더는 복원한 world 위치로 그 텍스처를 한 번 샘플한다(O(1)). 모양·색·페이드는 마스크에 구워 넣으므로 셰이더를 고칠 일이 없다. 비교안(픽셀당 세그먼트 루프)은 구현하지 않는다.

상수 버퍼(프레임마다): `inv_projection`(외부 pass projection 역행렬), `view_to_world_rotation`, 카메라 world 위치(double로 마스크 원점과 빼서 float), `depth_range`(외부 pass `viewport_depth`), `mask_affine`(world→uv 2×3), `ground_z`, `z_tolerance` 1.0 m, `slope_tolerance` tan 8°, `rgba`.

픽셀 셰이더:
1. `d = depth.Load(pixel)`; `d < near || d > far`(외부 범위 밖: 실내·하늘)면 discard.
2. NDC(x,y,d) → `inv_projection` → 뷰 → world 상대 위치 `p`(✔ 깊이 복원 식은 `gpu_pack.cpp`의 라이다 깊이 셰이더와 같다).
3. 노면 판정: `|p.z - ground_z| < z_tolerance + |p.xy|·slope_tolerance`이고 `ddx/ddy` 법선의 z > 0.7. 실패면 discard. **앞차 지붕·벽·미러 하우징은 이 판정이 막는다(셰이더 책임, 스택은 경로를 자르지 않음).**
4. `mask.Sample(p.xy)` → 알파·호 길이 → 거리 페이드 → `float4(rgb, alpha)`, 블렌드 SRC_ALPHA/INV_SRC_ALPHA. (구현: 0.44.0부터 마스크 대신 경로 점까지의 거리, 0.45.0부터 블렌드는 dual-source 칠 — §0 "주행 중 사용".)

깊이 SRV: ✔ `gpu_capture.cpp:GpuCapture::geometry_constants`가 같은 DSV 리소스를 얻는다. `BindFlags`에 `SHADER_RESOURCE`가 있으면 `R32_FLOAT_X8X24_TYPELESS` SRV 직접, 없으면 ✔ `GpuPack::depth`처럼 `CopyResource`(1920×1080×8 B). 그리는 동안 DSV는 바인드하지 않는다.

드로우 범위: 폴리라인 화면 투영 AABB로 시저 제한.

**GPU 상태 보존(하나로 통일): deferred context + `ExecuteCommandList(list, RestoreContextState=TRUE)`.** 명령 리스트는 리소스(RTV·DSV·마스크·상수 버퍼) 포인터나 크기가 바뀔 때만 재기록하고, 매 프레임은 즉시 컨텍스트에서 `UpdateSubresource`(상수·마스크, 파이프라인 상태를 바꾸지 않음) 뒤 같은 리스트를 재실행한다. 기존 `gpu_pack.cpp:ComputeState`(compute 상태 저장·복원)는 draw 상태를 덮지 않으므로 쓰지 않는다.

셰이더 소스는 기존 compute 셰이더처럼 `.hlsl`을 빌드 때 fxc로 헤더에 넣는다(구현: `overlay_decal.hlsl`, `vs_main`·`ps_main`). 초안의 "✔ 문자열 내장 `D3DCompile`"은 기존 셰이더 방식을 잘못 적은 것이었다(§0).

## 6. 변경 파일

| 파일 | 내용 |
| --- | --- |
| `ot/schema/1.61.1.9/fields.json` | `render_graph.main_namespace="main0"`, `backbuffer_namespace=""`, pass 이름 `main_scene_pass="scene defattr"`·`blit_pass="blit_to_target"`, 이미지 `main_scene_image`·`main_color_image`·`backbuffer_image` (게임 업데이트로 1.61.1.1이 아니라 1.61.1.9) |
| `ot/native/pass_commands.*`, `render_sample.*` | main0·backbuffer pass 이름 짓기(camera만, `MainPass`), `main_scene_[3]`, `main_at`, `main_status` |
| `ot/native/ipc_layout.hpp`, `ipc_reader.cpp` | `OverlayBlock`·seqlock 함수, Python용 `ot_overlay_write`(ot_ipc.dll) |
| `ot/native/overlay_mask.hpp`(새) | 게임 없는 부분: 마스크 래스터·affine·카메라 투영/복원·화면 사각형 |
| `ot/native/overlay_decal.{hpp,cpp,hlsl}`(새) | 셰이더·상수·마스크 worker·deferred context·SRV/복사·`capture`, `draw(context, rtv, dsv, camera, sdk_time)` |
| `ot/native/render_probe.*` | `overlay` 모드(OM+compile 훅만, rig·stream 없음), `overlay_observe`의 블릿 직전 판정 → `decal_.draw` |
| `ot/native/core.cpp`, `transport.cpp` | 명령 `overlay`(enable/disable/status/capture), 매핑 생성, `version.overlay=true` |
| `ot/native/overlay_test.cpp`(새) | `ot_overlay_test` |
| `bridge/windows/ros_endpoint.cpp`, `overlay_writer.*`(새), `main.cpp` | 구독, 자세 이력 변환, 공유메모리 쓰기 |
| `bridge/windows/render_capture.cpp` | overlay 활성 중 `overlay enable`, 센서 수집이 끝나면 `overlay` 모드로 내려감 |
| `ros2/src/ets2_relay/test/overlay_writer_test.cpp`(새) | 자세 이력·변환·writer gtest |
| `ot/py/otpy/overlay.py`(새), `__main__.py` | `overlay` 서브커맨드(파일 주입: lease·overlay 모드·공유메모리 쓰기, clear, status, capture) |
| `docs/12` | 메인 그래프 사실 추가 |

## 7. `overlay` 모드 gating

- `RenderProbe::enable(mode="overlay")`: `hook_`(OM)·`compile_begin_hook_`·`compile_end_hook_`만 켠다. rig·stream·draw_batch 훅은 끄고 `vehicle_metadata=false`.
- `compile_begin_callback`: overlay 모드에서는 stream mask와 무관하게 `pass_commands_.begin(..., camera_only=true)`로 main0만 표본화.
- `observe()`: overlay 모드에서는 stream 조기 반환 없이 블릿 직전 판정만 수행.
- observe 모드(센서 수집)와 동시 사용: 릴레이가 센서 출력과 overlay를 모두 원하면 observe 모드에 `overlay=true` 옵션을 더해 같은 훅을 공유한다. 두 모드는 `camera_only` main 표본화와 블릿 판정만 공통.
- 구현: `RenderProbe::enable(…, overlay)`, `RenderProbe::overlay`. observe 중 `overlay enable`은 훅을 바꾸지 않고 플래그만 켠다. 꺼진 상태에서는 `overlay` 모드로 켠다. rig 모드는 거절. overlay 모드에서는 일반 bind 기록·stream·Present 기록을 하지 않고, main 표본은 mirror 이름 없이(`camera_mask=0`) 만든다. overlay가 꺼져 있으면 렌더 스레드 경로는 원자 변수 하나만 읽는다.

## 8. 시험 계획

| 단계 | 방법 | 통과 기준 |
| --- | --- | --- |
| T1 pass 이름·카메라 | T2 실행 중 다른 창에서 `ot.cmd overlay status`(또는 `ot.cmd hooks`의 `overlay`): `main_passes.scene_cameras`의 `viewport_depth`·`projection_row_major`, `main_passes.compiled`·`spans`, `blit_detection` | 매 프레임 갱신(compiled 수 증가), 값이 §1과 일치, `blit_detection.both`가 늘고 `neither`·`*_only`는 0에 가까움 |
| T2 정적 데칼 | `ot.cmd overlay --file bridge/recordings/gt-path/current-lane.json`(world) | 정차에서 차로 중심에 붙음. 대시보드·A필러·미러에 가려짐. 센서 pass 영상에 없음 |
| T3 주행 안정성 | 80 km/h 직선, core 자체 캡처 진단 명령 `ot.cmd overlay capture --png`로 연속 2프레임 저장(`main0/composition_out`, 데칼 직후·블릿 전) | 각 프레임 그림의 궤적 끝과 `marks`의 같은 프레임 카메라 투영 위치 차 ≤ 1 px. ✔ RenderDoc은 `RenderProbe::enable`이 거절하므로 쓰지 않음 |
| T4 비용 | [18](18_performance.md) `display` 조건에서 데칼 on/off 60 s, PresentMon | frame time p99 증가 ≤ 0.5 ms, 렌더 스레드 콜백 timing(`hooks` 응답) 0.1 ms/프레임 이내 |
| T5 base_link 프레임 | 릴레이 실행 중 `ros2 topic pub`으로 `base_link` 직선을 `/ets2/overlay/path`에 발행(stamp 0은 릴레이가 최신 자세로 변환, [브리지 문서](../bridge/README.md#hud-궤적-오버레이)) | 커브에서 자차 뒤에 끌리지 않음 |
| T6 해상도·창 전환 | 창/전체화면, `r_scale` 변경 | 다음 프레임부터 정상, 크래시 없음 |

## 9. 리스크

| 리스크 | 대응 |
| --- | --- |
| 블릿 판정 오탐 | (a)·(b) 둘 다 구현해 비교. 판정 실패 프레임은 그리지 않음 |
| DSV에 SHADER_RESOURCE 없음 | CopyResource 경로 |
| manager 파이프라이닝으로 카메라 한 프레임 어긋남 | graph index 매칭(§3). T3에서 확인 |
| 렌더 스레드 CPU 초과 | camera만 읽기, 마스크 래스터는 CPU worker, 상수 갱신만 렌더 스레드 |
| 게임 업데이트 | 모든 RVA와 함께 무효. W01 |
| 렌더러를 가로채는 모드(✔ snowymoon: AA·DLSS가 메인 후처리를 바꾸고 센서 pass와 충돌) | §1 구조는 순정 렌더 기준. 모드가 켜져 있으면 데칼을 그리지 않거나 지원 범위 밖으로 둔다([브리지 문서](../bridge/README.md#렌더-모드와의-충돌)) |
