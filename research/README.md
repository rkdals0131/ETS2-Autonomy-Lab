# 로컬 조사 자료

2026-10-08, ETS2 1.61.1.1에 대한 오프라인 조사 및 사용자 수동 주행·도로 정차 중 관측 자료다. 외부 읽기 조사 요약은 [정적 분석과 미러 구조](../docs/10_installed_game_static_analysis.md), [도로 상태 데이터와 실측](../docs/11_idle_memory_and_telemetry.md)에 있다. 이후 SDK DLL 설치와 RenderDoc 준비 과정의 게임 폴더 변경·백업 현황은 [ot 사용법](../ot/README.md)에 기록한다. 게임 `.scs` 원본은 변경하지 않았다.

| 위치 | 내용 |
| --- | --- |
| `live/2026-10-08-render-probe/` | 실제 hook 관측·패닉·언로드 기록. 0.4.1 mirror5 첫 픽셀, 0.5.0의 5초·50표본·10 Hz 기록과 Present 간격. raw·NPY·RGB/Z/재질 그림. 세 hook 코드 복원과 SDK 언로드 확인 |
| `ets2-mirrors.cap`, `open_renderdoc.cmd` | RenderDoc 1.46의 DX11 캡처 설정과 사용자 실행 진입점. Steam 재실행 문제 해결 후 실제 frame 3160 캡처 성공 |
| `live/2026-10-08-renderdoc/` | 보존한 RDC·화면 PNG·XML 명령·RT 바인딩과 호출 스택 분석. `export_frame3160.py`의 실제 replay로 4뷰의 원시 텍스처 12개 확보, `convert_pixels.py`로 NPY·Z·재질 bits·비교 그림 변환 및 시각 확인. `export_frame3160_constants.py`로 16개 draw의 shader·상수·viewport 추출, `reconstruct_pixels.py`로 4뷰 카메라 공간 점군 NPY·PLY 생성 및 확인 |
| `inspect_renderdoc_capture.py` | RenderDoc에서 열린 RDC의 action 트리·출력 텍스처·API 호출 스택을 JSON으로 추출. Python Scripting 패널에서 Load → Run. 구문 확인만 완료; 실제 RDC 실행은 미확인. 결과는 `live/2026-10-08-renderdoc/`에 저장하며 픽셀 추출이나 미러 자동 식별을 주장하지 않음 |
| `read_render_memory.py` | 외부 읽기 전용 렌더 그래프·DX11 Texture2D 메타데이터 수집. 최대 30초·10Hz. 이 EXE와 로컬 d3d11.dll layout 전용이며 픽셀·COM 호출 없음 |
| `live/2026-10-08-render-path/` | 미러 제출·alias·rendergraph·DX11 pool 추적. render-graph.jsonl, summary.json, 실제 color/depth 리소스 및 재사용 관측. 해석은 [DX11 미러 경로](../docs/12_dx11_mirror_render_path.md) |
| `findings/effect_paths.txt`, `findings/deferred_shader_mapping.json` | effect.scs 경로 및 선택 효과의 SM5X shader 연결 |
| `extracted/effect/`, `extracted/effect-analysis/` | deferred 효과와 shader 선택 추출, DXBC 분리·fxc disassembly·GLSL 대조. attributes_0의 Z/normal 해석에 사용 |
| `read_live_memory.py` | 조사한 로컬 빌드용 외부 ReadProcessMemory 도구. 최대 60초·5Hz, 실제 관측 2Hz. `--world`로 신호·주차 차량, `--mirrors`로 미러 pose/projection 추가. 게임 함수 호출·주입·쓰기 없음 |
| `live/2026-10-08-readonly/` | 실제 메모리 표본 JSONL, summary.json, 모듈 목록, 게임 로그 발췌. 다른 tick 값이 섞일 수 있는 관측 자료 |
| `live/2026-10-08-idle/` | 정차 중 월드 60초, 미러 포함 30초, AI 전체 배열 마지막 5초 기록. summary.json, world-observation.png, mirror-parameters.csv, 미러 descriptor·이름·disassembly. 초기 탐색 실패 후보도 포함 |
| `tools/scs_packer.exe` | SCS 공식 1.55+ packer, 조사용 추출에 사용 |
| `tools/extractor/extractor.exe` | sk-zk Extractor 2026-07-29, 선택 추출·3nK decode에 사용 |
| `sdk/` | 공식 SDK 1.15 헤더, 예제, readme, 라이선스 |
| `sources/` | GitHub commit을 고정해 받은 참고 소스. submodule dependency까지 빌드용으로 설치한 것은 아님 |
| `extracted/` | 원본 게임 아카이브에서 추출한 조사 대상. 공개 배포용이 아님 |
| `findings/source_revisions.json` | 각 공개 소스의 commit·원본 URL |
| `findings/sdk_1_15_channels.csv` | SDK channel 정의 108개, 타입·원본 헤더 위치 |
| `findings/archive_inventory.csv` | 설치 아카이브 이름·크기·헤더 버전 |
| `findings/binary_summary.json` | PE section과 import 정보, ETS2LA 패턴 결과 |
| `findings/ets2la_pattern_matches.csv` | 현재 실행 파일의 패턴 match 위치 |
| `findings/ets2la_resolved_offsets.json` | 소스의 주소 계산 방식으로 해석한 슬롯·멤버 offset |
| `findings/camera_function_candidates.json` | SPF/MobileCam의 카메라 함수 앵커 후보 |
| `findings/camera_initialize_disassembly.txt` | MSVC dumpbin으로 읽은 초기화 후보의 일부 코드 |
| `findings/binary_camera_render_strings.csv` | 카메라·렌더 관련 실행 파일 문자열 위치 |
| `findings/base_map_extraction.txt` | base_map 전체 경로 목록 |
| `downloads/` | 사용한 도구·SDK 원본 ZIP |

공식 ZIP URL:

- [SCS SDK 1.15](https://download.eurotrucksimulator2.com/scs_sdk_1_15.zip)
- [SCS packer 1.55+](https://download.eurotrucksimulator2.com/scs_packer_1_55.zip)
- [Extractor Windows 2026-07-29](https://github.com/sk-zk/Extractor/releases/download/2026-07-29/extractor-2026-07-29-win-x64.zip)

공식 SDK ZIP의 취득 시 SHA256은 `77504f14d2ac1405ba70ee3a97351662adbb39c0cf9d3a085423f01d37bc28ec`, packer ZIP은 `82f716a0261d1fd1582f2df30f536612a52ac267c74f1fd3c0f93115bf49d1ce`다. 공식 서버에서 받은 파일의 식별 기록이며 별도 게시 digest와의 대조 주장은 아니다. Extractor는 GitHub asset digest와 대조했고 값은 `findings/extractor_download.json`에 있다.

선택 추출 예:

```powershell
.\research\tools\extractor\extractor.exe 'D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2\dlc_volvo_fh_2024.scs' --quiet --partial=/def,/ui/dashboard --filter=*.sii,*.sui --dest=research/extracted/dlc_volvo_fh_2024
```

도구 실행은 항상 별도 출력 폴더에서 끝나는 유한 작업으로 사용한다. 추가 센서 데이터나 GPU 캡처를 이 폴더에 무제한 저장하지 않는다. 추출된 게임 파일·다운로드·제3자 소스는 프로젝트 `.gitignore`로 제외했다. 필요한 소스를 제품에 편입할 때 원저작권·라이선스를 보존한다.
