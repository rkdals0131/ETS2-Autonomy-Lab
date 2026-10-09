# ot — 게임 DLL과 개발 도구

core 0.22.0은 독립 카메라·RGB-D·GPU 라이다·차량 GT를 제공합니다. 현재 FH5 프리셋은 슬롯 3·4·6·7을 사용하고 기본 미러 0·1·2·5를 유지합니다. 일반 사용은 [브리지 런처](../bridge/README.md)에서 시작합니다.

## 빌드와 설치

```powershell
.\ot\build.cmd
```

x64 MSVC Build Tools, CMake·Ninja, 공식 SCS SDK가 필요합니다. `build.cmd`의 OT_VS와 CMake의 SCS_SDK·ETS2_EXE 경로를 설치 환경에 맞춥니다. 주소는 `schema/1.61.1.1/`에서 관리합니다.

| 파일 | 설치 위치·용도 |
| --- | --- |
| dist/ot_loader.dll | 게임 bin/win_x64/plugins/의 상주 SDK 플러그인 |
| dist/ot_core.dll | plugins/ot_runtime/의 교체 가능한 기능 DLL |
| dist/ot_config.json | plugins/ot_runtime/의 실행 권한·초기 상태 |
| dist/ot_ipc.dll | Python 프로세스의 shared-memory reader |

최초 설치는 게임을 종료한 상태에서 수행합니다. DX11 실행 후 SDK 알림을 확인하고 운전석으로 들어갑니다. 초기 Tier는 0입니다. 내부 접근에는 `singleplayer_research`, `allow_tier1`, 렌더에는 `allow_render_probe`, 리그에는 `allow_camera_rig` 설정이 필요합니다. 싱글플레이 연구용이며 멀티플레이에서는 플러그인을 제거합니다.

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

## Phase 1 고속도로 4뷰

[phase1-highway-private.json](presets/phase1-highway-private.json)과 [phase1-lidar-private.json](presets/phase1-lidar-private.json)을 함께 사용합니다. 장착 좌표와 해상도는 [FH5 리그](../docs/14_phase1_highway_sensors.md)에 있습니다.

전방은 선바이저 바깥에, 측면은 미러 하우징 뒤쪽에 부착했습니다. 네 카메라는 캐빈 서스펜션을 따릅니다. 자차 body는 엔진의 full-list 제출 경로를 사용합니다.

## 실시간 미리보기

```powershell
.\ot\ot.cmd preview --config .\ot\presets\phase1-highway-private.json --format rgbd8
.\ot\ot.cmd camera_rig apply --config .\ot\presets\phase1-highway-private.json --rig-only
.\ot\ot.cmd panic
```

Python 미리보기는 배치 실험용입니다. ROS 실시간 표시는 런처와 Foxglove를 사용합니다. 과거 슬롯 0·1·2·5 실험에는 `phase1-highway.json`을 유지합니다.

## 기록·오프라인 도구

```powershell
.\ot\ot.cmd record_bundles --config .\ot\presets\phase1-highway-private.json --vehicles --hz 10 --duration 5 --output '<새 출력 폴더>'
.\ot\ot.cmd lidar '<frame.tar.zst>' --config .\ot\presets\phase1-lidar-private.json --output lidar.npz
```

디렉터리·ZIP·TAR.ZST 묶음은 `otpy.bundles`에서 읽습니다. Zstandard 의존성은 `requirements-recording.txt`에 있습니다. 장시간 ROS 기록은 외장 SSD의 `~/Storage/ROS2_Workspace_offload/ETS2-Autonomy-Lab/`를 사용합니다.

### 오프라인 객체 박스 투영과 가림 판정

`otpy project_boxes`는 해당 pass의 카메라와 렌더 모델 자세로 차량 박스를 투영합니다. `otpy reconstruct`는 geometry DSV 또는 attributes Z를 선택해 복원합니다. 명령별 입력은 `ot.cmd <command> --help`에서 확인합니다. 이동 AI 2대·6시점에서 현재 자세의 RGB/DSV 정합이 ±0.5초 자세보다 높았습니다.

## IPC·시각

명령 pipe는 `\\.\pipe\ot`, 로더는 `\\.\pipe\ot_loader`입니다. SDK 상태는 `Local\OT_State`, 센서 묶음은 `Local\OT_Bundles`를 사용합니다. 슬롯은 원자적 소유권 전환 뒤 읽고 즉시 반환합니다. 공유 GPU stream 소비자는 fence 반환까지 담당합니다.

SDK frame_end는 물리 결과 이후이며 렌더 보간·장면 준비가 뒤따릅니다. 센서 묶음은 실제 pass Present ID를 기준으로 맞춥니다. [좌표·시각](../docs/04_sensors_and_data.md), [렌더 구조](../docs/12_dx11_mirror_render_path.md).

[현재 진행](../docs/13_game_operating_table.md), [성능](../docs/18_performance.md), [중요한 시행착오](../docs/history/lessons.md).
