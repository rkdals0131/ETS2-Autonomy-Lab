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
| tools/ | SCS packer·Extractor·ConverterPIX·RenderDoc |
| live/2026-10-08-render-path/ | 카메라·물리·가시성 disassembly와 표본 |
| live/2026-10-08-renderdoc/, live/2026-10-08-render-probe/ | frame3160 GPU 상수·깊이 복원, DLL 모델·픽셀 정합 |
| live/2026-10-09-side-artifact/ | 하우징 가림의 on/off 비교·메시 교차·수정 영상 |
| live/2026-10-09-optimization/ | 0.22.0 전경 성능 원본 |

이전 관측 파일은 기존 경로에 보존합니다. 신규 대용량 ROS 결과는 마운트와 여유 공간을 확인한 외장 SSD의 `~/Storage/ROS2_Workspace_offload/ETS2-Autonomy-Lab/<run>/`에 저장합니다. 추출 게임 자산·다운로드·제3자 소스·대용량 캡처는 Git에서 제외합니다.

## 도구와 취득 기록

- [SDK 1.15 ZIP](https://download.eurotrucksimulator2.com/scs_sdk_1_15.zip), 취득 SHA256 `77504f14d2ac1405ba70ee3a97351662adbb39c0cf9d3a085423f01d37bc28ec`.
- [SCS packer 1.55+](https://download.eurotrucksimulator2.com/scs_packer_1_55.zip), 취득 SHA256 `82f716a0261d1fd1582f2df30f536612a52ac267c74f1fd3c0f93115bf49d1ce`.
- [Extractor 2026-07-29](https://github.com/sk-zk/Extractor/releases/tag/2026-07-29), asset digest 대조 기록 `findings/extractor_download.json`.
- [ConverterPIX 3cd4e73a86d0](https://github.com/mwl4/ConverterPIX/tree/3cd4e73a86d0c6bd28e117664c50a36cabeccf38), 실행 파일 Git blob 대조·SHA256·LICENSE는 `tools/converterpix-3cd4e73a86d0/`의 취득 기록에 있습니다.

`inspect_renderdoc_capture.py`는 RenderDoc의 Python Scripting에서 실행합니다. 외부 읽기 도구 `read_live_memory.py`·`read_render_memory.py`는 해당 게임 빌드의 짧은 관측에 사용합니다. 실시간 수집·DLL 교체는 [ot 도구](../ot/README.md), 제품 의존성은 [THIRD_PARTY](../ot/THIRD_PARTY.md)에 있습니다.
