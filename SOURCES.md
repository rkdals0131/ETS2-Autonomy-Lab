# 출처와 소스 탐색 위치

엔진 조사는 2026년 10월 8일 확보한 게임 빌드와 아래 소스 revision을 기준으로 합니다. 현재 제품의 SDK·렌더·ROS 계약과 중요한 조사 출처를 함께 보관합니다.

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
- [ReadProcessMemory](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-readprocessmemory), [OpenFileMappingW](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-openfilemappingw): 외부 읽기 및 기존 shared-memory 연결 계약.

ETS2LA/plugin은 MIT, SPF는 Apache-2.0 라이선스 파일을 확인했습니다. 참고 조사한 MobileCam과 PrismTextureStreamer의 확보한 루트에는 LICENSE 파일이 없었습니다. 제품에 포함한 의존성과 라이선스는 [THIRD_PARTY](ot/THIRD_PARTY.md)에 있습니다.

## SCS 공식 자료

| 출처 | 읽을 위치와 용도 |
| --- | --- |
| [도구와 SDK 목록](https://modding.scssoft.com/wiki/Documentation/Tools) | Telemetry & Input SDK, Extractor, Blender·Conversion Tools |
| [Game Archive Extractor](https://modding.scssoft.com/wiki/Documentation/Tools/Game_Archive_Extractor) | 실행 형식과 아카이브 버전 지원 |
| [ETS2 1.31 업데이트](https://blog.scssoft.com/2018/04/euro-truck-simulator-2-update-131-open.html) | 머리 위치 기반 FOV와 곡면 미러 |
| [미러 FOV 변환 설명](https://forum.scssoft.com/viewtopic.php?t=253252) | SCS 개발자 Max의 mirror_size·FOV 설명 |
| [추가 미러 설정 설명](https://forum.scssoft.com/viewtopic.php?p=1208872) | SCS 개발자의 cam_m_h와 대응 설정 |
| [실내 애니메이션과 UI ID](https://modding.scssoft.com/wiki/Documentation/Engine/Truck_Interior_Animations_and_IDs) | 디지털미러 1620~1650, 주차 화면 910~913 |
| [미러 갱신 문제](https://forum.scssoft.com/viewtopic.php?t=259895) | SCS 개발자 Komat의 가시성 검사 설명 |
| [미러 렌더 품질 관련 답변](https://forum.scssoft.com/viewtopic.php?start=3600&t=330624) | VR 논의 내 r_deferred_mirrors와 r_mirror_view_distance |
| [맵 에디터 기능](https://modding.scssoft.com/wiki/Documentation/Tools/Map_Editor/New_Editor_Features_info_-_old_%2B_1.47) | No mirror reflection 속성 |

## 모더의 경험과 카메라 코드

| 출처 | 읽을 위치와 용도 |
| --- | --- |
| [Sanax 추가 카메라 사례](https://forum.scssoft.com/viewtopic.php?t=323580) | 조수석 코너 카메라와 데이터 덮어쓰기 충돌 보고 |
| [ets2_nav_mod](https://github.com/thePromisedKing/ets2_nav_mod) | 미러 영상을 대시보드에 표시하는 모드 |
| [대시보드 SII](https://github.com/thePromisedKing/ets2_nav_mod/blob/main/src/ui/dashboard/volvo_fh_2024_gps.sii) | far_mirror_camera와 far_s_mirror_camera 실제 참조 |
| [SPF Camera API](https://github.com/TrackAndTruckDevs/SPF-Framework/blob/main/docs/api/SPF_Camera_API.md) | 기존 카메라 제어 API |
| [SPF Camera Manager](https://github.com/TrackAndTruckDevs/SPF-Framework/blob/main/src/GameCamera/GameCameraManager.cpp) | 객체 조회·초기화 호출 구현 |
| [텍스처 큐 구현](https://github.com/Baldywaldy09/PrismTextureStreamer/blob/main/PrismTextureStreamerFB/prism/memserver_texture_queue.cpp) | 엔진 큐의 리소스 경로 처리 |

## 그래픽과 데이터 전달

| 출처 | 용도 |
| --- | --- |
| [RenderDoc 캡처와 호출 스택](https://github.com/baldurk/renderdoc/blob/v1.x/docs/window/capture_attach.rst) | API 호출 주소를 엔진 코드로 연결 |
| [RenderDoc texture viewer](https://github.com/baldurk/renderdoc/blob/v1.x/docs/window/texture_viewer.rst) | 텍스처와 사용 이력 조사 |
| [D3D11 공유 리소스](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_resource_misc_flag) | 공유 생성 플래그와 지원 조건 |
| [D3D11 스레드 사용](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro) | device와 context의 차이 |
| [Foxglove Windows 다운로드](https://foxglove.dev/download) | Windows 앱 배포 |
| [MCAP 시작 안내](https://mcap.dev/guides/getting-started) | 메시지·스키마·로컬 파일 재생 |

## ROS와 센서 규약

| 출처 | 용도 |
| --- | --- |
| [REP 145](https://github.com/ros-infrastructure/rep/blob/master/rep-0145.rst) | IMU 축·단위·specific force |
| [NavSatFix](https://github.com/ros2/common_interfaces/blob/jazzy/sensor_msgs/msg/NavSatFix.msg) | WGS84 위치·ENU 공분산 |
| [NavSatStatus](https://github.com/ros2/common_interfaces/blob/jazzy/sensor_msgs/msg/NavSatStatus.msg) | GNSS 상태·서비스 비트 |
| [Foxglove 3D](https://docs.foxglove.dev/docs/visualization/panels/3d#point-cloud) | 점군 필드·거리 색상 |
