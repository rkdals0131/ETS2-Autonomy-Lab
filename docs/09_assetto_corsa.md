# Assetto Corsa 후속 프로젝트

AC는 서킷과 도로 맵에서 차량 상태·가상 센서·상태머신·제어를 실험할 후속 프로젝트입니다.

## 공개 API에서 확인한 단서

CSP는 Assetto Corsa의 커뮤니티 확장입니다. Kunos 기본 API와 CSP API를 구분하고 설치한 CSP 버전과 스크립트 실행 환경을 함께 확인해야 합니다.

[CSP lib_scene.lua](https://github.com/ac-custom-shaders-patch/acc-lua-sdk/blob/main/lib_scene.lua)의 ac.GeometryShot은 장면 참조와 해상도를 받아 텍스처를 만들고, 카메라 위치·방향·FOV로 갱신하며 선택적으로 depth를 제공합니다. 센서 카메라 구현에 직접적인 출발점입니다. 필요한 트랙·차량·효과가 장면 참조에 모두 포함되는지와 외부 전달 비용은 별도 확인합니다.

[CSP ac_ray.lua](https://github.com/ac-custom-shaders-patch/acc-lua-sdk/blob/main/common/ac_ray.lua)는 track, scene, cars, carCollider, physics에 대한 ray 교차 인터페이스를 정의합니다. 빔 방향과 반환 거리를 구성하면 가상 LiDAR를 설계할 수 있지만, 빔 수·주기·충돌 대상·성능을 실제 실행에서 확인해야 합니다.

## 센서와 제어의 기본안

| 항목 | 초기 접근 | 구분할 점 |
| --- | --- | --- |
| 카메라 | GeometryShot | 센서 장면에 포함되는 렌더 효과 확인 |
| LiDAR | raycast 또는 depth 역투영 | 충돌 메쉬와 시각 메쉬의 차이 |
| 위치 센서 | 월드 pose를 로컬 기준 좌표로 변환 | RTK 오차·지연·결측을 모델링하기 전에는 이상적인 위치 센서 |
| 경로 | 트랙 기준 경로와 차량 상태 | 사용할 맵의 실제 기준선 확인 |
| 판단 | 주행·감속·정지·복귀 등 필요한 상태 | 실제 시나리오에 필요한 상태만 |
| 제어 | 속도 제어와 경로 추종부터 | 입력 단위·주기·차량별 동역학 |

GNSS는 월드 좌표에 지도 기준점과 좌표 변환을 적용합니다. RTK 센서 모델은 위치 오차·지연·보정 상태를 추가합니다.

처음에는 단일 차량과 짧은 코스에서 관측→제어→해제 경로를 확인합니다. 이후 위치 오차와 지연, 점군, 회피 행동을 확장합니다.
