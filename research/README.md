# 로컬 조사 자료

ETS2 1.61.1.1의 설치 파일·SDK·엔진 분석 원본입니다. 현재 해석은 [설치 분석](../docs/10_installed_game_static_analysis.md), [물리 상태](../docs/11_idle_memory_and_telemetry.md), [DX11 렌더](../docs/12_dx11_mirror_render_path.md)에 있습니다.

| 위치 | 용도 |
| --- | --- |
| sdk/ | 공식 SDK 1.15 헤더·예제·라이선스 |
| sources/, findings/source_revisions.json | 참고 소스와 고정 commit·원본 URL |
| findings/sdk_1_15_channels.csv | SDK 채널·타입·헤더 위치 |
| findings/archive_inventory.csv, binary_summary.json, ets2la_* | 아카이브·PE·패턴·주소 조사 |
| extracted/ | 정의·UI·재질·셰이더·차량 메시 선택 추출 |
| extracted/fh5-mount-models/, live/2026-10-08-camera-rig/ | FH5 로케이터·표면 마운트·SDK와 모델 좌표 정렬 |
| extracted/scania-s-mount-models/, scania-s-mount-pix/, live/2026-10-14-scania/ | Scania S 캐빈·미러·선바이저 원본과 ConverterPIX 변환, SDK 트럭 구성·native 미러 위치, 장착 확인 영상 |
| tools/ | SCS packer·Extractor·ConverterPIX·RenderDoc·Ghidra |
| ghidra/ | 실행 파일 Ghidra 프로젝트(`projects/`, Git 제외)와 조회 스크립트 `ghidra.py`·`scripts/`([27](../docs/27_main_thread_parallel_research.md#ghidra-영구-프로젝트)) |
| rebase_rvas.py | 게임 업데이트 뒤 주소 옮기기(아래) |
| live/2026-10-08-render-path/ | 카메라·물리·가시성 disassembly와 표본 |
| live/2026-10-08-renderdoc/, live/2026-10-08-render-probe/ | frame3160 GPU 상수·깊이 복원, DLL 모델·픽셀 정합 |
| live/2026-10-09-side-artifact/ | 하우징 가림의 on/off 비교·메시 교차·수정 영상 |
| live/2026-10-09-optimization/ | 0.22.0 전경 성능 원본 |

이전 관측 파일은 기존 경로에 보존합니다. frame3160 전용 추출·복원 스크립트는 Git 기록에서 복구할 수 있으며, 같은 기능의 현재 수집·오프라인 처리는 ot를 사용합니다. 신규 대용량 ROS 결과는 마운트와 여유 공간을 확인한 외장 SSD의 `~/Storage/ROS2_Workspace_offload/ETS2-Autonomy-Lab/<run>/`에 저장합니다. 추출 게임 자산·다운로드·제3자 소스·대용량 캡처는 Git에서 제외합니다.

## 도구와 취득 기록

- [SDK 1.15 ZIP](https://download.eurotrucksimulator2.com/scs_sdk_1_15.zip), 취득 SHA256 `77504f14d2ac1405ba70ee3a97351662adbb39c0cf9d3a085423f01d37bc28ec`.
- [SCS packer 1.55+](https://download.eurotrucksimulator2.com/scs_packer_1_55.zip), 취득 SHA256 `82f716a0261d1fd1582f2df30f536612a52ac267c74f1fd3c0f93115bf49d1ce`.
- [Extractor 2026-07-29](https://github.com/sk-zk/Extractor/releases/tag/2026-07-29), asset digest 대조 기록 `findings/extractor_download.json`.
- [Ghidra 12.1.4](https://github.com/NationalSecurityAgency/ghidra/releases/tag/Ghidra_12.1.4_build), `ghidra_12.1.4_PUBLIC_20260921.zip` 취득 SHA256 `ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db`(릴리스 공지 값과 일치). JDK 21 이상이 필요하고 이 머신은 JDK 23을 씁니다.
- [ConverterPIX 3cd4e73a86d0](https://github.com/mwl4/ConverterPIX/tree/3cd4e73a86d0c6bd28e117664c50a36cabeccf38), 실행 파일 Git blob 대조·SHA256·LICENSE는 `tools/converterpix-3cd4e73a86d0/`의 취득 기록에 있습니다.

## 게임 업데이트 뒤 주소 옮기기

Steam이 실행 파일을 바꾸면 core의 해시 게이트가 Tier 1을 끕니다. 처음부터 다시 분석하지 않고 이전 빌드의 분석 결과를 옮깁니다.

```powershell
py -3 research/ghidra/ghidra.py image                       # 업데이트 전에 분석한 빌드의 이미지 덤프(프로젝트에서 읽기만 함)
# ot/schema/<새 버전>/game.json 작성(game_version, exe_sha256)
py -3 research/rebase_rvas.py --new-version <새 버전> --analyzed   # 표 확인, research/ghidra/out/<새 버전>/rebase.json
py -3 research/rebase_rvas.py --new-version <새 버전> --write [--accept <RVA>=<확인 내용>]
.\ot\build.cmd -DOT_GAME_VERSION=<새 버전>
```

- `rebase_rvas.py`는 스키마의 `*_rva`와 `ot/native`의 주소 리터럴을 옮깁니다. 함수는 본문 바이트 창 투표로 찾은 뒤 capstone으로 명령어 단위 비교를 하고, 분기·RIP 상대·이미지 상대 대상을 정규화해 "이동만 있음"을 판정합니다. 이 판정이 나면 함수 안의 구조체 오프셋도 그대로입니다. 데이터는 이전 코드의 참조를 새 코드에서 찾아 새 변위로 옮기고, vtable(이 게임은 RTTI가 없음)은 슬롯 함수 대응으로 확인합니다.
- `--analyzed`는 `ghidra/out/decomp/`의 근거 함수 전체를 비교해 구조체 레이아웃 변화를 찾습니다. `--write`는 사용 중인 주소가 하나라도 확정되지 않으면 거절합니다. 손으로 확인한 것만 `--accept`에 이유와 함께 적으며, 그 기록이 `ot/schema/<새 버전>/rebase.json`에 남습니다.
- 문서(docs/)의 주소는 발견한 빌드 기준입니다. 현재 빌드 주소는 그 `rebase.json`으로 찾습니다.
- 새 빌드를 Ghidra로 디컴파일하려면 `ghidra.py --version <새 버전> import`(30분 정도, 메모리 8 GB 이상)가 필요합니다. 주소 옮기기 자체에는 필요 없습니다.

1.61.1.9(2026-10-15 업데이트) 결과입니다.
- 사용 중인 주소 82개 중 80개가 자동 확정이고, 나머지 2개(카메라 리그의 호출 복귀 주소)는 디스어셈블로 확인했습니다. `.text`는 43개 구간으로 나뉘어 +0x70~+0x1E80 이동했고, `.data`·`.rdata` 항목도 균일하지 않게(+0xFA0~+0x1408) 이동했습니다.
- 근거 함수 140개 중 136개는 이동만 있습니다. 0x4CFDB0(뷰 준비, core는 쓰지 않음)은 호출 몇 개가 빠졌고, 0xECD610·0x147B0E0은 읽는 상수 하나씩의 값이 다릅니다. 구조체에 걸린 실제 변경은 0x4D0D80에서 드러난 차량 객체 레이아웃입니다. 0x688까지는 그대로이고 0x690부터 +0x28입니다(바이너리 전체에서 0x690 이상 316곳이 일관되게 이동). 스키마가 쓰는 차량 오프셋(body 0x30, origin shift 0x464)은 경계 아래라 그대로입니다.
- 1.61.1.1 스키마의 `parallel_loop_vtable_rva`(0x21EC888)는 iostream 문자열 안을 가리키는 오타였고, 코드와 디컴파일이 쓰는 0x21EC488로 고쳤습니다.

`inspect_renderdoc_capture.py`는 RenderDoc의 Python Scripting에서 실행합니다. 외부 읽기 도구 `read_live_memory.py`·`read_render_memory.py`는 해당 게임 빌드의 짧은 관측에 사용합니다. 실시간 수집·DLL 교체는 [ot 도구](../ot/README.md), 현재 지도 provider는 `tools/dotnet10`과 `sources/TruckLib` 계열의 고정 소스를 사용합니다. 제품 의존성은 [THIRD_PARTY](../ot/THIRD_PARTY.md)에 있습니다.
