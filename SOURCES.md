# 출처와 소스 탐색 위치

조사 기준일은 2026년 10월 8일입니다. 공식 문서는 기능의 공개 계약을, 소스는 해당 revision의 구현을, 모더의 글은 경험과 조사 단서를 제공합니다. 오래된 성공 사례가 현재 게임 빌드의 호환성을 보장하지는 않습니다.

설치 파일에 대조한 후속 결과는 [정적 분석](docs/10_installed_game_static_analysis.md)과 [도로 정차 데이터](docs/11_idle_memory_and_telemetry.md)에 있습니다. 이 조사에서는 아래 revision의 소스를 로컬에 확보했습니다.

| 소스 | 고정 revision과 확인 범위 |
| --- | --- |
| [ETS2LA/plugin](https://github.com/ETS2LA/plugin/tree/3b01d90b5be2469c0d94fcfdee4b354184ddeb02) | `3b01d90b5be24` — 1.61 target, Windows 패턴, core, traffic, memory ABI |
| [ETS2LA/ETS2LA](https://github.com/ETS2LA/ETS2LA/tree/cf0fc395b0d093036a61bc765a1d7b65844ea6d2) | `cf0fc395b0d0` — C# camera/route consumer와 SDK 전달 경로 |
| [RenCloud/scs-sdk-plugin](https://github.com/RenCloud/scs-sdk-plugin/tree/d7216cb16c178be34feb04d9d6f67dd8e71ab8de) | `d7216cb16c17` — 공식 SDK → shared memory, revision 12 구조 |
| [SPF-Framework](https://github.com/TrackAndTruckDevs/SPF-Framework/tree/1ca67f04ec90727f3777eaedbd94ad7887a6bb95) | `1ca67f04ec90` — camera hooks, manager와 data finder 진입점 |
| [ETS2MobileCam](https://github.com/Baldywaldy09/ETS2MobileCam/tree/3e9cd29f03da29f10589512d3cd4e25b75bd265c) | `3e9cd29f03da` — freecam tick/manager 패턴과 화면 캡처 방식 |
| [PrismTextureStreamer](https://github.com/Baldywaldy09/PrismTextureStreamer/tree/fdf7a874f1bc7bf956ad9cb96ff84e3f0143a6e4) | `fdf7a874f1bc` — 외부 화면 → 게임 텍스처 방향. 독립 센서 출력과 구분 |
| [TruckLib](https://github.com/sk-zk/TruckLib/tree/c42dbe4f2d6bfb7b87c9ff1199c3cf5b7d1e27b0) | `c42dbe4f2d6b` — map format 907, map/road 파서 진입점 |
| [TruckLib.HashFs](https://github.com/sk-zk/TruckLib.HashFs/tree/5e33031db281f5a9e5ab880772560cd496a4325d) | `5e33031db281` — HashFS v2 reader와 선택 추출 도구 조사 |

추가 1차 자료:

- [공식 SDK 1.15 ZIP](https://download.eurotrucksimulator2.com/scs_sdk_1_15.zip): 배포판 안의 헤더·타입·시간 계약을 직접 읽음.
- [sk-zk Extractor](https://github.com/sk-zk/Extractor): 2026-07-29 standalone release로 설치 아카이브를 선택 추출.
- [SDK 채널 문서에 대한 SCS 답변](https://forum.scssoft.com/viewtopic.php?t=240843): 헤더를 목록의 기준으로 안내.
- [제한속도 특수값에 대한 SCS 답변](https://forum.scssoft.com/viewtopic.php?t=186527): 2015년 당시 설명이며 현재 버전의 완전한 상태표는 아님.
- [ReadProcessMemory](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-readprocessmemory), [OpenFileMappingW](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-openfilemappingw): 외부 읽기 및 기존 shared-memory 연결 계약.

ETS2LA/plugin은 MIT, SPF는 Apache-2.0 라이선스 파일을 확인했습니다. MobileCam과 PrismTextureStreamer의 확보한 루트에는 LICENSE 파일이 없었습니다. 이들은 참고 조사 자료이며 제품 코드에 편입한 것이 아닙니다.

본문의 주요 주장에는 직접 링크를 붙였습니다. 아래 목록은 후속 작업에서 다시 찾아갈 위치입니다. 저장소 전체를 검증했다는 뜻은 아닙니다.

## 기존 자율주행 프로젝트

| 출처 | 읽을 위치와 용도 |
| --- | --- |
| [marsauto Europilot](https://github.com/marsauto/europilot) | README의 Linux 화면 캡처·입력과 커스텀 카메라 로드맵 |
| [ETS2LA 현재 저장소](https://github.com/ETS2LA/ETS2LA) | README, C# 프로젝트 구성, TruckLib 관계 |
| [ETS2LA 개발 문서](https://docs.ets2la.com/) | 현재 플러그인 개발 안내의 시작점 |
| [ETS2LA 게임 플러그인](https://github.com/ETS2LA/plugin) | 공유 메모리 채널의 개요. README 예제는 실제 ABI와 대조 |
| [core.cpp](https://github.com/ETS2LA/plugin/blob/main/src/core.cpp) | 카메라·보간 자세·입력·패턴 탐색·버전 검사 |
| [core.hpp](https://github.com/ETS2LA/plugin/blob/main/src/core.hpp) | 실제 packed 구조체와 크기. InputMemData 차이 확인 |

## SCS 공식 자료

| 출처 | 읽을 위치와 용도 |
| --- | --- |
| [도구와 SDK 목록](https://modding.scssoft.com/wiki/Documentation/Tools) | Telemetry & Input SDK, Extractor, Blender·Conversion Tools |
| [Game Archive Extractor](https://modding.scssoft.com/wiki/Documentation/Tools/Game_Archive_Extractor) | 실행 형식과 아카이브 버전 지원 |
| [ETS2 1.31 업데이트](https://blog.scssoft.com/2018/04/euro-truck-simulator-2-update-131-open.html) | 머리 위치 기반 FOV와 곡면 미러 |
| [미러 FOV 변환 설명](https://forum.scssoft.com/viewtopic.php?t=253252) | SCS 개발자 Max의 mirror_size·FOV 설명 |
| [추가 미러 설정 설명](https://forum.scssoft.com/viewtopic.php?p=1208872) | SCS 개발자의 cam_m_h와 대응 설정 |
| [실내 애니메이션과 UI ID](https://modding.scssoft.com/wiki/Documentation/Engine/Truck_Interior_Animations_and_IDs) | 디지털미러 1620~1650, 주차 화면 910~913 |
| [Coaches 차량 기능](https://blog.scssoft.com/2026/07/coaches-new-vehicle-features.html) | 개발 중 기능 소개의 후방 카메라. 출시·API 보장과 구분 |
| [미러 갱신 문제](https://forum.scssoft.com/viewtopic.php?t=259895) | SCS 개발자 Komat의 가시성 검사 설명 |
| [미러 렌더 품질 관련 답변](https://forum.scssoft.com/viewtopic.php?start=3600&t=330624) | VR 논의 내 r_deferred_mirrors와 r_mirror_view_distance |
| [맵 에디터 기능](https://modding.scssoft.com/wiki/Documentation/Tools/Map_Editor/New_Editor_Features_info_-_old_%2B_1.47) | No mirror reflection 속성 |
| [multimon](https://eurotrucksimulator2.com/multimon_config.php) | 디스플레이 뷰 설정과 주 카메라 관련 조정 |

## 모더의 경험과 카메라 코드

| 출처 | 읽을 위치와 용도 |
| --- | --- |
| [Sanax 추가 카메라 사례](https://forum.scssoft.com/viewtopic.php?t=323580) | 조수석 코너 카메라와 데이터 덮어쓰기 충돌 보고 |
| [후방 로케이터 문제 질문](https://forum.scssoft.com/viewtopic.php?p=2149374) | 해결 미확인의 조사 단서 |
| [ets2_nav_mod](https://github.com/thePromisedKing/ets2_nav_mod) | 미러 영상을 대시보드에 표시하는 모드 |
| [대시보드 SII](https://github.com/thePromisedKing/ets2_nav_mod/blob/main/src/ui/dashboard/volvo_fh_2024_gps.sii) | far_mirror_camera와 far_s_mirror_camera 실제 참조 |
| [SPF Camera API](https://github.com/TrackAndTruckDevs/SPF-Framework/blob/main/docs/api/SPF_Camera_API.md) | 기존 카메라 제어 API |
| [SPF Camera Manager](https://github.com/TrackAndTruckDevs/SPF-Framework/blob/main/src/GameCamera/GameCameraManager.cpp) | 객체 조회·초기화 호출 구현 |
| [ETS2MobileCam](https://github.com/Baldywaldy09/ETS2MobileCam) | 작성자의 카메라 구조체와 업데이트 후킹 설명 |
| [PrismTextureStreamer](https://github.com/Baldywaldy09/PrismTextureStreamer) | 외부 앱 화면을 게임 텍스처에 공급 |
| [텍스처 큐 구현](https://github.com/Baldywaldy09/PrismTextureStreamer/blob/main/PrismTextureStreamerFB/prism/memserver_texture_queue.cpp) | 엔진 큐의 리소스 경로 처리 |
| [Prism3D Unit Resolver](https://github.com/Baldywaldy09/x64dbgPrism3DUnitResolver) | 추가 디버거 도구 후보. 내부 구현 미검증 |
| [ets2-data-capture](https://github.com/dmariaa/ets2-data-capture) | 색상·depth 추출 참고 |
| [ETS2 1.61 DX12 조사](https://github.com/NemoByteCore/ets2-1.61-stutter-investigation) | 작성자의 렌더 경로 분석. 성능 진단 전체 미검증 |

## 그래픽과 데이터 전달

| 출처 | 용도 |
| --- | --- |
| [RenderDoc 캡처와 호출 스택](https://github.com/baldurk/renderdoc/blob/v1.x/docs/window/capture_attach.rst) | API 호출 주소를 엔진 코드로 연결 |
| [RenderDoc texture viewer](https://github.com/baldurk/renderdoc/blob/v1.x/docs/window/texture_viewer.rst) | 텍스처와 사용 이력 조사 |
| [D3D11 공유 리소스](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_resource_misc_flag) | 공유 생성 플래그와 지원 조건 |
| [D3D11 스레드 사용](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro) | device와 context의 차이 |
| [CUDA D3D11 interop](https://docs.nvidia.com/cuda/cuda-runtime-api/cuda_runtime_api/group__CUDART__D3D11.html) | 등록·포맷·수명·매핑 조건 |
| [Foxglove Windows 다운로드](https://foxglove.dev/download) | Windows 앱 배포 |
| [Foxglove SDK](https://docs.foxglove.dev/docs/sdk) | 실시간 시각화와 MCAP 기록 |
| [MCAP 시작 안내](https://mcap.dev/guides/getting-started) | 메시지·스키마·로컬 파일 재생 |

## Windows 도구

| 출처 | 용도 |
| --- | --- |
| [Visual Studio와 Build Tools](https://visualstudio.microsoft.com/downloads/) | C++ x64 빌드 |
| [Git for Windows](https://git-scm.com/downloads/win) | 버전 관리 도구 |
| [CMake](https://cmake.org/download/) | 저장소가 요구하는 빌드 버전 선택 |
| [Python Windows 배포](https://www.python.org/downloads/windows/) | 사용할 ML 스택에 맞는 Python |
| [PyTorch 설치 선택기](https://pytorch.org/get-started/locally/) | Windows·CUDA·Python 조합 |
| [x64dbg](https://x64dbg.com/) | Windows 디버거 |
| [Ghidra](https://github.com/NationalSecurityAgency/ghidra) | 역공학 도구와 설치 요구사항 |
| [RenderDoc 소스와 배포 안내](https://github.com/baldurk/renderdoc) | GPU 프레임 캡처 도구 |

## AC 후속 자료

| 출처 | 읽을 위치 |
| --- | --- |
| [CSP Lua SDK](https://github.com/ac-custom-shaders-patch/acc-lua-sdk) | 스크립트 종류와 API |
| [lib_scene.lua](https://github.com/ac-custom-shaders-patch/acc-lua-sdk/blob/main/lib_scene.lua) | GeometryShot 생성·update·depth |
| [ac_ray.lua](https://github.com/ac-custom-shaders-patch/acc-lua-sdk/blob/main/common/ac_ray.lua) | track·scene·cars·physics raycast |

## 후속 조사 방식

웹 자료로 해결할 수 있는 다음 작업은 선택한 저장소의 빌드·라이선스·지원 버전과 실제 헤더를 읽는 것입니다. 게임 파일을 확보하면 카메라·재질·로케이터 정의를 실제 빌드에서 찾습니다. GPU 리소스 연결, 생성 함수와 성능은 런타임에서 해결합니다.

한 주장이 공식 지원인지, 특정 버전의 소스 동작인지, 작성자의 보고인지 유지합니다. 직접 실험한 결과가 생기면 해당 문서의 미확인 항목을 구체적인 관찰로 교체합니다.
