# CPU 최적화 계획

2026-10-13. 근거 측정은 [18 CPU 병목 분석](18_performance.md#cpu-병목-분석)에 있습니다. 표시는 ✔ 확인, △ 추정입니다.

## 1. 전제

- ✔ ETS2는 메인 스레드 하나가 게임 로직·장면 준비·렌더 명령 컴파일을 맡고, 이 스레드가 병목입니다. 그 일을 다른 코어로 나누는 설정·옵션·모드는 없습니다. 컴파일된 D3D11 명령 버퍼의 실행은 엔진 렌더 스레드(`r_device_t::rendering_thread_loop`)가 맡고, 정차 시 약 0.2코어로 대부분 기다립니다(✔ 2026-10-14 스택, [27 §6](27_main_thread_parallel_research.md#6-m0-결과-정차브리지-끔)). DX12·Vulkan 렌더러(`-rdevice`)는 실험 단계이고, 우리 훅은 DX11 경로의 코드 주소에 걸려 있습니다.
- 따라서 멀티코어를 쓰는 방법은 세 가지뿐입니다.
  1. 메인 스레드에 올리는 일을 줄입니다: 센서 렌더 횟수, 뷰당 엔진 비용.
  2. 메인 스레드가 코어를 혼자 쓰게 합니다: CPU 배치, 뷰어 부하.
  3. 우리 일을 다른 스레드나 프로세스로 옮깁니다: JSON·GPU 후처리·자원 생성.
- 목표는 센서 주기 유지(라이다 10 Hz, 요청한 카메라 주기), frame time p99 감소, fps 증가 순입니다. 시뮬레이션은 렌더 fps와 관계없이 SDK 60 Hz로 돕니다. 따라서 fps는 사람이 보는 화면과 센서 지연의 지표입니다.
- 센서 출력 내용이 바뀌는 변경(렌더 설정, 수집 직전 렌더)은 영상·라이다 비교를 함께 합니다. 교통량·LOD는 인지 대상이므로 성능 측정 때만 고정하고 운영 기본값으로 낮추지 않습니다.

## 2. 현재 비용

| 항목 | 크기 | 근거 |
| --- | --- | --- |
| 플레이어 화면 + 게임 미러 0·1·2·5 | △ 렌더 프레임당 약 24–28 ms. 화면에 보이는 게임 미러는 매 프레임 장면 전체를 다시 렌더 | 브리지 끔 24.0 ms(거리 400) |
| 센서 뷰 렌더 | 초당 80회(뷰어만 구독). △ 1회 0.8–2 ms, 절반이 수집 직전 렌더 | `camera_rig.views[].applied` |
| Foxglove 뷰어와의 경쟁 | ✔ +16 ms/프레임, 배치 뒤 △ 약 +10 ms | 헤드리스 구독과 비교 |
| 우리 훅 본문 | ✔ 수집 프레임당 약 1.7 ms, 최대 한 번 600 ms | 0.31.1 `hooks` |
| SDK 콜백 | ✔ 초당 약 12 ms(Tier 0). Tier 1·2 미측정 | `sdk_timing` |

## 3. 작업

크기 S·M·L은 [20](20_windows_native_port_plan.md)과 같은 기준입니다. "게임"은 완료 확인에 게임이 필요하다는 뜻이고, 구현은 대부분 게임 없이 합니다.

### C0. 측정 기반 (S, 게임)

✔ 2026-10-13 구현(core 0.33.0, 게임 확인 전): `ot.cmd perf <label> [--profile-hz 250]`, `ot.cmd perf_table <json...>`, core `main_profile`·`traffic` 명령. 샘플러는 게임 없는 MSVC 시험(`ot_sampler_test`)에서 바쁜 스레드를 자기 이미지로 100% 귀속하고, 힙을 계속 잡는 스레드도 교착 없이 끝까지 샘플했습니다. 샘플당 정지는 평균 25 µs·최대 235 µs라 250 Hz면 메인 스레드에 약 0.6%입니다.

✔ 2026-10-14 core 0.34.0: `main_profile` 호출 스택(`depth`, 락 없는 자체 unwinder), perf의 게임 스레드별 CPU와 `<label>.stacks.json`, 함수별 집계 `ot.cmd hotspots`. 설계와 시험 결과는 [27 메인 스레드 병렬화 연구](27_main_thread_parallel_research.md#4-도구)에 있습니다.

✔ 2026-10-14 게임 확인(브리지 끔 정차, `off-long-1` 120 s): 52.7 fps, CPU busy 18.5 ms, GPU 9.0 ms. 메인 스레드 0.95코어, 모듈 비중 `eurotrucks2.exe` 83.8%·`d3d11` 1.3%·`nvwgf2umx` 0.7%·snowymoon `dxgi.dll` 프록시 3.8%·`ot_core` 0.9%. 엔진 worker는 각각 0.03코어 이하로 놉니다. 프레임 갱신·뷰 준비 약 8.9 ms, 렌더 제출 약 9 ms로 나뉩니다. 함수별 표는 [27 §6](27_main_thread_parallel_research.md#6-m0-결과-정차브리지-끔)에 있습니다. 센서 켬 분포는 아직입니다.

- 저장소 도구로 ABAB 측정을 합니다. 조건 A/B를 30초씩 3회 번갈아 재고 아래를 함께 기록합니다.
  - PresentMon fps·p50·p99·CPU/GPU busy
  - 센서 렌더/초, `hooks`·`sdk_timing` 증분, stream 카운터
  - 교통 차량 수, 뷰어 프로세스 CPU, 게임 창 전경 여부
- 메인 스레드 샘플러(`main_profile`): core 안의 worker가 메인 스레드를 잠깐 멈추고(`SuspendThread`·`GetThreadContext`) RIP의 모듈을 셉니다. `eurotrucks2.exe`·`d3d11.dll`·`nvwgf2umx.dll`·`ot_core.dll`·`ntdll`/`kernel*` 비중으로 엔진·D3D·드라이버·우리 코드를 나눕니다. 요청한 시간만 돌고 기본은 끔입니다. 관리자 권한이 있으면 WPR(ETW) 샘플링으로 교차 확인할 수 있습니다.
- 완료 기준: 같은 조건 ABAB 3회의 fps 편차 ±3% 이내. 메인 스레드 모듈별 비중 표(브리지 끔·센서 켬). Tier 1·2 `sdk_timing`.

### C1. 수집 직전 렌더 줄이기 (M–L, 연구, 게임)

- ✔ 10 Hz 수집 사이에 쉬는 프레임이 있으면 수집마다 수집 직전 렌더가 하나씩 붙어, 센서 렌더의 절반이 수집 직전 렌더입니다. 수집 직전 렌더 없이 수집하면 나무·가로등이 빠집니다([lessons](history/lessons.md#센서-렌더-생략과-정적-물체-깜빡임)).
- 비용 측정: 릴레이 설정 `capture_warmup:false`로 끈 상태와 A/B 합니다. 영상 결함은 측정 중에만 감수합니다.
- 원인 조사: △ 엔진이 뷰마다 앞 프레임의 가시성 결과를 다음 프레임에 쓰는 것으로 보입니다. 가시성 제출만 하고 drawable을 NULL로 비운 실험은 실패했습니다. 수집 직전 렌더에 어느 단계까지 필요한지 이분 탐색합니다(장면 준비만, G-buffer까지, viewport 축소). [12 렌더 경로](12_dx11_mirror_render_path.md)의 CPU 후보 선택 단계부터 봅니다.
- 완료 기준: 수집 직전 렌더의 메인 스레드 비용이 정식 렌더의 30% 이하이거나 0. 같은 pose 비교에서 나무 영역 녹색 비율·깊이 중앙값이 lessons의 기준과 같음.

### C2. 렌더 주기 정리 (S)

- ✔ 0.32.0: 자세(bit 32)만 남은 카메라는 미리보기 주기로만 렌더합니다. 센서 렌더 −33%.
- ✔ 0.33.0(게임 확인 전): GT 메타데이터(bit 16)도 자세처럼 다른 출력의 렌더에 얹고, 혼자면 미리보기 주기로 렌더합니다(`capture_stream.cpp` `CaptureStream::run`의 `rider_products`). 이전에는 카메라 GT만 구독해도 `camera_hz`(30 Hz)로 렌더했습니다. GT 주행 노드는 카메라 GT가 아니라 `/ets2/ground_truth/ego/pose`·`traffic`을 쓰므로 영향이 없습니다.
- 카메라별 주기: 인지 RGB 30 Hz는 구독한 카메라만, 측면은 10 Hz. 렌더 fps가 수집 주기의 두 배를 넘으면 수집마다 수집 직전 렌더가 붙어 30 Hz 수집은 매 프레임 렌더가 됩니다. 이 비용을 카메라별로 고를 수 있게 합니다.
- 완료 기준: 뷰어만 구독 시 렌더 = 뷰 수 × 10 Hz × 2 이하. GT 구독 시 30 Hz 강제 없음. `camera_rig.applied` 증분으로 확인.

### C3. 메인 스레드 전용 코어 (S, 구현 완료, 게임 확인 남음)

- ✔ 0.32.0 커밋 런처 **CPU 배치**(`apps/launcher/cpu_layout.py`): 게임은 CPU 0–7, 메인 스레드는 CPU 2에 혼자, 릴레이·Foxglove 서버·기록기(Job affinity)와 뷰어(BELOW_NORMAL)는 CPU 8–15. 연속 A/B −5.9 ms(−13.6%).
- 남은 것
  - C0 프로토콜로 배치 켬/끔 ABAB 재확인.
  - 메인 코어를 `GetSystemCpuSetInformation`의 SchedulingClass(선호 코어)로 고르기. 이 머신은 지금과 결과가 같습니다(CPU 2 = 6등급 코어 1).
  - 런처가 비정상 종료되면 배치가 게임 재시작까지 남습니다. 복원은 중지 경로에만 있습니다. 다음 시작 때 이전 배치를 감지해 되돌립니다.
  - Steam·브라우저 같은 다른 프로세스는 여전히 CPU 2·3에 올 수 있습니다. 측정 때는 닫습니다.
- 완료 기준: ABAB에서 배치 켬의 CPU busy p50이 끔보다 낮고, 중지 뒤 게임 affinity가 전체로 돌아옴.

### C4. 뷰어 부하 (S, 게임)

- ✔ 뷰어는 렌더를 더 일으키지 않는데도 헤드리스 구독보다 프레임당 16 ms를 더 씁니다. 뷰어 앱은 약 1.9코어와 GPU를 씁니다.
- 측정: 패널별 끔(영상 4개, 점군 3개, 3D 패널)과 창 최소화를 A/B 합니다.
- 대책 후보: 미리보기 5 Hz, 표시용 점군 stride 증가, 영상 패널 축소, 필요할 때만 뷰어 열기.
- 완료 기준: 뷰어 켬과 헤드리스 구독의 차이 3 ms 이하. 또는 남는 원인(GPU 공유 등)을 확인하고 운영 권장을 [브리지 문서](../bridge/README.md)에 적기.

### C5. 우리 코드를 메인 스레드에서 빼기 (M)

- GPU 자원·셰이더를 stream 시작 때 미리 만듭니다(`GpuPack::dispatch`·`gather`, `prepare_staging`). OM bind 최대 600 ms 같은 끊김을 없앱니다.
- 수집마다 하는 일을 캐시합니다: `seal`의 Context4 QueryInterface·GetAdapter/GetDesc(✔ 0.33.0 장치당 한 번), color·preview pack의 compute 상태 저장·복원 두 번, 라이다 패턴 JSON 복사. metadata JSON은 stream worker가 숫자 구조체로 만듭니다.
- SDK 콜백: 게임 스레드는 채널 값을 고정 배열에 넣고 엔진 값은 미리 구한 오프셋으로 읽기만 합니다. JSON 생성·`dump()`·`OT_State` 기록은 worker가 합니다. traffic 읽기는 30 Hz로 제한하고, 패드가 없을 때 `XInputGetState`는 주기를 늘립니다.
- compile begin: pass마다 분류 결과(pass 포인터·출력 id)를 캐시해 미러가 아닌 pass를 바로 넘깁니다.
- 완료 기준: 수집 프레임당 훅 본문 0.5 ms 이하, OM bind 최대 5 ms 이하, SDK 콜백은 SDK 프레임당 50 µs 이하. 출력 내용은 이전과 같음(점군·stamp 비교).

### C6. 뷰당 엔진 비용 (S–L)

- ✔ `r_mirror_view_distance` 400 → 250(사용자 설정): 정차 CPU busy 57.4 → 43.2 ms.
- 뷰별 거리: 전방 250 m, 측면 150 m(라이다 최대 거리). cvar는 전역이라 private camera 구조체에서 far plane·시야거리 필드를 찾아야 합니다([12](12_dx11_mirror_render_path.md)). 전체를 150으로 내렸을 때 250보다 7.7 ms 줄었습니다.
- `r_deferred_mirrors` 1·0: 센서 영상의 조명·AA가 바뀝니다. 인지 데이터 영향을 비교한 뒤 정합니다.
- 완료 기준: 측면 뷰만 150 m를 적용하고 전방 영상·라이다가 250 m 설정과 같음. CPU busy 감소를 A/B로 확인.

### C7. 렌더 제출을 다른 스레드로 (L, 연구, 보류)

- C0 샘플러에서 `d3d11.dll`·드라이버 비중이 클 때만 시작합니다. ✔ 2026-10-14 브리지 끔 정차에서는 둘을 합쳐 메인 스레드의 2.0%라 조건이 성립하지 않습니다. 센서 켬 분포로 다시 판단합니다.
- 후보 DXVK(D3D11→Vulkan): 드라이버 일을 별도 command stream 스레드가 맡습니다.
  - 먼저 브리지 끔으로 DX11과 fps만 비교합니다.
  - 위험: 릴레이와 주고받는 공유 텍스처·펜스가 DXVK에서 동작하지 않을 수 있습니다. 그러면 CPU readback 경로로 물러나야 하고 메인 스레드 비용이 다시 늘 수 있습니다.
- 하지 않음: `-rdevice dx12|vk`(훅 경로 비호환, 실험적·더 느리다는 보고). 자체 D3D11 명령 오프로드(엔진이 컴파일한 명령을 immediate context에서 직접 실행).

### C8. 뷰 분산 (S–M, p99)

- ✔ 지금은 4뷰가 한 프레임에 몰리고(묶음은 같은 Present의 뷰만 허용), 그 직전 프레임에도 수집 직전 렌더 4뷰가 몰립니다. 수집마다 긴 프레임이 두 번 생깁니다.
- 카메라별 묶음을 허용하면 프레임 k에 V1 준비, k+1에 V1 수집과 V2 준비…로 프레임당 최대 2뷰가 됩니다. 총량은 같고 p99만 줄어듭니다.
- 라이다는 카메라 하나에서 나오므로 영향이 없습니다. 다중 카메라를 같은 시각으로 융합할 필요가 있는지 P2 설계에서 확인한 뒤 정합니다.

### C10. 게임 미러 렌더 끄기 (S, 구현 완료, ✔ 2026-10-23 게임 확인)

- ✔ 화면에 보이는 게임 미러(FH4 슬롯 0·1·2·5)는 매 렌더 프레임 같은 `r_mirror_view_distance`로 장면을 다시 렌더합니다. △ 센서 뷰 1회가 250 m에서 약 0.8 ms라면 게임 미러 4개는 프레임당 약 3 ms로, 10 Hz 센서 렌더 전체보다 큽니다. 자율주행 시험에서는 센서가 미러를 대신하므로 사람용 미러는 필요 없습니다.
- ✔ 2026-10-14 브리지 끔 정차 스택: 미러 뷰 준비(`0x4D0D80` → `0x4D4450`)가 1.40 ms/프레임(7.4%)입니다. 여기에 미러의 렌더 제출 몫이 더해집니다([27 §6](27_main_thread_parallel_research.md#6-m0-결과-정차브리지-끔)).
- 0.33.0(게임 확인 전): 리그 설정 `native_mirrors:false`이면 `CameraRig::select`가 엔진이 고른 슬롯 마스크(r12)에서 우리 것이 아닌 미러 비트를 지웁니다. 엔진이 화면 밖 미러를 건너뛸 때와 같은 결과이고, 미러 텍스처는 마지막 장면에서 멈춥니다. 릴레이 설정 `native_mirrors`(기본 true), 런처 체크박스 **게임 미러 렌더 끄기**.
- 계측: `camera_rig`의 `views[].native_applied`(엔진이 고른 게임 미러 렌더 수)와 `native_suppressed`. 끄기 전에도 게임 미러가 실제로 몇 번 렌더되는지 이 값으로 봅니다.
- 확인할 것: 충돌·경고 없음, 센서 출력과 `applied`가 이전과 같음, CPU busy 감소, 끄고 다시 켰을 때 미러 복귀. `config.cfg`의 `r_mirror_group`(의미 미확인)도 같은 세션에서 값만 바꿔 미러 갱신이 어떻게 달라지는지 봅니다.
- 완료 기준: 정차·뷰어 켬 ABAB에서 CPU busy p50 감소, 센서 렌더 수·묶음 수 동일, 미러 복귀.
- ✔ 2026-10-23 결과(core 0.59.0, 도심 정차, 센서 켬, 스노위문 켬, `parallel_passes --order concurrent`, 20초씩, 켬 → 끔 → 켬): 끄는 동안 `native_applied` 증가 0, 센서 `applied`는 네 슬롯이 같이 늘었습니다(초당 약 80회로 켬과 같음).

| 게임 미러 | fps | CPU busy | GPU busy | 교통 |
| --- | --- | --- | --- | --- |
| 켬 (1차 2회) | 56.9 / 56.2 | 16.9 / 17.1 ms | 14.8 / 14.8 ms | 64–67 |
| 끔 (2회) | **66.6 / 68.2** | **13.6 / 13.2 ms** | 12.8 / 12.8 ms | 60–61 |
| 켬 (2차 2회) | 55.4 / 56.0 | 16.2 / 16.0 ms | 15.9 / 15.7 ms | 49–53 |

  - 미러 4개(슬롯 0·1·2·5, 매 프레임)를 끄면 CPU busy 약 −3.2 ms, GPU busy 약 −2.5 ms, fps 약 +20%입니다. 2차 켬은 교통이 더 적었는데도 끔보다 느렸습니다.
  - 릴레이를 재시작하면 tier가 내려가면서 `parallel_passes`도 꺼지므로, 재시작 뒤 다시 켜야 합니다.
  - 런처에는 체크박스가 없고, `bridge/config/bridge.local.json`에 `"native_mirrors": false`를 넣고 수집을 다시 시작해야 합니다.

### C9. GPU 후처리 worker 장치 (L, 보류)

- pack·gather·노출·라이다 Map을 별도 D3D11 장치(릴레이 `gpu_readback.cpp` 방식)로 옮깁니다. 게임 스레드에는 CopyResource와 Signal만 남깁니다.
- △ 수집 프레임당 0.1–0.3 ms. C5 뒤에 남는 비용을 보고 정합니다.

## 4. 하지 않을 것

| 방법 | 이유 |
| --- | --- |
| affinity 반복·"Multicore.dll" | 스레드 하나의 일을 나누지 못함. SCS 포럼에서 효과 없음 보고 |
| NVIDIA Threaded optimization | OpenGL 설정 |
| HAGS | Microsoft가 큰 변화 없다고 함 |
| 해상도 축소 | GPU busy가 CPU보다 훨씬 짧음 |
| D3D11 deferred context | 엔진이 immediate context로 제출. 우리 호출은 카메라당 수십 개뿐 |
| iceoryx2·rmw_iceoryx2로 ROS 전송 교체 | 게임 메인 스레드를 줄이지 못함. ✔ ROS 경로는 릴레이 약 0.3코어·Foxglove 서버 약 0.1코어이고 발행 p50 0.2 ms(2026-10-12). 게임→릴레이는 이미 공유 메모리 ring과 공유 GPU 텍스처. rmw_iceoryx2는 alpha(rolling 대상, graph·service·QoS 미완)라 foxglove_bridge·릴레이 서비스·bag이 깨질 수 있음. 다시 볼 조건: 발행 멈춤이 DDS 원인으로 확인되고 Fast DDS 설정으로 못 고칠 때, 또는 ROS 없는 Windows 네이티브 구조로 갈 때 |

## 5. 순서

1. ✔ 게임 없이(2026-10-13, core 0.33.0 빌드, 설치 전): C0 측정 도구·샘플러, C2 GT rider, C5 `seal` 캐시, C10 게임 미러 끄기. 남은 C2 카메라별 주기와 C5의 나머지(SDK JSON worker, compile 분류 캐시, 자원 미리 생성)는 측정 뒤 우선순위를 다시 정합니다.
2. 게임 세션 한 번에 C0 ABAB(`ot.cmd perf`): 샘플러 분포(브리지 끔·센서 켬), 게임 미러 켬/끔(C10), 배치 켬/끔(C3), `capture_warmup` 켬/끔(C1 비용), 뷰어 패널·최소화(C4).
3. 결과로 C1 조사 범위와 C4 운영 권장을 정합니다.
4. C6 뷰별 거리 조사. 샘플러 결과에 따라 C7 시험 여부를 정합니다.
5. C8·C9는 P2 요구와 남은 비용을 보고 정합니다.

## 6. 다음 게임 세션 진행

같은 주차 위치에서 정차, snowymoon 안티앨리어싱 끔, `r_mirror_view_distance 250`, 브라우저 등 무거운 앱은 닫습니다. 측정은 모두 `.\ot\ot.cmd perf <label> [--profile-hz 250]`(30 s)이고, 조건을 바꾼 뒤 10 s 기다렸다 잽니다. 결과는 `ot.cmd perf_table research\live\<날짜>-perf\*.json`으로 봅니다.

1. core 0.34.0 설치(런처 중지 상태): `ot.cmd loader unload` → 이전 `plugins/ot_runtime/ot_core.dll`을 `ot/backup/<날짜>-before-0.34.0/`에 복사 → `ot/dist/ot_core.dll` 복사·해시 확인 → `ot.cmd loader load` → `ot.cmd version`이 0.34.0. 릴레이도 새로 빌드한 것을 씁니다.
2. 스택 smoke: `smoke-1 --seconds 5 --profile-hz 250` 뒤 `ot.cmd hotspots research\live\<날짜>-perf\smoke-1.json`. `main_profile.error`가 비고, 스택 끝 `top`이 90% 이상인지, 샘플당 정지 최대(`suspend_us_max`)를 봅니다. `depth`로 끝나는 비율이 크면 이후 `--profile-depth 64`, `bad_frame`·`unknown_code`가 크면 C 시리즈는 `--profile-depth 0`으로 잽니다.
3. 브리지 끔 기준과 분포: `off-1 --profile-hz 250`. 게임 스레드별 CPU는 항상 남고, `--profile-hz`를 주면 호출 스택(기본 depth 48)도 남습니다. 끝나면 `ot.cmd hotspots research\live\<날짜>-perf\off-1.json`으로 함수별 비중을 봅니다([27](27_main_thread_parallel_research.md) M0). ABAB도 250 Hz를 유지합니다(정지 비용 약 0.8%로 0.33.0 계획과 같음).
4. 주행 샘플(사람이 운전할 때): `drive-1 --seconds 60 --profile-hz 1000`. 순위와 "주행 중 엔진 worker가 노는가"는 정차로는 답이 안 나옵니다. 정지 비용이 약 3%라 fps 비교에는 쓰지 않습니다.
5. 런처 시작, Foxglove 뷰어 연결: `view-1 --profile-hz 250`. 분포에서 `eurotrucks2.exe`·`d3d11.dll`·`nvwgf2umx.dll`·`ot_core.dll` 비중을 C7 판단에 씁니다.
6. C10 게임 미러: 체크박스와 **재시작**으로 켬/끔을 바꿔 `mirror-on-1`·`mirror-off-1`·`mirror-on-2`·`mirror-off-2`·`mirror-on-3`·`mirror-off-3`. 끈 동안 화면 미러가 멈추는지, 다시 켜면 돌아오는지 눈으로 확인합니다.
7. C3 CPU 배치: 같은 방식으로 `layout-on-*`·`layout-off-*` 3쌍.
8. C1 수집 직전 렌더 비용: `bridge/config/bridge.local.json`에 `"capture_warmup": false`를 넣고 재시작해 `warmup-off-*`, 지우고 `warmup-on-*` 3쌍. 끈 동안 나무·가로등 결함은 감수하고, 끝나면 반드시 되돌립니다.
9. C4 뷰어: 뷰어 창 보임·최소화를 번갈아 `viewer-shown-*`·`viewer-min-*` 3쌍.
10. 각 단계의 `slow_publishes.recent`와 런처 "발행" 줄을 함께 기록합니다(330 ms 정지 추적).
