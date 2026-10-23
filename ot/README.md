# ot — 게임 DLL과 개발 도구

저장소와 설치본 core는 0.59.0입니다(2026-10-23 설치; 0.59.0은 SDK 프레임 끝의 교통 JSON·직렬화·게시를 게시 스레드로 옮겨 메인 스레드 몫을 1.89 → 0.52 ms로 줄임, 도심·센서 ABAB에서 동시 기록 CPU busy −4.64 ms, docs/28 §11.20; 0.58.3은 합류 50 ms 초과가 30프레임 연속일 때만 off, 센서 켬 상태에서도 언로드되도록 종료 순서 수정, docs/28 §11.19; 0.58.2는 2026-10-23 설치; 0.58.x는 큰 draw 배치를 아이템 범위로 나눠 남는 실행자가 함께 기록(M4, 스노위문 켬 정차 CPU busy −2.47 ms, docs/28 §11.18); 0.57.0·0.57.1은 배열 락 공유, 재질 락 스핀, 긴 패스 먼저 배분으로 스노위문 켬 정차 CPU busy −2.97 ms, docs/28 §11.17, 0.54.3 백업 `ot/backup/2026-10-23-core-0.54.3`). 0.56.0·0.56.1은 큐 묶음 준비를 묶음별 락으로, `draw_items`·`resource_ready`를 재질 락으로 옮겨 풀 락 대기를 8분의 1로 줄였습니다(스노위문 켬 정차 CPU busy −1.70 ms, docs/28 §11.16), 상주 loader는 0.48.0입니다(게임 1.61.1.9). 0.55.0은 스노위문 프록시(AA 켬)와 `parallel_passes --order concurrent`의 공존(B안: 장면 키·합성 잎 훅, 합류 재생, 장치 슬롯 락, 직렬 프레임 그림자 검증, `serial --proxy-check`)을 더합니다. 2026-10-23 정차 교대 비교 4,819 동시 프레임에서 크래시·불일치 0이었고(성능 비교·장시간은 미측정), 내용은 [docs/28 §11.15](../docs/28_render_pass_parallel_prototype.md)입니다. 0.52.0–0.54.3은 동시 기록(M3c·M3d, docs/28 §11.6–11.14)입니다. 0.52.0은 `parallel_passes --order concurrent`(M3c: 패스를 동시에 기록, worker별 프레임 할당 아레나와 장치 풀 함수 8개의 SRW 락, 프록시 없는 세션 전용, 게임 시험 전, docs/28 §11.6)를 더합니다. 0.51.1–0.51.3은 `parallel_passes`만 바꿉니다: `alternate --report-only`(끄지 않고 구조 차이 키를 보고), 교대 패턴 직렬·직렬·병렬·병렬(엔진이 홀짝 프레임에 다르게 그리는 패스 때문, docs/28 §11.5), 직렬에서 병렬로 바로 바꿀 때 자기 훅을 외부 패치로 오인하던 게이트 수정. 0.51.0은 `proxy_coexistence trace`·`dump`(프록시 링 커서와 히스토리 레코드를 읽기만 하는 추적, [조사 §9](../research/snowymoon/sensor_coexistence.md))를 더합니다(core만). 0.50.1·0.50.2는 이 설정을 lease 없이 켜게 하고 AA 값 범위를 0..6(DLSS 포함)으로 고쳤습니다. 2026-10-22 첫 게임 시험에서 게이트는 메인 깨짐을 더 심하게 만들어 기각했습니다. 0.50.0은 snowymoon 프록시와 센서 카메라 렌더의 공존을 위한 "렌더 스레드 구간 설정"(core 명령 `proxy_sensor_coexistence`, 설정 `allow_proxy_sensor_coexistence`, 기본 꺼짐)을 더합니다. core만 바뀌며(같이 빌드되는 loader는 번호만 0.50.0), 근거는 정적 분석뿐이고 라이브 검증 전이며 게임 설치 전입니다(아래 "snowymoon 프록시와 센서 카메라"). 0.49.0은 `parallel_passes`의 합류점 직렬 실행 모드 이름을 `defer`에서 `serial`로 바꿉니다(core 명령 값, `ot.cmd` 인자, 상태 키 `frames.serial`·`joined_frame_mean`). core만 바뀌며(같이 빌드되는 loader는 번호만 0.49.0이고 동작은 0.48.0과 같음), 0.48.0 이하가 설치된 동안에는 core가 `serial`을 거절하므로 설치 뒤에 새 이름을 씁니다. 0.48.0은 loader의 `ot_drive` 장치에 semantical `lblinker`·`rblinker`(방향지시등 레버, 게임 mix `lblinker`·`rblinker`가 이미 `semantical.*?0`을 받아 다시 연결할 필요 없음)를 등록하고, 운전 API에 선택적 ABI-1 꼬리 `signal`(무장된 소유자·epoch의 레버 한 번 누르기, 100 ms 누르고 100 ms 쉼, 4개까지 대기, 해제 시 버림)과 core 파이프 `drive` 동작 `signal`(`side` −1/1)을 더합니다. loader가 바뀌므로 게임을 정상 종료하고 설치한 뒤 재시작해야 하며, 예전 loader에서는 core가 `signal`을 "updated resident loader" 오류로 거절합니다(나머지는 그대로). △ 게임 설치·시험 전입니다. 0.47.1은 교통 GT에 차체 상자 중심 높이 `z`와 상자 높이 `height_m`을 더합니다(core만, 2026-10-22 설치). 아래는 그 전 기록입니다. 0.43.0은 HUD 궤적 데칼([docs/24](../docs/24_hud_overlay_design.md): `overlay` 명령, `Local\OT_Overlay`, render_probe `overlay` 모드)을 더합니다. core만 바뀌며(같이 빌드되는 loader는 번호만 0.43.0이고 동작은 0.42.0과 같음) 게임 시험 전입니다([사용법](#hud-궤적-데칼)). 0.42.0은 운전 API에 보조 제동(축 4: 리타더 단 0–5와 엔진브레이크, ABI-1 `request_aux` 꼬리)을 더하고 loader의 `ot_drive` 장치에 semantical `retarder0`–`retarder5`·`motorbrake` 입력을 등록합니다. loader가 바뀌므로 게임을 정상 종료하고 설치한 뒤 재시작해야 하며, 예전 loader와 새 core, 새 loader와 예전 core 조합은 보조 제동 없이 그대로 동작합니다([운전 명령 API](../bridge/README.md#운전-명령-api)). 0.41.0은 Tier 1 `engine.drivetrain`에 첫 트레일러의 바퀴별 브레이크 토크를 더합니다. 0.40.0은 첫 트레일러 채널(`trailer.0.*`, 바퀴별 채널 포함)과 트럭 외 SDK 구성(`configs`: 트레일러, 작업·화물, 조작의 변속 방식 등)을 받습니다. 0.39.0은 Tier 1 `engine.drivetrain`에 실제 엔진 토크·변속기 출력 토크·축 회전·바퀴별 브레이크 토크를 더했습니다([docs/30](../docs/30_can_like_signals.md)). 0.38.0은 설치 후 Tier 1·렌더 훅·카메라 리그·릴레이를 확인했습니다. 0.37.0(실차 CAN에 해당하는 SDK 채널·트럭 구성 값)과 0.38.0도 core만 바뀝니다. 0.31.1(lease 반납을 `panic` 대신 `disconnected`로 기록), 0.32.0(자세만 남은 카메라는 미리보기 주기로만 렌더, `sdk_timing`), 0.33.0(GT 메타데이터도 rider, 리그 `native_mirrors`, `main_profile`·`traffic` 명령), 0.34.0(`main_profile` 호출 스택), 0.34.1(`main_profile`의 `thread_id`로 다른 게임 스레드 샘플), 0.35.0(`parallel_passes` 직렬 모드, 당시 이름 `defer`), 0.36.0(`parallel`·`alternate`)은 core만 바뀌어 설치된 loader 0.31.0을 그대로 씁니다. 0.31.0 loader는 custom modifier(`!`·`&`·`joy.bN`·빈 mix)가 있는 controls 프로필도 수동 개입 판정에 읽습니다(loader 교체 후 게임 재시작 필요). 센서와 독립된 운전 명령·AI traffic GT를 연결하며, 선택적 ABI-1 확장으로 ACC·LCC 축 소유권을 분리합니다. 입력 장치는 상주 loader가 공식 Input SDK로 등록합니다. 센서 수집과 독립된 가상 3축 장치이며, 저장소 기본 권한은 꺼져 있습니다. 실제 지도 차로·GT 위치 기반 주행은 core 0.28.0에서 처음 확인했습니다. 구 host prefix는 유지하며, 독립 ACC/LCC 모드에는 새 상주 loader 설치 후 정상 게임 재시작이 필요합니다. [입력 API와 남은 게임 시험](../bridge/README.md#운전-명령-api), [독립 ACC/LCC 사용법](../bridge/README.md#gt-도로-자동-주행).

브리지는 카메라 30 Hz·라이다 10 Hz·표시용 JPEG 10 Hz를 요청합니다. FH4·Scania S private 프리셋은 슬롯 3·4·6·7을 센서 제출에 사용하며 native mirror 배열은 보존합니다. 일반 사용은 [브리지 런처](../bridge/README.md)에서 시작합니다.

## 빌드와 설치

```powershell
.\ot\build.cmd
```

x64 MSVC Build Tools, CMake·Ninja, 공식 SCS SDK가 필요합니다. 기계별 절대 경로는 저장소에 두지 않습니다. MSVC는 vswhere로 찾고 `OT_VS` 환경 변수로 지정할 수 있습니다. SCS SDK는 저장소 상대 경로 `research/sdk/include`(또는 `-DSCS_SDK=`)를 사용합니다. 게임 EXE는 `-DETS2_EXE=` → `ETS2_DIR` 환경 변수(게임 설치 폴더) → Steam 라이브러리 순으로 찾고, 처음 찾은 값이 `build/` CMake cache에 남습니다. 주소는 `schema/<게임 빌드>/`에서 관리하고 `-DOT_GAME_VERSION=`(기본 1.61.1.9)으로 고릅니다. 게임이 업데이트되면 [주소 옮기기](../research/README.md#게임-업데이트-뒤-주소-옮기기)로 새 디렉터리를 만듭니다. 그 전에 SDK 기능만 다시 빌드하려면 `-DOT_SDK_ONLY_BUILD=ON`을 씁니다(Tier 1은 런타임 게이트가 끕니다).

| 파일 | 설치 위치·용도 |
| --- | --- |
| dist/ot_loader.dll | 게임 bin/win_x64/plugins/의 상주 SDK 플러그인 |
| dist/ot_core.dll | plugins/ot_runtime/의 교체 가능한 기능 DLL |
| dist/ot_config.json | plugins/ot_runtime/의 실행 권한·초기 상태 |
| dist/ot_ipc.dll | Python 프로세스의 shared-memory reader |

최초 설치는 게임을 종료한 상태에서 수행합니다. DX11 실행 후 SDK 알림을 확인하고 운전석으로 들어갑니다. 초기 Tier는 0입니다. 내부 접근에는 `singleplayer_research`, `allow_tier1`, 렌더에는 `allow_render_probe`, 리그에는 `allow_camera_rig` 설정이 필요합니다. 싱글플레이 연구용이며 멀티플레이에서는 플러그인을 제거합니다.

0.27.0의 최초 입력 장치 등록에는 loader 교체 후 게임 재시작이 필요합니다. SDK 입력 콜백은 게임의 `scs_input_shutdown`까지 loader에 남으며 core hot reload로 다시 등록하지 않습니다. ABI 1의 기존 core/loader는 센서 기능을 유지하고 선택적 입력 확장만 건너뜁니다. 설치된 `ot_config.json`의 `allow_drive`·`singleplayer_research`와 활성 프로필의 `drive_controls_path`를 명시적으로 설정해야 arm이 가능합니다. 저장소 기본값은 `allow_drive: false`입니다.

## 상태·패닉·DLL 교체

```powershell
.\ot\ot.cmd ping
.\ot\ot.cmd version
.\ot\ot.cmd snapshot
.\ot\ot.cmd state
.\ot\ot.cmd panic
.\ot\ot.cmd loader status
.\ot\ot.cmd loader unload
# module_state=unloaded 확인 후 plugins/ot_runtime/ot_core.dll 교체
.\ot\ot.cmd loader load
```

먼저 브리지를 중지한 뒤 기능 DLL을 교체합니다. 로더는 남아 있고 기존 FFB 플러그인은 유지됩니다. `loader reload`는 파일 교체 없이 기능 모듈을 재초기화합니다. F11은 내부 접근·리그·수집을 해제하고 SDK 수신을 유지합니다.

## 렌더 패스 미루기와 worker 기록 (M3a·M3b)

```powershell
.\ot\ot.cmd parallel_passes serial  # 타입 1 패스를 패스 반복 끝(0x227B1F)에서 원래 순서로 기록
.\ot\ot.cmd parallel_passes         # 프레임·패스·합류 시간, 패스별(이름/공간) 시간·dword 상위 60
.\ot\ot.cmd parallel_passes off
```

렌더 패스 병렬 기록([docs/28](../docs/28_render_pass_parallel_prototype.md))의 첫 단계입니다. `serial`은 메인 스레드에서 합류점에 원래 순서대로 기록하므로 성능 이득은 없고, 미루기·splice·패스별 카운터를 확인하는 용도입니다.

M3b는 미룬 패스를 엔진 작업 관리자(`parallel_loop_task_t`)로 worker에 나눕니다. 아직 한 번에 한 패스만 기록하므로 이득은 없고, worker 스레드에서 렌더 기록이 도는지를 확인하는 단계입니다.

```powershell
.\ot\ot.cmd parallel_passes parallel --order ticket --accept-foreign-hooks   # 원래 순서로 한 번에 한 패스(스레드 전환만 검증)
.\ot\ot.cmd parallel_passes alternate --order ticket --accept-foreign-hooks  # 프레임마다 serial과 parallel을 번갈아 키별 명령 요약 비교
.\ot\ot.cmd parallel_passes parallel --order free                           # 순서 자유 + 락 하나(외부 패치 없는 .text에서만)
```

`--order`는 필수입니다. 외부 코드 패치가 기록 경로에 있으면(status `gate.workers_allowed`가 false) worker 모드는 거절되고, `order=ticket`만 `--accept-foreign-hooks`로 예외를 둡니다. 큐에 같은 패스 객체가 두 번 있으면 그 프레임은 직렬로 기록합니다. alternate는 직렬·직렬·병렬·병렬 4프레임 주기로 기록하고 각 패스 본체를 요약해 2프레임 전(다른 모드, 교차)과 4프레임 전(같은 모드, 기준선)을 비교하고, 교차 불일치율이 기준선의 두 배와 기준선+1%p를 모두 넘으면 꺼집니다(`--report-only`면 끄지 않고 status `alternate.structure_keys`에 키별로 보고). 이 판정은 세 단계 합계, 구조(크기·토큰) 단계만, 토큰 단계만(여유 +0.1%p)에 각각 적용합니다. 게임에서는 내용 단계가 정차 중에도 77% 다르기 때문입니다(저장소 다음 core부터, docs/28 §11.2). status의 `parallel`(순서 대기·패스 보유 시간, 스레드별 패스 수)과 `alternate`(비교 수·불일치율·처음 어긋난 키)가 결과입니다. 설치된 `ot_config.json`에 `allow_parallel_passes`(저장소 기본값 false), `allow_tier1`, `singleplayer_research`가 필요합니다. 브리지 lease와 무관하게 켜고 끄며, F11·lease 종료·권한 재읽기에서 꺼집니다. 끄기는 미룬 패스가 있는 프레임이 합류를 마칠 때까지 기다렸다가 훅을 내립니다. 일시정지 프레임과 장치가 바뀐 프레임은 원래 경로로 기록합니다. `perf` 표본에 이 상태의 증가분이 함께 저장됩니다.

켤 때 게임 `.text`를 exe 파일(현재 베이스로 `.reloc` 적용)과 비교해 다른 구간을 우리 훅, `ot_config.json`의 `accepted_text_patches`(`[{"rva": "0x…", "size": n}]`), 외부로 나눠 status의 `gate`에 냅니다. 기록 경로 함수(`fields.json`의 `render_parallel.closure_rvas`)와 우리 훅 자리의 앞 32바이트는 켜져 있는 동안 1초마다 다시 비교하고, 바뀌면 off로 내려갑니다. 외부 패치가 있어도 `serial`은 켭니다. 메인 스레드에서 원래 순서로 기록하기 때문이며, snowymoon `dxgi.dll`과 함께 2,200프레임을 확인했습니다. 외부 패치가 있으면 `gate.workers_allowed`가 false가 되고, worker 기록(M3b 이후)은 이 값을 따릅니다.

```powershell
# 주행 판정 세트(docs/28 §6): 20 km/h를 10초 유지하면 off/serial을 30초씩 번갈아 2쌍
.\ot\ot.cmd passes_abab pp-drive --pairs 2 --min-kmh 20 --profile-hz 250
# 다른 두 모드: --a/--b, worker 모드는 --order(필요하면 --accept-foreign-hooks)
.\ot\ot.cmd passes_abab pp-ticket --a serial --b parallel --order ticket --accept-foreign-hooks --min-kmh 20
```

티켓 순서(M3b-1)는 정합성 시험용입니다. 2026-10-22 시험에서 worker 기록이 엔진 assert·alien 경고 없이 메인 기록과 같은 명령을 만들었지만, 차례 넘김 때문에 합류가 가끔 50 ms를 넘어 자동으로 꺼지고 주행에서 fps가 떨어졌습니다(docs/28 §11).

같은 프레임에서 패스를 두 번 기록해 비교하는 방식은 쓸 수 없습니다. 2026-10-15 시험에서 두 번째 기록이 엔진 assert(`array_t<rg_blit_pair_t>` 인덱스 범위, blit 패스 콜백 `0x151BF80`)로 게임을 종료시켰습니다. 패스 콜백이 기록하면서 자기 패스 객체의 blit 쌍 배열(`pass+0xDB8`)을 소비하기 때문입니다.

## snowymoon 프록시와 센서 카메라 (검증 전 가설, 0.50.0)

**원인과 해결(2026-10-22)**: 프록시가 시간축 AA 히스토리를 렌더 타깃 크기로 찾아서, 메인 내부 해상도와 같은 크기의 센서 타깃이 메인 히스토리를 가져갔습니다. 비공개 센서 프리셋을 1288x720·968x540(미러 배율 2배로 2576x1440·1936x1080)으로 바꿔 해결했고, 아래 게이트는 메인을 더 깨뜨려 기각했습니다([조사 §10](../research/snowymoon/sensor_coexistence.md)). 게임 렌더 배율·창 크기를 바꿔 메인 내부 해상도가 센서 타깃과 같아지면 센서 해상도를 비킵니다. 기록용으로 남긴 원래 설명: snowymoon `dxgi.dll`이 있으면 센서 카메라가 10 Hz로 렌더될 때 메인 화면이 깨집니다([lessons](../docs/history/lessons.md), [조사](../research/snowymoon/sensor_coexistence.md)). 이 설정은 센서 패스가 도는 동안만 프록시의 AA 설정 사본(`dxgi+0xC7382C`)을 0으로 두고 구간이 끝나면 `dxgi+0xA3D840`을 읽어 되돌리며, 센서 장면 패스가 프록시 링에 남긴 항목의 건너뜀 바이트를 1로 세웁니다. 프록시 코드는 패치하지 않고 데이터만 씁니다. **정적 분석만 근거이고 라이브 검증 전입니다.** 증상이 사라지는지, 센서 영상이 어떻게 달라지는지 모릅니다. 게임 세션에서 확인할 것은 [sensor_coexistence.md](../research/snowymoon/sensor_coexistence.md) §6의 8개 항목입니다(문서 번호 §6, 요청서는 §5로 적음).

- 켜는 법: 설치된 `ot_config.json`에 `allow_proxy_sensor_coexistence: true`(저장소 기본값 false), `allow_tier1`, `singleplayer_research`, `allow_render_probe`가 필요하고, `ot.cmd render_probe on`(observe 모드)과 `camera_rig apply`(`private_outputs`) 뒤에 `ot.cmd proxy_coexistence on`을 씁니다. `off`는 구간이 열려 있으면 되돌린 뒤 끕니다. 센서 슬롯이 `private_outputs`가 아니면 소유 판정이 센서를 찾지 못해 아무것도 하지 않습니다. F11·lease 종료·권한 재읽기·render_probe off에서도 꺼지고 값이 복원됩니다.
- 사전 검사(하나라도 틀리면 켜지 않고 status의 `proxy_sensor_coexistence.reason`에 이유를 남김): 게임 폴더의 로드된 `dxgi.dll`, 그 파일 sha256 `d819c168…f4dc`, `dxgi+0x9FE10`·`+0x17580`의 .text 32바이트, `0xC737AC==1`·`0xC7381C==0x3D`·`0xA3D840<=6`·`0xB10FE8<=4`, 쓰는 자리가 `.data`의 쓰기 가능 페이지. 켠 뒤에도 구간 진입마다 `0xC737AC==1`을 다시 보고 틀리면 값을 복원하고 멈춥니다. 프록시 내부 오프셋은 모두 `native/proxy_gate.hpp`의 `Layout`에 있고 출처 절을 달아 두었습니다. 프록시 버전이 바뀌면 sha256에서 걸러집니다.
- 동작: 렌더 스레드의 기존 `0x2B3203` 훅에서 `PassCommands`로 패스 소유를 판정합니다(이름 없는 패스는 메인). 켜 있는 동안 센서 슬롯의 모든 패스(라이팅·블러 포함)에 이름을 붙이는 모드가 컴파일 훅에서 같이 켜집니다. 센서 진입 때 프록시가 프레임이 바뀔 때 하는 재복사를 먼저 재현한 뒤 0을 씁니다. Present 훅은 쓰지 않습니다(스레드가 다를 수 있어서). 프레임이 바뀌면 프록시의 재복사가 값을 되돌리므로 열린 구간이 다음 프레임까지 남지 않습니다.
- status `proxy_sensor_coexistence`: `enabled`, `reason`, `in_sensor_interval`, `counters`(`interval_entries`·`interval_exits`·`sensor_binds`·`main_binds`·`lookup_miss`(이름 없는 패스, 메인 대부분 포함)·`ring_flagged`·`ring_invalid`·`ring_count_max`·`entries_with_nonidle_draw_state`·`recopies`). 시험: `cmake --build build --target ot_proxy_gate_test`(가짜 프록시 메모리 블록으로 사전 검사 분기, 쓰기·복원 순서, 링 표시).
- 아직 코드로 확인하지 못한 가정(△): 링 키가 센서 `pass` 포인터와 같음, `private_outputs`의 네임스페이스 라벨이 `mirrorN`, 프레임 브래킷과 렌더 스레드의 관계, 장치 vtable `[0x108]` 확인(조사 §4.2 항목 6)은 구현하지 않았고 훅 시그니처 검사(`find_target`)만 걸립니다.

## HUD 궤적 데칼

메인 화면의 도로 위에 계획 궤적을 반투명하게 그립니다([설계와 구현 상태](../docs/24_hud_overlay_design.md)). 평소에는 릴레이가 `/ets2/overlay/path`를 받아 그립니다([브리지 문서](../bridge/README.md#hud-궤적-오버레이)). 릴레이 없이 world 경로 파일을 그리는 시험 명령은 다음과 같습니다. 런처·릴레이를 중지한 상태에서 실행합니다.

```powershell
.\ot\ot.cmd overlay --file bridge\recordings\gt-path\current-lane.json   # lease를 잡고 Ctrl+C(또는 --duration)까지 표시
.\ot\ot.cmd overlay status          # 다른 창: main0 카메라·pass span·블릿 판정·데칼 통계
.\ot\ot.cmd overlay capture --png   # 다른 창: 데칼이 그려진 연속 2프레임(main0/composition_out) 저장
.\ot\ot.cmd overlay clear           # 공유메모리의 경로를 숨김으로
```

`--file`은 `frame_id: "world"`, `points: [[x, y], …]`(ROS world = SCS X, −Z) JSON입니다. 1초마다 SDK 자세에서 트럭에 가장 가까운 점부터 앞쪽 200 m(최대 256점)를 골라 쓰고, 노면 높이는 core `resolve_layout`의 base_link 원점(구동축 아래 명목 지면점)으로 정합니다. `--width`(기본 3.5 m)·`--alpha`(0.22), 노면 판정 `--z-tolerance`(1 m)·`--slope-deg`(8°), 블릿 판정 `--detector compiled_span|backbuffer_usage`를 고릅니다. 설치된 `ot_config.json`의 `allow_tier1`·`singleplayer_research`·`allow_render_probe`가 필요합니다. 종료하면 overlay를 끄고 lease를 반납합니다.

`status`(lease 불필요)의 `main_passes.scene_cameras`는 graph buffer 0–2별 `scene defattr` 카메라(`viewport_depth`·projection), `compiled`는 main pass compile 수, `blit_detection`은 composition_out 다음 첫 단일 RT bind의 판정 집계(`both`가 정상), `decal.skips`는 그리지 않은 이유별 수입니다. 렌더 스레드 비용은 `hooks`의 `overlay.blit_observation` 항목(QPC tick)입니다. `capture`는 `%LOCALAPPDATA%\ETS2AutonomyLab\ot\<게임 PID>\overlay-capture-*`(또는 `--out`)에 raw·`capture.json`을 쓰고, 같은 프레임 카메라로 투영한 경로 시작·페이드 시작·끝의 픽셀(`marks`)을 함께 남깁니다. `--png`에는 NumPy·Pillow가 필요합니다.

## Phase 1 고속도로 4뷰

[phase1-highway-fh4-private.json](presets/phase1-highway-fh4-private.json)과 [phase1-lidar-private.json](presets/phase1-lidar-private.json)을 함께 사용합니다. Scania S는 [phase1-highway-scania-s-private.json](presets/phase1-highway-scania-s-private.json)을 씁니다. 장착 좌표와 해상도는 [차량별 리그](../docs/14_phase1_highway_sensors.md)에 있습니다.

전방은 선바이저 바깥에, 측면은 미러 하우징 뒤쪽에 부착했습니다. 네 카메라는 캐빈 서스펜션을 따릅니다. 자차 body는 엔진의 full-list 제출 경로를 사용합니다.

core 0.26.1의 브리지 private 리그는 `capture_warmup: true`를 사용합니다. 유휴 뒤 요청된 뷰를 한 프레임 준비하고, 바로 다음 Present의 같은 뷰에서 수집합니다. 요청이 없으면 센서 렌더를 생략합니다. 이전 queued graph가 끝난 뒤 private 배열을 갱신합니다. rig-only와 non-private 리그는 연속 렌더하며, 비교용으로 `capture_warmup: false`를 지정할 수 있습니다.

## 실시간 미리보기

```powershell
.\ot\ot.cmd preview --config .\ot\presets\phase1-highway-fh4-private.json --format rgbd8 --duration 60
.\ot\ot.cmd camera_rig apply --config .\ot\presets\phase1-highway-fh4-private.json --rig-only
.\ot\ot.cmd panic
```

Python 미리보기는 장착 위치·회전·FOV를 편집하는 개발 UI입니다. 수집은 릴레이와 같은 native stream/IPC를 사용하고 종료 시 자기 lease만 해제합니다. `--duration`으로 실행 시간을 명시합니다.

core는 Tier·render probe·camera rig·stream 변경과 운전 명령을 lease 소유자에게만 허용합니다. 따라서 `camera_rig apply`는 lease를 잡고 heartbeat를 보내며 Ctrl+C(또는 `--duration`)까지 리그를 유지하고, 끝나면 lease 해제로 Tier 0에 돌아갑니다. CLI 단독으로는 `tier`·`render_probe`·`stream`·`camera_rig`의 상태 조회와 `panic`만 동작합니다. 장착 원점 계산은 DLL과 릴레이가 같은 SDK 기반 함수를 사용합니다. 일반 실시간 표시는 apps 런처와 Foxglove를 사용합니다.

## 기록·오프라인 도구

```powershell
.\ot\ot.cmd lidar '<frame.tar.zst>' --config .\ot\presets\phase1-lidar-private.json --output lidar.npz
```

디렉터리·ZIP·TAR.ZST 묶음은 `otpy.bundles`에서 읽습니다. Zstandard 의존성은 `requirements-recording.txt`에 있습니다. ROS 기록은 [런처](../bridge/README.md#기록과-재생)에서 관리하며, 차량 상태는 기본 `bridge/recordings`, 영상·라이다는 지정한 저장 폴더에 남깁니다.

### 오프라인 객체 박스 투영과 가림 판정

`otpy project_boxes`는 해당 pass의 카메라와 렌더 모델 자세로 차량 박스를 투영합니다. `otpy reconstruct`는 geometry DSV 또는 attributes Z를 선택해 복원합니다. 명령별 입력은 `ot.cmd <command> --help`에서 확인합니다. 이동 AI 2대·6시점에서 현재 자세의 RGB/DSV 정합이 ±0.5초 자세보다 높았습니다.

## IPC·시각

명령 pipe는 `\\.\pipe\ot`, 로더는 `\\.\pipe\ot_loader`입니다. SDK 상태는 `Local\OT_State`, 센서 묶음은 `Local\OT_Bundles`, HUD 궤적은 `Local\OT_Overlay`(seqlock generation, 쓰는 쪽 하나)를 사용합니다. 슬롯은 원자적 소유권 전환 뒤 읽고 즉시 반환합니다. 공유 GPU stream 소비자는 fence 반환까지 담당합니다.

카메라·차량 관측은 `render_sample.*`의 숫자 구조체로 전달합니다. SDK 스냅샷은 참조로 공유하고, 완성된 슬롯의 JSON은 전송 작업자가 생성합니다. `ros` 형식은 촬영 자세·시각·요청한 GT를 담고, `raw`·`raw+ros` stream과 진단 관측은 pass·이미지 연결·전체 SDK 진단도 유지합니다.

`hooks`는 훅별 호출 시간, `sdk_timing`은 SDK channel·frame_start·frame_end 콜백의 누적·최대 QPC tick을 돌려줍니다. 둘 다 게임 메인 스레드 비용입니다. `main_profile`은 메인 스레드의 명령 포인터를 10–1000 Hz로 최대 120초 샘플해 모듈별 비중과 256바이트 단위 hot RVA를 돌려줍니다(샘플당 정지 평균 약 25 µs). `depth`(0.34.0, 최대 64)를 주면 정지한 동안 호출 스택도 풉니다. 락을 잡는 `RtlLookupFunctionEntry`·`RtlVirtualUnwind` 대신 시작 때 읽어 둔 모듈 `.pdata`와 대상 스택 범위만 읽는 자체 unwinder(`stack_unwind.cpp`)라, 로더 락이나 힙 락을 쥔 채 멈춘 메인 스레드와 교착하지 않습니다(정지 평균 약 31 µs, depth 48). 끝난 뒤 `action:"stacks"`가 같은 스택을 묶어 페이지 단위로 돌려줍니다(`ot.cmd main_profile stacks --out <file>`). `thread_id`(0.34.1, CLI `--thread`)를 주면 메인 스레드 대신 다른 게임 스레드를 샘플합니다. `traffic`은 교통 차량 목록을 읽습니다. 둘 다 읽기 전용이라 lease가 필요 없습니다. 측정은 `.\ot\ot.cmd perf <label> --profile-hz 250`으로 PresentMon과 이 카운터들, 게임 스레드별 CPU를 한 JSON(`research/live/<날짜>-perf/`)에 남기고 `perf_table`로 비교합니다. 스택은 `<label>.stacks.json`에 남고 `ot.cmd hotspots <json...> [--focus eurotrucks2.exe+0xRVA] [--folded out.txt]`가 함수별 ms/프레임으로 묶습니다([성능](../docs/18_performance.md#cpu-병목-분석), [계획](../docs/26_cpu_optimization_plan.md)).

SDK frame_end는 물리 결과 이후이며 렌더 보간·장면 준비가 뒤따릅니다. 센서 묶음은 실제 pass Present ID를 기준으로 맞춥니다. [좌표·시각](../docs/04_sensors_and_data.md), [렌더 구조](../docs/12_dx11_mirror_render_path.md).

## 게임 업데이트

새 게임 빌드에서는 실행 파일·SDK와 `schema`의 주소·필드를 대조하고, 정차 상태에서 SDK·물리 자세 → 한 센서의 RGB·DSV·투영 → 전체 리그 순으로 확인합니다. 기본 미러·자차 가림·같은 프레임 정합과 메타로더 해제·패닉 복구를 확인한 뒤 사용자에게 알리고 전경 성능을 측정합니다.

| 증상 | 확인할 경로 |
| --- | --- |
| 영상 이름과 픽셀 혼합 | pass 명령 구간·pool 리소스 재사용 |
| HUD 미러 더미 그림 | 출력 namespace·alias·같은 index의 camera/drawable |
| 차체 일부 누락 / 가까운 검은 띠 | geometry subset / 픽셀 depth와 외판 메시 교차 |
| 먼 차량 누락 | LOD·도로 visibility·cut plane·pass 제출 목록 |
| 깊이·박스 불일치 | DSV·실제 projection·pass 시각의 렌더 모델 자세 |
| 프레임 저하 | 전경 PresentMon·hook 비용·readback·ROS 부하 |

RenderDoc 캡처는 일반 hook 실행과 분리합니다. Steam이 새 프로세스를 만들면 RenderDoc 로딩 여부를 확인하고 스크립트는 RenderDoc의 Python 환경에서 실행합니다.

[포팅 계획](../docs/20_windows_native_port_plan.md), [성능](../docs/18_performance.md), [중요한 시행착오](../docs/history/lessons.md).
