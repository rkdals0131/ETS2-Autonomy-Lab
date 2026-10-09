# 게임 업데이트와 렌더 경로 개발

현재 1.61.1.1 DX11 경로는 독립 센서 4뷰와 RGB-D·라이다 전달까지 구현돼 있습니다. 새 게임 빌드·차량·렌더 기능을 추가할 때 아래 순서로 진행합니다.

1. 실행 파일과 SDK·설치 구성을 확인하고 해당 빌드의 `ot/schema`를 준비합니다.
2. 기존 pass·camera/drawable·texture 경로를 새 코드와 대조합니다.
3. 정차한 게임에서 SDK와 내부 물리 자세를 읽고 기본 미러를 확인합니다.
4. 한 센서의 RGB·DSV·상수·TF를 확보한 뒤 네 뷰로 확장합니다.
5. 같은 프레임 정합·기본 미러 유지·자차 가림과 모델 제출을 확인합니다.
6. 메타로더 재로딩·패닉·lease 종료 후 자원을 정리합니다.
7. 사용자에게 알리고 전경 성능을 같은 장면에서 측정합니다.

RenderDoc은 pass와 셰이더를 확인할 때 사용합니다. 캡처용과 일반 hook 실행을 분리하고, Steam이 새 게임 프로세스를 만들면 RenderDoc 로딩 여부를 확인합니다. [개발 도구의 시행착오](history/lessons.md#개발-환경).

| 증상 | 먼저 볼 곳 |
| --- | --- |
| 영상 이름과 픽셀이 섞임 | pass 명령 구간·pool 리소스 재사용 |
| HUD 미러에 더미 그림 | private 출력 namespace·alias·동일 index camera/drawable |
| 차체 일부 누락 | 미러 mask의 geometry subset |
| 가까운 검은 띠 | 해당 픽셀 depth·외판 메시·카메라 장착점 |
| 멀리 있는 차량 누락 | LOD·도로 visibility·cut plane·pass 제출 목록 |
| 깊이와 재질 Z 차이 | DSV·viewport·projection·잎 셰이더 보정 |
| 움직이는 박스 불일치 | SDK 시각과 pass 렌더 자세·모델 기준점 |
| 프레임 저하 | 전경 PresentMon·hook 비용·readback·ROS 부하 |

세부 구조는 [SDK·물리](11_idle_memory_and_telemetry.md), [DX11](12_dx11_mirror_render_path.md), [성능 진단](18_performance.md)에 있습니다.
