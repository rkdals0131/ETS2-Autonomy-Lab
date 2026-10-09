# 엔진과 참고 프로젝트

| 자료 | 프로젝트에서 참고하는 부분 |
| --- | --- |
| [SCS SDK](https://modding.scssoft.com/wiki/Documentation/Engine/SDK) | 플러그인 로딩·차량 텔레메트리·입력 인터페이스 |
| [Europilot](https://github.com/marsauto/europilot) | 게임 관측과 제어를 Python 실험으로 연결하는 구조 |
| [ETS2LA](https://github.com/ETS2LA/ETS2LA) | 지도·상태·제어·인지 연결 |
| [ETS2LA plugin](https://github.com/ETS2LA/plugin) | 내부 메모리 패턴·차량·신호·경로·카메라·입력 ABI |
| [ConverterPIX](https://github.com/mwl4/ConverterPIX) | FH5 모델 변환과 외판 장착점 계측 |
| [RenderDoc](https://renderdoc.org/) | DX11 pass·리소스·GPU 상수 분석 |
| [SCS 포럼의 디지털미러 사례](https://forum.scssoft.com/viewtopic.php?t=323580) | 미러 슬롯과 game_data·mirror_data·UI 재질 연결 |

받아 둔 소스 revision과 라이선스는 로컬 `research/findings/source_revisions.json`, 제품에 포함한 의존성은 [THIRD_PARTY](../ot/THIRD_PARTY.md)에 있습니다. 전체 출처는 [SOURCES](../SOURCES.md)에 정리했습니다.

## 적용한 엔진 지식

미러·디지털미러·대시보드 카메라는 camera/drawable과 화면 재질로 연결됩니다. 현재 DLL은 이 렌더 경로에 private 카메라와 출력을 공급합니다. 기존 미러의 머리 위치 반사 계산, 9슬롯 참조, 차체 부분 제출과 LOD·도로 visibility는 별도로 다룹니다. [구현 구조](12_dx11_mirror_render_path.md).

## 유지할 ABI 교훈

조사한 ETS2LA 입력 README는 18바이트·float timestamp를 사용했지만 해당 소스의 InputMemData는 26바이트·double timestamp 두 개였습니다. 명령 만료도 예제의 1초와 구현의 0.2초가 달랐습니다. 입력 연결 때 선택 revision의 구조체·메모리 할당·소비 코드를 함께 맞춥니다.

## 후속 선택지

현재 제품 경로는 DX11입니다. DX12 비교는 CPU 제출 비용을 별도로 진단할 때 검토합니다. multimon은 사용자 화면 배치 기능이며 현재 센서 전달은 private pass를 사용합니다. ATS·Assetto Corsa는 [별도 확장](09_assetto_corsa.md)으로 둡니다.
