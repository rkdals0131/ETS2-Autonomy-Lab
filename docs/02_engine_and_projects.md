# ETS2 엔진과 공개 프로젝트 조사

기존 자료는 상태 접근, 카메라 제어, 영상 추출, 추가 뷰 생성으로 나눠 읽어야 합니다. 한 기능을 제공한다고 나머지까지 제공하는 것은 아닙니다.

## Europilot과 ETS2LA

[Europilot](https://github.com/marsauto/europilot)은 Python으로 ETS2를 제어하며 자율주행 알고리즘을 개발하는 초기 프로젝트입니다. 현대적 계승 대상은 게임 관측과 제어를 실험 가능한 인터페이스로 묶는 발상입니다. 오래된 의존성을 그대로 Windows에 옮기는 것을 기본안으로 삼지는 않습니다.

원본 README는 Linux에서 화면 캡처와 가상 조이스틱을 사용하는 구성을 설명하며, 좌우 사용자 정의 카메라 수집은 로드맵 항목으로 남겨 두었습니다. 따라서 원 프로젝트가 독립 다중 카메라 API까지 완성했다고 가정하지 않습니다.

[현재 ETS2LA](https://github.com/ETS2LA/ETS2LA)는 공개 main에서 C#/.NET 프로젝트 구성이 확인됩니다. README는 개발에 .NET 10과 Bun을 안내하고, TruckLib와 맵 파싱 프로젝트를 명시합니다. 과거 Python 중심 설명만으로 현재 버전을 이해하면 안 됩니다. 웹사이트 일부가 최신 버전과 맞지 않을 수 있다는 안내도 README에 있습니다.

따라서 ETS2LA 전체를 순수 카메라 E2E 모델 하나라고 분류하지 않습니다. 게임 상태·지도·제어·ML이 결합될 수 있는 구조이며, 어떤 정보가 실제 정책 입력인지 판단하려면 사용할 버전의 실행 경로를 읽어야 합니다. 현재 전체 플래너와 ML 경로를 끝까지 감사한 상태는 아닙니다.

[ETS2LA 게임 플러그인](https://github.com/ETS2LA/plugin)은 공유 메모리로 제어 명령, 현재 카메라 속성, 주변 차량·주차 차량·신호·경로 정보를 다룹니다. 카메라 자세와 투영행렬을 읽는 기능을 독립 RGB 카메라 생성 API로 해석하면 안 됩니다.

소스 [core.cpp](https://github.com/ETS2LA/plugin/blob/main/src/core.cpp)의 get_camera_data는 현재 카메라와 보간된 트럭 자세를 함께 읽습니다. 서로 다른 시점의 텔레메트리와 이미지를 결합하면 투영이 흔들릴 수 있다는 점에서 참고할 가치가 있습니다.

### 연결 전 반드시 읽을 실제 계약

조사 시점 README 입력 예제는 18바이트와 float timestamp를 사용하지만, [core.hpp](https://github.com/ETS2LA/plugin/blob/main/src/core.hpp)의 InputMemData는 26바이트이며 timestamp 두 개가 double입니다. README는 오래된 명령을 1초 기준으로 설명하지만 core.cpp의 현재 비교값은 0.2초입니다.

따라서 README의 struct.pack 예제를 실행 코드로 그대로 복사하지 않습니다. 선택한 commit의 구조체, 메모리 생성 크기, 실제 읽기·쓰기 구현을 함께 확인해야 합니다. 이 차이는 ABI라는 프로세스 간 이진 데이터 계약의 문제입니다.

## 공식 SDK와 내부 접근의 경계

SCS는 [Telemetry & Input SDK](https://modding.scssoft.com/wiki/Documentation/Tools)를 배포합니다. 차량 상태와 입력 연동의 공식 진입점입니다. 공개 자료에서 임의 위치의 RGB·depth 카메라를 생성하는 공식 API는 확인하지 못했습니다. SDK 다운로드 패키지의 헤더·예제를 최종 기준으로 삼습니다.

공식 플러그인 로딩 경로를 이용하는 DLL이라도 내부 메모리와 함수 후킹 부분까지 공식 API가 되는 것은 아닙니다. ETS2LA 플러그인은 내부 구조와 패턴 탐색을 사용하고, 소스에 대상 게임 버전 검사도 있습니다.

## 미러와 디지털미러

공식 [1.31 업데이트](https://blog.scssoft.com/2018/04/euro-truck-simulator-2-update-131-open.html)는 머리 위치에 따른 FOV와 곡면 미러 표현을 명시합니다. [SCS 개발자 Max의 설명](https://forum.scssoft.com/viewtopic.php?t=253252)에는 mirror_size와 mirror_fov_extension이 등장합니다.

일반 미러를 센서로 사용할 경우 관찰자 위치에 따른 투영 변화와 화면의 좌우 반전을 확인해야 합니다. 디지털미러는 고정 센서 투영을 조사할 우선 후보이나, 실제 머리 움직임 독립성은 실행 후 확인합니다.

[SCS 개발자 답변](https://forum.scssoft.com/viewtopic.php?p=1208872)은 ETS2에서 기본 사용하지 않는 cam_m_h 슬롯과 대응 mirror_data 설정을 설명합니다. [Sanax의 Volvo FH 사례](https://forum.scssoft.com/viewtopic.php?t=323580)는 추가 조수석 코너 카메라와 game_data·mirror_data 수정 사례입니다. 기존 슬롯을 활용한 성공 선례이지 임의 개수의 뷰 생성 보장은 아닙니다.

[ets2_nav_mod의 대시보드 소스](https://github.com/thePromisedKing/ets2_nav_mod/blob/main/src/ui/dashboard/volvo_fh_2024_gps.sii)는 다음 재질을 영상으로 표시합니다.

~~~text
/material/environment/far_mirror_camera.mat
/material/environment/far_s_mirror_camera.mat
~~~

이 재질에서 실제 GPU 리소스까지 연결하는 경로가 조사 출발점입니다. 작성자는 F2 오른쪽 미러와 같은 피드라고 설명하지만, F2가 GPU 수준에서 영상을 재사용하는지 별도 렌더하는지는 캡처로 확인해야 합니다.

## 주차 카메라

[공식 실내 UI 문서](https://modding.scssoft.com/wiki/Documentation/Engine/Truck_Interior_Animations_and_IDs)는 디지털미러 UI 1620~1650과 주차 카메라 화면 910~913을 정의합니다. 후자는 최대 4개의 화면과 후진·사용자 활성 조건을 설명합니다.

4개의 UI 화면을 4개의 독립 동시 카메라라고 단정하지 않습니다. [Coaches 개발 글](https://blog.scssoft.com/2026/07/coaches-new-vehicle-features.html)의 후방 카메라도 보조 경로의 단서입니다. 개발 소개를 모든 현재 트럭에서 사용할 수 있다는 보장이나 DLC 구매 필요성으로 해석하지 않습니다.

## 역공학 코드의 역할

| 자료 | 실제 출발점 | 남는 과제 |
| --- | --- | --- |
| [SPF Camera API](https://github.com/TrackAndTruckDevs/SPF-Framework/blob/main/docs/api/SPF_Camera_API.md) | 기존 카메라 위치·FOV·클리핑·전환 | 임의 offscreen 센서 생성 |
| [SPF GameCameraManager.cpp](https://github.com/TrackAndTruckDevs/SPF-Framework/blob/main/src/GameCamera/GameCameraManager.cpp) | 카메라 객체 조회와 초기화 호출 | 생성·등록과 뷰 제출의 정확한 관계 |
| [ETS2MobileCam](https://github.com/Baldywaldy09/ETS2MobileCam) | camera_manager_u, core_camera_u, 갱신 함수에 대한 작성자 설명 | 현재 빌드에서 구조와 호출 검증 |
| [PrismTextureStreamer](https://github.com/Baldywaldy09/PrismTextureStreamer) | 외부 앱 영상을 게임 UI 텍스처로 공급 | 반대 방향 센서 출력과 동적 RT의 대응 |
| [텍스처 큐 후킹 소스](https://github.com/Baldywaldy09/PrismTextureStreamer/blob/main/PrismTextureStreamerFB/prism/memserver_texture_queue.cpp) | memserver_texture_queue_processor와 mem_tobj_t 경로 처리 | 미러 RT가 해당 경로를 거치는지 |
| [ets2-data-capture](https://github.com/dmariaa/ets2-data-capture) | D3D11 기반 색상·깊이 추출 접근 | 원하는 추가 뷰의 생성 |
| [Prism3D Unit Resolver](https://github.com/Baldywaldy09/x64dbgPrism3DUnitResolver) | 엔진 객체 조사 도구 후보 | 내부 구현·현재 호환성 미검증 |

PrismTextureStreamer의 대시보드 갱신 조건 패치를 미러 카메라 가시성 패치라고 혼동하지 않습니다. 또한 텍스처 크기·포맷만 같다고 같은 카메라라고 식별하지 않습니다. 실제 생성·바인딩·소비 관계를 추적합니다.

## 센서 품질에 영향을 주는 엔진 동작

[SCS 개발자 Komat의 미러 멈춤 설명](https://forum.scssoft.com/viewtopic.php?t=259895)은 미러 가시성 검사와 FOV 문제를 다룹니다. [후방 카메라 로케이터 관련 모더 질문](https://forum.scssoft.com/viewtopic.php?p=2149374)은 운전석에서 보이지 않는 카메라의 동작 문제를 제기합니다. 후자는 해결이 확인되지 않은 질문입니다.

[맵 에디터 문서](https://modding.scssoft.com/wiki/Documentation/Tools/Map_Editor/New_Editor_Features_info_-_old_%2B_1.47)에는 No mirror reflection 속성이 있습니다. 특정 물체를 미러에 그리지 않는 최적화이므로 센서 패스의 물체 제외 규칙을 조사해야 합니다. 야간 조명, LOD, 거리에 의한 누락도 실제 장면에서 확인할 항목입니다.

[Komat의 VR 관련 답변](https://forum.scssoft.com/viewtopic.php?start=3600&t=330624)에는 r_deferred_mirrors와 r_mirror_view_distance가 등장합니다. 미러 조명·물체 상세도·거리와 비용의 관계를 조사할 후보 설정입니다. 해당 답변의 버전과 현재 설치 빌드에서 같은 의미로 동작하는지 확인한 뒤 조정합니다.

## multimon과 DX12

[공식 multimon 문서](https://eurotrucksimulator2.com/multimon_config.php)는 디스플레이용 여러 뷰와 주 카메라에 대한 조정을 설명합니다. 문서의 모니터 개수 제한을 엔진 전체 카메라 개수 제한이라고 해석하지 않습니다. 물리 FHD 화면 하나와 offscreen 센서 텍스처의 해상도는 별개입니다.

[ETS2 1.61 DX12 조사 저장소](https://github.com/NemoByteCore/ets2-1.61-stutter-investigation)는 렌더 경로 분석의 추가 참고자료입니다. 작성자의 성능 문제 진단 전체를 검증한 것은 아닙니다. 처음 DX11을 조사하자는 제안은 기존 도구와의 연결 때문이며, 현재 Windows 게임에 DX11만 존재한다는 주장이 아닙니다.
