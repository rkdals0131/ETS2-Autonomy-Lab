# 설치 게임과 정적 분석

## 대상

| 항목 | 값 |
| --- | --- |
| 게임 | Euro Truck Simulator 2 1.61.1.1 |
| Steam build | 25482642 |
| 렌더 경로 | Windows x64 DX11 |
| 현재 차량 | Volvo FH5 4x2 / l2h1 / LHD |
| SDK | SCS SDK 1.15 |
| 빌드 식별 | ot/schema/1.61.1.1/game.json |

## 확보한 자료

- 설치 아카이브 목록·헤더, 게임 정의·미러 UI·재질 연결.
- SDK 108개 채널의 이름·타입·원본 헤더 위치.
- ETS2LA Windows 패턴 11개와 카메라 함수 후보의 실행 파일 대조.
- 미러 camera·drawable·texture descriptor, 9슬롯 생성·갱신·제출 경로.
- FH5 chassis·cabin·mirror_01·sunshield_01 메시와 로케이터.
- RGB·DSV·셰이더·투영·visibility 경로의 후속 런타임 연결.

현재 센서 장착은 추출한 FH5 메시를 기준으로 하고, 실제 SDK 바퀴 위치·반지름으로 base_link를 계산합니다. [현재 리그](14_phase1_highway_sensors.md).

## 자료 위치

| 경로 | 내용 |
| --- | --- |
| research/findings | SDK 채널·아카이브 목록·PE 정보·패턴과 소스 revision |
| research/extracted | 게임 정의·UI·재질·셰이더·변환 모델 |
| research/sdk | 공식 SDK 원본 |
| research/sources | revision을 고정해 받은 참고 소스 |
| research/tools | extractor·ConverterPIX·RenderDoc |
| ot/schema | 현재 DLL이 사용하는 빌드·주소·필드 |

SDK·추출 게임 자산·대용량 관측 원본은 Git에서 제외합니다. 공급 도구와 라이선스는 [research](../research/README.md), 제품 의존성은 [THIRD_PARTY](../ot/THIRD_PARTY.md)에 있습니다.

[SDK·물리 참조](11_idle_memory_and_telemetry.md), [렌더 참조](12_dx11_mirror_render_path.md), [게임 업데이트](../ot/README.md#게임-업데이트).
