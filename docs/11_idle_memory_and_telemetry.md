# 도로 정차 상태에서 읽을 수 있는 데이터와 방법

게임이 도로를 로드한 채 정상 시간으로 진행되고 트럭만 정차해 있다면, 자차 상태뿐 아니라 주변 차량·신호·카메라 상태를 조사할 수 있다. **자차 데이터는 공식 SCS SDK를 우선 사용하고, 주변 교통·현재 카메라·경로는 ETS2LA 계열 내부 접근을 추가하는 구성이 가장 직접적이다.** RGB와 depth는 GPU 렌더 경로를 별도로 연결해야 한다.

2026-10-08 기준 설치 파일은 ETS2 1.61.1.1이다. 아래의 공식 제공 범위는 SDK 1.15 헤더, 내부 접근 범위는 고정한 공개 소스에서 확인했다. 이후 사용자의 빠른 일자리 수동 주행과 도로 정차 상태에서 외부 읽기 전용 관측을 수행했다. 실제 관측 결과는 다음 절에, 바이너리 대조와 미러 소유 구조는 [설치 파일 분석](10_installed_game_static_analysis.md)에 있다. 후속 조사에서 미러 4개의 실제 DX11 색상·깊이 텍스처와 리소스 재사용까지 연결했다. 최신 렌더 결과는 [DX11 미러 경로](12_dx11_mirror_render_path.md)를 참고한다.

## 2026-10-08 도로 정차 상태의 추가 관측

**플러그인을 설치하지 않고 주변 교통·주차 차량·신호등·미러 카메라까지 읽었다.** 게임을 조작하거나 멈추지 않았다. 동일한 DX11 프로세스 PID 24940에서 자차 속도는 0이었고 주변 AI와 신호는 계속 변했다. 엔진 RPM은 이번 reader에서 해석하지 않았으므로 정차를 엔진 공회전 확인으로 표현하지 않는다.

| 대상 | 실제 결과 | 해석의 경계 |
| --- | --- | --- |
| 자차 | world 약 `(10286.024, 44.306, -9161.934)`, speed/acceleration 0, 미세한 pose 변화 | 좌표축은 아래 설명 참조. SDK timestamp는 없음 |
| 주변 AI | 60초 관측에서 내부 배열 34–43개. 마지막 5초 관측은 49–50개를 전부 읽음 | 초기 관측은 배열 앞 40개로 제한. 마지막에는 reader 한도를 128로 늘려 잘림 없음. 월드 전체 AI 수가 아님 |
| 주차 차량 | 21개, pose·AABB·trailer 포인터 | trailer 포인터의 상세 연결 구조는 미해석 |
| 신호등 | 9개 prefab에서 29개. 상태·타이머·prefab UID·semaphore ID | 29개 모두 60초 동안 상태 변화. 가장 가까운 신호가 자기 차선의 신호라는 뜻은 아님 |
| 차단기 | 2개, 상태와 애니메이션 필드 | 관측 중 closed 상태 1. 개방 동작은 재현하지 않음 |
| 일반 카메라 | 14슬롯, 10개 객체, 활성 index 2 `vehicle_interior_camera` | 14개 동시 영상이라는 뜻이 아님 |
| 미러 카메라 | 별도 배열 9슬롯 중 0–5에 실제 `mirror_camera` 6개 | 포인터뿐 아니라 클래스·pose·projection을 읽음. 영상은 미추출 |
| 미러 텍스처 설정 | 7개 descriptor와 실제 reflection TOBJ 이름 | `hood` descriptor는 있으나 해당 카메라 슬롯은 NULL. GPU 표면의 실측 해상도가 아님 |
| 현재 내비 | `route_task == nullptr` | 이번 정차 구간에는 활성 경로가 없음. 이전 주행 경로를 현재 경로로 재사용하지 않음 |
| 현재 트레일러 | 자차 trailer 포인터 NULL | 연결 트레일러의 관절 운동은 관측하지 못함 |

신호 raw enum `1, 2, 4, 8`을 모두 관측했고, ETS2LA 소스의 의미는 각각 ORANGE_TO_RED, RED, ORANGE_TO_GREEN, GREEN이다. 상태 전환 때 타이머가 다시 커지고 이후 감소했다. 가장 가까운 신호는 수평 거리 약 3.4m지만 차선 연결을 확인하지 않았으므로 제어용 정답으로 선택하지 않았다.

![메모리로 읽은 주변 객체와 신호 변화](../research/live/2026-10-08-idle/world-observation.png)

그림의 파란 점은 60초 동안 샘플링한 AI 위치, 회색 사각형은 주차 차량, 색 원은 첫 표본의 신호, 보라색 X는 차단기다. 실제 게임 화면이나 지도 도로 형상을 캡처한 것이 아니다. 화면 밖 객체도 포함되며, 점의 궤적 모양 자체를 정밀 차선 지도로 사용하지 않는다.

### 미러에서 확인한 내용

미러 카메라는 `camera_manager`의 일반 카메라 목록에 없었다. `game_actor → visual_interior → +0x13A8 array`에 있었다. 같은 미러 index에 대응하는 텍스처 descriptor는 `base_ctrl +0xA8 array`에서 찾았다. 이름은 메모리의 alias 문자열 및 texture object pool에서 직접 읽었다.

| index | reflection 이름 | FOV 설정값 | descriptor 기본 크기 | 30초간 서로 다른 CPU pose 표본 |
| --- | --- | ---: | --- | ---: |
| 0 | `close_mirror_reflection.tobj` | 55° | 256×512 | 60 / 60 |
| 1 | `close_s_mirror_reflection.tobj` | 90° | 256×256 | 60 / 60 |
| 2 | `far_mirror_reflection.tobj` | 55° | 256×512 | 60 / 60 |
| 3 | `far_s_mirror_reflection.tobj` | 90° | 256×256 | 1 / 60 |
| 4 | `side_mirror_reflection.tobj` | 60° | 256×128 | 1 / 60 |
| 5 | `front_mirror_reflection.tobj` | 100° | 256×128 | 60 / 60 |
| 6 | `hood_mirror_reflection.tobj` | 카메라 NULL | 256×128 | 해당 없음 |

모두 `/material/environment/` 아래 이름이다. index 7–8은 카메라와 descriptor 모두 NULL이었다. 생성 코드가 넣는 폭·높이를 읽은 것이므로 그래픽 옵션의 mirror scale 등이 적용된 최종 `ID3D11Texture2D` 크기와 구분한다. 포인터, 텍스처 설정, 실제 draw의 개수는 서로 다를 수 있다.

6개 카메라 객체의 near/far는 약 0.1/500이고 projection은 관측 동안 고정이었다. 후속 코드 추적에서는 렌더용 카메라의 far를 현재 설정값 400으로 덮어쓰는 경로를 확인했다. 0·1·2·5번은 매 표본에서 미세하게 pose가 달랐고, 3·4번은 30초 내내 같았다. **CPU pose만으로 GPU 영상의 갱신 여부나 빈도를 증명하지 않는다.** 이후 `mirror0/1/2/5` 렌더 그래프와 리소스는 읽었지만 픽셀의 변화는 미확인이다. 머리 회전이나 F2 전환은 수행하지 않았으므로 사용자 시점과 무관한 센서라는 확인도 아직 없다.

CPU projection에서 계산한 수평/수직 FOV는 단순 설정값과 다를 수 있다. 예를 들어 55° 설정 미러의 행렬은 수평 약 55°, 수직 약 92.31°를 나타낸다. 60° 설정 미러는 약 81.79°/46.83°다. 원시 행렬과 계산치는 `mirror-parameters.csv`에 있다. 반사·화면 flip·UI crop과 GPU 최종 변환을 대조하기 전에는 이를 영상 캘리브레이션 완료로 취급하지 않는다.

### 관측 비용과 재현

주변 월드 60초/2Hz 기록은 120표본, ReadProcessMemory 82,242회, 3,932,530 bytes였다. 미러를 포함한 30초/2Hz 기록은 60표본, 42,541회, 2,044,690 bytes였다. 두 기록의 읽기 오류는 0건이었다. 미러 포함 표본 한 번의 reader 측 경과시간은 중앙값 4.339ms, 최대 5.563ms였다. **게임 FPS·GPU 지연은 측정하지 않았다.** 서로 다른 필드가 다른 tick의 값일 수 있다.

마지막 5초 기록은 AI 읽기 예산을 배열당 128개로 늘린 뒤 실행했다. 실제 49–50개를 잘림 없이 읽었고 10표본, 7,432회, 350,526 bytes, 읽기 오류 0건이었다. 이 예산은 외부 reader의 읽기 상한이며 엔진이나 ETS2LA 출력 버퍼의 개수 제한이 아니다.

기록은 [idle 관측 폴더](../research/live/2026-10-08-idle/)의 `world-cycle.jsonl`, `mirrors-world.jsonl`, `final-all-nearby.jsonl`, `summary.json`에 있다. 미러의 클래스·소유자 추적, disassembly, texture descriptor 표본도 같은 폴더에 보존했다. 모든 관측 프로세스는 유한 실행 후 핸들을 닫고 종료했다.

```powershell
# 아래 PID/base는 이번 프로세스의 예시다. 게임 재실행 후 현재 값으로 바꾼다.
py -3.13 .\research\read_live_memory.py --pid 24940 --base 0x7FF707440000 --seconds 30 --hz 2 --world --mirrors --output .\research\live\next-observation.jsonl
```

`--output`은 기존 파일을 덮어쓰지 않는다. 도구는 이 빌드 전용이며 입력·메모리 쓰기·원격 함수 호출·DLL 주입 권한을 요청하지 않는다. 후속 `read_render_memory.py`는 실제 GPU resource의 CPU 메타데이터까지 읽는다. SDK 채널 callback, RGB/depth 픽셀, 시점 독립성, 추가 센서 생성, 성능은 아직 후속 단계다. 읽을 수 있는 필드를 계속 추가하는 것과 센서 구현 완료는 별개의 작업이다.

## 2026-10-08 실제 주행 중 읽기 전용 관측

사용자가 직접 실행하고 운전한 PID 24940에 `OpenProcess(0x1010)`으로 접근했다. 권한은 `PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION`이다. 게임 입력, 메모리 쓰기, DLL 설치·주입, 디버거 연결·중단, 게임 함수 호출은 수행하지 않았다. 에이전트가 게임을 실행하거나 종료하지 않았다.

`game.log.txt`와 실행 인자에서 **DX11**을 확인했다. 시스템 `d3d12.dll`도 로드되어 있지만 이것으로 DX12 렌더러 사용을 판단할 수 없다. 기존 게임 폴더의 `dxgi.dll` 및 `Real_G27_ffb_x64.dll`은 로드되어 있었다. ETS2LA/RenCloud/capture/controller 공급자 DLL은 모듈 목록에 없었고, 검사한 7개 named mapping도 모두 `ERROR_FILE_NOT_FOUND(2)`였다. Workshop 자료 마운트 기록과 별도로 현재 프로필 로그의 활성 모드 수는 0이었다.

첫 3초 표본 후, 2Hz로 30초 동안 60개 표본을 기록했다. 아래 수치는 두 번째 관측에 해당한다.

| 대상 | 실제 읽은 결과 | 해석 범위 |
| --- | --- | --- |
| 기본 관리자 | 클래스명 `game_ctrl`, game actor·physics vehicle·trailer 포인터 | 정적으로 찾은 슬롯과 일부 구조가 현재 프로세스에서 유효함 |
| 내 트럭 | traffic player wrapper의 object 포인터가 physics vehicle 포인터와 60/60 표본 일치 | 외부 차량을 자차로 오인하지 않고 연결함 |
| 속도 | raw 17.2509–23.4214; m/s 해석 시 **62.10–84.32 km/h** | 29.50초 표본 간 위치 이동 합 645.23m, 속도 적분 648.64m로 대략 일치. 계기판·공식 SDK 동시 대조는 안 함 |
| 가속도·조향 | acceleration raw −0.1872–0.7316, steering raw −0.03261–0.04095 | 주행 중 변화 확인. steering을 바퀴 각도나 SDK raw input과 동일시하지 않음 |
| 주변 AI 차량 | `spawned_vehicles_1` 크기 27–40, 위치·회전·AABB·속도·가속도 읽음 | 앞 40개 읽기 예산이며 최근접 정렬이나 전체 월드 목록이 아님 |
| 현재 카메라 | index 2, `vehicle_interior_camera`; 위치·quaternion·FOV·near/far·projection | 운전석 pose의 실제 변화 확인. GPU 프레임과의 동시성은 미확인 |
| 카메라 배열 | 14 slots, 객체 10개, NULL 4개 | 렌더 센서 14개를 의미하지 않음 |
| 내비 | physical route 320개; 첫 16개 node pointer·64-bit UID·정수 좌표·비용 읽음 | 전체 geometry 추출·차선 연결은 미수행. GPS trip distance는 진행 중 감소 |

카메라 클래스는 `debug_camera`, `vehicle_behind_rotation_camera`, `vehicle_interior_camera`, `vehicle_bumper_camera`, `vehicle_cabin_camera`, `vehicle_top_camera`, `vehicle_tv_camera`, `wander_camera`였다. 클래스명은 vtable의 descriptor getter에서 관측한 `lea rax,[rip+disp32]; ret`를 **바이트로 해석해** descriptor와 문자열을 읽었다. 해당 함수를 원격 실행하지 않았다. 이 배열에서 `mirror_camera`는 식별하지 못했으며 미러가 다른 구조에 없다는 뜻은 아니다. manager +0x14 값은 이 관측에서 14였지만 다른 상태에서의 의미까지 확정하지 않는다.

일부 비활성 카메라는 pose가 원점·항등 회전이고 projection이 0 또는 미초기화로 보이는 값이었다. 따라서 카메라 포인터가 존재한다고 유효한 센서 관측으로 사용할 수 없다. 활성 카메라의 raw projection은 near=0.2, far=1600, 음의 view-Z를 대입하면 NDC Z가 약 −1과 +1이 되는 형태였다. **DX11 실행이라는 이유로 이 CPU 행렬을 그대로 D3D depth [0,1] 역투영에 사용하면 안 된다.** 최종 GPU 행렬·변환·depth target은 아직 읽지 않았다.

읽기 루프 오류는 0건, 요청 데이터 합계 391,072 bytes, 호출 9,795회였다. 표본 한 번을 읽는 데 관측 프로세스 기준 중앙값 1.197ms, 최대 2.370ms가 걸렸다. 게임 FPS나 프레임 지연의 실측값은 아니다. 프로세스를 정지시키지 않았으므로 표본 안의 값도 서로 다른 tick에서 읽힐 수 있다. 신호등, SDK 채널 등록, pause/로드 전환, RGB/depth, 독립 뷰 생성은 이번 실행에서 검증하지 않았다.

관측용 Python 프로세스와 핸들은 종료·해제했다. 원본 표본·요약·프로세스 모듈 목록·게임 로그 발췌는 [실측 폴더](../research/live/2026-10-08-readonly/)에, 사용한 도구는 [read_live_memory.py](../research/read_live_memory.py)에 있다. 이 도구의 offset은 조사한 1.61.1.1 빌드에 한정되고, 재실행할 때 PID와 ASLR module base는 현재 프로세스에서 새로 얻어야 한다.

## idle의 의미

| 상태 | 얻는 정보와 주의점 |
| --- | --- |
| 도로에 정차, pause 해제 | 자차 위치는 거의 일정하지만 교통·신호·시간은 진행. 메모리 및 센서 조사에 적합 |
| 엔진 공회전 | RPM, 연료·엔진 상태 관찰 가능. 정차와 엔진 ON은 별개 조건 |
| F1 pause 또는 메뉴 | SDK pause 이벤트를 구분해야 함. 이전 값이 메모리에 남아도 현재 센서 관측으로 취급하지 않음 |
| alt-tab / 최소화 | 게임·렌더·SDK가 어떻게 갱신되는지는 실제 설정과 빌드에서 확인. 도로 정차와 동일하지 않음 |
| 게임 종료 | SDK 공급자와 게임 객체가 없음. 읽는 쪽이 빈 shared memory를 생성했다고 연결된 것이 아님 |

트럭이 정차해도 주변 차량은 움직인다. 따라서 idle 수집 역시 RGB, pose, 교통 객체의 시간 정합이 필요하다. 메뉴 정지나 디버거 전체 중단은 첫 수집 기준 상태로 사용하지 않는다.

## 공식 SDK로 얻는 값

공식 [Telemetry SDK 1.15](https://modding.scssoft.com/wiki/Documentation/Engine/SDK/Telemetry)를 내려받아 공통 채널 헤더를 읽었다. 채널 문자열 정의는 총 **108개**다: truck 84, trailer 18, common 5, job 1. 바퀴·트레일러 인덱스를 펼친 스칼라 개수도, 런타임 등록 성공 개수도 아니다. configuration과 gameplay event는 별도다.

전체 이름·타입·헤더 위치는 `research/findings/sdk_1_15_channels.csv`, 원본은 `research/sdk/include/`에 있다. [SCS 개발자의 설명](https://forum.scssoft.com/viewtopic.php?t=240843)도 채널 목록의 기준을 SDK 헤더로 안내한다.

| 범주 | 실제 채널 예 | 단위·의미와 idle에서의 용도 |
| --- | --- | --- |
| 자차 위치와 자세 | `truck.world.placement` | double 위치 + Euler 방향. 위치 기준 확보 |
| 속도와 가속도 | `truck.speed`, `truck.local.velocity.linear`, `truck.local.acceleration.linear` | m/s, m/s². speed는 후진 시 음수 |
| 회전 운동 | `truck.local.velocity.angular`, `truck.local.acceleration.angular` | **회전/초**, 회전/초². rad 단위가 아님 |
| cabin과 head | `truck.cabin.offset`, `truck.head.offset`, cabin angular 채널 | 차체 흔들림·운전자 고개 변화. 카메라 projection 자체는 아님 |
| 운전자 입력 | `truck.input.steering/throttle/brake/clutch` | steering [-1,1], 나머지 [0,1]. steering은 왼쪽 양수 |
| 시뮬레이션 입력 | `truck.effective.steering/throttle/brake/clutch` | 키보드 보간·자동 변속·cruise 등의 적용 후 값. effective brake에는 retarder/parking/engine brake가 포함되지 않음 |
| 구동계 | `truck.engine.rpm`, `truck.engine.gear`, `truck.displayed.gear`, `truck.cruise_control` | 공회전·중립·크루즈 구분 |
| 제동 | parking/engine brake, retarder, air pressure와 warning 채널 | 주차 상태와 브레이크 계기값 |
| 엔진과 계기 | fuel, AdBlue, oil/water temperature, voltage, engine/electric enabled | 일부 압력·온도·전압은 SDK 문서상 느슨한 근사값이며 실제 차량의 물리 센서 모델이 아님 |
| 등화·상태 | blinker, hazard, lights, wipers, differential lock, lift axle | 라벨과 운전자 상태 |
| 바퀴 | `truck.wheel.steering`, `.rotation`, `.angular_velocity`, `.suspension.deflection`, `.on_ground`, `.substance`, `.lift` | 각 바퀴 인덱스별. 조향 입력값과 실제 바퀴 각을 구분 |
| 손상·거리 | `truck.wear.*`, `truck.odometer` | wear와 km 단위 누적거리. 정차 시 대부분 일정 |
| 내비 요약 | `truck.navigation.distance`, `.time`, `.speed.limit` | advisor 기준 m, s, m/s. 노드 목록이나 차선 중심선은 제공하지 않음 |
| 트레일러 | `trailer.world.placement`, `.connected`, velocity/acceleration, wheel, wear, cargo damage | 연결체 자세와 관절 운동 분석. 다중 trailer는 `trailer.0.*`, `trailer.1.*` 등 |
| 시간·스케일 | `game.time`, `local.scale`, `multiplayer.time.offset`, `rest.stop`, `mandatory.break` | 게임 시각은 게임 내 분, local.scale은 지도 거리/시간 보정. 실제 센서 timestamp와 구분 |
| job | `job.cargo.damage` + job configuration | 운송 맥락·화물 라벨 |

configuration callback에는 트럭 브랜드/ID, fuel capacity, 엔진 최대 RPM, 기어비, differential ratio, wheel count/radius/position, cabin/head 기본 위치, 화물·출발지·도착지 등이 있다. gameplay event에는 job, 벌금, tollgate, ferry/train 등 사건 정보가 있다. 정적 설정은 매 프레임 재검색하지 않고 변경 이벤트 때 갱신한다.

SDK 배포판 **1.15**, Telemetry API의 협상 버전, EUT2 game-specific telemetry 버전 **1.20**, 게임 **1.61**은 서로 다른 번호다. `scssdk_telemetry_eut2.h`는 1.61의 버스 관련 정보가 아직 미문서화이며 바뀔 수 있다고 명시한다. 미문서화 값이나 `dev.*` 채널을 안정적인 계약으로 채택하지 않는다.

내비 제한속도는 Route Advisor 설정을 따른다. [2015년 SCS 개발자 답변](https://forum.scssoft.com/viewtopic.php?t=186527)은 0 이하 값의 특수 의미를 설명하지만 이를 현재 빌드의 완전한 상태표로 재사용하지 않는다. 0을 곧바로 '정지해야 하는 도로'로 해석하지 않는 정도의 주의가 필요하다.

## 공식 SDK 밖에서 얻을 수 있는 값

| 값 | 확인한 구현 또는 방법 | 범위 |
| --- | --- | --- |
| 현재 카메라 pose/FOV/projection | ETS2LA `get_camera_data()` | 현재 선택된 카메라. 임의 6개 센서의 영상이 아님 |
| 렌더 보간 자차 pose | 같은 camera memory record의 truck pose | 영상에 맞춘 위치 후보. SDK 물리 pose와 다른 시점이며 AABB 중심 기준 |
| 주변 움직이는 차량 | ETS2LA traffic processor | 가까운 최대 40개, 위치·quaternion·크기·speed·acceleration, 각 최대 3 trailer |
| 주차 차량 | 별도 parked buffer | 가까운 최대 40개, pose·크기·ID·trailer 여부 |
| 신호등과 gate | semaphore buffer | 가까운 최대 40개, pose·종류·상태·시간 필드·ID |
| 현재 내비 경로 | route buffer | 최대 6000개의 node UID, 끝까지 거리/시간. 좌표는 map과 join 필요 |
| 차선·교차로 연결 | map sector + road look + prefab parser | 주로 오프라인 지도 정보. 현재 달리는 차선/횡오차는 pose를 지도에 연결해 계산 |
| RGB / depth / projection 정합 | 그래픽 API 캡처 또는 게임 내 렌더 후킹 | GPU resource를 읽는 별도 경로. 일반 CPU 메모리 읽기로 픽셀을 바로 얻지 못함 |
| semantic/instance segmentation, LiDAR | 별도 렌더·라벨 또는 raycast 구현 필요 | 공식 SDK나 확인한 ETS2LA buffer에 완성된 센서로 제공되지 않음 |

신호 상태 enum은 읽은 소스에서 OFF=0, ORANGE_TO_RED=1, RED=2, ORANGE_TO_GREEN=4, GREEN=8, SLEEP=32다. Gate는 별도 상태 체계를 사용한다. 가까운 신호가 곧 자기 차선의 신호는 아니므로 방향·prefab·차선 연결을 확인해야 한다.

40개 제한은 플러그인 출력 배열의 제한이다. 월드 전체 객체 목록이나 모든 보행자·장애물을 의미하지 않는다. 차량 ID는 포인터 기반으로 플러그인이 배정하는 short 값이므로 재시작을 넘는 영구 객체 ID로 쓰지 않는다.

## 접근 방법의 선택

### 공식 SDK DLL과 shared memory

게임이 `bin/win_x64/plugins`의 DLL을 로드하고 `scs_telemetry_init`에서 callback을 등록한다. 채널 callback으로 상태를 모은 뒤 `frame_end`에서 묶어 외부 프로세스에 전달할 수 있다. 게임 밖의 Python에서 SDK 함수를 바로 호출하는 구조는 아니다.

SDK 호출은 게임이 callback을 호출한 main thread 안에서 이뤄져야 한다. callback 안에서 파일 저장이나 긴 GPU 작업을 기다리지 않고, 필요한 상태를 복사해 외부 작업으로 넘기는 구성이 적합하다. 이것은 SDK readme에 명시된 thread 계약이다.

[RenCloud scs-sdk-plugin](https://github.com/RenCloud/scs-sdk-plugin)은 이 연결을 구현한 공개 예다. 확보한 commit `d7216cb16c178be34feb04d9d6f67dd8e71ab8de`의 `Local\SCSTelemetry`는 32 KiB mapping이며 plugin revision 12, sdkActive, paused와 timestamp 필드를 둔다. 오래된 bundled SDK가 최신 1.15의 모든 채널을 전달한다고 가정하지 않는다. 선택한 reader와 DLL의 실제 구조체를 맞춘다.

HTTP/JSON이 편하면 [Funbit telemetry server](https://github.com/funbit/ets2-telemetry-server) 같은 wrapper가 있지만 새로운 차량 상태가 생기는 것은 아니고 전달 방식만 바뀐다. 이 프로젝트의 로컬 고빈도 수집은 작은 shared memory가 먼저다. 서버나 방화벽 규칙은 이번 조사에서 만들지 않았다.

### ETS2LA 내부 reader

확보한 [ETS2LA/plugin](https://github.com/ETS2LA/plugin/tree/3b01d90b5be2469c0d94fcfdee4b354184ddeb02)은 SDK DLL로 로드된 뒤 실행 모듈에서 패턴을 찾아 내부 객체를 읽고 named mapping에 복사한다. 공식 telemetry와 내부 역공학이 결합된 플러그인이다.

아래는 `src/core.hpp`의 `#pragma pack(1)`과 memory 생성 코드를 함께 읽은 계약이다. Python format은 little-endian, padding 없음 기준이다.

| Windows mapping | 크기 | 읽는 내용 / Python record format |
| --- | ---: | --- |
| `Local\ETS2LAPluginStatus` | 6 | plugin version, steering/acceleration override 상태: `<i??` |
| `Local\ETS2LACameraProps` | 128 | FOV+position, cell x/z, quaternion wxyz, 4×4 projection, 보간 truck pose: `<4f2h27f` |
| `Local\ETS2LATraffic` | 6960 | 40 × (54-byte vehicle + 3 × 40-byte trailer). vehicle `<12f2h2?`, trailer `<10f` |
| `Local\ETS2LAParkedVehicles` | 1720 | 40 × 43 bytes: `<10fh?` |
| `Local\ETS2LASemaphore` | 1920 | 40 × 48 bytes: `<3f2h4fifii` |
| `Local\ETS2LARoute` | 96000 | 6000 × 16 bytes: `<qff`. UID는 필요 시 64-bit bit pattern으로 보존 |
| `Local\ETS2LAPluginInput` | 26 | **명령 쓰기용**: `<f?df?d`. 읽기 전용 조사에 필요하지 않음 |

README 예제는 status에서 2 bytes만 잘라 6-byte format에 넘기고, camera에서도 전체 format보다 짧게 자르며, route mapping 크기를 80,000으로 표기한다. 입력 예제도 18 bytes/float timestamp인 반면 실제 코드는 26 bytes/double timestamp, 만료 기준 0.2초다. 따라서 README 코드를 그대로 클라이언트로 쓰지 않는다. 현재 ETS2LA C# consumer의 camera=128, route=96000도 별도로 대조했다.

읽는 쪽에서는 [OpenFileMappingW](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-openfilemappingw)와 `FILE_MAP_READ`, `MapViewOfFile`을 사용해 이미 있는 mapping을 연다. .NET은 `MemoryMappedFile.OpenExisting` 후 read-only view가 가능하다. Python은 `ctypes`로 같은 Win32 API를 호출할 수 있다. producer가 없으면 실패로 처리하고, `mmap(..., tagname=...)`가 빈 mapping을 생성하는 것을 연결 성공으로 오인하지 않는다. 끝나면 view를 unmap하고 handle을 닫는다.

### 외부 프로세스 메모리 직접 읽기

DLL 없이 [ReadProcessMemory](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-readprocessmemory)를 사용할 수도 있다. `OpenProcess`의 조회/읽기 권한, 모듈 base, 해당 빌드의 RVA와 구조체 해석이 필요하다. 쓰기 권한이나 kernel driver가 필요한 작업은 아니다.

현재 파일에서 카메라 진입점을 설명하면 다음과 같다.

```text
실행 중 eurotrucks2.exe의 module base
  + 0x36AE740 → 8-byte camera_manager pointer를 읽음
  → manager의 current camera와 camera array를 해석
  → 선택된 camera의 FOV / placement / projection을 읽음
```

`+0x36AE740`은 현재 1.61.1.1 파일의 **포인터 슬롯**이며 camera 객체 자체가 아니다. 게임 업데이트 후 이 상수를 그대로 쓰지 않는다. ETS2LA/MobileCam의 서로 다른 패턴이 이번 파일에서 같은 슬롯을 가리킨다는 근거가 있다. 모든 중첩 구조체가 실측 검증됐다는 뜻은 아니다.

외부 reader는 게임 객체가 바뀌는 중간에 읽을 수 있다. `ReadProcessMemory` 성공은 프레임 일관성을 보장하지 않는다. 객체 수명·읽기 길이·외부 데이터 타입을 읽는 경계에서 확인하고, 장시간 기록에는 게임 callback에서 묶어 발행하는 구조가 더 적합하다. vtable 함수 주소를 찾았다고 외부 프로세스에서 그 함수를 보통 함수처럼 호출할 수는 없다.

Cheat Engine이나 x64dbg의 값 검색은 새 필드를 찾는 보조 방법이다. 정차 한 장면에서 0인 speed만 검색하면 후보를 구별하기 어렵다. 이미 제공되는 값은 SDK를 쓰고, 새로운 값은 pause 전환·좌우 고개 움직임·신호 변화 같은 대응 관찰로 의미를 좁힌다. 임의의 메모리 쓰기나 패치는 현재 읽기 조사에 필요하지 않다.

## 좌표와 시간에서 이미 확정할 수 있는 것

SDK `scssdk_value.h`는 축과 각도 단위를 명시한다.

- 차체 좌표: X 오른쪽, Y 위, Z 뒤. 프로젝트의 전방·좌측·위 축으로는 `(x,y,z) → (-z,-x,y)`.
- 월드 좌표: X 동쪽, Y 위, Z 남쪽. ENU로는 `(x,y,z) → (x,-z,y)`.
- Euler 값은 1회전=1. heading 0은 북쪽, 0.25는 서쪽. 각도를 rad로 바꿀 때 `2π`를 곱한다. quaternion이나 Euler 회전 순서는 축 변환과 함께 다룬다.
- ETS2LA 내부 placement는 `world=(cx*512+x, y, cz*512+z)`. 이는 map file의 sector 크기와 같은 개념이라고 가정하면 안 된다.
- 내부 quaternion은 memory record에서 `w,x,y,z`. C# `Quaternion`이나 다른 라이브러리 생성자의 인자 순서와 구분한다.
- ETS2LA camera buffer의 truck position은 AABB 중심으로 보정한 값이다. 현재 차량의 로컬 중심은 약 `(-0.0163, 1.8689, -0.1532)`이므로 SDK truck placement와 원점이 다르다. 엔진 자체의 SDK 차량 자세와 렌더 본체 이력은 같은 물리 getter의 결과를 사용하며 시각 보간 여부가 다르다. [원점과 보정 경로](12_dx11_mirror_render_path.md#sdk-차량-자세와-렌더-이력은-같은-기준점을-사용한다)에 근거를 정리했다.

SDK frame-start에는 `render_time`, `simulation_time`, `paused_simulation_time`과 timer restart flag가 있다. SDK timestamp 단위는 microseconds다. **simulation_time은 pause 중에도 변하고 paused_simulation_time은 pause 중 멈춘다.** 이름만 보고 반대로 처리하기 쉬운 부분이다. 게임 내 분 단위 `game.time`이나 PC wall clock을 카메라 시각으로 대신 쓰지 않는다.

SDK 문서는 telemetry frame을 물리 step으로 설명하고, 렌더 pose는 두 물리 step 사이를 보간한다고 명시한다. 따라서 60Hz client polling은 60개의 새로운 렌더 관측을 뜻하지 않는다. 정지한 채 값이 변하지 않을 때 channel callback이 매번 올 필요도 없다. 마지막 유효값과 frame timing을 함께 다룬다.

설치된 1.61.1.1의 호출 경로도 이 차이를 뒷받침한다. 현재 step 길이는 16,666 μs였고, AI 사전 갱신→물리 world 갱신（PhysX `simulate`/`fetchResults(true)` 포함）→사후 갱신→SDK `frame_start`·채널 전달·`frame_end`가 simulation 반복 안에 있다. 반복 뒤 자세 이력 발행과 렌더 시각 증가, 상위 함수의 자세 보간, 카메라·가시성·미러 준비가 이어진다. 같은 렌더 반복의 여러 SDK 전달에서 `render_time`이 같을 수 있으므로, **`frame_end`나 shared-memory polling을 GPU 프레임 완료 신호로 사용하지 않는다.** 실제 vtable·시각·분기와 남은 동기화 경계는 [렌더 경로의 물리·SDK 순서](12_dx11_mirror_render_path.md#물리-step과-sdk-frame-이벤트는-렌더-준비보다-먼저다)에 정리했다. callback 설치와 실행 시각 계측은 수행하지 않았으며, 아래처럼 기존 채널의 원본 상태를 추가로 연결했다.

### 실행 중인 SDK 채널에서 실제 원본까지 연결

**기존 FFB 플러그인이 구독한 채널의 등록 정보와 게임 내부 값 캐시를 외부에서 읽었다.** SDK 전달 대상 목록 `EXE +0x2E046C0`은 sentinel까지 44개 항목이었다. 이 수는 이름과 index의 조합 수이며 SDK가 지원하는 채널 전체 수가 아니다. provider가 있는 항목은 34개, NULL인 항목은 10개였다. 예를 들어 현재 목록의 wheel suspension index 4–7과 trailer 일부 채널은 provider가 없었다. 등록됐다는 사실만으로 유효한 현재 값이 있다는 뜻은 아니다.

frame_start·frame_end·paused·started의 등록 callback은 각각 하나였고 기존 `Real_G27_ffb_x64.dll` 안에 있었다. 아래 다섯 채널의 callback 여섯 개（placement가 두 타입）도 모두 같은 DLL과 owner에 연결됐다. 전체 44개 채널의 callback 구현을 분석하거나 FFB 출력을 변경한 것은 아니다.

channel 객체 `H +0x30`이 provider를 가리킨다. 실제 provider vtable `0x221DCE0`의 slot `+0x08 → 0x2FD200`은 저장된 context와 함수 포인터로 전달한다. 이어 getter의 대상 객체·this 보정·함수 포인터를 읽어 다음 경로를 연결했다. 대상은 모두 `game_actor +0x18`의 **`game_physics_vehicle` V**였으며, 주소만 비슷하다는 이유로 연결한 것이 아니다.

| SDK 채널 | 현재 빌드의 실제 원본 | 해석 |
|---|---|---|
| `truck.world.placement` | provider `0x5A79D0` → thunk `0x5A7EEC` → V slot `+0xF8 → 0x866C10` → 본체 body slot `+0x130 → 0x163C680` | 본체 물리 자세에 V `+0x464`를 부호 반전한 로컬 이동을 적용. 렌더 이력도 이 getter 결과를 게시하므로 차량 기준점은 같고 시간 처리가 다름 |
| `truck.speed` | generic float provider `0x5A7AC0` → `0x4CCAC0` → V `+0x2F0` float | 물리 사후 갱신에서 저장한 차량 좌표계 속도의 `-Z`. 벡터 길이가 아닌 부호 있는 전후방 속도 |
| `truck.local.velocity.linear` | vector provider `0x5A7A10` → `0x872BB0` → `*(V +0x30)` body slot `+0x140 → 0x163C950` | 물리 backend의 속도와 자세를 읽는 차량 좌표계 벡터 경로 |
| `truck.local.velocity.angular` | 같은 vector provider → `0x872BE0` → body slot `+0x160 → 0x163CE70` | 물리 backend의 각속도와 자세를 읽은 뒤 약 `0.1591549367`（`1/(2π)`）배. SDK 단위는 rotations/s |
| `truck.local.acceleration.linear` | 같은 vector provider → `0x872C40` → V `+0xD38` float3 | 물리 사후 갱신에서 local velocity 차분을 계산한 뒤 유지하는 10-step 이동평균. 아래에 활성 조건과 계산식 정리 |

관측한 body vtable은 RVA `0x2448928`, body `+0xF8`의 backend vtable은 `0x2506950`이었다. body의 자세 getter는 backend slot `+0xA0 → 0x1AF6220`으로 연결된다. linear/angular velocity getter는 backend slot `+0x128 → 0x1AF8CA0`, `+0x138 → 0x1AF8CC0` 및 같은 자세 getter를 사용한다. 이 메서드들을 우리가 실행한 것은 아니다. backend 전체의 상태나 모든 physics 채널을 재구현한 결과도 아니다.

SDK 전달 함수 `0x1664710`은 provider를 호출해 native 캐시 `H +0x58`을 갱신하고, 타입 변환용 캐시 `H +0x128`을 준비한 뒤 등록된 각 타입의 callback을 호출한다. `0x1661310`은 그에 앞선 gameplay event 임시 목록 정리 함수이며 채널의 값을 만드는 함수가 아니다. 실제 코드는 callback flag bit 0을 `each_frame`, bit 1을 `no_value`로 처리한다. 선택 채널에서 speed는 flag 1, placement의 dplacement는 0·Euler는 2, 세 vector는 2였다. `no_value`만 켜졌다고 매 step 전달을 요구하는 것은 아니다. 이는 로컬 공식 SDK의 `scssdk_telemetry_channel.h`와 대응한다.

읽은 native 캐시 `K = H +0x58`에서는 값 존재 상태와 타입 mask 뒤, float 값은 `K +0x28`, float3은 `K +0x38`, dplacement의 world double3는 `K +0x88`에 있었다. placement 변환 `0x1661D10`은 내부 cell X/Z에 512를 곱해 로컬 X/Z와 더한다. 다섯 native 캐시는 값 존재 상태가 1이었고, 해당 타입의 mask가 켜져 있었다. 선택 표본의 위치는 약 `(10286.02434, 44.30590, -9161.93387)`, scalar 속도는 약 `8.93e-6 m/s`였다. 선형 속도는 `(-9.26e-5, -0.00108674, -0.00020573) m/s`, 각속도는 `(0.000397609, 3.48e-6, -0.000143993) rotations/s`였다. 정차 중 작은 물리 변화가 남는 사례이며, 서로 다른 읽기를 묶어 속도끼리 정확히 일치해야 한다고 검증하지 않는다.

이 캐시를 외부에서 읽는 것은 **SDK callback을 구독하는 것과 동등하지 않다.** 구독·provider 구성에 따라 존재 여부가 달라지고, simulation 정지 분기에서는 채널 전달 자체를 건너뛴다. 이전 유효값과 현재 render timestamp를 임의로 묶으면 안 된다. 구조·원본 연결은 확인했지만 단일 simulation step의 원자적 snapshot이나 영상과의 정합은 확보하지 않았다.

자료는 `research/live/2026-10-08-render-path/`의 `telemetry-channel-registry.json`, `telemetry-callback-module.json`, `telemetry-native-provider-inputs.json`, `telemetry-vehicle-physics-link.json`, `telemetry-selected-channel-callbacks.json`과 해당 `telemetry-*.txt` disassembly다. 각각의 외부 읽기는 209회·12,376 bytes·약 2.72 ms, 27회·2,536 bytes·약 0.28 ms, 23회·272 bytes·약 0.26 ms, 24회·8,352 bytes·약 0.41 ms였다（module 정보는 별도 OS 모듈 조회）. observer PID 18016·20704·1840·15000은 handle을 닫고 종료했다.

### 속도·가속도는 사후 물리 갱신에서 계산되며, 가속도에는 10-step 필터가 있다

**`truck.speed`와 `truck.local.acceleration.linear`의 쓰기 위치를 물리 결과 수신 뒤·SDK 전달 전으로 연결했다.** 현재 V의 vtable을 다시 읽어 slot `+0x98 → 0x643D10`, `+0x2B8 → 0x64BCF0`을 확인했다. 정상적인 player actor 갱신 경로는 다음과 같다.

```text
controller 사후 갱신 0x4CF8A0
  → player actor 0x5D8880
  → V slot +0x98: 0x643D10
  → 공통 차량 갱신 0x863980
      → 0x8659B0: scalar speed와 관련 상태 저장
      → V slot +0x2B8: 0x64BCF0
          → 0x872650: 본체 선형·각가속도 필터 갱신
          → 조건에 맞으면 캐빈 각가속도 필터 갱신
  → 나머지 actor/controller 사후 처리
→ simulation 함수의 SDK frame_start·채널·frame_end 전달
```

이 표는 관련 호출을 추린 것이며 다른 사후 처리도 중간에 있다. 함수 진입을 실시간 추적하거나 게임에 breakpoint를 건 것이 아니다. 후속 코드가 차량 상태를 보정하는 모든 경우까지 배제한 것으로 해석하지 않는다.

`0x8659B0`은 body slot `+0x140`에서 얻은 차량 좌표계 속도 `(vx, vy, vz)`의 Z 부호를 뒤집어 **V `+0x2F0 = -vz`**로 저장한다（쓰기 위치 `0x865BFE`）. 같은 함수는 이전 scalar speed와의 차이를 step 초로 나눠 V `+0x2F4`에 저장한다. 따라서 `+0x2F4`는 이 경로의 한 step 전후방 속도 차분이며, SDK의 3축 가속도 `+0xD38`과 다른 값이다. body slot `+0x138 → 0x163C910`의 월드 속도는 V `+0x2F8` float3에 저장한다. 음수 speed가 후진이라는 공식 SDK 설명과도 맞는다.

선형 가속도 함수는 현재 body local velocity와 V `+0xC80`에 저장한 이전 local velocity를 사용한다. 이상적인 산술로 쓰면 다음과 같고, 실제 구현은 float 연산이다.

```text
dt = step_microseconds * 1e-6
sample = (local_velocity_now - local_velocity_previous) / dt
filtered = (filtered - 0.1 * history[i]) + 0.1 * sample
history[i] = sample
local_velocity_previous = local_velocity_now
i = (i + 1) mod 10
```

실제 코드는 old sample을 빼고 새 sample을 더하는 순서이며, 10개를 매번 다시 합산하지 않는다. `0.1`의 float 상수는 약 `0.10000000149`다. 각가속도도 같은 index의 별도 10개 이력을 쓰며, body 각속도를 `1/(2π)`배해 rotations/s로 바꾼 뒤 차분한다. 관련 필드는 다음과 같다.

| V 필드 | 내용 |
|---|---|
| `+0xC80`, `+0xC8C` | 직전 local linear velocity와 local angular velocity（rotations/s） |
| `+0xC98` | 다음 갱신에서 교체할 이력 index |
| `+0xCA0`, `+0xD48` | 선형·각가속도 이력 배열. 각 float3 10개 |
| `+0xD38`, `+0xDE0` | 선형·각가속도 필터 출력 |
| `+0x14C8`, `+0x14D8`, `+0x1570` | 캐빈의 직전 local angular velocity, 각가속도 이력 배열, 필터 출력. 캐빈 body가 있는 분기 |

현재 16,666 μs step에서 10개 가속도 sample은 약 **166.66 ms의 속도 변화 구간**을 반영한다. 이는 필터가 참조하는 구간이지 실측된 GPU 지연이나 고정 167 ms 출력 지연이 아니다. 초기화·reset의 `0x872430`은 현재 속도를 이전값으로 설정하고 두 이력과 출력을 0으로 채운다. 그 직후에는 최근 10개 모두가 새 변화량으로 채워진 상태가 아니다.

활성 조건도 확인했다. 갱신·reset 함수는 전역 plugin 목록의 각 항목 `+0x08`이 NULL이 아닌지를 검사하며, 그런 항목이 없으면 해당 계산을 건너뛴다. 현재 목록에는 앞서 callback owner로 읽은 기존 FFB 객체 하나가 있었고 `+0x08 = Real_G27_ffb_x64.dll +0x11880`이었다. DLL export에서 이 주소는 **`scs_telemetry_init`**으로 확인했다. `+0x10`은 `scs_telemetry_shutdown`의 RVA `0x12B20`과도 맞았다. 따라서 이 경로는 telemetry plugin의 존재를 조건으로 하며, **플러그인이 없는 환경에서도 내부 가속도 필드가 계속 갱신된다고 보장할 수 없다.** 이번 관측을 위해 DLL을 추가·해제하지 않았다.

읽기 표본의 이력 배열은 실제로 각 10개였고 index는 2였다. 같은 vehicle 메모리 구간의 speed `1.352445906e-5 m/s`는 저장된 local velocity Z의 부호 반전과 일치했다. 배열 데이터는 V `+0xCC0`, `+0xD68`에 들어 있었다. 이력의 산술 평균과 저장 출력은 완전히 같지는 않았다（선형 최대 차이 약 `2.83e-6 m/s²`, 각가속도 약 `8.17e-8 rotations/s²`）. 누적 float 갱신과 비원자적 읽기를 구별하는 추가 계측은 하지 않았으며, 이를 비트 단위 일치나 단일 tick snapshot의 증거로 쓰지 않는다. 이동평균이라는 판단의 중심 근거는 실제 쓰기 명령·이력 교체·modulo 10 경로다.

센서 데이터에는 실질적인 차이가 생긴다. SDK 선형 가속도는 매 step 갱신돼도 여러 step의 변화량을 담고 있고, 이 경로는 서로 다른 시각의 **차량 좌표계 속도 성분을 직접 차분**한다. 이 함수에서 중력·센서 장착점의 회전 운동 등을 조합한 가속도계 모델을 만드는 것은 아니다. 따라서 이를 보정 없이 순간 IMU 측정값이나 해당 영상 시각의 가속도 정답으로 저장하지 않는다. 차량 상태 기록에는 원래의 단위·필터 특성·simulation 시각을 보존하고, 별도 가상 IMU가 필요하면 그 센서 모델을 명시해야 한다.

근거는 `game-actor-post-simulation.txt`, `vehicle-post-simulation.txt`, `vehicle-common-post-simulation.txt`, `vehicle-speed-state-update.txt`, `vehicle-telemetry-dynamics-update.txt`, `vehicle-acceleration-update.txt`, `vehicle-acceleration-reset.txt`, `vehicle-body-global-velocity.txt`와 `vehicle-dynamics-history-observation.json`이다. 모두 `research/live/2026-10-08-render-path/`에 있다. 후보 offset·call 검색 결과는 탐색용이며 위 disassembly와 실제 vtable로 확인한 경로만 결론에 사용했다. observer PID 7008은 18회·3,232 bytes를 약 0.45 ms에 읽고 handle을 닫아 종료했다.

### SDK 선형 속도는 질량중심의 속도이며 위치 원점 보정이 없다

**설치 빌드의 `truck.local.velocity.linear`는 물리 질량중심의 선형 속도를 차량 축으로 표현한다. `truck.world.placement`가 반환하는 차량 기준점으로 속도의 측정점을 옮기는 보정은 없다.** 좌표축을 회전시키는 것과 강체 위의 다른 점으로 속도를 옮기는 것은 별개다. 후자는 차량이 회전할 때 영향을 준다.

SDK getter `0x872BB0`은 body slot `+0x140 → 0x163C950`을 호출하며 세 번째 인자를 NULL로 준다. 이 함수는 backend slot `+0x128`에서 월드 선형 속도를 얻고, 같은 backend slot `+0xA0`의 actor 회전으로 역회전한다. 위치·질량중심 offset·각속도를 읽거나 외적 항을 더하지 않는다. 월드 속도 getter `0x163C910`도 같은 backend 속도를 그대로 반환한다. 따라서 앞 절의 scalar speed와 선형 가속도 필터도 이 질량중심 속도에서 출발한다.

현재 backend의 vtable은 RVA `0x2506950`이다. slot `+0x08 → 0x1AF54B0`은 문자열 `PxRigidDynamic`（RVA `0x2447E58`）을 반환한다. 다음 getter들과 공개 PhysX 3.4 구현을 대조했다. 게임 함수는 호출하지 않고 실행 파일의 명령과 해당 입력 메모리만 읽었다.

| backend 경로 | 실행 파일에서 확인한 읽기 |
|---|---|
| `+0x128 → 0x1AF8CA0` | backend `+0x15C`의 float3 선형 속도를 복사 |
| `+0x138 → 0x1AF8CC0` | backend `+0x168`의 float3 각속도를 복사. 이 단계의 단위는 rad/s |
| `+0xF0 → 0x1AF8A80` | actor 기준 질량중심 자세. flags `+0x17C & 0x200`이면 `*(backend+0x70)+0xE0`, 아니면 backend `+0xB0`의 28 bytes |
| `+0xA0 → 0x1AF6220` | backend `+0x140`의 질량중심 자세에 위 로컬 질량중심 자세의 역변환을 합성하여 actor 자세 반환 |

공개 [`NpRigidBodyTemplate.h`](https://github.com/NVIDIAGameWorks/PhysX-3.4/blob/master/PhysX_3.4/Source/PhysX/src/NpRigidBodyTemplate.h)는 질량중심 getter를 `getBody2Actor()`에, 두 속도를 Scb body의 속도에 연결한다. [`ScbBody.h`](https://github.com/NVIDIAGameWorks/PhysX-3.4/blob/master/PhysX_3.4/Source/PhysX/src/buffering/ScbBody.h)는 이 속도가 buffered 값임을 보여준다. [`NpRigidDynamic.h`](https://github.com/NVIDIAGameWorks/PhysX-3.4/blob/master/PhysX_3.4/Source/PhysX/src/NpRigidDynamic.h)의 actor 자세는 `body2world * inverse(body2actor)`다. 특히 [`ExtRigidBodyExt.cpp`의 위치별 속도 계산](https://github.com/NVIDIAGameWorks/PhysX-3.4/blob/master/PhysX_3.4/Source/PhysXExtensions/src/ExtRigidBodyExt.cpp#L406-L429)은 질량중심에서 목표점까지의 벡터에 대한 `angular_velocity × displacement`를 선형 속도에 더한다. 이 관계가 선형 속도의 측정점 판정 근거다. 게임은 patched PhysX 3.4를 포함하므로 upstream 구조체 offset을 그대로 가정하지 않고 위 로컬 getter들과 대조했다.

현재 차량 표본의 질량중심 로컬 자세는 위치 `(0,0,0)`, quaternion xyzw `(0,0,0,1)`이었다. **이 차량 상태에서는 actor 원점이 질량중심이다.** 반면 SDK 위치는 actor에서 로컬 `-d`만큼 이동한 점이다. 앞 절과 같은 `d=(0,0.449999988,-0.636846125)`를 다시 읽었으므로 SDK 기준점에서 질량중심까지는 위쪽 약 0.45 m·전방 약 0.637 m, 직선 거리 약 0.780 m다. 다른 차량·구성에서도 질량중심 로컬 자세가 항등이라고 일반화하지 않는다.

actor 기준 질량중심 위치를 `c`, 차량 기준점에서 센서까지의 고정 로컬 위치를 `s`라 하면, 같은 물리 시각의 강체 운동 관계는 다음과 같다. `R`은 actor에서 월드로의 회전이며 `d`는 앞서 확인한 차량 원점 보정이다.

```text
r_COM_to_sensor_world = R * (s - d - c)
v_sensor_world = v_COM_world + omega_world_rad_s × r_COM_to_sensor_world

# SDK 차량 위치 원점 자체의 속도: s = 0
v_vehicle_origin_local = v_SDK_local - omega_local_rad_s × (d + c)
omega_local_rad_s = 2*pi * SDK_local_angular_velocity_rotations_s
```

이 식은 동일 강체에 고정된 센서용이다. 캐빈 서스펜션이나 트레일러처럼 상대 운동이 있으면 그 운동을 추가해야 한다. 또한 렌더 보간 위치와 물리 시각의 속도를 섞는 시간 차이는 이 보정으로 해결되지 않는다. SDK가 이미 반환한 위치에 `-d`를 다시 적용하는 것도 잘못이다.

관측 파일은 `physics-com-and-velocity-observation.json`이며 18회·352 bytes를 약 0.28 ms에 읽었다. 질량중심 자세·선형 속도·각속도는 한 backend 메모리 구간에서 읽었지만 원자적 snapshot은 아니다. 별도로 읽은 V의 월드 속도 캐시는 backend 값과 달랐고, 두 갱신 시점의 일치를 주장하지 않는다. 선회 실험으로 보정 효과를 계측하지도 않았다. 근거 disassembly는 `physics-backend-concrete-type.txt`, `physics-backend-linear-velocity.txt`, `physics-backend-angular-velocity.txt`, `physics-backend-mass-local-pose.txt`, `physics-backend-actor-pose.txt`와 기존 `telemetry-linear-velocity-accessor.txt`, `telemetry-physics-linear-velocity.txt`다. 모두 `research/live/2026-10-08-render-path/`에 있다. observer PID 5656은 handle을 닫고 종료했다.

### 차량 중력 경로와 가상 IMU에 필요한 보정

**SDK 가속도는 가속도계의 specific force（단위 질량당 비중력 힘）가 아니다. 현재 트럭은 PhysX의 자동 중력을 끄고, 차량 코드에서 중력에 해당하는 힘을 별도로 누적한다.** 따라서 scene 중력 getter 하나만 읽어 차량에 적용되는 모든 힘이나 IMU 측정값을 안다고 판단하면 안 된다.

먼저 현재 트럭이 속한 scene을 연결했다. backend `PxRigidDynamic` slot `+0x30 → 0x1AEB340`은 concrete type에 맞는 Scb 객체 offset을 EXE `+0x301CB48` 표에서 얻고, 소속 상태와 scene 포인터를 확인한다. 현재 concrete type은 6, Scb offset은 `+0x60`, control state는 2였다. 그 getter가 반환할 scene은 `0x1F55F8A3A20`이며, body `+0x58`의 물리 member가 `+0x178`에 보유한 scene과 같았다. 메서드를 원격 호출한 결과가 아니라 getter 명령에 따른 포인터 해석이다.

scene vtable `0x2500CD8`의 slot `+0x218 → 0x1ADF2C0`은 중력 getter다. scene `+0x2478` bit 0이 켜져 있으면 `+0x2448`의 buffered gravity를, 꺼져 있으면 `+0x7A4`의 적용 중인 gravity를 반환한다. setter `+0x210 → 0x1ADF260`의 저장 위치·분기도 이에 대응한다. 이 관계는 공개 [`NpScene.cpp`](https://github.com/NVIDIAGameWorks/PhysX-3.4/blob/master/PhysX_3.4/Source/PhysX/src/NpScene.cpp#L254-L264)와 [`ScbScene.h`](https://github.com/NVIDIAGameWorks/PhysX-3.4/blob/master/PhysX_3.4/Source/PhysX/src/buffering/ScbScene.h#L405-L424)의 중력 처리와 대조했다.

현재 scene buffer flag는 0이고 유효한 중력은 **`(0, -9.8100004196, 0)`**이었다. 비활성 buffer 구간에 있던 작은 float 값들은 다음 중력으로 예약된 값으로 해석하지 않는다. 물리 member slot `+0xD0 → 0x16335F0`도 이 scene getter를 그대로 감싼다.

그러나 현재 트럭의 actor flags는 **`0x03`**이었다. backend slot `+0x60 → 0x1AEB470`의 분기에 따라 읽었으며, [`PxActor.h`](https://github.com/NVIDIAGameWorks/PhysX-3.4/blob/master/PhysX_3.4/Include/PxActor.h#L62-L99)의 bit 1은 `eDISABLE_GRAVITY`다. SCS body getter `+0x78 → 0x163A7C0`도 이 bit를 반전해 중력 활성 상태를 반환하고, setter `+0x80 → 0x163A7F0`은 반대 값을 PhysX flag 2에 전달한다. 이 상태를 '게임에 중력이 없다'고 해석하면 잘못이다.

별도의 차량 중력 경로는 다음과 같다.

```text
현재 차량의 사후 갱신 0x643D10
  → 0x863980: 속도·SDK 가속도 상태 갱신
  → 0x645FD0: 차량의 다른 사후 처리
  → V slot +0x250: 0x86D140
      if V[+0x73D] != 0:
        m_internal = body slot +0x88: 0x16124F0 → body[+0xE0]
        g_vehicle = EXE[+0x2D83098] float3
        F_gravity = m_internal * g_vehicle
        body slot +0x268: 0x163D3D0
          if body[+0x98] != 0: body[+0x9C] float3 += F_gravity
          else: backend slot +0x148, force mode 0, autowake = true
```

중력 곱셈은 `0x86D599..0x86D5C1`, 전달은 `0x86D5E9`다. 같은 함수는 다른 힘·토크도 처리하므로 누적 필드 전체를 중력만의 값이라고 부르지 않는다. 현재 V의 slot `+0x250`은 실제로 이 함수였고 V `+0x73D = 1`, body `+0x98 = 1`이었다. **현재 구성은 직접 PhysX에 전달하는 분기가 아니라 내부 힘 누적 분기다.** 후속 조사에서 이 누적값을 속도·자세 계산에 소비하고 PhysX 결과와 연결하는 경로를 확인했다. 아래 절에 정리하며, 순간 총힘을 실시간 추적한 결과와는 구분한다. 사후 갱신에서 새 힘을 준비하는 위치를 현재 읽은 SDK 가속도의 계산 이전으로 바꾸어 서술하지 않는다.

실행 중 EXE `+0x2D83098`에서도 `(0, -9.8100004196, 0)`을 읽었다. body의 내부 질량은 약 `0.794999957`이고 이 분기가 만들 중력 항은 `(0, -7.79894972, 0)`이다. 후자는 읽은 입력으로 계산한 값이며 힘 전달 함수를 실시간 추적한 측정값은 아니다. **내부 질량 수치를 kg로 해석하지 않는다.** body 질량 setter `+0x1C0 → 0x163A8B0`은 같은 `+0xE0`과 PhysX mass setter를 갱신하며, player actor의 `0x5D868A`에는 상위 질량 입력을 약 `1e-4`배하는 경로가 있다. 내부 힘을 그대로 N으로 내보내는 것도 피해야 한다.

정차 표본의 scalar speed는 약 `3.16e-5 m/s`, SDK 선형 가속도 필드는 약 `(-2.26e-4, 5.01e-6, -1.08e-4) m/s²`였다. 이는 거의 0인 차량 속도 변화량이다. 이상적인 가속도계는 수평 정차 중 위쪽으로 약 `+9.81 m/s²`를 보고한다. [ROS REP 145의 가속도계·자이로 설명](https://github.com/ros-infrastructure/rep/blob/master/rep-0145.rst#data-sources)도 specific force와 rad/s를 구별한다（해당 REP의 상태는 Draft）.

가상 IMU의 모델은 같은 물리 시각에 수집한 질량중심의 월드 속도·회전과 센서 장착 변환에서 만드는 편이 명확하다. 같은 강체에 고정된 센서라면 다음과 같다. 여기서 `r_world`는 질량중심에서 센서까지의 월드 벡터, `R_world_sensor`는 센서 축에서 월드 축으로의 회전, `alpha`는 각가속도다.

```text
a_COM_world = d(v_COM_world) / dt
a_sensor_world = a_COM_world
                 + alpha_world × r_world
                 + omega_world × (omega_world × r_world)
f_sensor = transpose(R_world_sensor) * (a_sensor_world - g_world)
gyro_sensor = transpose(R_world_sensor) * omega_world
```

현재 차량에서 확인한 중력 벡터를 `g_world`로 사용하되, 이 식은 **센서 모델의 설계식이며 구현·실측된 IMU 출력이 아니다.** 캐빈·트레일러의 상대 운동에는 추가 항이 필요하다. 만약 월드 속도 대신 차량 축 속도 `u = transpose(R) * v_COM_world`를 차분한다면 `a_COM_local = du/dt + omega_local × u`가 된다. SDK는 앞 절에서 확인한 것처럼 `du/dt`의 10-step 평균을 만든다. 일정 속도로 선회하는 경우에도 회전 항은 남을 수 있으므로 SDK 가속도에 중력만 더하는 방법으로는 충분하지 않다. 또한 평균된 가속도에 순간 각속도·속도를 섞어 원래의 순간 IMU를 정확히 복원할 수는 없다. 필터를 적용한다면 같은 시각의 물리량으로 센서 모델을 계산한 뒤 적용해야 한다.

근거는 `physics-scene-gravity-getter.txt`, `physics-scene-gravity-setter.txt`, `physics-actor-scene-getter.txt`, `physics-actor-flags-getter.txt`, `physics-body-gravity-enabled.txt`, `physics-body-gravity-setter.txt`, `physics-member-gravity-getter.txt`, `vehicle-gravity-force-update.txt`, `physics-body-force-apply.txt`, `physics-body-mass-getter.txt`, `physics-body-mass-setter.txt`, `physics-backend-mass-set-get.txt`와 기존 차량 사후 갱신 disassembly다. 관측은 `physics-gravity-observation.json`（22회·565 bytes·약 0.45 ms）과 `vehicle-gravity-force-observation.json`（17회·122 bytes·약 0.28 ms）이다. 모두 `research/live/2026-10-08-render-path/`에 있다. 각각 비원자적 외부 읽기이며 observer PID 13740·19180은 handle을 닫고 종료했다. 자동 중력 설정을 바꾸거나 게임 함수를 호출하지 않았다.

### 힘 누적값의 소비와 PhysX 결과의 반영

**현재 차량 코드는 바깥 물리 step 하나 안에서 힘·자세 계산을 3번 수행한다. 그 결과와 PhysX의 다음 계산 결과를 연결하는 별도 상태도 유지한다.** 힘 누적 버퍼의 0값이나 SDK callback 시각만으로 모든 물리량의 의미·시각을 판정할 수 없는 이유다.

실행 중 `g_physics_sim_merge_explicit = 0`, `g_physics_sim_velocity_derived = 0`을 읽었다. 두 설정 객체의 정수 캐시가 유효했고 override 포인터는 NULL이었다. 이하 순서는 이 분기의 해석이다. 설정을 변경하거나 다른 분기의 실행 결과를 관측하지 않았다.

차량 `V+0x210` 배열에는 두 계산 기록 P가 있었다. 각각의 첫 포인터는 `V+0x30`의 본체와 `V+0x1278`의 캐빈 body에 일치했다. `V+0x23C`의 반복 횟수는 **3**, 각 P `+0x0C`의 시간간격은 **0.00555533357 s**였다. 세 번의 합은 바깥 step **16666 μs**에 해당한다. 이는 차량 내부 계산의 분할이며, SDK가 180 Hz로 호출된다거나 PhysX scene을 step당 세 번 실행한다는 근거는 아니다.

이 값의 생성 경로도 확인했다. 트럭 초기화 `0x641510 → 0x8620F0`에서 **`V+0x23C = 3`을 직접 저장**하고, `V+0x238 = float(step_microseconds * 1e-6) * 0.3333333433`으로 시간간격을 만든다（`0x862175..0x8621A1`）. 본체 계산 기록의 생성자 `0x16579F0`는 이 값을 P `+0x0C`에 저장한다. 캐빈 기록도 `0x641B96..0x641BA9`에서 같은 V `+0x238`을 전달받는다. 따라서 이 초기화 경로의 3회 분할은 관측한 FPS에서 추정한 값이나 FPS에 따라 선택하는 식이 아니다. 다른 빌드나 모든 차량 구현에 동일한 값이 적용된다는 주장은 아니다.

| P 내부 필드 | 확인한 역할 |
| --- | --- |
| `+0x00` | 계산 대상 body 포인터 |
| `+0x08` | 생성자에서 1로 저장하는 integration scheme 값. enum의 이름별 대응은 미확인 |
| `+0x0C` | 내부 계산의 시간간격, float seconds |
| `+0x10` | 내부에서 갱신하는 월드 자세, 32-byte 셀 좌표·quaternion |
| `+0x30`, `+0x3C` | 내부 월드 선형 속도·각속도 float3. 각속도는 rad/s |
| `+0x48` | PhysX 결과를 받아 두는 기준 자세 |
| `+0x68`, `+0x74` | PhysX에 전달할 속도와 돌아온 결과를 처리하는 작업 필드 |

`0x86D140`의 내부 반복에서 `0x1657B40`을 호출한다. 이 함수는 body slot `+0x2A8 → 0x163E850`에서 힘 `body+0x9C`, slot `+0x2C8 → 0x163EBF0`에서 토크 `body+0xA8`을 읽는다. 역질량, 역관성 값과 관성 축의 월드 회전을 모아 `0x165A1C0`에 넘긴다. 해당 함수는 다음 증가량을 P의 속도에 더한다.

```text
v_world += F_world * inverse_mass * dt_inner
omega_world += inverse_inertia_world * torque_world * dt_inner
```

여기서 역관성 텐서는 물체의 회전과 body `+0xB4`의 관성 축 회전을 반영한다. 위 식은 이 함수의 계산이며 엔진 전체의 모든 회전 효과를 설명한다는 주장이 아니다. 이어 `0x1657B40`은 body slot `+0x2A0`, `+0x2C0`에 0 벡터를 넘겨 두 누적 버퍼를 비운다. **관측 당시 두 버퍼는 0이었지만, 이는 힘이 없는 상태의 증명이 아니다.** 읽기 시점이 소비·초기화 뒤일 수 있다.

현재 world `+0x40 = 1`이므로 자세 갱신은 `0x1659C40` 경로였다. 갱신된 P의 속도·각속도·자세는 body slot `+0x240`, `+0x248`, `+0x110`을 통해 PhysX backend의 `setLinearVelocity`, `setAngularVelocity`, `setGlobalPose`에 전달된다. 분석 도구가 게임 함수를 호출한 것이 아니라 게임 실행 파일에 있는 이 호출 경로를 확인한 것이다.

다음 바깥 step에서는 아래와 같이 상태를 연결한다.

1. **사전 처리 `0x8635F0`:** 기준 자세 P `+0x48`과 내부 자세 P `+0x10`의 차이를 바깥 step 길이로 나눠 전달할 선형·각속도를 구한다（`0x1659A80`）. body에는 기준 자세와 이 속도를 설정한다. 자동 중력이 켜진 body의 선형 속도에서는 scene 중력에 의한 증가량을 빼는 분기가 있으나, 현재 본체는 자동 중력이 꺼져 있다.
2. **PhysX scene 계산:** 앞 절에서 확인한 `simulate`와 `fetchResults` 경로가 실행된다.
3. **사후 처리 `0x863980 → 0x1657CE0`:** 현재 `g_physics_sim_velocity_derived = 0` 분기는 body에서 자세·선형 속도·각속도를 읽어 P `+0x48`, `+0x68`, `+0x74`를 갱신한다. P의 내부 속도에는 `돌아온 속도 - 전달했던 속도`를 더한다. 내부 자세는 돌아온 기준 자세로 맞춘다.
4. **차량 내부 계산 `0x86D140`:** 다시 힘·토크를 모으고 세 번의 속도·자세 계산을 진행한다.

즉, P `+0x30`의 내부 속도와 P `+0x68`의 작업 속도는 용도가 다르다. 외부에서 두 값이 다르게 읽혔다고 어느 한쪽이 잘못되었다고 판단하지 않는다. 이번 표본은 비원자적 읽기이므로 그 차이로 정확한 시간 지연을 역산하지도 않는다.

**SDK 채널 사이에도 갱신 단계 차이를 확인해야 한다.** `0x643D10`은 먼저 `0x863980`에서 scalar speed·SDK 가속도 상태를 갱신하고, 이후 slot `+0x250`에서 위 내부 계산과 backend 상태 변경을 수행한다. 그 뒤 SDK 전달이 이루어지며 위치·속도 getter는 body를 다시 읽는 경로다. 따라서 같은 SDK callback 묶음이라는 이유만으로 scalar speed, 필터된 가속도, 위치, 벡터 속도가 동일한 적분 단계의 값이라고 보장할 수 없다. 확인한 것은 호출 순서와 각 getter의 원본이다. 이후 사후 처리 전체가 미치는 영향과 채널 간 정확한 시간 차이는 아직 계측하지 않았으며, 모든 채널에 일률적인 1-step 보정을 적용하지 않는다.

정밀 IMU나 영상 정답을 기록할 때는 필요한 자세·속도를 같은 물리 단계에서 수집해야 한다. 이번 관찰은 내부 substep, SDK 전달, 렌더 보간이 서로 다른 단계임을 확인한 것이며 동기화된 센서 기록기를 구현한 것은 아니다.

계산 기록의 생성자에 연결된 assert 문자열에는 원래 소스 경로 `prism/src/physics/physics_integrator.cpp`와 `physics_integrator_t`, `physics_actor_dynamic_t`, `physics_integration_scheme_t`가 남아 있다. P를 차량 내부 물리 적분 상태로 해석한 근거를 보강한다. 이 문자열만으로 integration scheme 1을 특정 알고리즘 이름에 대응시키지는 않는다.

근거 disassembly는 `vehicle-body-integrator.txt`, `vehicle-force-velocity-integrate.txt`, `vehicle-pose-integrator-mode1.txt`, `vehicle-pose-difference-to-velocity.txt`, `vehicle-body-result-correction.txt`, `physics-body-force-accumulator-getter.txt`, `physics-body-torque-accumulator-getter.txt`, 두 accumulator setter, `physics-body-inverse-inertia-getter.txt`, `physics-body-global-velocity-setter.txt`, `physics-body-global-angular-velocity-setter.txt`, `physics-body-world-placement-setter.txt`와 기존 pre/post-simulation 파일이다. 실행 상태는 `vehicle-force-integrator-observation.json`（22회·852 bytes·약 1.11 ms）과 `vehicle-feedback-configuration-observation.json`（13회·712 bytes·약 0.83 ms）에 있다. 모두 `research/live/2026-10-08-render-path/`에 있으며 observer PID 19144·14296은 읽기 handle을 닫았다. 게임 입력·설정·메모리는 변경하지 않았다.

### 내부 계산 뒤에 추가되는 driveshaft torque

**힘·토크의 추가가 전부 세 번의 내부 계산 안에서 끝나는 것은 아니다.** 차량 사후 처리 `0x643D10`의 끝에 연결된 `0x64C470`은 별도의 조건부 토크 추가 경로다. `0x86D140`의 내부 계산과 버퍼 초기화보다 뒤에 위치한다.

이 경로는 EXE `+0x2D470C0` 설정을 float getter `0x1DB4C0`으로 읽는다. 실행 중 설정 이름은 **`g_driveshaft_torque`**, 유효한 float 캐시 값은 **0.0**, override 포인터는 NULL이었다. 함수는 이 값이 양수인지 확인하는 조건（`0x64C700..0x64C708`）을 통과해야 최종 토크를 추가한다. **현재 읽은 설정에서는 이 추가 분기가 비활성이다.** 설정을 바꾸어 차체 반응을 실험하지 않았다.

조건이 충족되면 차량 자세로 로컬 전방 축을 월드 축으로 회전하고, 계산된 스칼라와 곱한 토크를 본체 slot `+0x2B0 → 0x163E870`에 전달한다（`0x64C8A0`）. 이 setter는 현재처럼 body `+0x98 != 0`이면 `+0xA8`의 토크 누적값에 더한다. 그러므로 내부 적분 직후 초기화한 버퍼라도 이후 처리에서 다시 채워질 수 있다. 이 사실만으로 그 값의 다음 소비 시각이나 모든 토크의 발생원을 확정하지 않는다.

[SCS의 ETS2 1.46 설명](https://blog.scssoft.com/2022/11/euro-truck-simulator-2-146-update.html?m=1)은 Driveshaft Torque를 가속 중 차체가 비틀리며 앞바퀴 한쪽이 약간 들리는 효과로 설명한다. 설치 빌드에서는 이를 실제 본체 토크 누적 경로까지 연결했다. 따라서 카메라 흔들림만의 설정으로 취급하지 않으며, 차량·IMU 거동을 비교할 때는 이 설정도 조건에 포함한다. 효과의 크기나 현실 차량과의 일치도를 측정한 것은 아니다.

추가 근거는 `vehicle-internal-step-setup.txt`, `truck-physics-initialize.txt`, `vehicle-body-integrator-initialize.txt`, `vehicle-post-simulation-tail.txt`, `physics-body-torque-apply.txt`, `physics-cvar-float-getter.txt`다. `vehicle-step-and-post-torque-observation.json`은 V의 시간간격·반복 횟수와 설정을 9회·353 bytes·약 1.91 ms에 읽은 기록이다. 모두 `research/live/2026-10-08-render-path/`에 있으며 observer PID 11448은 읽기 handle을 닫았다.

### 바퀴·캐빈 힘의 작용점과 누적 토크

**바퀴·서스펜션 쪽의 힘은 작용점과 함께 본체에 전달된다.** 바퀴 주변 접촉을 조사하는 경로와 힘을 만들어 적분 버퍼에 넣는 경로를 연결했다. 바퀴별 타이어 모델 전체나 개별 접지력을 SI 단위로 복원한 단계는 아니다.

내부 반복 `0x86D140`은 먼저 `0x869BC0`을 호출한다. 이 함수는 V `+0xC38`의 접촉 조회 객체를 사용한다. 현재 객체의 vtable은 RVA `0x231DB50`, 주요 slot `+0x08`, `+0x10`은 각각 `0xB501C0`, `0xB50330`이었다. 후자의 실행 파일에는 물리 충돌 조회 호출과 `physics_ray_collision_t` 배열 접근이 있다. 이어 V slot `+0x248 → 0x644120 → 0x86B280`에서 `sagging_probe_result_t` 결과와 `physics_suspension_t` 객체를 소비하고, 본체의 힘 적용 메서드를 호출한다. 두 타입 이름은 해당 경로의 배열 접근 assert 문자열에도 남아 있다.

본체의 slot `+0x280 → 0x163D9D0`은 **월드 힘과 actor 로컬 작용점을 받는 경로**다. actor 회전과 질량중심의 로컬 위치를 backend에서 얻은 뒤 다음 계산을 수행한다.

```text
r_world = R_world_actor * (point_actor - COM_actor)
force_accumulator_world += force_world
torque_accumulator_world += r_world × force_world
```

힘은 입력값 그대로 누적하고, 작용점에서 질량중심까지의 거리·방향으로 토크를 만든다. 현재 `body+0x98 = 1` 분기에서는 앞서 확인한 `body+0x9C`, `body+0xA8`에 더한다. 다른 분기는 PhysX의 `addForce`·`addTorque`에 전달한다. 바퀴 힘 호출 `0x86C0FF`에서는 작용점에 차량 원점 보정 `d`를 먼저 빼서 넘기는 코드도 확인했다. 따라서 SDK 차량 기준 로컬 위치를 이 함수의 actor 로컬 위치와 혼동하지 않는다.

같은 차량 함수 `0x644120`은 캐빈이 존재하고 해당 상태 조건을 통과하면 캐빈에도 slot `+0x280`을 호출한다. 확인한 `0x644883`, `0x6448D0`의 대상은 모두 `V+0x1278`의 캐빈이며 작용점과 힘 입력이 다르다. 이 메서드는 바퀴 힘 전용이 아니다. 누적값을 해석할 때는 본체·캐빈 중 어느 body인지와 힘을 추가한 경로를 함께 확인한다. 이 두 호출만으로 본체에 같은 크기의 반력이 동시에 누적된다고 주장하지 않는다. 내부 질량·힘의 스케일과 앞 절의 갱신 시점 문제도 그대로 적용된다.

바퀴 관련 V `+0xC48` 배열의 현재 네 byte는 모두 0이었다. 그러나 `0x869BC0`은 이 byte를 조건으로 부가 처리를 호출하고, 마지막 내부 반복에서 지우기도 한다（`0x86AA84..0x86AACD`）. **이 표본을 '네 바퀴가 모두 지면에서 떨어졌다'는 판정으로 사용하지 않는다.** 해당 플래그의 전체 생성 조건, 접촉 결과의 필드별 의미, 마찰·슬립 모델과 정상력의 단위는 추가 분석이 필요하다.

근거는 `vehicle-internal-wheel-update.txt`, `wheel-probe-query.txt`, `vehicle-internal-body-forces.txt`, `vehicle-wheel-force-update.txt`, `physics-body-force-at-point.txt`다. `vehicle-wheel-probe-observation.json`은 실제 접촉 조회 객체의 vtable과 바퀴 관련 배열을 13회·220 bytes·약 0.23 ms에 읽은 기록이다. 모두 `research/live/2026-10-08-render-path/`에 있으며 observer PID 26492는 읽기 handle을 닫았다. 관측은 비원자적이며 게임 함수를 호출하지 않았다.

## 현재 공개 구현을 사용할 때 해결할 부분

| 조건 | 발생 가능성과 영향 | 조치 시점 |
| --- | --- | --- |
| 여러 shared-memory buffer를 순서대로 읽음 | 동시 memcpy와 겹치거나 서로 다른 tick의 camera/traffic을 묶을 수 있음. 빈도 미측정, 정밀 센서 학습에는 중요 | 학습용 기록 전 producer의 snapshot 발행/sequence와 실제 render 시점 연결 필요. 독자가 두 번 같은 값을 읽었다고 동기화가 증명되지는 않음 |
| 같은 node 수로 경로 재계산 | source는 route 길이가 달라질 때만 복사하므로 변경 내용이 남을 수 있음. 해당 재계산이 발생하면 실제 영향 있음 | 경로를 제어/정답으로 쓰기 전 발행 조건 수정 |
| 경로 제거 | `route_task == nullptr` 분기에서 buffer를 비우지 않음. 이전 경로가 남을 수 있음 | route의 유효성·삭제를 producer 계약에 반영 |
| 경로 6000개 초과 | 복사 loop에 bound가 없어 메모리 침범 가능. 기본 단거리 관찰에서 가능성은 낮고 장거리/모드에서 미측정, 발생 시 영향 큼 | 큰 경로를 쓰기 전 count bound 적용 |
| 게임 업데이트 / 프로필 전환 | 고정 구조체 offset·객체 수명이 바뀔 수 있음. 플러그인 내부 역참조 실패 시 게임 충돌 가능 | 선택 빌드와 라이프사이클에서 확인. 버전 검사를 지워 강행하지 않음 |
| 기존 dxgi/FFB DLL과 새 후킹 도구 동시 로딩 | 현재 조합의 충돌 여부 미확인. 발생 시 캡처 실패·게임 충돌 | 첫 캡처 전 기존 설치 파일을 보존한 채 조합을 분리해 관찰 |

이 표는 읽은 소스에서 파악한 실제 조건이다. 현재 설치에 해당 플러그인을 넣거나 게임에서 재현한 결과는 아니다. 이번 범위에서는 upstream 코드를 수정하지 않았다.

## 남은 런타임 확인 순서

1. 연구용 프로필에서 차량이 놓인 도로를 로드하고 주차브레이크를 건다. 게임은 pause하지 않는다. 실제 빌드·렌더 API와 DLL 로딩을 로그로 확인한다.
2. 공식 SDK의 frame timing, pause 상태, truck pose/speed/RPM/input, wheel과 trailer config부터 읽는다. 예상되는 0값과 미지원 채널을 구분한다.
3. 지원 버전의 내부 reader로 camera pose/projection, 주변 차량과 semaphore를 본다. 운전하지 않아도 지나가는 차와 신호 변화를 대조할 수 있다.
4. 고개를 좌우로 움직여 current camera pose와 truck pose가 어떻게 달라지는지 본다. 이때 영상과 SDK pose를 혼용하면 왜 흔들리는지 관찰한다.
5. 내비 경로를 설정·변경·제거해 route UID와 갱신을 확인한다. 실제 도로 좌표는 활성 맵 자료와 연결한다.
6. DAF 2021 또는 Volvo FH 2024 디지털미러의 실제 draw와 color/depth target을 GPU 캡처로 연결한다. F2/고개 회전/화면 밖 상태에서 갱신을 비교한다.

일반적인 자차·교통·카메라·신호 값과 미러의 실제 GPU target 메타데이터는 외부 읽기에서 확인했다. 위 절차 중 아직 필요한 것은 SDK의 frame timing과 유효 채널, 의도적인 머리/F2/경로 전환 대조, GPU 픽셀 수집 및 RGB/depth 정합이다. 이번 읽기 전용 조사에서는 그러한 상태 전환이나 설치를 수행하지 않았다.

BEV/E2E 실험에서는 차량/신호의 내부 상태를 디버깅·정답 생성에 활용할 수 있다. 최종 모델을 영상 기반으로 평가하려면 해당 상태를 입력에 섞지 않는 데이터 흐름을 유지한다. 카메라 값에 접근할 수 있게 된 것과 독립 센서 4–6뷰를 완성한 것은 구분한다.
