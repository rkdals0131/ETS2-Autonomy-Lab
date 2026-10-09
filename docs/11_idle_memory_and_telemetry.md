# SDK와 물리 상태

ETS2 1.61.1.1의 공식 SDK와 내부 물리·렌더 경로를 연결했습니다. 현재 core는 SDK 기본 9채널과 바퀴별 각속도·조향·접지, 차량 구성, 내부 자차 물리 자세, pass 시점 모델 정보를 제공합니다.

## 채널과 단위

| 데이터 | 출처·의미 |
| --- | --- |
| world placement | SDK 물리 차량 기준점, 위치 m·orientation 회전 단위 |
| local linear velocity | 질량중심 속도, 차량 축, m/s |
| local angular velocity | 차량 축, 회전/초 |
| linear acceleration | 차량축 속도 차분의 10-step 평균, m/s² |
| angular acceleration | 회전/초² |
| wheel angular velocity | 바퀴 회전/초, 전진 양수 |
| wheel steering | 회전 단위, 좌측 +0.25가 90° |
| input steering | 정규화 운전자 입력 [-1,1] |
| speed | 차량 축 속도의 -Z, m/s |
| truck configuration | 바퀴 위치·반지름·구동축, 캐빈·head·hook 위치 |

SDK 원본은 로컬 `research/sdk`, 채널 목록은 `research/findings/sdk_1_15_channels.csv`, 현재 등록 필드는 `ot/schema`에 있습니다. 0.23.0은 바퀴별 각속도·조향·접지를 등록하고 ROS 각속도에 2π 변환을 적용합니다. [추가 센서](17_sensor_expansion.md).

## 실행 중인 SDK 채널에서 실제 원본까지 연결

`truck.world.placement`는 physics vehicle slot `+0xF8 → 0x866C10`, 렌더 parent는 `+0xE8 → 0x866B10`에서 읽습니다. 약 16.666ms simulation step의 흐름은 AI 사전 갱신 → 물리 world·PhysX 결과 대기 → 사후 갱신 → SDK frame 이벤트 → 자세 이력·보간 → 렌더 준비입니다.

가속도 이력은 10개 step의 속도 변화를 반영합니다. 현재 step에서 약 166.66ms의 구간이며, 이를 고정 출력 지연으로 대입하지 않습니다. 초기화 시 이전 속도와 이력이 재설정됩니다.

## SDK 선형 속도는 질량중심의 속도이며 위치 원점 보정이 없다

SDK 자세의 기준점과 질량중심은 현재 FH5에서 약 0.78m 떨어져 있습니다. SDK local velocity는 질량중심 속도입니다. 임의 장착점의 속도는 다음과 같습니다.

```text
v_sensor_world = v_COM_world + omega_world × r_COM_to_sensor_world
```

SDK 차량 자세에는 물리 actor의 원점 보정이 이미 반영돼 있습니다. 렌더 차량 자세도 같은 기준점을 사용합니다. ETS2LA camera record는 별도로 회전된 AABB 중심을 더합니다. 리그 위치 계산은 SDK·렌더 기준으로 통일합니다.

## 차량 중력 경로와 가상 IMU에 필요한 보정

현재 차량은 PhysX 자동 중력을 끄고 차량 코드에서 중력에 해당하는 힘을 누적합니다. 설치 빌드의 중력 벡터는 `(0, -9.8100004196, 0)`입니다. 내부 질량·힘에는 게임의 스케일이 적용되므로 이를 그대로 kg·N으로 발행하지 않습니다.

```text
a_COM_world = d(v_COM_world)/dt
a_sensor_world = a_COM_world + alpha × r + omega × (omega × r)
f_sensor = R_world_sensor^T * (a_sensor_world - g_world)
```

차량 축 속도 u를 미분하면 `a_COM_local = du/dt + omega_local × u`입니다. SDK 가속도는 du/dt의 평균이므로 중력을 더하는 것만으로 IMU를 구성할 수 없습니다. 같은 물리 시각의 속도·회전에서 계산합니다. 캐빈·트레일러 센서는 상대 운동을 추가합니다.

## 내부 힘과 자세

차량은 force·torque 누적값과 역질량·역관성을 내부 적분기에 넣고 PhysX 결과를 반영합니다. 바퀴·캐빈의 힘은 작용점에서 생기는 토크까지 누적합니다. driveshaft torque는 내부 계산 뒤 추가되는 경로가 있습니다. 이 상태와 SDK 사후 캐시는 갱신 시점을 맞춰 읽습니다.

코드·원시 분석은 로컬 `research/live/2026-10-08-render-path/`의 `vehicle-body-integrator`, `vehicle-force-velocity-integrate`, `vehicle-body-result-correction`, `telemetry-linear-velocity-accessor`, `physics-backend-mass-local-pose` 자료에 있습니다.

## 주변 객체와 렌더 상태

외부 읽기로 주변 AI·주차 차량·신호등·차단기·미러 카메라·텍스처 descriptor를 연결했습니다. AI body와 trailer는 부모·연결 모델 경로가 다릅니다. 현재 ROS 차량 GT는 해당 pass의 준비 모델을 사용합니다. 객체의 미러 제외·LOD·도로 visibility는 [렌더 구조](12_dx11_mirror_render_path.md)에 있습니다.

외부 메모리 관측 도구는 `research/read_live_memory.py`이며 짧은 읽기 실험에 사용합니다. 라이브 센서 전달은 게임 DLL과 공유 메모리를 사용합니다.
