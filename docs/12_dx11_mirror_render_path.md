# DX11 렌더·카메라·가시성

현재 구현은 미러의 장면 준비 경로로 독립 센서 pass를 만들고, 각 pass의 RGB·DSV·투영·모델 자세를 묶습니다. 기본 미러와 센서 출력은 분리합니다. 빌드별 주소는 `ot/schema`, 구현은 `ot/native/camera_rig.cpp`, `pass_commands.cpp`, `gpu_capture.cpp`에 있습니다.

## 리소스와 복사 시점

그래프 이미지 이름 → pool image ID → DX11 texture/view를 연결합니다. 미러 0·2처럼 같은 리소스를 재사용하는 pass는 컴파일된 명령 구간의 namespace로 식별합니다. composition 타깃을 떠날 때 실제 OM 타깃을 확인하고 해당 pass 데이터를 복사합니다.

센서 RGB는 composition에서, 깊이는 geometry DSV에서 옵니다. 현재 private 출력은 `ot/sensorN` 이름과 alias 0xffff를 사용합니다. 사용자가 보는 HUD·물리 미러 텍스처와 분리돼 있습니다.

## 미러 자세와 운전석 시점 의존성

일반 미러 방향 계산은 운전자 시점 위치를 사용하고, 디지털미러 경로에는 별도 기준 위치가 있습니다. 센서 제출은 RBX를 private camera 복사본으로 돌려 지정된 pose를 사용합니다. 원본 미러의 지속 상태는 유지합니다.

본체와 캐빈 서스펜션 상대 자세를 합성한 parent에 센서를 붙입니다. 렌더 이력의 보간 시각을 적용하고 head pose는 제외합니다. SDK pose는 물리 결과 시각이며 [기준점·속도](11_idle_memory_and_telemetry.md)를 따릅니다.

## 9슬롯과 private 출력

엔진은 0–8 슬롯을 순회합니다. 그래프 구성 `0x4D46E4`는 같은 index의 카메라를 NULL 검사 없이 참조하므로 camera와 drawable을 함께 공급합니다. 현재 센서 3·4·6·7은 0·1·2·5 템플릿의 private 배열을 사용합니다. FH5 기본 미러는 0·1·2·5입니다.

캡처 요청이 다음 select 기회에서 한 번 선택됩니다. pass의 실제 Present ID로 묶고 비수집 프레임의 센서 제출·상세 관측은 생략합니다. 266개 묶음에서 슬롯별 제출은 각각 266회였습니다.

## 자차 body와 마운트

원본 미러 mask에 맞춘 geometry subset은 다른 위치의 센서에서 필요한 차체를 생략합니다. 플레이어 body의 센서 제출은 `0xA3CAD0`의 full-list 경로를 사용합니다. 현 hook 범위는 플레이어 본체 두 모델이며 트레일러 모델은 연결 후 확인합니다.

선바이저·미러 하우징의 근접 가림은 실제 모델에 맞춰 광학 중심을 이동했습니다. [현재 마운트](14_phase1_highway_sensors.md), [발생 원인과 수정](history/lessons.md).

## 미러 제외·LOD·도로 가시성

| 단계 | 동작·영향 |
| --- | --- |
| 맵 정의 | 다섯 종류의 미러 제외 설정이 공통 플래그로 변환됨 |
| CPU 후보 선택 | 뷰 frustum과 미러 제외 플래그 검사 |
| 주차 차량 | Hookup·Prefab 배치점의 설정이 모델 제출에 전달됨 |
| AI·연결 모델 | trailer 등은 부모에서 호출되며 개별 LOD·제외 조건 검사 |
| 차량 LOD | 선택된 게임 카메라와 수평 거리 기준, 주 화면에 FOV 보정 추가 |
| 도로 | 소속 경로·도로 표시 조건을 먼저 적용 |
| cut plane·Visibility area | 게임 카메라 기준 공간 판정, UID 포함·제외 예외 |
| Compound | 부모 segment와 개별 자식 모델 목록을 구분 |
| Mover 인물 | 게임 카메라의 3D 거리·FOV로 모델 선택 후 뷰별 구 경계 검사 |
| pedestrian_hookup | 별도 모델 경로, 게임 카메라 기준 LOD |

독립 pose만으로 이 선택 경로가 모두 센서 기준이 되지는 않습니다. M5는 센서 frustum 안 actor, pass 제출 모델, 최종 영상의 가림을 구분해 거리별 누락을 측정합니다. 전방 150m 이상과 측후방 인접 차로가 우선입니다.

## 투영과 깊이

DX11의 reversed Z와 viewport 깊이 범위를 적용합니다. 관측 미러 surface 범위는 0.01–0.9, main 실내는 별도 투영과 0.9–1.0 범위를 사용합니다. pass의 실제 값을 읽어 역투영합니다.

CPU 준비 작업의 projection·viewport·camera/ray와 GPU VS/PS 상수를 대조했습니다. RenderDoc fog ray와 geometry projection의 pixel-center 차이는 최대 0.000033px 미만이었습니다. GPU 센서 경로는 미터 광축 깊이를 R32F로 출력합니다.

## 잎 billboard의 Z 보정과 DSV 차이

`eut2.leaves` defattr 셰이더는 다음 값을 기록합니다.

```text
attributes0.w = interpolated_eye_position.z + 2 * mask_texture.b * input_normal_eye.z
```

같은 셰이더는 SV_Depth를 쓰지 않으므로 DSV는 rasterized geometry 깊이를 유지합니다. 센서 깊이는 DSV로 복원합니다. attributes Z는 deferred shading 연구에 사용합니다. FP16 표현과 절삭에 따른 차이도 별도로 존재합니다. 원본 GLSL·DXBC 분석은 로컬 `research/extracted/effect-analysis/`에 있습니다.

## 차량 박스와 실제 영상

actor AABB의 기준점과 렌더 모델 자세를 변환해 같은 pass 카메라로 투영합니다. 준비 시점 차량 목록과 GPU VS 상수 구간을 연결했습니다. 이동 AI 2대·6시점에서 현재 모델 자세의 박스 정합이 ±0.5초 자세보다 높았고, GPU 상수 투영 대조의 최대 차이는 0.000206px였습니다.

개별 draw 실행 여부와 최종 픽셀 가림은 별도 관측값입니다. 트레일러와 주행 중 누락 확인을 이어갑니다.

## 개발 자료

- `research/live/2026-10-08-render-path/`: 카메라·물리·가시성 disassembly와 표본
- `research/live/2026-10-08-renderdoc/`: frame3160 캡처·상수·복원
- `research/live/2026-10-08-render-probe/`: DLL 깊이·모델 정합
- `research/live/2026-10-09-side-artifact/`: 미러 테두리 off/on/off·mesh 교차·수정 후 영상

[실행 도구](../ot/README.md), [센서 전달](16_ros2_bridge.md), [성능](18_performance.md).
