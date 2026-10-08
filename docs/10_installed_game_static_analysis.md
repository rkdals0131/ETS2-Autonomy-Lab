# 설치된 ETS2의 정적 분석

2026-10-08 조사. 게임을 실행하지 않고 설치 파일, 공식 SDK, 공개 플러그인 소스를 분석했다. 현재 빌드는 **1.61.1.1**이며, ETS2LA의 1.61용 Windows 패턴 11개가 실행 파일에서 모두 발견됐다. 카메라 관리자, 교통 관리자, GPS 구조체의 접근 지점과 기존 카메라 함수 후보까지 확보했다. 실제 객체 값과 렌더 타깃은 게임 실행 후 확인해야 한다.

이후 같은 날 사용자가 실행한 게임에서 읽기 전용 메모리 관측도 수행했다. 일반 객체 값과 미러 소유 경로를 확인했고, 이어 미러 4개의 실제 DX11 색상·깊이 리소스까지 연결했다. 픽셀 수집은 아직 하지 않았다. 실측 결과는 [런타임 데이터 조사](11_idle_memory_and_telemetry.md), 후속 렌더 구조는 [DX11 미러 경로](12_dx11_mirror_render_path.md)에 있다.

## 설치와 개발 환경

| 항목 | 직접 확인한 상태 |
| --- | --- |
| 프로젝트 | `C:\Users\kikiw\Desktop\ETS2-Autonomy-Lab`에 실제 존재. 조사 시작 시 문서만 있었음 |
| 게임 | `D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2` |
| 실행 파일 | `bin\win_x64\eurotrucks2.exe`, AMD64, 53,299,600 bytes |
| PE 제품 버전 | `1.61.1.1` |
| 전체 파일 버전 | `1.61.1.1 (6949e633e77902f7e023819d3131cc6ccce3707f)` |
| Steam | App 227300, build `25482642`, StateFlags `4`. `version.scs`도 `1.61.1.1` |
| 아카이브 | `.scs` 108개, 합계 약 35.790 GiB. 개별 목록은 `research/findings/archive_inventory.csv` |
| OS / CPU | Windows 11 Home `10.0.26200`, Ryzen 7 3700X |
| RAM / GPU | OS 보고 약 23.95 GiB, RTX 3060 Ti 8192 MiB, 드라이버 591.86 |
| Python | 3.13, 3.9, 2.7 설치. PATH의 `python`은 **2.7**이므로 조사 명령은 `py -3.13` 사용 |
| C++ | Visual Studio Build Tools 2026, MSVC 14.44와 14.50의 x64 도구 확인 |
| .NET | runtime 6–9는 있으나 SDK 없음. 현재 ETS2LA 전체를 바로 빌드하는 환경은 아님 |
| CMake | PATH와 확인한 VS 기본 CMake 경로에서 발견되지 않음. 설치 전체의 부재를 증명한 것은 아님 |
| 조사 시작 시 여유 | C 약 65.1 GiB, D 약 43.4 GiB. 대용량 영상 기록 드라이브는 아직 선택하지 않음 |

표준 Documents 경로 `C:\Users\kikiw\Documents\Euro Truck Simulator 2`와 확인한 OneDrive 후보에는 사용자 게임 폴더가 없었다. 다른 `-homedir` 사용 가능성은 남아 있으며, 활성 프로필·모드 순서·현재 그래픽 옵션을 확인했다고 볼 수 없다. DLC 파일의 존재는 해당 파일을 조사할 수 있다는 뜻이며, 계정 소유권이나 실제 활성화 상태를 판정한 것은 아니다.

## 이미 들어 있는 후킹 파일

`bin/win_x64`에는 `dxgi.dll`, `vrperfkit.yml`, `vrperfkit.log`가 있고, `plugins`에는 `Real_G27_ffb_x64.dll`, 설정과 로그가 있다. 조사한 x64 plugins 폴더에는 ETS2LA나 RenCloud 텔레메트리 DLL이 없다.

VR Performance Toolkit 로그는 **2022-02-22**에 작성된 과거 로그다. 당시 Oculus/D3D11 후킹과 FSR render scale 0.77을 기록하지만, 현재 1.61 빌드에서 그 구성이 작동한다는 근거는 아니다. DLL은 아직 설치 폴더에 있으므로 RenderDoc 등과 함께 로딩할 때 충돌 여부를 확인할 대상이다. 이 조사에서 이동·삭제·덮어쓰지 않았다.

현재 실행 파일에는 `D3D11CreateDevice`, `D3D12CreateDevice`, DX11/DX12 장치 구현 문자열과 `-rdevice` / `-r_device` 처리 문자열이 모두 있다. 이는 두 렌더 경로의 정적 단서다. 현재 선택된 API와 렌더 성공 여부는 첫 실행의 `game.log.txt`로 확인한다. import table에 DLL 이름이 없다는 이유로 동적 로딩되는 그래픽 API를 배제하면 안 된다.

## 아카이브에서 확보한 내용

원본 `.scs`는 읽기만 했고 결과는 `research/extracted/<archive>/`에 저장했다. 대형 텍스처 전체 덤프 대신 정의·카메라 UI·재질·모델 메타데이터를 중심으로 추출했다.

| 대상 | 확보 범위와 분석 결과 |
| --- | --- |
| `version.scs` | 패키지 버전과 플랫폼 정보 |
| `def.scs` | 공식 packer로 64,830개 파일 추출. 카메라, 물리, 도로, 교통, 신호, 차량 정의 |
| `base_cfg.scs` | 루트 디렉터리가 없어 공식 packer 실패. 커뮤니티 extractor의 deep mode로 3nK 내용을 해독했으며 언어 정의임을 확인 |
| `core.scs` | 공식 packer는 파일을 출력하지 못함. deep mode로 20개 복원, 일부 경로 이름은 `_unknown`으로 남음 |
| `base.scs` / `base_share.scs` | 환경 재질·텍스처 오브젝트와 관련 정의를 선택 추출 |
| `base_vehicle.scs` | mirror 재질·텍스처 및 interior `.pmd` 등 선택 추출 |
| 트럭 패키지 9개 | DAF 2021/XD/XF Electric, MAN TGX 2020, IVECO S-Way, Volvo FH 2021/2024, Scania S 2024E, Renault E-Tech의 `/def`, `/ui/dashboard` 텍스트 |
| `base_map.scs` | 전체 경로 목록과 `europe.mbd`, `sec+0000+0000`의 `.base/.aux/.data` 표본 |

SCS 공식 packer와 SDK는 공식 배포 서버에서 받았다. 선택 추출에는 [sk-zk Extractor](https://github.com/sk-zk/Extractor)의 `2026-07-29` Windows 배포를 사용했고 GitHub release의 SHA256 digest와 다운로드를 한 번 대조했다. 버전·소스 위치는 [조사 파일 안내](../research/README.md)에 있다.

## 미러에서 화면까지의 실제 연결

설치 파일에서 확인한 중심 경로는 다음과 같다.

```text
트럭 accessory와 interior 정의
  → 차량 모델의 카메라/미러 로케이터와 엔진 mirror_camera
  → *_mirror_reflection 재질/텍스처 이름
  → ui/dashboard/*_cam_mirrors.sii
  → 디지털미러 화면
```

이 중 UI → 재질 → TOBJ 파일 연결은 추출 파일로 확인했다. 모델 로케이터 → 런타임 카메라 객체 → 실제 GPU 이미지의 연결은 아직 실행 관찰이 필요하다. 추출된 DDS 파일은 정적인 파일이며 실시간 미러 프레임이 아니다.

주요 파일은 다음과 같다.

- `research/extracted/def/def/game_data.sii`: 기존 미러 FOV 배열 6개. close 55°, close small 90°, far 55°, far small 90°, side 60°, front 100°.
- `research/extracted/def/def/mirror_data.sii`: 크기와 FOV extension은 hood 포함 7개. FOV 배열과 개수가 같다고 가정하면 안 된다.
- `research/extracted/base_share/material/environment/far_mirror_reflection.mat`: `ui.rfx` 효과가 `far_mirror_reflection.tobj`를 참조. TOBJ는 동일 이름의 DDS를 참조.
- `research/extracted/dlc_daf_2021/ui/dashboard/daf_2021_cam_mirrors.sii`: ID 1620 그룹, close/far/main/small 재질 네 개.
- `research/extracted/dlc_daf_2021/ui/dashboard/daf_2021_front_cam_mirror.sii`: ID 1640, `front_mirror_reflection.mat` 사용.
- `research/extracted/dlc_volvo_fh_2024/ui/dashboard/volvo_fh_2024_cam_mirrors.sii`: ID 1620 그룹과 네 미러 화면.
- 각 트럭의 `def/vehicle/truck/<truck>/interior/mirrors.sui`: `mirror_camera_presets` 값.

| 차량 | main mirror의 UI bottom / top | small mirror의 UI bottom / top |
| --- | --- | --- |
| DAF 2021 | 0.08 / 0.92 | 0.0898 / 0.910 |
| Volvo FH 2024 | 0.04 / 0.96 | 0.12 / 0.88 |

따라서 디지털미러 화면을 캡처하면 원래 텍스처 전체와 다른 영역을 얻게 된다. RGB/depth와 내부 파라미터를 맞출 때 UI crop을 그대로 무시할 수 없다. 기존 조사에서 모드 예제로 사용한 `far_mirror_camera.mat`은 이번 설치 파일에서 확인한 `far_mirror_reflection.mat`과 이름이 다르다. 현재 빌드 조사는 위의 실제 파일에서 시작한다.

실행 파일에는 `mirror_camera_u`, `parking_camera_max_speed`, `mirror_camera_presets`, `r_mirror_scale_x/y`, `r_mirror_view_distance`, `r_deferred_mirrors` 문자열이 있다. 공식 [실내 UI 문서](https://modding.scssoft.com/wiki/Documentation/Engine/Truck_Interior_Animations_and_IDs)의 910–913은 최대 4개 주차 화면이며, 그 자체가 독립 동시 렌더 카메라 4개를 보장하지 않는다. 조사한 차량 정의에서는 해당 parking camera 설정과 정확한 910–913 화면 ID를 찾지 못했다. 따라서 주차 경로는 후보로 남기고, 이미 정의가 존재하는 DAF/Volvo 디지털미러를 첫 관찰 대상으로 삼는 편이 근거가 강하다.

[추가 미러에 대한 SCS 개발자 답변](https://forum.scssoft.com/viewtopic.php?p=1208872)과 [Sanax의 충돌 사례](https://forum.scssoft.com/viewtopic.php?t=323580)는 `cam_m_h`와 전역 mirror 정의의 연관을 설명한다. 현재 배열 끝에 값 하나만 추가하면 임의 센서가 생긴다고 해석하지 않는다.

## 메모리 접근 지점의 정적 대조

분석 대상은 ETS2LA/plugin commit `3b01d90b5be2469c0d94fcfdee4b354184ddeb02`다. CMake에 plugin 1.61.0, 대상 게임 1.61.x가 명시돼 있다. 실행 파일 `.text`를 읽어 Windows 패턴을 비교했고, 소스에서 사용하는 RIP 상대 주소와 멤버 오프셋을 계산했다.

RVA는 실행 모듈 시작점으로부터의 상대 주소다. 아래 숫자는 **현재 파일 전용 조사 위치**이며, ASLR이 적용되는 실제 프로세스의 절대 주소가 아니다. 전역 슬롯에는 실행 중 생성된 객체의 포인터가 들어간다. 오프라인에서는 그 객체를 읽을 수 없다.

| 대상 | 패턴 match RVA | 소스 방식으로 계산한 값 |
| --- | --- | --- |
| base controller | `0x9FFA20`, `0xA013EA` | 둘 다 전역 슬롯 `0x36AE6D8`, game actor 멤버 `+0x31B0` |
| camera manager | `0x3F9DFA` | 전역 슬롯 `0x36AE740` |
| traffic manager | `0x5277CC` | 전역 슬롯 `0x36AE728` |
| GPS manager | `0x5E1468`, `0x5E14C8` | 둘 다 controller 내 `+0x4128` |
| 주변 비 AI 차량 | `0x4729F0` | base controller 내 `+0x2DC8` |
| 주변 공간 항목 | `0x47AF2D` | base controller 내 `+0x650` |
| steering | `0x649319` | vehicle shared 내 `+0x690` |
| trailer | `0x5DDA6B` | physics vehicle 내 `+0x1580` |
| 다음 trailer | `0x8757F3` | trailer actor 내 `+0x1060` |
| input mix table | `0x10EE3C1` | 전역 슬롯 `0x27391E0` |
| input mix value | `0x10EE3E3` | vtable offset `0x48`, value member `+0x178` |

11개 패턴 중 9개는 한 위치, 2개는 두 위치에서 발견됐다. 중복된 두 패턴은 각각 같은 슬롯/멤버 값으로 해석된다. 이는 조사 진입점이 일관된다는 증거이며 모든 중첩 구조체·가상함수·런타임 수명이 맞는다는 증거는 아니다. 실제 플러그인도 모든 읽기 경로를 패턴으로 복원하는 것은 아니며 구조체 내 고정 offset을 사용한다.

결과: `research/findings/ets2la_pattern_matches.csv`, `ets2la_resolved_offsets.json`. 패턴 검색 재실행:

```powershell
py -3.13 .\research\inspect_binary.py
```

## 카메라 함수 후보

SPF commit `1ca67f04ec90727f3777eaedbd94ad7887a6bb95`의 `CameraHooks.cpp`에 있는 디스어셈블리 예시를 바탕으로 바이트 앵커를 대조했다. SPF 자체의 semantic pattern engine을 실행한 것은 아니다. MobileCam은 공개 코드의 원래 바이트 패턴을 사용했다.

| 후보 | 발견 위치 | 의미와 한계 |
| --- | --- | --- |
| SPF InitializeCamera | anchor `0x5DFDE2`, unwind 영역 시작 `0x5DFDD0` | 현재 게임 disassembly에서도 `0x36AE740` 슬롯을 읽고 camera manager를 사용 |
| SPF GetCameraObjectByID | `0x5015A0` | 기존 카메라 ID로 객체를 찾는 함수 후보 |
| SPF UpdateCameraProjection | `0x796B40` | 카메라 투영 갱신 후보 |
| MobileCam free camera tick | `0x54FAD0` | 기존 free camera의 갱신 경로 후보 |
| MobileCam camera manager | anchor `0x741D0F` | ETS2LA와 동일한 전역 슬롯 `0x36AE740` |

모두 `.text`에서 한 위치씩 발견됐다. `.pdata`의 unwind 영역은 분리된 코드 구간일 수 있어 전체 함수 끝으로 사용하지 않는다. 실제 MSVC `dumpbin`으로 초기화 후보 일부를 디스어셈블했고 `research/findings/camera_initialize_disassembly.txt`에 저장했다.

특히 이 코드에서는 manager `+0x14`에 인자 값을 쓴다. ETS2LA 헤더가 그 위치를 `total_camera_count`라고 부른다는 이유로 개수 필드라고 확정해서는 안 된다. 공개 구조체의 주석보다 실제 사용과 런타임 관찰을 우선한다.

이 후보들은 기존 카메라 접근을 앞당긴다. **운전석을 유지한 채 새 offscreen 뷰 4–6개를 생성·렌더하는 함수까지 확보한 것은 아니다.** 기존 카메라 선택, 객체 생성, 렌더 제출은 서로 다른 작업이다. MobileCam도 주 카메라 조작과 화면 캡처이며 독립 다중 센서 생성 사례가 아니다.

## 런타임 대조로 찾은 미러 카메라와 텍스처 구조

일반 `camera_manager`에 미러가 없다는 관측 후, 현재 EXE의 unit descriptor·vtable·미러 배열 assertion을 추적했다. 모든 주소는 이 빌드의 RVA이고 아래 코드는 실행하지 않고 읽기만 했다.

```text
module base + 0x36AE6D8 → base_ctrl pointer
  *(ctrl + 0x31B0) → game_actor
    *(actor + 0x20) → visual_interior
      interior + 0x13A8 → mirror camera array
        +0x08: data pointer, +0x10: size, +0x18: capacity
        data[index] → mirror_camera
          +0x20: FOV / near / far (3 float)
          +0x40: placement (32 bytes)
          +0x60: CPU projection (16 float)
  ctrl + 0xA8 → mirror texture descriptor array
    data[index] → descriptor, size 0x168
      +0x08 / +0x0C: requested/base width and height
      +0x38: generated description string pointer, including TOBJ alias
      +0x160: r_texture_id_t (후속 렌더 경로 분석으로 식별)
```

| 근거 | 확인한 위치와 역할 |
| --- | --- |
| `mirror_camera` 식별 | descriptor `0x1E1BA40`, getter `0x87DC50`, vtable `0x22C8540` |
| 객체 생성 | `0x87D930`이 해당 vtable을 기록. descriptor와 해제 코드의 객체 크기는 `0x548` |
| 미러 배열 | `0x546D20` 구간에서 소유자 `+0x13A8` 접근과 `array_t<owner_ptr_t<mirror_camera_u>>` assertion 확인 |
| 초기 pose 전달 | vtable의 `0x87DC60`에서 `+0x4A0` placement를 공통 `+0x40`으로 복사하고 projection 갱신 가상함수 호출. 이후 `0x87E170`은 내부 모드에 따라 현재 자세를 다시 계산하므로 고정 외부 파라미터의 근거가 아님 |
| 렌더 카메라로 설정 전달 후보 | `0x87FD70`에서 인자로 받은 다른 객체에 placement·FOV 관련 필드·far 값을 복사하고 quaternion 변환. 실제 GPU draw 함수라고 명명하지 않음 |
| 텍스처 descriptor 생성 | `0x473450`에서 `0x168` bytes 객체를 만들고 `0x15657A0`이 폭·높이를 `+8/+0xC`에 기록 |
| descriptor 소유권 | `0x547980` 구간의 `0x547EA1` 호출이 임시 9슬롯 배열을 controller `+0xA8`로 옮김. 대상 함수 `0x4902B0`의 포인터 이동도 대조 |
| 리소스 이름 | `0x1566310`의 description/alias 구성과 실제 메모리 문자열을 대조. `r_texture_object_t` pool의 이름 조회도 동일 이름을 반환 |

실제로 `visual_interior` 배열의 9슬롯 중 6개가 `mirror_camera`였고, descriptor는 7개였다. `close`, `close_s`, `far`, `far_s`, `side`, `front`, `hood` reflection TOBJ 이름을 읽었다. hood 카메라는 NULL이므로 descriptor 수를 뷰 개수로 세면 안 된다. 첫 여섯 FOV는 game_data 정의의 `55, 90, 55, 90, 60, 100`과 일치했다.

세부 증거는 `research/live/2026-10-08-idle/`의 `camera-type-locations.json`, `mirror-live-objects.json`, `mirror-update-access.txt`, `mirror-resource-owner.txt`, `mirror-texture-constructor.txt`, `mirror-texture-descriptors.json`에 있다. `mirror-object-search*.json`에는 초기 탐색의 실패 후보도 포함돼 있으며 최종 소유 경로의 근거로 사용하지 않는다.

이 구조는 기존 미러의 pose와 이름 있는 렌더 입력을 연결한다. **descriptor는 GPU 픽셀 버퍼가 아니며, 새 임의 카메라 생성이나 color/depth readback을 구현한 것은 아니다.** 후속 분석에서 렌더 제출 mask, drawable alias → rendergraph image → D3D11 resource 연결을 확인했다. 미러 0·2의 중간 색상과 depth 리소스 재사용도 관측했다. 자세한 값과 남은 픽셀 수집 작업은 [DX11 미러 경로](12_dx11_mirror_render_path.md)에 있다.

## 지도와 차량에서 실행 없이 얻는 정보

`base_map.scs` 경로 목록에서 기본 Europe의 `.base` sector 282개를 확인했다. 이는 해당 아카이브의 sector 수이며 설치된 전체 DLC 지도의 크기가 아니다. 실제 `europe.mbd`와 sector 표본 첫 32비트 값은 **907**이고, 확보한 [TruckLib](https://github.com/sk-zk/TruckLib) `Header.cs`도 지원 버전 907이다.

지도 분석의 결합 대상은 다음과 같다.

- `.base/.aux/.data`: 도로·prefab·노드와 배치 정보. `.mbd`만으로 차선 그래프가 완성되지 않는다.
- `def/world/road_look*.sii`: 양방향 차선 목록, 도로 offset, 관련 재질과 형상 정의.
- prefab `.ppd`: 교차로 내 곡선과 연결 정보. 도로 중심선을 그대로 차선 중심으로 쓰면 안 된다.
- runtime route의 node UID: 정적인 map node와 연결할 키. UID만으로 월드 좌표를 바로 얻는 것은 아니다.
- 국가·교통·신호 정의: 규칙과 profile. 실제 켜진 신호 상태와 주변 차량 위치는 런타임 정보다.

전체 도로 그래프 생성은 수행하지 않았다. 표본 형식 대조와 기존 파서의 진입점까지 확인했으며, 실제 모드·DLC mount 순서를 반영한 지도 로더 구현은 후속 개발이다. 기본 archive만 분석한 지도를 사용자의 활성 맵이라고 간주하지 않는다.

차량 정의에는 chassis, engine, transmission, cabin, interior, accessory 구성과 물리 계수가 있다. 예를 들어 Volvo FH 2024의 `4x2.sii`에는 두 축의 powered flag와 kerb weight가 있다. `def/vehicle/physics.sii`의 옛 `max_visual_rotation`은 1.58 이후 obsolete로 표시돼 있다. 주석의 과거 조향 각도를 현재 모든 트럭의 바퀴 최대각으로 고정하면 안 된다.

## 다음 실행으로 넘기는 경계

정적 자료로 준비된 것은 파일 위치, 데이터 종류, IPC 구조체, 패턴과 함수 후보, 단위와 좌표·시간 계약이다. 다음 항목은 파일 분석만으로 확정할 수 없다.

1. SDK 채널 등록 성공과 현재 프로필에서의 유효값.
2. 실행 시 할당되는 카메라·교통·차량 객체와 구조체의 실제 해석.
3. 미러의 GPU color/depth target, projection, viewport, 자원 재사용 시점.
4. 화면 밖 미러의 갱신, 동적 물체 누락과 RGB/depth 시간 정합.
5. 독립 정면 센서 및 4–6뷰의 생성 가능성과 성능.

최초 정적 조사에서는 게임 실행·프로세스 메모리 읽기를 수행하지 않았다. 이후 사용자 소유의 실행 프로세스에서 2번의 객체 접근과 미러 CPU projection·descriptor 일부를 확인했다. DLL 배치, 게임 설정·세이브 변경, 플러그인 빌드, GPU/FPS 측정은 수행하지 않았다. 최신 관측 범위는 [도로 상태 데이터](11_idle_memory_and_telemetry.md)를 기준으로 한다.
