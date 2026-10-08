# ot 0.17.0 — FH5 전방·좌우 4뷰 연속 RGB-D 기록

> **확정:** 기존 미러 슬롯 0–5를 임의 위치·회전·FOV의 센서로 전용했습니다. 서로 다른 샤시 상대 위치의 여섯 영상을 같은 Present 구간 27에서 수집했습니다. 월드 고정 카메라도 동작합니다.
> **0.11.0:** 여섯 RGB·깊이 버퍼를 각각 640×360으로 맞추고 공유 메모리로 실시간 표시했습니다. 약 10.3초에 완전한 6뷰 묶음 45개, 누락 0개였습니다.
> **0.12.0:** 미리보기 창에서 XYZ·yaw/pitch/roll·FOV를 바꾸고 배치를 JSON으로 저장합니다. 실제 편집 2회를 포함한 20.3초 실행에서 6뷰 묶음 94개·누락 0개였습니다.
> **0.12.1:** 미러 출력이 없는 pass의 문자열·JSON 생성을 생략합니다. 같은 코드 경로의 짧은 관측에서 compile-begin 평균 15.29 → 10.40µs, 이후 6뷰 12묶음 모두 완료했습니다. 게임 전경 FPS 비교 결과는 아직 없습니다.
> **0.13.0:** 카메라 배치만 유지하는 4-hook 모드를 추가했습니다. 배치를 유지하면서 영상 관측을 켜고 끌 수 있으며, 전환 직후 6뷰 5묶음 모두 완료했습니다. Present 기록에 실제 게임 전경 여부도 포함합니다.
> **0.14.0:** GPU에서 RGBA8·R32F로 변환하는 `rgbd8` 형식을 추가했습니다. 640×360 6뷰 픽셀 전송량은 44.24 → 11.06 MB로 75% 감소했습니다. 같은 프레임의 원본과 경량 깊이·복원 좌표를 대조했습니다.
> **0.15.0:** 기본 리그를 고속도로용 전방 협각·광각과 좌우 포드 4뷰로 변경했습니다. SDK 바퀴 구성으로 후축 기준 장착 좌표를 변환합니다. 같은 해상도에서 GPU 복사·변환 자원을 재사용하며 실제 4뷰 수집·크기/형식 전환을 확인했습니다.
> **0.16.0:** 현재 FH5의 모델 외판·미러 하우징에 장착점을 맞췄습니다. `basis: cabin`은 운전자 고개 회전 전의 캐빈 parent를 사용해 서스펜션 운동을 따릅니다. 실제 4뷰 수집과 종료 복구를 확인했습니다.
> **0.17.0:** 3슬롯 GPU ring과 native 수집 worker를 추가했습니다. 차량 메타데이터를 포함한 4뷰 RGB-D를 5초·50묶음·실측 10.006 Hz로 Python에 저장했고 누락은 0개였습니다. 수집 중 패닉·payload unload도 확인했습니다.
> **무손실 기록:** Python에 Zstandard 압축·2개 저장 worker를 추가했습니다. 후속 실제 5초·50묶음을 누락 없이 629MB에 저장했습니다(원시 바이너리 대비 약 45.5% 절감). 기존 core 0.17.0을 그대로 사용합니다.
> **움직이는 AI:** 저장한 연속 기록의 AI 2대·6시점에서 모델 박스와 RGB/DSV의 정합을 확인했습니다. 약 ±0.5초 다른 자세보다 같은 프레임 자세의 깊이 겹침 비율이 높았고, 해당 GPU draw 상수 29개의 투영 차이는 최대 0.000206px였습니다. 아래 명령으로 압축 묶음을 직접 분석합니다.
> **다음:** 전경 성능 비교, 트레일러 가림·주행 중 가시성 누락과 장시간 저장량 절감. 가상 LiDAR·레이더와 주행보조 모델은 후속 구현입니다.

### Phase 1 고속도로 4뷰

기본 실행은 [phase1-highway.json](presets/phase1-highway.json)과 `rgbd8`입니다. 전방 관측과 좌우 인접 차로를 우선하며, 문서의 예시 치수 대신 **현재 ETS2 Volvo FH5 4×2·l2h1 낮은 캡·좌핸들·mirror_01**의 형상으로 장착점을 정했습니다. 트랙터 정후방 카메라는 두지 않으며, 기존 6뷰 프리셋은 연구용 선택지로 유지합니다.

```powershell
.\ot\preview.cmd
# 배치만 유지하고 영상 수집은 끄기
.\ot\ot.cmd camera_rig apply --config .\ot\presets\phase1-highway.json --rig-only
# 원래 미러로 복귀
.\ot\ot.cmd panic
```

| 센서 / 슬롯 | base_link 위치 x/y/z (m) | yaw / pitch | HFOV | 실제 RGB-D 크기 |
| --- | --- | --- | --- | --- |
| C_FN / 0 | 5.038 / 약 0 / 3.092 | 0 / −1.5° | 30° | 1280×720 |
| C_FW / 1 | 같은 M_FC 원점 | 0 / −8° | 120° | 1280×720 |
| C_RL / 2 | 4.653 / +1.484 / 2.645 | +145 / −12° | 100° | 960×544 |
| C_RR / 5 | 4.653 / −1.484 / 2.645 | −145 / −12° | 100° | 960×544 |

`base_link`는 **구동 후축 중심 아래의 명목 지면점에 고정한 차량 좌표**입니다. +x 전방·+y 왼쪽·+z 위이며, 도로 요철마다 원점을 다시 옮기지 않습니다. `truck_config`의 바퀴 2·3 중심에서 반지름을 뺀 평균으로 정했고, SDK/chassis 기준 `[0, -0.0041681, 2.0315917]`입니다. 모델의 차축·cabin 로케이터가 SDK와 일치하며 축간거리는 약 3.8241 m입니다. 실제 접지면 측정값은 아닙니다. `position_base_link`는 `p_model = origin + (-y, z, -x)`로 변환합니다.

전방은 `sleeper_2021`의 **윈드실드 위 도장 외판** `(0, 3.08, -2.97208)`에, 좌우는 `mirror_01`의 **플라스틱 하우징** `(±1.45050, 2.63585, -2.61380)`에 붙입니다. 여기의 수치는 SDK와 같은 neutral chassis 축입니다. 표면 삼각형의 바깥쪽 법선을 따라 35mm 브래킷을 둔 점이 광학 중심입니다. 그림은 추출한 실제 모델의 정점이며, 게임에 카메라 하드웨어 모델을 새로 그린 것은 아닙니다.

![차량 형상과 장착점](../docs/images/fh5-body-mount-layout.png)

전방 협각은 먼 차로·선행차, 광각은 가까운 노면을 함께 보도록 pitch를 나눴습니다. 좌우는 기존 ±165°·70°에서 **±145°·100°**로 바깥을 향하게 하고 넓혔습니다. pitch −12°로 근거리 노면 비중도 늘렸습니다. 수치는 장착 가능 위치와 현재 영상을 보고 고른 주행 실험용 설정이며 빈틈없는 커버리지를 보장하지 않습니다.

네 카메라의 `basis: cabin`은 캐빈 운동이 없는 상태의 chassis/model 좌표를 입력받고, 렌더 시 캐빈 서스펜션을 포함한 parent로 변환합니다. 캐빈 pivot을 원점으로 하는 좌표와 다릅니다. `visual_interior +0x118 → vehicle +0x1098 → interior camera +0x4D4`를 읽으며 운전자 head pose는 포함하지 않습니다. 모델의 미러 로케이터와 실제 원본 미러 위치는 최대 2.49e−6 m 차이였습니다. 수집 앞뒤의 parent로 계산한 새 카메라 위치와 실제 pass 위치 차이는 정차 표본에서 최대 2.40e−5 m였습니다. 큰 제동·선회 중 부착 유지 시험은 남아 있습니다.

여러 구동축이면 `base_link_wheels`로 기준축 바퀴 index를 지정합니다. 변환 결과의 `mount_calibration`에 차량 ID·바퀴 index·원점을 남깁니다. 변환은 파일을 열거나 CLI로 적용할 때 실행됩니다. 편집기에서 저장한 JSON은 **변환된 model/chassis 좌표와 attachment basis**를 보존합니다. 이 프리셋은 `truck_id`가 다른 트럭에는 적용을 거절하지만, 같은 FH5의 캡·미러·좌핸들/우핸들 변경까지 자동 판별하지는 않습니다. 다른 구성에는 외판 보정을 다시 해야 합니다.

현재 미러 배율 2×2에서 base 크기 `[640,360]`은 1280×720, `[480,270]`은 엔진 정렬로 960×544가 됐습니다. 목표 960×540으로 잘라 저장하지 않으며 실제 projection·viewport·크기를 함께 사용합니다. 메인 화면 스케일링과 별개이고 게임 설정을 바꾸지 않습니다.

![외판 장착과 각도 조정 후 실제 영상](../docs/images/fh5-body-fit-0.16.0.png)

0.16.0의 캐빈 부착 4뷰 수집 4회 모두 완료했고, 최종 각도의 미리보기 worker도 5초에 16묶음·누락/오류 0회였습니다. 창은 띄우지 않았습니다. 마지막은 Tier 0·hook 0·리그 꺼짐이며 FFB가 유지됐습니다. 현재 트럭의 trailer actor는 NULL이므로 트레일러 가림은 시험하지 않았습니다. 전경 FPS, 선회·제동과 30분 안정성도 남아 있습니다. 원본은 로컬 `fh5-model-live-alignment.json`, `fh5-surface-mounts.json`, `fh5-body-fit-run.json`, `fh5-body-preview-result.json`입니다(`research/live/2026-10-08-camera-rig/`).

4뷰 경량 픽셀은 묶음당 **23,101,440 B**(현재 원본의 25%)입니다. 고해상도 두 전방 뷰 때문에 기존 640×360 6뷰보다 픽셀 수는 많습니다. 컴퓨트 입력·출력·shader·상수 버퍼, staging과 완료 query를 재사용합니다. 기본 차량 draw 추적이 꺼진 상태에서 첫 형식별 준비 뒤 추가 GPU 자원 생성 없이 반복 수집했습니다. 해상도 변경은 필요한 자원만 다시 만들며 raw/경량 전환 후 실제 출력도 확인했습니다. 같은 프레임의 원본 DSV와 경량 깊이 차이는 유효 픽셀에서 0이었습니다.

0.15.0에서 도입한 카메라별 자원 재사용에 이어, 0.17.0은 아래의 연속 ring과 차량 draw 상수 staging 재사용까지 지원합니다. 패닉·관측 종료·payload unload에서 보유 GPU 자원을 해제합니다. 0.15.0 원본은 로컬 `research/live/2026-10-08-camera-rig/phase1-first-run.json`, `phase1-resource-reuse.json`, `phase1-preview-run.json`, `phase1-first/`에 있습니다.

### 연속 4뷰 기록

미리보기 창 없이 기록하며 포커스를 바꾸지 않습니다. 게임이 실제로 렌더링 중이어야 합니다. 새 출력 디렉터리를 지정합니다.

압축 기록은 Python 선택 의존성 `zstandard`를 사용합니다. 이 PC에는 `ot/.venv`에 설치했습니다. 다른 PC에서는 최초 한 번 다음 명령을 실행합니다. wheel 버전·SHA256은 [requirements-recording.txt](requirements-recording.txt)에 고정했으며 Python 3.13 / Windows x64용입니다. `--system-site-packages`는 기존 NumPy·Pillow 설치를 재사용합니다. `ot.cmd`는 이 가상환경이 있으면 사용하고 없으면 기존 `py -3.13`을 사용합니다.

```powershell
py -3.13 -m venv --system-site-packages .\ot\.venv
.\ot\.venv\Scripts\python.exe -m pip install --only-binary=:all: --require-hashes -r .\ot\requirements-recording.txt
```

```powershell
.\ot\ot.cmd record_bundles --config .\ot\presets\phase1-highway.json --hz 10 --duration 5 --vehicles --output .\research\live\my-road-run
```

한 번의 `stream start`로 DLL이 촬영을 예약하고, Python은 `OT_Bundles`를 읽어 저장합니다. 묶음마다 arm/poll/publish 명령을 보내지 않습니다. 3개 GPU 슬롯은 필요한 자원을 첫 사용에 준비한 뒤 순환 재사용하며, immediate-context 복사와 readback은 기존 렌더 callback에서만 수행합니다. native worker는 완성된 CPU 데이터의 공유 메모리 발행을 담당합니다. 차량 상수도 기존 staging을 재사용하고 실제 크기가 바뀔 때만 다시 만듭니다.

기본 출력은 `frame-<Present 구간>.tar.zst`, `index.jsonl`, `run.json`입니다. Zstandard level 1의 **무손실 압축**이며 내부의 카메라별 `images.json`·RGB·깊이·상수 파일은 기존 디렉터리 형식과 같습니다. `--archive zip`은 추가 코덱 없이 기존 비압축 ZIP으로 저장합니다. `otpy.bundles.load_bundle()`과 `birdseye`는 디렉터리·ZIP·TAR.ZST를 읽습니다. 카메라별 기존 분석 명령에는 해당 형식을 지원하는 압축 도구로 푼 디렉터리를 넘길 수 있습니다. 파일 덮어쓰기는 하지 않습니다.

기본 `--workers 2`는 독립된 프레임을 두 스레드에서 압축·저장합니다. 대기 중인 묶음 수도 worker 수로 제한해 느린 디스크에서 메모리가 계속 쌓이지 않게 했습니다. 각 압축기는 스레드 안에서 소유하고, index는 수신 순서대로 하나의 스레드가 씁니다. 종료 시 모든 writer를 합류시킨 뒤 결과를 저장합니다. 압축 해제는 색상 바이트·float32 깊이의 비트 표현을 복원하며 Zstandard checksum과 프레임 완료 여부를 확인합니다.

기본 `rgbd8`은 첫 raw 표본으로 공통 노출을 계산해 실행 내내 고정합니다. `--color-gain`을 주면 이 준비 표본을 생략합니다. `--vehicles`는 같은 pass의 차량 모델 자세·draw 상수 수집을 켜며 생략하면 카메라 데이터만 기록합니다. `raw`·`raw+rgbd8`도 선택할 수 있습니다. 색상 gain은 물리적 카메라 노출 모델이 아닙니다.

`--duration` 뒤에는 새 촬영을 멈추고 진행 중인 복사를 최대 1초 더 회수합니다. 종료·예외·Ctrl+C에서 stream을 멈추고 Tier 0으로 돌아갑니다. 수집 중 배치 변경은 거절하며 `stream stop` 후 수정합니다. 소비가 밀리면 공유 메모리의 새 묶음을 버리고 `queue_dropped`에 기록합니다. 이미 소비 중인 슬롯을 덮어쓰거나 다른 Present 구간의 뷰를 합치지 않습니다.

직접 API를 쓰는 경우, 적용된 observe 리그에서 `stream start --hz 10 --duration 5 --color-gain 0.55`, `stream status`, `stream stop`을 사용할 수 있습니다. 자동 기간 종료는 worker와 GPU 자원을 정리하지만 카메라 리그는 유지합니다. 원래 미러 복귀는 `panic`입니다. 기록 CLI는 이 복귀까지 수행합니다. 공유 메모리 크기를 키워야 하면 기존 reader를 닫고 core를 reload합니다.

실제 FH5 정차 실험에서는 전방 1280×720 두 장·측후방 960×544 두 장과 차량 메타데이터를 **5초에 50묶음, 저장 누락/수집 오류 0회**로 기록했습니다. 복사 제출 시각의 실측 간격은 중앙값 101.65ms, 전체 처리율은 10.006Hz였습니다. 50묶음의 200뷰 모두 묶음의 Present 구간·세션과 일치했고, 첫·중간·마지막 묶음의 기존 점군 복원과 마지막 RGB를 확인했습니다. 30분 주행·전경 FPS 결과는 아닙니다.

초기 파일별 저장은 2초·20수집 중 14저장·6누락이었습니다. 111개 파일을 여닫는 비용과 반복 JSON 쓰기를 ZIP 한 개로 묶어 해결했습니다. 같은 묶음의 오프라인 저장은 약 0.246초 → 0.043초였고, 원본 파일 바이트와 metadata를 다시 읽어 대조했습니다. 비압축 50묶음의 바이너리는 1,155,725,568B로 **약 231MB/s**였습니다.

후속 압축 실험은 **5초·50수집·50저장·누락/오류 0회**, 1,155,710,976B의 바이너리를 metadata 포함 **629,479,352B**로 저장했습니다. 약 45.5% 절감·126MB/s이며, 30분으로 환산하면 약 227GB입니다. 짧은 정차 장면 기준이므로 주행·날씨·주변 물체에 따라 압축률과 비용은 달라집니다. 대표 기존 3표본은 압축 전후 모든 파일 바이트와 metadata가 일치했고, 새 압축 기록의 첫·중간·마지막 점군 복원과 마지막 RGB를 확인했습니다. 잘린 프레임과 checksum 손상도 읽기에서 거절했습니다. 원본은 로컬 `stream-zstd-live/`와 `zstd-first/`입니다(`research/live/2026-10-08-camera-rig/`). 장시간 전체 기록의 저장량은 여전히 큽니다.

수집 중 패닉에서 worker 종료·hook 0, 수집 중 메타로더 unload에서 core와 pipe 해제를 확인했습니다. 다시 로드하면 Tier 0이며 SDK·기존 FFB가 유지됩니다. 로컬 원본은 `research/live/2026-10-08-camera-rig/stream-017-archive/`와 `stream-017-lifecycle.json`입니다.

### 화면을 보며 카메라 배치 조절

`ot\preview.cmd`를 실행하면 영상 아래에 편집기가 열립니다. 게임 재실행이나 DLL 재로딩 없이 적용됩니다.

1. 목록에서 카메라 슬롯을 고릅니다. `basis`는 읽어 온 JSON의 `chassis`, `cabin`, `world`를 유지하며 창에 표시합니다.
2. XYZ와 yaw·pitch·roll(도), 가로·세로 FOV를 입력합니다. 샤시 기준 +X 오른쪽, +Y 위, +Z 뒤이며 yaw 양수는 왼쪽, pitch 양수는 위입니다. 회전 순서는 `Ry(yaw) Rx(pitch) Rz(roll)`입니다.
3. `Auto apply`는 마지막 입력 250ms 뒤 적용합니다. 끄면 `Apply`·Enter 또는 다른 카메라 선택 시 적용합니다. 잘못된 입력은 오류를 표시하고 마지막 유효 배치를 계속 보여 줍니다.
4. `Reset camera`는 선택한 카메라만 파일을 열었을 때의 배치로 되돌립니다. `Save layout…`은 DLL에서 적용이 확인된 전체 배치를 저장합니다. 아직 적용 중이면 안내가 나온 뒤 다시 저장할 수 있습니다.

저장 형식은 기존 `views`·`quaternion_wxyz`·FOV 계약을 유지하므로 아래처럼 다시 엽니다. 위치나 FOV만 바꾸면 기존 quaternion 값은 보존합니다. PNG 저장은 현재 표시 영상, JSON 저장은 카메라 배치입니다.

```powershell
.\ot\ot.cmd preview --config .\camera-layout.json
```

![첫 카메라와 마지막 카메라를 수정한 실제 영상](../docs/images/camera-editor-0.12.0.png)

실제 게임에서 첫 카메라를 높이 12·pitch -60°로 옮기고 초기화한 뒤 XYZ·방향·FOV를 다시 바꿨습니다. 저장한 JSON을 다시 열어 슬롯 5를 편집했으며, FOV 0° 거절 후 유효 값으로 복구되는 동작도 확인했습니다. 최종 빌드는 편집 두 번을 포함해 20.318초에 94묶음·누락 0개·수집/종료 오류 0개였습니다. 창의 위젯 callback과 실제 수집 worker를 실행한 결과이며 시스템 마우스·키보드 자동화는 사용하지 않았습니다.

초기 편집 실험의 누락 2/70회 및 4/48회를 조사하면서, 상태 조회가 mutex를 잡으면 렌더 callback이 `try_to_lock` 실패로 필수 바인딩을 건너뛰는 경로를 수정했습니다. 수집 중에만 같은 mutex에서 순서를 기다리고, idle/ready는 atomic phase로 즉시 반환합니다. 상태 조회의 대기 비용은 남으며 장기 성능 비교를 대신하는 결과는 아닙니다. 여섯 카메라가 `armed`인 상태의 메타로더 unload/load도 성공했고 Tier 0·hook 0·SDK 9채널로 복귀했습니다. 실행 원본은 로컬 `research/live/2026-10-08-camera-rig/editor-final-result.json`, `editor-pending-unload.json`에 있습니다.

### 실시간 미리보기

일반 DX11 게임의 운전석에서 프로젝트 루트의 다음 명령을 실행합니다. Python 3.13의 NumPy·Pillow·Tk를 사용하며 연구 PC에는 설치되어 있습니다. 아래 리그 사용 절의 권한 설정이 필요합니다.

```powershell
.\ot\preview.cmd
# 10초 후 자동 종료하고 마지막 표시 영상을 새 PNG에 저장
.\ot\preview.cmd --duration 10 --snapshot phase1-views.png
# 다른 카메라 배치 파일 사용
.\ot\preview.cmd --config .\ot\presets\surround-preview.json --hz 5
```

창에서 선택한 영상을 함께 보고 공통 노출(EV)을 바꾸거나 PNG로 저장할 수 있습니다. 4뷰는 2×2, 6뷰는 3×2로 표시합니다. 창을 닫으면 수집을 멈추고 `panic`으로 기본 미러·Tier 0으로 돌아갑니다. 게임이 전경이면 DLL의 F11이, 미리보기에 포커스가 있으면 창의 F11 종료 callback이 동작합니다. 다른 앱에 포커스가 있을 때의 전역 단축키는 아니며 물리 키 시험은 수행하지 않았습니다. 미리보기용 임시 이미지 파일은 만들지 않으며 선택한 형식의 묶음을 `OT_Bundles`에서 읽습니다. 별도 창이므로 게임 화면을 가리거나 비활성 FPS 제한에 영향을 줄 수 있습니다.

[surround-preview.json](presets/surround-preview.json)은 6방향 샤시 리그에 16:9 FOV와 `base_resolution: [320, 180]`을 지정합니다. 이 값은 **게임의 미러 렌더 배율을 적용하기 전 크기**입니다. 연구 PC의 미러 배율 2×2에서 실제 RGB·깊이 버퍼는 모두 640×360이었습니다. 메인 화면 스케일링과 별개이며 게임 설정은 변경하지 않습니다. 실제 크기는 각 영상 제목에 표시합니다.

![실시간 미리보기에서 마지막으로 표시한 실제 6뷰](../docs/images/surround-preview-0.11.0.png)

5 Hz를 요청한 첫 실행은 시작·종료 시간을 포함해 10.2906초 동안 완전한 묶음 45개를 수집했습니다. 누락·수집 오류·종료 오류는 0개였습니다. 이 값은 미리보기 수집 속도이며 게임 렌더 FPS가 아닙니다. 아직 GPU staging ring을 사용하지 않으므로 고해상도·고주파 수집 성능을 보장하지 않습니다. 로컬 실행 결과는 `research/live/2026-10-08-camera-rig/preview-first.stdout.txt`와 `preview-first.png`입니다.

해상도는 미러 그래프가 context 크기를 전달하는 호출의 인자만 바꿉니다. 새 관측 지점은 `0x1610250`, 대상 호출의 복귀 주소는 `0x4D46B8`이며 원본 drawable과 설정은 유지합니다. 실제 6뷰가 렌더되는 상태에서 새 hook을 포함한 아홉 hook의 메타로더 해제·재로딩도 완료했습니다. 재로딩 후 리그 꺼짐·Tier 0·hook 0이며, 원본 기록은 로컬 `resolution-active-unload.json`입니다.

### 저장한 6뷰의 월드 점군과 위에서 본 관측 영역

`otpy.bundles.save_bundle`로 저장한 묶음은 아래 명령으로 합칩니다. 게임에 연결하지 않는 오프라인 명령입니다.

```powershell
.\ot\ot.cmd birdseye .\capture-bundle --output observed.png --points world-points.npz
# 원래 픽셀 중심을 두 칸 간격으로 표본화하고 반경 25만 표시
.\ot\ot.cmd birdseye .\capture-bundle --output observed-small.png --stride 2 --radius 25
```

기존 DSV 복원을 그대로 사용하고 각 pass 카메라의 회전·월드 원점으로 합칩니다. 서로 다른 관측 세션 또는 Present 구간의 영상을 하나로 합치지 않습니다. PNG는 월드 +X 오른쪽·+Z 아래 방향이며 카메라 위치 평균을 중심으로 그립니다. 차량 heading에 정렬한 BEV나 주행 가능 영역 분류는 아닙니다. 같은 격자에서는 가장 높은 관측 점을 표시하고 빈칸을 보간하지 않습니다.

NPZ에는 `xyz_world`, `rgb_linear`, `camera_index`, `camera_origins_world`, `observed`, `height_above_camera_mean`, `center_world_xyz`, `game_units_per_pixel`, `metadata_json`을 저장합니다. 원시 점은 표시 반경 밖도 보존합니다. `observed=false`는 관측하지 못한 영역이며 장애물 부재를 뜻하지 않습니다. RGB 노출 조정은 PNG에만 적용합니다.

기존 640×360 6뷰의 Present 구간 14에서 **1,201,153점**을 합쳤습니다. 반경 40·800×800 격자 중 124,405칸에 관측 점이 있었습니다. 파일 입력의 기존 복원 결과와 공유 바이트 입력의 결과, stride 2와 원본 픽셀 부분집합의 월드 좌표 차이는 이 표본에서 0이었습니다. [실제 출력](../docs/images/six-world-birdseye.png). 새 캡처를 주장하는 결과가 아니며 원본은 로컬 `six-resolution-first.json`, 변환 결과는 `six-world-points.npz`입니다.

### 경량 RGB-D 형식

```powershell
# 첫 원본 묶음으로 공통 노출을 맞춘 뒤 경량 수집으로 전환
.\ot\preview.cmd --format rgbd8
# 노출을 직접 고정할 때; 841.55는 이번 저녁 도로 표본에서 얻은 값
.\ot\preview.cmd --format rgbd8 --color-gain 841.55
# 이미 활성화된 리그에서 한 묶음 요청
.\ot\ot.cmd capture_mirrors arm --format rgbd8 --color-gain 841.55
.\ot\ot.cmd capture_mirrors status
# ready 이후 기존 save 또는 publish / BundleReader / save_bundle 사용
.\ot\ot.cmd capture_mirrors publish
```

| 형식 | 픽셀 내용 | 640×360 6뷰 픽셀 크기 |
| --- | --- | --- |
| `raw` (기본) | 기존 color·attributes0·attributes3·DSV 원본 | 현재 DSV 형식에서 44,236,800 B |
| `rgbd8` | RGBA8 색상 + R32F 깊이 | 11,059,200 B |
| `raw+rgbd8` | 같은 프레임의 두 형식; 변환 대조용 | 55,296,000 B |

RGBA8의 alpha는 255입니다. 색상은 음수·비유한 성분을 0으로 처리하고 공통 `color_gain` → Reinhard → sRGB를 적용합니다. `*_color_ldr.bin`의 metadata에 적용한 `linear_gain`을 저장합니다. 양자화·톤매핑된 색상이며 원본 HDR을 복구하는 형식은 아닙니다. 단일 캡처 API의 기본 gain은 1입니다. 미리보기에서 gain을 생략하면 첫 완전한 원본 묶음의 공통 밝기 표본으로 한 번 정하고 이후 고정합니다. 첫 보정 묶음의 전송량은 원본과 같습니다.

`*_depth_f32.bin`은 **viewport 변환 후 DSV 값**을 보존한 R32F입니다. 축 방향 거리나 미터를 직접 저장하지 않습니다. attributes Z가 0/비유한, 재질 bit 16, viewport 밖, 깊이 범위 밖인 픽셀은 GPU에서 NaN으로 만듭니다. 같은 pass의 projection·viewport·pose는 계속 전달하므로 `reconstruct`·`birdseye`·`project_boxes`가 기존 식으로 카메라/월드 좌표를 복원합니다. 경량 형식에는 원본 재질 배열이나 shader-adjusted attributes 깊이가 없으며 `material_bits` 출력도 생략합니다. 복원의 `rgb_linear`는 이 형식에서 톤매핑 후 선형 색상이고, metadata `color_encoding=linear_reinhard`로 원본 `linear_hdr`와 구분합니다.

원본 render target은 유지하고 private GPU copy를 compute shader 입력으로 사용합니다. 변경한 CS shader·SRV·UAV·constant buffer 범위는 즉시 복원합니다. UAV counter는 [D3D11의 유지 값](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-cssetunorderedaccessviews)을 사용합니다. HLSL은 Windows SDK의 `fxc`로 빌드 때 컴파일하며 런타임 컴파일은 없습니다.

실제 6뷰 대조에서 유효 마스크 불일치 0, 유효 DSV 값 차이 0, 복원 월드 좌표 차이 0이었습니다. 이는 변환 경로의 보존 결과이며 실제 장면의 깊이 정확도 보증은 아닙니다. gain 1 및 841.55에서 CPU 색상 변환과 최대 1/255 차이였습니다. 경량 수집 32회 요청 중 첫 리그 시작의 슬롯 3·4 누락 1회, 이후 31묶음 완료(전체 3.65초); 완전한 묶음당 전송 payload는 약 11.09 MB였습니다. 별도 미리보기 worker는 자동 노출 보정을 포함해 4초 동안 15묶음·누락/오류 0회였습니다. pending 상태의 payload unload/load와 원본 형식 재수집도 확인했습니다.

![경량 형식에서 표시한 실제 6뷰](../docs/images/six-rgbd8-0.14.0.png)

이번 0.14.0 계측은 백그라운드이며 고정 10 Hz·장시간 주행 결과가 아닙니다. 후속 0.15.0에서 staging·compute 자원 재사용을 추가했습니다. `OT_Bundles` 슬롯은 첫 발행 크기로 정해지므로, 경량 묶음으로 시작한 세션에서 더 큰 원본 묶음을 발행하려면 reader를 닫고 payload를 reload해야 합니다. 실제 결과는 로컬 `research/live/2026-10-08-camera-rig/rgbd-compact-run.json`, `rgbd-first-comparison.json`, `rgbd-reconstruction-comparison.json`, `rgbd-color-gain-comparison.json`, `rgbd-preview-auto.json`에 있습니다.

### 미러 pass만 자세히 관측

0.12.1은 `pass+0xC0` 대신 연결된 **출력 이미지 namespace**를 먼저 읽습니다. 실제 미러 geometry pass의 namespace는 `deferred`, color pass는 `quad_drawer_t*`여서 pass namespace를 `mirror`로 제한하면 필요한 영상도 놓칩니다. 미러 출력이 없는 명령 구간은 이름 없이 계속 기록하여 재사용된 명령 buffer의 범위를 구별합니다.

전·후 약 4초 관측에서 compile-begin 평균은 15.290 → 10.396µs, 초당 callback 본문 시간은 123.39 → 103.14ms였습니다. 호출 빈도도 8,070 → 9,921회/s로 달라졌으므로 평균 감소 약 32%를 게임 FPS 증가율로 해석하지 않습니다. 같은 세션의 전경 조건은 확인되지 않았습니다. 후속 12묶음의 여섯 카메라가 모두 ready였으며 원본은 로컬 `compile-before-filter.json`, `compile-after-filter.json`, `filtered-capture-results.json`입니다.

### 리그만 유지하고 필요할 때 수집

```powershell
# 카메라 배치 4개 hook만 활성화
.\ot\ot.cmd camera_rig apply --config .\ot\presets\surround-preview.json --rig-only
# 배치를 유지하며 Present 계측 추가: 5개
.\ot\ot.cmd render_probe on --mode rig --frames
.\ot\ot.cmd frames
# 배치를 유지하며 영상 수집 모드: 8개
.\ot\ot.cmd render_probe on
.\ot\ot.cmd capture_mirrors arm
.\ot\ot.cmd capture_mirrors status
# 차량 draw 메타데이터도 필요하면 9개
.\ot\ot.cmd render_probe on --vehicles
# 관측과 진행 중 수집을 멈추고 배치만 유지
.\ot\ot.cmd render_probe on --mode rig
.\ot\ot.cmd panic
```

`rig` 모드는 카메라 선택·제출 begin/end·해상도 hook만 유지합니다. SDK 및 기존 Tier 1 내부 상태 읽기는 계속됩니다. 캡처 요청은 `observe`에서만 받습니다. 모드 변경 시 진행 중 캡처는 취소하지만 네 카메라 hook과 배치는 계속 유지합니다. 같은 모드·옵션을 다시 요청하면 진행 중 캡처도 유지합니다. 기본 모드는 기존 클라이언트와 호환되는 `observe`입니다.

`frames.enabled`는 Present hook 상태, 각 record의 `game_foreground`는 해당 Present 시점의 전경 창이 게임 프로세스 소유인지 나타냅니다. 일시정지 여부나 GPU 사용률을 대신하는 값은 아닙니다. 리그만 켜고 계측하지 않으면 프레임 기록도 생성하지 않습니다.

실제 게임에서 4 → 5 → 8 → 9 → 4 전환, 모든 슬롯의 갱신, 관측 callback 정지, 전환 직후 6뷰 5묶음 완료, 진행 중 캡처 취소 및 활성 리그 상태의 메타로더 unload/load를 확인했습니다. 마지막은 Tier 0·hook 0·리그 꺼짐입니다. 계측 표본 30개는 모두 백그라운드였으므로 전경 FPS 비교는 남아 있습니다. 원본은 로컬 `research/live/2026-10-08-camera-rig/rig-only-transition.json`입니다.

### 자유 배치 리그 사용

게임 옆 `ot_runtime/ot_config.json`의 `allow_camera_rig`, `allow_tier1`, `allow_render_probe`, `singleplayer_research`를 켜야 합니다. 저장소 기본 설정은 모두 꺼져 있습니다. 연구 PC에는 권한을 적용했으며 시작 상태는 계속 Tier 0입니다.

프로젝트 루트에서 실행합니다. 콘솔 입력이나 게임 재실행은 필요 없습니다.

```powershell
.\ot\ot.cmd camera_rig apply --config .\ot\presets\surround-six.json
.\ot\ot.cmd capture_mirrors arm
.\ot\ot.cmd capture_mirrors status
# phase가 ready이면 RGB·깊이·재질·카메라 상수 저장
.\ot\ot.cmd capture_mirrors save
.\ot\ot.cmd panic
```

`camera_rig apply`는 probe와 Tier 2 리그를 켭니다. `camera_rig off`는 기본 미러로 복귀하고 선택된 render hook은 유지합니다. `panic` 또는 게임 전경의 F11은 리그와 활성 hook을 모두 끕니다. `capture_mirrors`는 arm 때 설정된 리그 슬롯을 묶으며, 리그가 없으면 기존 0·1·2·5를 요청합니다.

[surround-six.json](presets/surround-six.json)의 각 view에서 다음을 바꿉니다.

- `slot`: 기존 출력 슬롯 0–5. 새 카메라 객체나 엔진 슬롯 확장은 필요하지 않습니다.
- `basis`: `chassis`는 렌더 보간된 본체 기준, `world`는 절대 월드 좌표입니다. `cabin`은 neutral chassis/model 좌표로 지정한 장착점을 캐빈 서스펜션 parent에 붙입니다. 세 모드 모두 운전자 head pose는 입력에 사용하지 않습니다.
- `position`: XYZ. 샤시 기준 +X 오른쪽, +Y 위, -Z 앞입니다. 단위는 게임 길이 단위입니다.
- `quaternion_wxyz`: 카메라에서 기준 좌표계로의 회전. 단위 회전은 -Z를 봅니다. yaw·pitch·roll을 함께 지정할 수 있습니다.
- `hfov_deg`, `vfov_deg`: 가로·세로 FOV. 정사각 픽셀을 원하면 `tan(hfov/2) / tan(vfov/2)`와 출력 가로/세로 비율을 맞춥니다.
- `base_resolution`: 선택적인 `[width, height]`. 생략하면 기존 미러 크기를 사용하며 지정하면 게임 미러 배율을 적용하기 전 기본 크기를 바꿉니다.

![독립 배치한 여섯 카메라의 실제 RGB](../docs/images/surround-six-0.10.0.png)

그림은 같은 Present 구간의 실제 RGB를 공통 노출과 FOV 비율로 표시한 것입니다. 원시 크기는 슬롯 순서대로 512×1024, 512×512, 512×1024, 512×512, 512×256, 512×256입니다. 이미지 flip은 하지 않았습니다. 원본 픽셀·DSV·재질은 그대로 보존합니다.

구현은 `0x5389CD`에서 센서 요청 bit를 합치고, `0x538B11`에서 제출 함수의 RBX만 private camera 복사본으로 돌립니다. 엔진이 그 복사본의 pose/FOV로 렌더 카메라와 후보 선택 frustum을 만듭니다. 원래 미러 객체는 수정하지 않습니다. `0x538CE3`에서 사용 종료를 추적하며, 패닉·언로드는 복사본을 사용하는 호출이 끝날 때까지 종료 hook을 유지합니다. 카메라 생성자의 Z축 반회전을 보정해 일반적인 pinhole 좌표계를 제공합니다.

임의 yaw/pitch/roll을 함께 넣은 월드 카메라의 요청 회전과 실제 pass 회전 차이는 최대 9.01e-8, 위치 차이는 4.70e-6 게임 길이 단위였습니다. 최종 6뷰는 서로 다른 위치·방향과 아래로 8° 기울어진 리그입니다. 원본은 로컬 `research/live/2026-10-08-camera-rig/world-oblique-corrected.json`, `surround-six-final.json`에 있습니다. 운전석 고개 조작·장시간 주행 시험과 원거리 객체 누락 측정은 아직 수행하지 않았습니다.

CLI로 6뷰 리그를 적용한 상태에서 메타로더 `unload`/`load`도 실제 실행했습니다. private 제출 호출·여덟 hook의 임시 코드가 정리됐고, 새 모듈은 Tier 0·hook 0·SDK 9채널로 돌아왔습니다. 기존 FFB는 유지됐습니다. 기록은 로컬 `active-unload.json`입니다.

0.9.0의 `OT_Bundles`는 선택적 공유 메모리 전달입니다. `publish` 후 `otpy.BundleReader`로 읽고 `otpy.bundles.save_bundle`로 Python에서 저장할 수 있습니다. `save_partial`/`publish_partial`은 누락 뷰를 명시한 진단 표본에만 사용하며 완전한 센서 묶음으로 간주하지 않습니다. 연속 GPU ring과 기록 CLI는 후속 0.17.0에서 추가했습니다.

### 0.8.4 — 차량별 GPU 변환 상수

`render_probe on --vehicles`로 수집하면 차량의 draw 목록과 **VS slot 0 상수 버퍼 원본**도 저장합니다. 메타로더 API로 기능 DLL을 교체했으며, 일반 DX11에서 첫 네 뷰의 차량 draw 16개(AI 6·주차 차량 10)를 읽었습니다. CPU 모델·카메라 자세로 계산한 박스 꼭짓점과 GPU 변환 행렬의 투영 차이는 최대 **0.0000188 px**였습니다. 이는 이 표본의 변환 대조이며 AI 픽셀 식별이나 모든 shader의 정합 결과는 아닙니다.

- `geometry_pass.vehicles_at_compile.vehicles[].draws`: 해당 모델 geometry에 연결된 draw 항목과 VS 상수 구간입니다. 빈 배열은 관측된 연결이 없다는 뜻입니다.
- `geometry_gpu.vehicle_constant_buffers`: actor·geometry·draw index, 원본 buffer, 16-byte 단위의 시작/길이와 저장 파일을 연결합니다. 원본은 `mirror*_vehicle_*_vs_cb0.bin`입니다.
- 상수는 G-buffer를 떠날 때 그 draw가 참조한 구간만 GPU staging에 복사합니다. 기존 이미지 완료 query로 함께 회수하고 파일 저장은 명령 worker가 담당합니다.
- 새 관측 지점 `0x2E6243`은 렌더 준비의 묶음 단위로 호출됩니다. 앞 draw의 바인딩을 재사용하는 경우도 순서대로 복원합니다. 최종 `Draw*` 실행을 개별 hook한 것은 아닙니다.
- draw 관측은 `--vehicles`에서만 수행합니다. 다섯 hook 모두 패닉에서 꺼지고 메타로더 unload에서 해제됩니다. 기본 Tier 0은 SDK만 수신합니다.

첫 표본은 뷰별 draw 6/4/6/0개, 추가 상수 데이터 합계 4 KiB였으며 읽기 오류와 관측 예산 초과는 없었습니다. 짧은 관측에서 새 callback 본문은 묶음당 평균 3.09 µs, 최대 270.1 µs였습니다. 전체 hook·GPU 비용이나 주행 성능 측정값은 아닙니다. 근거와 해석은 [차량 draw 상수 경로](../docs/12_dx11_mirror_render_path.md#차량-draw와-gpu-상수-구간의-연결)에 있으며 실행 원본은 로컬 `research/live/2026-10-08-object-projection/0.8.4/`에 보존합니다.

최종 빌드로 두 번째 묶음의 13개 draw도 대조했습니다. 두 묶음 29개 draw의 최대 투영 차이는 0.0000217 px였습니다. 차량 수집을 끈 기본 동작, 다섯 hook의 코드 복원과 완전한 DLL 해제, FFB 유지·SDK 9채널 재수신을 확인했습니다.

### 0.8.3 — 미러 pass의 차량 렌더 모델 관측

`render_probe on --vehicles`로 선택한 경우에만 차량 메타데이터를 추가로 읽습니다. Python API는 `Client().request("render_probe", enabled=True, vehicle_metadata=True)`입니다. Tier 1 권한은 기존과 같으며 `panic` 또는 probe off가 추가 읽기도 끕니다. 일반 `render_probe on`과 로더 재로딩의 기본값은 꺼짐입니다.

기존 compile-begin hook에서 pass의 준비된 draw 항목 `Q`와 AI·주차 차량 본체의 LOD geometry를 교차시킵니다. 각 Q의 추가 component 묶음이 실제 `pp_model_simple` 성분을 참조하는지 확인한 뒤 `images.json`의 `geometry_pass.vehicles_at_compile`에 다음을 저장합니다.

- actor·model·model object·component 주소, LOD index, 해당 geometry 주소
- 렌더 성분의 회전·local XYZ·cell·월드 원점, 모델의 기준점 보정값
- 별도로 읽은 simulation actor pose와 그 원점 기준 AABB
- compile 관측 QPC 구간, 읽기 오류·한도에 의한 생략, pass의 상태 override 수

사용하지 않는 LOD 전체를 현재 자세로 출력하지 않습니다. 소스 geometry가 있는데 준비된 draw 그룹이 없으면 차량 메타데이터에 오류를 남깁니다. 픽셀 수집은 계속 가능하며, 차량 메타데이터를 요청하지 않은 캡처의 경로는 유지합니다. trailer·부가 부품, 개별 draw의 최종 GPU 상수 및 픽셀 객체 ID는 이 기능의 수집 범위에 포함되지 않습니다.

실제 게임에서 네 뷰 묶음 3개를 수집했습니다. 첫 Present 구간 19의 미러 0·1·2·5는 각각 차량 모델 3·3·4·0개였으며 중복을 제거하면 AI 3대·주차 차량 3대였습니다. 각 pass의 준비 그룹은 7개, 상태 override 기록은 0개였고 오류·생략은 없었습니다. 이후 구간 16·49는 별도 관측 세션의 번호이며, 같은 AI 모델 원점이 두 표본 사이 약 5.40 게임 단위 이동했습니다. 차량 메타데이터 읽기 시간은 이 12개 pass에서 0.194–0.285ms였습니다. 전체 GPU 비용이나 장기 FPS 영향의 측정값은 아닙니다.

최종 빌드에서도 옵션을 끈 네 뷰 수집이 완료됐고 차량 필드는 생성되지 않았습니다. 같은 hook을 유지하며 옵션을 켠 다음 묶음에는 모델 2·4·5·0개가 들어왔고 오류·생략은 없었습니다. 패닉 후 hook·callback 수 0, 옵션 꺼짐, 로더를 통한 완전한 core 교체·재로딩과 SDK/FFB 유지도 확인했습니다. 최종 표본은 `final-build-capture.json`입니다.

actor와 model 원점은 같지 않습니다. 첫 표본에서 모델 기준점 보정을 뺀 잔차는 AI에서 약 0.029–0.108 게임 단위, 주차 차량에서 약 0.006–0.033단위였습니다. 이 잔차 전체를 보간 오차라고 단정하지 않습니다. 후속 코드 추적으로 actor 박스가 모델 박스에 같은 기준점 이동량을 더한 값임을 확인했습니다. `project_boxes --pose model`은 `P_model + R_model × (actor_local_point − offset)`으로 투영합니다. 기본값 `--pose actor`는 기존 simulation 관측값을 사용합니다. [변환 근거와 실제 비교](../docs/12_dx11_mirror_render_path.md#actor-박스를-렌더-모델-자세로-옮기는-변환)를 참고하세요. 후속 0.8.4의 GPU 상수 대조는 위 절에 정리했습니다. 이 단계의 실행 자료는 `research/live/2026-10-08-object-projection/0.8.3/` 및 `origin/`에 있습니다.

### 오프라인 객체 박스 투영과 가림 판정

압축 연속 기록은 `--camera`로 뷰를 선택합니다. `--actor`는 특정 actor 주소만 표시할 때 사용하며, 생략하면 그 pass에서 관측한 차량 전체를 출력합니다. 주소는 해당 프로세스·관측 구간의 참조이며 장기간 또는 재실행 사이의 영구 객체 ID가 아닙니다.

```powershell
.\ot\ot.cmd project_boxes '<frame.tar.zst>' --camera mirror2 --pose model --output boxes.json --overlay boxes.png
# 한 대만 보기 (주소는 해당 캡처의 vehicles_at_compile에서 선택)
.\ot\ot.cmd project_boxes '<frame.zip>' --camera mirror1 --pose model --actor 0x299e5ff6ab0 --output one-box.json --overlay one-box.png
```

디렉터리·ZIP·TAR.ZST 묶음을 지원하며 기존 단일 카메라 디렉터리 명령도 유지합니다. JSON에는 모서리·깊이 개수와 함께 `kind`, `lod_index`, `box_world_center_xyz`, `box_size_xyz`, `box_rotation_row_major`를 저장합니다. 크기는 actor 로컬 AABB의 축별 길이, 회전은 해당 로컬 축을 월드로 보내는 3×3 행렬입니다. 좌표 단위는 캡처의 `world_units`를 따릅니다. PNG에서 녹색은 DSV 점이 박스 안에 하나 이상 있다는 뜻이며 객체별 픽셀 정답 마스크는 아닙니다. 혼잡한 화면은 `--actor`로 한 대씩 확인할 수 있습니다.

![움직이는 AI 두 대의 같은 프레임 박스](../docs/images/moving-ai-model-boxes.png)

FH5가 정차한 상태에서 **움직이는 AI 두 대**를 기존 10Hz 기록으로 대조했습니다. 전방 광각의 대형 차량은 전체 5초 동안 모델 원점이 약 71.53 게임 길이 단위 이동했습니다. 좌측 포드의 승용차도 영상에서 멀어지는 움직임이 보입니다. 위 그림은 각 차량의 서로 다른 세 시점이며, 행마다 같은 원본 영역을 잘라 확대했습니다.

| 표본 | 같은 프레임의 박스 내부 깊이 / 투영 영역 | 약 0.5초 이전 자세 | 약 0.5초 이후 자세 |
| --- | --- | --- | --- |
| 전방 AI / frame 41 | 110/141 (78.0%) | 9/120 (7.5%) | 92/152 (60.5%) |
| 전방 AI / frame 93 | 159/204 (77.9%) | 30/178 (16.9%) | 114/229 (49.8%) |
| 전방 AI / frame 141 | 233/319 (73.0%) | 59/276 (21.4%) | 166/351 (47.3%) |
| 좌측 AI / frame 42 | 100/153 (65.4%) | 0/200 (0%) | 0/120 (0%) |
| 좌측 AI / frame 84 | 61/93 (65.6%) | 0/92 (0%) | 0/82 (0%) |
| 좌측 AI / frame 120 | 52/77 (67.5%) | 44/70 (62.9%) | 0/69 (0%) |

과거·미래 자세는 현재 이미지의 카메라·깊이를 그대로 두고 같은 차량의 다른 시점 모델 자세만 대입한 비교입니다. 실제 시간차는 −0.514~−0.475초, +0.482~+0.523초였습니다. 여섯 표본 모두 현재 프레임의 겹침 비율이 높았습니다. 이는 시간 연결을 지지하며 이 비율을 검출 정확도나 instance IoU로 해석하지 않습니다. 상자 모서리 주변에는 차량 외부 배경도 포함됩니다.

동일 6시점의 차량 GPU 상수 29개에서 관측된 model-view/MVP layout을 사용해 박스 꼭짓점을 투영했습니다. 캡처한 CPU 모델·카메라 자세의 투영과 최대 **0.000205866px** 차이였고, model-view 계수 차이는 최대 3.54e−5였습니다. 개별 `Draw*` 실행을 새로 추적한 것은 아닙니다. 이번 결과는 두 차량 본체의 움직이는 표본에 대한 정합이며 트레일러·모든 shader·객체 누락·자차 주행의 검증 범위로 확대하지 않습니다. 로컬 원본과 수치는 `research/live/2026-10-08-camera-rig/moving-boxes/`, `stream-017-archive/`, `stream-zstd-live/`에 있습니다.

다음은 초기 단일 카메라 디렉터리 사용법과 정적 객체 실험 기록입니다.

```powershell
.\ot\ot.cmd project_boxes '<capture-directory>' --objects actors.json --output boxes.json
# 0.8.3에서 --vehicles를 켜서 저장한 캡처는 내장 actor 관측값 사용 가능
.\ot\ot.cmd project_boxes '<capture-directory>' --output boxes.json
```

NumPy를 사용하는 오프라인 명령이며 게임 연결 없이 저장된 0.8.2 이후 캡처를 읽습니다. `actors.json`은 외부 메모리 reader와 같은 필드의 JSON 배열입니다. 각 항목에는 `address`, `placement.world_xyz`(3개), `placement.quaternion_wxyz`(4개), `aabb_raw`(로컬 min XYZ, max XYZ 순서의 6개)가 필요합니다. 추가 필드는 무시합니다. `--objects`를 생략하면 0.8.3 차량 메타데이터의 actor 관측값과 읽기 범위를 사용합니다. 좌표는 게임 월드 단위이며, simulation actor pose를 받았다고 해서 렌더 시각으로 보간하지 않습니다.

```powershell
# 0.8.3 --vehicles 캡처의 모델 자세와 기준점 보정 사용; DLL 재교체 불필요
.\ot\ot.cmd project_boxes '<capture-directory>' --pose model --output model-boxes.json
```

`--pose model`은 캡처에 저장된 AI·주차 차량 body model 성분을 사용하므로 `--objects`와 함께 쓰지 않습니다. 결과의 `actor_origin_range_game_units`는 선택한 자세에서 actor 로컬 박스 원점의 거리입니다. actor 모드에서는 simulation 원점, model 모드에서는 `P_model − R_model × offset` 기준입니다. 초기 여섯 캡처에서는 두 모드의 박스와 DSV 비교를 실행하고 노란 주차 트럭의 RGB 겹침을 확인했습니다. 당시 AI 두 표본은 앞의 깊이에 가렸으며, 움직이는 AI의 후속 결과는 위에 따로 기록했습니다.

출력은 카메라 frustum으로 잘라낸 박스의 12개 모서리 중 보이는 선분, 투영 영역의 픽셀 수, 유효 깊이 부재·박스 내부 깊이·앞의 가림·박스 뒤 깊이 개수입니다. 깊이는 DSV에서 복원하며 각 픽셀 광선의 OBB(회전한 3D 상자) 진입/이탈 거리와 비교합니다. 박스 중심까지의 거리를 표면 깊이 정답으로 삼지 않습니다. `segments_px`는 원시 영상 행 방향을 보존한 픽셀 경계 좌표입니다. 기존 출력은 덮어쓰지 않습니다.

실제 네 미러의 Present 구간 55에 대해 객체 56개의 외부 관측 자료를 넣어 실행했습니다. 약 130 게임 길이 단위 거리의 주차 트럭은 미러 0·1의 RGB와 박스가 겹쳤고, 박스 영역 422·237픽셀 중 각각 109·79픽셀이 박스 내부 깊이였습니다. 나머지는 앞의 나무 등에 가리거나 상자 뒤 배경을 보고 있었습니다. 약 56단위 거리의 다른 주차 차량은 미러 0·1의 박스 영역 514·136픽셀 모두 차고 벽에 가렸습니다. [두 뷰의 주차 트럭 확대](../research/live/2026-10-08-object-projection/cli-parked-truck-crops.png)를 확인했습니다. 색상은 보기 위한 노출 조정이며 원시 행 순서를 유지했습니다.

객체 읽기는 캡처 전후 약 93.28ms에 걸쳤습니다. 해당 주차 트럭의 위치는 같았지만 비교한 AI들은 0.43–1.16단위 이동했습니다. 따라서 이 결과는 정적 객체의 월드 정합을 지지하며, 움직이는 AI의 같은 프레임 정답·객체별 픽셀 라벨·거리별 정확도 측정은 아닙니다. 박스는 차량 표면보다 넓으므로 내부 깊이 비율 자체도 검출 정확도가 아닙니다. 실측 자료는 `research/live/2026-10-08-object-projection/`에 보존합니다.

### 0.8.2 — 실제 DSV 깊이와 attributes Z 비교

저장된 캡처를 게임 접속 없이 점군으로 복원하는 명령을 추가했습니다. NumPy가 필요하며 다른 pipe·공유 메모리 명령에는 새 의존성이 없습니다.

```powershell
# <capture-directory>는 capture_mirrors save가 반환하는 각 saved_directory
.\ot\ot.cmd reconstruct '<capture-directory>' --output geometry.npz
.\ot\ot.cmd reconstruct '<capture-directory>' --depth-source attributes --output shading-z.npz
```

기본 `geometry`는 DSV를 캡처한 viewport와 CPU pass 투영으로 역투영합니다. `attributes`는 게임의 deferred ray에 `attributes0.w`를 곱하므로 재질의 Z 보정도 포함합니다. 어느 쪽도 다른 쪽으로 자동 대체하지 않습니다. NPZ에는 픽셀 배열을 유지한 `xyz_camera`(float32), `xyz_world`(float64), `valid`(bool), `material_bits`(uint8), JSON 문자열 `metadata_json`이 들어 있습니다. `np.load(path, allow_pickle=False)`로 읽을 수 있으며 기존 출력 파일은 덮어쓰지 않습니다. 무효 픽셀은 NaN입니다.

월드 좌표는 `geometry_pass.camera_at_compile`의 기본 회전·원점을 사용하며 단위는 `game_length_units`입니다. projection modifier가 켜진 표본은 아직 해석하지 않습니다. 개별 draw의 별도 투영이나 billboard를 실제 입체 물체의 형상으로 바꿔 주는 기능은 아닙니다. 두 깊이 경로 모두 같은 pass 카메라로 복원하고 출처·관측 구간을 NPZ에 남깁니다.

Present 구간 60의 네 미러를 두 방식으로 실제 실행해 각 방식당 1,413,986개의 유효 점을 얻었습니다. 별도로 저장해 둔 GPU 상수 기반 DSV 복원과 비교했을 때, 새 CPU pass 기반 카메라 Z의 최대 차이는 1.53e-5 게임 길이 단위였습니다. [월드 점군과 재질별 Z 차이 그림](../research/live/2026-10-08-render-probe/0.8.2-world-reconstruction/geometry-world-and-material-offset.png)을 확인했습니다. 이는 같은 캡처의 두 상수 경로 대조이며 실제 거리 정확도의 검증은 아닙니다.

G-buffer 종료 시 현재 바인딩된 depth-stencil Texture2D도 복사합니다. 실제 게임의 형식은 `D32_FLOAT_S8X24_UINT`였으며 호환되는 typeless staging 텍스처로 전체를 복사하고 기존 query 뒤에 회수했습니다. `<camera>_geometry_depth.bin`은 픽셀당 8 bytes의 원본입니다. 앞 32 bits는 float depth, 다음 8 bits는 stencil이며 나머지는 해석하지 않습니다. `geometry_gpu.depth_texture`에 형식·크기·행 길이·원본 리소스를 기록합니다. 기존 attributes Z를 이 값으로 자동 대체하지 않습니다.

Present 구간 60에서 네 미러의 원본 DSV를 확보했습니다. 카메라 0·2는 같은 원본 depth 텍스처를 재사용하므로 각 geometry 종료 시점에 따로 복사했습니다. 묶음당 원시 이미지가 33 MiB에서 44 MiB로 늘고 VS/PS 상수 2 KiB가 추가됩니다. 지원 코드는 D32 및 D24 계열을 구별하지만 이번 실제 시험은 D32+stencil 경로입니다.

같은 묶음의 GPU 투영행렬과 실제 viewport로 DSV를 카메라 Z로 역투영한 뒤, 유효한 attributes Z와 픽셀별로 비교했습니다. 0·비유한 Z와 material bit 16은 제외했습니다. 표의 값은 **두 렌더 경로의 차이**이며 실제 물체까지의 미터 오차가 아닙니다.

| 미러 | 유효 픽셀 | 절대 Z 차이 중앙값 | p95 | 최댓값 |
| --- | ---: | ---: | ---: | ---: |
| 0 | 512,770 | 0.001807 | 0.007455 | 1.3814 |
| 1 | 256,530 | 0.001794 | 0.006992 | 1.3471 |
| 2 | 513,614 | 0.001702 | 0.007279 | 1.4046 |
| 5 | 131,072 | 0.000687 | 0.002490 | 0.003906 |

DSV에서 얻은 Z를 CPU 기본 반올림으로 FP16 변환하면 일치율이 약 50%였습니다. Direct3D의 높은 정밀도→낮은 정밀도 float 변환은 0 방향 절삭을 사용합니다([공식 규격 §3.2.2](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)). 이 규칙으로 비교하면 0–30 게임 길이 단위 영역의 일치율은 뷰별 약 99.98% 이상입니다. 일반적인 FP16 양자화와 일관되는 결과입니다.

원거리 일부 픽셀의 차이는 FP16 절삭만으로 설명되지 않습니다. 최대 차이는 mirror2의 `(y=592,x=142)`에서 attributes Z `-162.5`, DSV 복원 Z 약 `-163.9046`이었습니다. 큰 차이는 packed material 값 `2`에 집중됐습니다. 후속 정적 분석에서 `eut2.leaves`의 defattr pixel shader가 `attributes0.w = eye_z + 2 * mask_texture.b * input_normal_eye.z`를 기록하고, `SV_Depth`는 출력하지 않는 경로를 찾았습니다. 같은 GLSL에서 bit `2`는 billboard 분기, bit `8`은 전환 구간을 표시합니다. 이 플래그를 게임 전체의 의미론적 클래스 ID로 해석하지 않습니다.

SM5 DXBC에서도 법선 Z와 마스크 채널의 곱을 두 번 더해 `o0.w`에 넣는 명령을 확인했습니다. 따라서 attributes Z와 DSV는 재질에 따라 의도적으로 달라질 수 있습니다. 현재의 최대 차이 픽셀을 생성한 draw까지 직접 추적한 것은 아니므로 모든 잔차를 이 식으로 설명했다고 주장하지 않습니다. 이전 RenderDoc 네 미러 G-buffer의 실제 rasterizer state는 모두 depth bias 0이었습니다. 근거는 [12번 후속 셰이더 분석](../docs/12_dx11_mirror_render_path.md#잎-billboard의-z-보정과-dsv-차이)에 정리했습니다.

[깊이·차이 영상](../research/live/2026-10-08-render-probe/0.8.2-depth-comparison/comparison.png)을 확인했고 DSV 복원 Z·절대 차이 NPY를 보존했습니다. 수집 원본은 `0.8.2-depth-gpu.json`, 수치는 `0.8.2-depth-comparison/summary.json`과 `rounding-and-range.json`입니다. 수집 후 패닉·로더 재로딩으로 새 depth staging 자원까지 정리했습니다.

### 0.8.1 — 같은 영상의 GPU geometry 상수 대조

기존 OM hook에서 해당 미러의 G-buffer를 떠나기 직전 VS/PS slot 0의 실제 바인딩 범위와 viewport를 수집합니다. CPU pass 상태는 기존처럼 컴파일 시점에 보존합니다. VS/PS raw 파일과 `geometry_gpu` metadata가 기존 이미지 묶음에 추가되며 `images` 배열과 기존 CLI는 그대로입니다. 추가 hook은 없습니다.

`VSGetConstantBuffers1`·`PSGetConstantBuffers1`에서 시작 위치와 길이를 받아, 해당 범위를 작은 staging buffer로 GPU 복사합니다. 바인딩 offset의 단위는 16 bytes이며 source buffer 전체를 처음부터 읽지 않습니다. 이미지 복사 뒤의 기존 completion query가 앞서 제출한 상수 복사도 포함하므로 별도 GPU wait 없이 회수합니다. [GetConstantBuffers1 문서](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-vsgetconstantbuffers1), [CopySubresourceRegion 문서](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion).

저장된 RenderDoc 네 미러의 마지막 geometry draw와 다음 OM binding 사이에는 UAV/SRV 해제와 clear만 있었고 VS/PS shader·상수 변경은 없었습니다(`geometry-exit-api-order.json`). 이 순서가 수집 지점을 고른 근거입니다. metadata의 정확한 관측 시점은 `before_leaving_gbuffer_binding`이며 모든 draw의 상수를 기록한다는 뜻은 아닙니다. geometry의 PS 상수는 fog 상수와 구별합니다.

일반 DX11 PID 24748의 Present 구간 61에서 네 영상과 GPU VS/PS 상수 8개를 실제 수집했습니다. 각각 256 bytes이며 원본은 2 MiB constant buffer의 서로 다른 offset에 있었습니다. 네 카메라에서 GPU VS 상수로 분리한 회전 성분과 CPU pass 회전이 정확히 같았고, GPU에서 분리한 투영과 보정한 CPU 투영의 최대 차이는 5.61e-8 미만이었습니다. 실제 GPU viewport의 깊이 범위도 CPU 값과 같았습니다. CPU ray의 세 pixel-center 표본을 실제 GPU 투영으로 되돌린 최대 오차는 0.000022 pixel 미만입니다.

각 GPU 변환의 이동 성분 `t`에서 `camera_world + inverse(R) * t`를 계산하면 네 뷰가 같은 입력 기준 원점을 가리켰습니다. 뷰 간 최대 차이는 1.53e-7 게임 길이 단위였습니다. 이 수치는 CPU/GPU 카메라 변환의 일관성이며 깊이 센서의 거리 오차나 미터 정확도가 아닙니다. 개별 draw override·다른 shader·주행 중 상태 변화는 추가 관측이 필요합니다.

원본은 `research/live/2026-10-08-render-probe/0.8.1-geometry-gpu.json`, 수치 비교는 `0.8.1-gpu-camera-comparison.json`입니다. 각 상수 복사와 이미지 복사는 같은 Present 구간 안에 있었습니다. 캡처 후 패닉·메타로더 재로딩으로 staging buffer와 hook 임시 코드를 정리하고 Tier 0으로 돌아왔습니다.

### 0.8.0 — 영상에 pass 기본 카메라 상태 연결

컴파일 시작 hook에서 미러 surface pass의 작업 자료와 component batch를 읽어 `geometry_pass.camera_at_compile`에 저장합니다. 해당 JSON은 컴파일된 명령 구간과 함께 보존되며 실제 OM binding과 GPU 복사 metadata까지 전달됩니다. 나중에 주소를 다시 읽거나 외부 관측 시각이 가까운 자료를 결합하지 않습니다. 알려진 defattr callback·component 타입의 layout만 해석하며 주소·offset은 컴파일된 `render_pass_camera` 스키마에 있습니다.

내용은 CPU 투영행렬·viewport 깊이/원시 사각형·추가 투영 보정 값, 기본 카메라 회전·local/cell/world 위치, ray float4·렌더 크기와 QPC입니다. `sample_phase`는 `dx11_compile_pass_begin`이며 **pass의 기본 상태**입니다. 개별 draw의 component override나 특수 shader를 모두 확인한 최종 GPU 상태가 아닙니다. `available=false`이면 `error`를 함께 남기고 픽셀 자료는 그대로 보존합니다.

일반 DX11 PID 24748의 Present 구간 54에서 네 영상과 네 기본 카메라를 확보했습니다. 컴파일 시각은 각 GPU 복사 제출보다 3.036–11.042 ms 앞섰습니다. ray 값은 네 뷰 모두 이전 RenderDoc 프레임 3160의 fog 상수와 정확히 같았고, CPU 투영을 DX11 기본 보정한 행렬과 이전 geometry VS에서 분리한 투영행렬의 최대 차이는 9.71e-8 미만이었습니다. 현재 ray와 투영의 세 pixel-center 표본 왕복 오차는 0.000013 pixel 미만입니다. 이는 좌표 규약·고정 투영의 대조이며 **서로 다른 프레임의 카메라 자세나 최종 GPU 상수를 검증한 결과가 아닙니다.**

Z·ray로 카메라 점군을 만들고 기본 카메라 회전의 역행렬과 world 원점으로 변환한 NPY도 저장했습니다. [네 뷰 RGB/Z](../research/live/2026-10-08-render-probe/0.8.0-pass-world/rgb-depth.png), [기본 카메라 기반 월드 점군](../research/live/2026-10-08-render-probe/0.8.0-pass-world/world-pass-base.png)에서 차고 벽·바닥 형태를 확인했습니다. 이 월드 점군은 draw override·AI 박스·미터 정확도를 아직 대조하지 않은 중간 결과입니다.

활성화 후 준비가 끝난 구간의 바인딩 7,916회는 연결 실패·오류가 0이었습니다. 캡처 뒤 패닉 및 로더 재로딩으로 hook·GPU 자원을 정리했고 Tier 0·상주 로더 0.7.0·기능 모듈 0.8.0으로 돌아왔습니다. 원본은 `research/live/2026-10-08-render-probe/0.8.0-pass-camera.json`, 비교값은 `0.8.0-camera-comparison.json`입니다.

### 0.7.2–0.7.3 — 프로세스 내부 읽기와 pass 조회 비용 감소

`copy_memory`는 SEH로 감싼 직접 `memcpy`를 사용합니다. 접근 위반·in-page 오류는 false로 반환하며 호출자는 실패한 목적지를 사용하지 않습니다. 외부 프로세스 reader와 언로드 시 스택 검사 경로는 기존 방식을 유지합니다.

pass 조회는 입력 명령 버퍼의 소유 pass 주소와 `+0x1F8`의 인덱스로 해당 그래프 배열 원소를 확인합니다. 매번 세 버퍼의 모든 pass를 순회하던 루프를 제거했습니다. 인덱스 생성·조회 근거는 `render-pass-create.txt`, `render-pass-accessor.txt`이며 현재 게임의 90개 pass에서도 배열 인덱스 대응을 확인했습니다. 물리 텍스처에 이름 하나를 붙이거나 프레임을 넘어 주소를 캐시하지 않습니다. 기존 명령 구간→pass 연결은 유지합니다.

같은 정차 장면의 각 약 8초 관측에서 hook 본문 평균은 다음과 같았습니다. QPC 경과 시간이며, 전체 hook 비용이나 GPU 성능 개선율은 아닙니다.

| 경로 | OM bind 평균 | compile begin 평균 | compile end 평균 |
| --- | ---: | ---: | ---: |
| 0.7.1 RPM·전체 순회 | 2.901 µs | 50.224 µs | 1.370 µs |
| 0.7.2 직접 복사·전체 순회 | 0.637 µs | 12.969 µs | 0.453 µs |
| 0.7.3 직접 복사·인덱스 조회 | 0.596 µs | 12.447 µs | 0.421 µs |

0.7.3에서 네 미러를 Present 구간 457에 다시 수집했습니다. Z는 모두 유한했고 0·2의 색상 픽셀은 99.9998%가 달랐습니다. hook 활성화 직후 컴파일을 아직 관측하지 못한 바인딩 49개는 미연결로 남았고, 이후 준비를 마친 수집 구간의 4,029개 바인딩은 연결 실패·오류가 0이었습니다. 같은 구간의 픽셀 수집·패닉 후 로더 재로딩으로 hook 임시 코드와 GPU 자원까지 정리했으며 SDK만 켜진 Tier 0으로 돌아왔습니다. 실제 상주 로더는 0.7.0, 기능 모듈은 0.7.3입니다.

원본은 `research/live/2026-10-08-render-probe/0.7.2-memcpy-timing.json`, `0.7.3-indexed-timing.json`, `0.7.3-indexed-capture.json`입니다. 관측된 Present 횟수는 각각 296·380·400이었으나 전경/비활성 제한과 GPU 부하를 통제한 실험이 아니므로 제한 없는 FPS 개선 수치로 사용하지 않습니다.

### 0.7.1 — hook 본문 소요 시간

`hooks`의 각 `timing`은 기능 DLL 수명 동안 누적한 QPC `samples`, `total_ticks`, `max_ticks`, `log2_tick_buckets`입니다. `qpc_frequency`로 나누면 초입니다. bucket 0은 0–1 tick, bucket k는 `[2^k, 2^(k+1))`입니다. 활성 중 조회는 근사 스냅샷이며 `panic` 후 진행 중 callback이 0이면 값이 고정됩니다. DLL을 교체하면 초기화됩니다.

측정 구간은 허용된 callback의 본문입니다. 스레드가 기다리거나 선점된 시간도 포함하며 GPU 시간·순수 CPU 사용량은 아닙니다. detour의 레지스터 저장/복원, callback 진입 카운터, histogram 갱신 비용은 포함하지 않습니다. QPC 측정 자체의 작은 비용은 있습니다.

PID 24748에서 기존 `ReadProcessMemory` 경로로 약 8초 관측했습니다. OM bind 21,124회 평균 2.901 µs, compile begin 47,286회 평균 50.224 µs, compile end 47,285회 평균 1.370 µs였습니다. compile begin 본문 누적 2,374.9 ms가 가장 컸습니다. pass 연결 오류·누락은 0이며 패닉 후 활성 hook·진행 callback은 0입니다. 원본은 `research/live/2026-10-08-render-probe/0.7.1-rpm-timing.json`입니다. 이 비교 조건은 메인 scale 1×1, 미러 scale 2×2, vsync 0, 비활성 FPS 제한 60입니다. 제한 없는 FPS 영향은 별도 측정이 필요합니다.

상주 로더는 0.7.0을 유지한 채 ABI 1로 기능 DLL 0.7.1만 교체했습니다. 로더와 FFB 모듈은 유지됐고 기능 DLL의 실제 메모리 해제·재로딩을 확인했습니다.

### 0.7.0 상주 로더 — 실제 게임 전환·교체 확인

`ot_loader.dll`은 게임의 SDK 플러그인으로 남고, 하위 `ot_runtime/ot_core.dll`을 로드·해제합니다. 게임이 직접 찾는 `plugins/ot_core.dll`과 함께 설치하면 안 됩니다. 최초 전환은 `sdk unload` 후 기존 플러그인을 백업하고 아래 구조로 배치한 뒤 `sdk reload`합니다. 이후에는 기능 DLL만 아래 API로 교체합니다. 로더 자체를 업데이트할 때에는 여전히 SDK 언로드가 필요합니다.

```text
plugins/
  ot_loader.dll             SCS SDK 진입·이벤트 전달·교체 API
  ot_runtime/
    ot_core.dll             교체 가능한 기능 모듈
    ot_config.json          기존 연구 권한과 F11 설정
```

```powershell
.\ot\ot.cmd loader status
.\ot\ot.cmd loader unload
# module_state=unloaded를 확인한 후 ot_runtime/ot_core.dll 교체
.\ot\ot.cmd loader load
# 파일 교체 없이 현재 기능 모듈을 재초기화하려면:
.\ot\ot.cmd loader reload
```

로더 pipe는 `\\.\pipe\ot_loader`, 기능 pipe는 기존 `\\.\pipe\ot`입니다. 두 pipe는 현재 Windows 사용자만 접근할 수 있고 원격 클라이언트를 거부합니다. `load/unload/reload`는 `request_id` 문자열과 함께 한 번 제출하고 `status`로 결과를 조회합니다. CLI는 이 과정을 처리합니다. 서버는 진행 중 요청과 마지막 결과만 보존하고 동일한 마지막 ID는 중복 실행하지 않습니다. 응답을 잃은 요청은 자동 재전송하지 않습니다.

외부 스레드는 교체를 직접 수행하지 않습니다. 로더가 상주 SDK 이벤트 함수를 등록하고, 기능 모듈의 `frame_end` 콜백이 반환한 뒤 SDK 스레드에서 교체합니다. 같은 이벤트 안에서 그 이벤트를 다시 등록할 수 없다는 SDK 제약 때문에 이벤트 전달 함수는 계속 남습니다. 기능 모듈은 채널·이벤트 구독을 해제하고 pipe worker와 렌더 hook을 정리한 뒤 ABI 1의 `stop()`으로 해제 가능 여부를 반환합니다. 정리가 끝나지 않으면 `retained`로 남기며 `FreeLibrary`와 새 모듈 로드를 진행하지 않습니다.

대기 요청은 5초 안에 SDK 프레임 경계가 오지 않으면 만료되며 나중에 실행되지 않습니다. 새 모듈의 초기화가 실패해도 로더 API는 남으므로 파일을 고친 뒤 `load`를 다시 요청할 수 있습니다. hot load에는 현재 SDK configuration의 복사본과 started/paused 상태를 전달합니다. 다른 SDK 플러그인(예: FFB)은 이 API의 교체 대상이 아닙니다.

공유 메모리 reader는 교체 전에 닫고, 교체 후 새 `StateReader`로 연결합니다. 기존 reader가 매핑을 잡고 있으면 새 기능 모듈의 독점 매핑 생성이 실패할 수 있습니다. 시작 권한은 기존 `initial_tier` 설정을 따르며 설치 설정은 Tier 0입니다. 로더 수명은 SDK 종료 또는 게임 정상 종료까지이고, 로더 로그는 `%LOCALAPPDATA%/ETS2AutonomyLab/ot/<PID>/ot_loader.log`입니다.

Python에서도 같은 API를 사용합니다.

```python
from otpy import LoaderClient
loader = LoaderClient()
print(loader.status())
loader.control("unload")
# 기능 DLL 파일 교체
loader.control("load")
```

PID 24748에서 최초 설치 후 콘솔 입력 없이 기능 DLL을 실제 언로드하고 파일을 덮어쓴 뒤 재로딩했습니다. 메모리의 기능 DLL 부재와 기능 pipe 소멸을 확인했고, 로더와 기존 FFB DLL은 유지됐습니다. 네 렌더 hook을 켜고 캡처를 요청한 상태에서도 언로드가 완료됐으며 원래 게임 코드 네 지점이 복원됐습니다. 파일을 잠시 다른 이름으로 옮겨 발생시킨 `LoadLibraryExW failed: 126` 뒤에도 로더 API가 응답했고, 파일 복원 후 로드에 성공했습니다. 완료된 reload 요청 ID를 재전송했을 때 세대 번호가 증가하지 않았습니다.

재로딩 후 네 미러 픽셀 한 묶음(프레임 구간 14)을 다시 저장했고, 마지막 교체에서 hook 임시 코드까지 해제했습니다. 최종 세대 6은 Tier 0·활성 hook 0·SDK 9채널·새 공유 메모리 reader 수신 상태입니다. 차량 configuration 재전달도 `truck_generation=1`로 확인했습니다. 원본은 `research/live/2026-10-08-render-probe/0.7.0-loader-active-unload.json`, `0.7.0-loader-recovery.json`, `0.7.0-loader-final-idle.json`입니다. 로더 자체의 SDK 종료·재로딩, 의도적인 hook 정리 timeout, 장시간 반복 교체는 아직 시험하지 않았습니다.

### 0.6.1 — 같은 Present 구간의 네 미러

일반 DX11 PID 24748에서 프레임 구간 32·41·50의 세 묶음을 확보했습니다. 각 묶음은 미러 0·1·2·5의 색상·attributes0·attributes3, 총 12개 원시 텍스처(33 MiB)입니다. 네 카메라의 복사 제출 시각이 모두 해당 Present 반환 사이에 있음을 확인했습니다. 같은 Present 구간의 렌더 결과이며 SDK 자차 자세·AI 목록까지 같은 시각으로 묶었다는 뜻은 아닙니다.

| 카메라 | 해상도 | 첫 표본 Z 범위 | Z=0 비율 |
| --- | --- | --- | --- |
| mirror0 | 512×1024 | -236.25–0 | 2.196% |
| mirror1 | 512×512 | -188.625–0 | 2.142% |
| mirror2 | 512×1024 | -213.5–0 | 2.039% |
| mirror5 | 512×256 | -6.03516–-0.268066 | 0% |

미러 0·2의 세 원본 Texture2D 주소는 각각 같았지만 세 묶음 모두 색상 픽셀이 100% 달랐습니다. 첫 Z 표본도 99.9128% 달랐습니다. 이름을 물리 텍스처에 붙이는 대신 컴파일된 명령 구간에 연결해, 다른 카메라가 덮어쓰기 전에 복사했습니다. [RGB/Z/재질 비교](../research/live/2026-10-08-render-probe/mirrors-first/comparison.png)에서 차고 기둥·차체·배수구 윤곽을 대조했습니다. 원시 행 방향을 유지하므로 영상은 뒤집혀 보입니다. Z=0은 무효값으로 취급하며, 재질 비트는 의미 분할 클래스가 아닙니다.

```powershell
.\ot\ot.cmd tier 1
.\ot\ot.cmd render_probe on
.\ot\ot.cmd capture_mirrors arm
.\ot\ot.cmd capture_mirrors status
# 네 카메라가 모두 ready일 때 저장
.\ot\ot.cmd capture_mirrors save
.\ot\ot.cmd panic
```

`arm`은 진행 중인 구간을 건너뛰어 다음 완전한 구간을 요청합니다. 그 구간에 렌더되지 않은 카메라가 있으면 오류를 반환하며 다른 구간의 영상을 조합하지 않습니다. 기존 `capture_mirror5`도 유지합니다. 출력 경로는 각 `views[].saved_directory`, 상수·식별 자료는 `views[].metadata`입니다. 아직 공유 메모리 센서 묶음이나 연속 네 뷰 기록기는 아닙니다.

이 실행에서 바인딩 3,900회가 모두 pass에 연결됐고 연결 오류·누락은 0이었습니다. 관측한 Present 간격 중앙값은 33.328 ms, p95는 34.154 ms였습니다. 짧은 정차 표본이며 hook 비용이나 제한 없는 FPS의 대조 실험은 아닙니다. 패닉 뒤 네 지점의 원래 코드, 활성 hook 0·callback 0·Tier 0을 확인했습니다. 이어 `sdk unload` 후 DLL·pipe 부재와 worker·매핑·trampoline·stub 해제 로그를 확인했습니다. 원본과 요약은 `research/live/2026-10-08-render-probe/0.6.1-first-bundles.json`, `0.6.1-bundle-summary.json`, 첫 픽셀은 `mirrors-first/`에 있습니다.

재로딩 후 0.6.1·Tier 0·활성 hook 0·묶음 idle·SDK 9채널 수신을 확인했습니다(`0.6.1-reloaded-idle.json`).

### 0.5.0 — Present 구간과 제한 시간 기록

0.5.0을 일반 DX11 PID 24748에서 실제 실행했습니다. 세 번째 hook은 `IDXGISwapChain::Present` 반환 지점 RVA `0x2BFEDA`를 관측합니다. `frames` 명령은 최근 최대 600개의 호출 순번·QPC 시간·HRESULT·스레드를 반환합니다. 이는 CPU의 Present 호출 간격이며 GPU 실행 시간이나 모니터 표시 시각이 아닙니다.

캡처의 `render_frame_id`는 두 Present 반환 사이의 구간 번호입니다. DLL 인스턴스를 구분하는 `observation_session_qpc`와 함께 사용합니다. 첫 경계를 보기 전에는 캡처를 시작하지 않으며, G-buffer와 색상이 다른 구간에 속하면 해당 후보를 버립니다. `copy_submission_cpu_ticks`와 `readback_cpu_ticks`는 복사 제출·CPU 회수의 실행 시간이며 `qpc_frequency`로 나눠 초로 변환합니다.

```powershell
# 정차 상태에서 5초간 최대 10 Hz로 요청. 종료·예외·Ctrl+C에서 panic 실행
.\ot\ot.cmd record_mirror5 --hz 10 --duration 5 --output .\mirror5-run.jsonl
```

JSONL은 캡처 metadata·원시 파일 저장 경로·Present 기록·실제 처리율을 담습니다. 기존 단일 캡처를 순차 요청하므로 목표 속도를 보장하지 않으며, 지연된 요청은 건너뜁니다. 512×256 세 텍스처는 표본당 3 MiB로 10 Hz에 약 30 MiB/s입니다. 매번 staging 자원을 만드는 초기 구현이며 GPU ring·다중 뷰·MCAP 스트림은 아직 없습니다. CLI가 정상적으로 정리할 수 없는 강제 프로세스 종료에는 `panic` 명령 또는 F11이 필요합니다.

정차 중 5.0002초에 **50표본, 9.9996 Hz**, 요청 슬롯 누락 0회, raw 150 MiB를 기록했습니다. 프레임 구간은 153부터 300까지 3씩 증가했고, 50개 모두 복사 제출 시각이 해당 Present 반환 사이에 있었습니다. Present 기록 손실은 0, HRESULT는 모두 S_OK였습니다. 같은 세 hook으로 픽셀 수집 없이 관측한 5초는 29.996회/s, 수집 구간은 30.001회/s였고 p95 간격은 각각 34.031/34.080 ms였습니다. 이 장면의 약 30 Hz 제출 속도에서는 차이가 관측되지 않았지만, hook 자체 비용·제한 없는 처리율·장시간 FPS 영향까지 검증한 결과는 아닙니다.

복사 자원 준비·제출 CPU 시간의 중앙값은 0.341 ms, readback CPU 시간은 0.414 ms였습니다. GPU 시간은 아닙니다. 마지막 50번 표본의 RGB/Z/재질 그림을 확인했고 Z는 전부 유한·비영, 범위는 `-6.03125~-0.26806640625`였습니다. 세 hook 지점의 원래 코드 복원과 Tier 0·활성 hook 0도 확인했습니다. 이어 사용자 `sdk unload` 후 DLL·pipe 부재와 `Render probe drained; trampoline and stub released` 로그를 확인했습니다. 기록은 `research/live/2026-10-08-render-probe/0.5.0-record-5s.jsonl`, `0.5.0-first-record.json`, `0.5.0-record-summary.json`, 대표 픽셀은 [`mirror5-record-sample50`](../research/live/2026-10-08-render-probe/mirror5-record-sample50/)입니다. 50개 전체 raw 경로는 JSONL에 있습니다. 최종 재로딩에서도 0.5.0·Tier 0·활성 hook 0·캡처 idle·SDK 9채널 수신을 확인했습니다(`0.5.0-reloaded-idle.json`).

## 구현 범위

0.6.0에서 pass 명령 연결을 실제 확인했습니다. `0x227140`은 pass의 명령 버퍼(`+0x2E8`)를 목록에 넣고, renderer slot `+0x108`이 이를 DX11 명령으로 컴파일합니다. `0x2B1B40`/`0x2B266A`에서 입력 버퍼 하나가 만든 출력 token 범위를 기록하고, OMSetRenderTargets 관측 지점에서 실제 token 주소·compiled pool ID로 찾습니다. pass 이름은 그래프의 pass 배열에서 읽고 연결 이미지의 namespace를 함께 보존합니다. 3초 동안 바인딩 6,157회가 모두 연결됐으며 오류는 0이었습니다. 0.6.1은 이전 이미지 생성 hook을 제거해 총 4개(명령 컴파일 시작/끝, 바인딩, Present)만 사용합니다. 0.6.0의 다섯 hook은 패닉 원상 복원과 완전한 SDK 언로드를 확인했습니다. 정적 근거는 `graph-pass-command-submit.txt`, `rt-binding-token-read.txt`, 실제 기록은 `0.6.0-pass-commands.json`입니다.

`dist/ot_loader.dll`이 공식 SCS telemetry SDK 플러그인으로 시작해 기능 모듈 `ot_core.dll`을 관리합니다. SDK 종료 때 두 DLL의 콜백·명령 스레드·pipe·공유 메모리를 정리합니다. 별도 injector나 DXGI 프록시를 사용하지 않습니다.

| 기능 | 현재 구현 |
| --- | --- |
| SDK | world placement, 로컬 선형·각속도, 로컬 선형 가속도, 속력, RPM, 조향·가속·브레이크 입력 9채널 |
| 버전 게이트 | 조사한 EXE의 SHA-256과 시작 시 비교. 불일치 시 내부 주소 접근 차단, SDK만 유지 |
| 내부 읽기 | 설정과 해시가 허용할 때 `sdk_frame_end`에서 미러 배열·pose·projection, PhysX 기반 차량 자세 읽기 |
| 명령 | `ping`, `version`, `hooks`, `frames`, `render_probe`, `capture_mirror5`, `capture_mirrors`, `schema`, `read`, `snapshot`, `state`, `tier`, `panic`, `dump`, `reload_permissions`; CLI `record_mirror5` |
| 렌더 관측 | Tier 1의 `allow_render_probe`와 `render_probe on`으로 네 hook 활성화. pass 명령 구간·D3D 바인딩·Present 반환 관측·요청한 네 미러 GPU 복사 |
| 데이터 | `Local\OT_State`에 JSON 스냅샷. 네트워크 소켓 없음 |
| 패닉 | 게임 창이 전경일 때 F11 또는 pipe `panic`: Tier 0으로 복귀 |
| 진단 | `%LOCALAPPDATA%\ETS2AutonomyLab\ot\<PID>\ot_core.log`, 요청 시 수동 minidump |
| Python | `Client`와 공유 메모리 `StateReader`, CLI |

현재 Tier 0은 **SDK 수신을 유지하고 내부 필드 읽기와 render probe를 끈 상태**입니다. Tier 1에서 내부 읽기와 별도 opt-in 렌더 hook을 지원합니다. ImGui, 카메라 필드 쓰기, Tier 3 게임 동작 패치, `OT_Bundles`, MCAP, Foxglove는 아직 구현하지 않았습니다. `dump`는 수동 진단이며 자동 크래시 덤프 기능은 아닙니다.

### 0.4.1 mirror5 GPU 캡처 — 실제 픽셀 4회 확보

일반 DX11 PID 24748에서 첫 캡처의 렌더 바인딩 28번(G-buffer) → 31번(색상 타깃 이탈)을 연결했습니다. 동일 immediate context·렌더 스레드 18012에서 512×256 RGBA16F 두 장과 RGBA16UINT 한 장, 총 3 MiB를 얻었습니다. SDK 힌트는 둘 다 3755였으나 GPU frame ID는 아직 없습니다. 첫 Z는 전부 유한·비영이며 범위는 `-6.03515625~-0.26806640625`였습니다. RGB와 Z에서 트럭 앞부분·바닥·배수구의 윤곽이 대응했습니다. 이는 정성적 이미지 확인이며 거리 단위·정량 정합 검증은 아닙니다.

이후 세 요청도 성공해 캡처 순번 2·3·4, SDK 힌트 9672·9706·9742를 얻었습니다. 두 hook 지점의 32-byte 원상 복원, callback 0, Tier 0을 확인했습니다. 사용자 `sdk unload` 후 DLL 모듈·pipe 부재와 임시 코드 해제 로그, 재로딩 후 0.4.1·Tier 0·SDK 9채널 등록·캡처 idle도 확인했습니다. 첫 raw·NPY·PNG는 [`mirror5-first`](../research/live/2026-10-08-render-probe/mirror5-first/), 후속 raw는 같은 폴더의 `mirror5-repeat-2..4`에 있습니다. 기록은 `0.4.1-first-capture.json`, `0.4.1-repeat-captures.json`입니다. 표시 이미지는 원시 행 순서를 유지합니다.

0.4.0의 실제 첫 시도에서는 25초 동안 호출이 들어왔지만 이름 연결이 0건이라 GPU 복사를 시작하지 않았습니다. 원본은 `research/live/2026-10-08-render-probe/0.4.0-first-capture.json`입니다. 0.4.1은 렌더 그래프 이미지 생성 코드 RVA `0x21F09D`도 관측해 이름과 image ID의 연결을 보존합니다. 해당 29-byte signature는 설치 EXE에서 한 곳이며, 캡처 요청이 있을 때만 이름을 수집합니다. pool ID가 다른 이름에 재할당되면 이전 연결을 지웁니다.

`capture_mirror5 arm`은 한 장만 요청합니다. callback에서 실제 RTV의 Texture2D와 `mirror5/attributes_0`, `mirror5/attributes_3`, `mirror5/composition_raw`의 image ID → resource를 대조합니다. 같은 context에서 해당 G-buffer를 시작하고 색상을 렌더한 뒤 그 색상 타깃을 떠나는 바인딩에서 세 텍스처를 staging으로 복사합니다. RenderDoc의 해당 전환 다음에는 전방 미러 최종 출력이 있었습니다. 이름 수집 hook까지 포함해 활성 hook 수는 2개이며 패닉·SDK 종료에서 둘 다 해제합니다. 다른 미러와 연속 수집은 아직 없습니다.

GPU API 호출은 게임 렌더 스레드에서만 수행합니다. [CopyResource](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource) 뒤 EVENT query를 `DONOTFLUSH`로 조회하고, [Map](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map)은 `DO_NOT_WAIT`로 호출합니다. 완료되지 않았으면 다음 callback으로 넘기며 강제 Flush·대기 루프는 없습니다. CPU로 옮길 때 RowPitch 패딩을 제거하고 원시 행 방향·RGBA16F/UINT 값을 유지합니다. GPU 리소스는 읽기 완료 또는 취소 때 해제합니다.

```powershell
.\ot\ot.cmd tier 1
.\ot\ot.cmd render_probe on
.\ot\ot.cmd capture_mirror5 arm
.\ot\ot.cmd capture_mirror5 status
# phase가 ready이면 기존 명령 worker에서 파일 저장
.\ot\ot.cmd capture_mirror5 save
.\ot\ot.cmd panic
```

출력은 `%LOCALAPPDATA%/ETS2AutonomyLab/ot/<PID>/mirror5-<tick>/`의 `images.json`과 raw 파일 3개입니다. metadata는 마지막에 기록합니다. `capture_sequence`는 DLL의 캡처 순번이며 GPU frame ID가 아닙니다. `sdk_frame_hint` 역시 SDK 힌트입니다. 패닉은 미완료 캡처를 취소하지만 이미 확보한 CPU 픽셀은 저장할 수 있게 유지합니다. 요청 후 30초가 지난 callback에서 timeout을 보고합니다. NPY·PNG 변환에는 기존 `research/live/2026-10-08-renderdoc/convert_pixels.py`에 출력 폴더를 인자로 전달합니다.

### 0.3.0 렌더 호출 관측 — 실제 게임 시험 완료

일반 DX11 게임 PID 24748에서 10초 동안 20,824회 호출을 관측했습니다. 단일 렌더 스레드 18012·단일 D3D context였고 `missed_records=0`이었습니다. 패닉 뒤 활성 hook 0, 호출 수 정지, 원래 게임 명령 바이트 복원, SDK frame 증가를 확인했습니다. 이어 사용자 `sdk unload` 후 DLL 모듈 부재·pipe 부재와 `trampoline and stub released` 로그를 확인했습니다. `sdk reload` 후 버전 0.3.0, Tier 0, SDK 9채널 수신이 복구됐습니다. 원본은 `research/live/2026-10-08-render-probe/first-hook-10s.json`, `reload-and-graph.json`입니다. 짧은 정차 실험이며 장시간 주행이나 GPU 복사 검증은 아닙니다.

RenderDoc 캡처에서 확인한 명령 소비 함수의 `OMSetRenderTargets` 인수 준비 지점에 SafetyHook mid hook을 둡니다. EXE 시작 해시가 일치해야 하고, 실행 영역의 명령 서명이 유일하게 RVA `0x2B3193`에 연결돼야 활성화합니다. 실제 설치 EXE의 오프라인 검색에서도 한 곳이었습니다. `renderdoc.dll`이 로드된 게임에서는 활성화를 거부합니다.

배포 설정은 `allow_render_probe: false`입니다. 연구 실행에서는 설치 설정에 `allow_tier1`, `singleplayer_research`, `allow_render_probe`를 모두 켠 다음 다음 순서로 관측합니다.

```powershell
.\ot\ot.cmd reload_permissions
.\ot\ot.cmd tier 1
.\ot\ot.cmd render_probe on
.\ot\ot.cmd hooks
.\ot\ot.cmd panic
```

`hooks`는 활성 수, 호출 수, 아직 끝나지 않은 callback 수, 마지막 128개 binding 기록을 반환합니다. 기록에는 D3D context, 새 RTV 목록, DSV, thread ID가 들어갑니다. `sdk_frame_hint`는 가장 최근 SDK frame 번호일 뿐 GPU frame ID가 아닙니다. 0.3.0 자체에는 GPU 복사가 없으며 후속 0.4.1에서 mirror5 식별·단일 복사를 추가했습니다. `version.writes`는 활성 코드 hook이 있으면 true이며 `field_writes`는 false입니다.

패닉·Tier 0·권한 재로딩은 새 callback 수집을 막고 원래 게임 명령을 복원합니다. 이때 이미 진입한 callback을 위해 stub와 trampoline 할당은 DLL 안에 유지합니다. SDK 종료 때에는 SDK callback과 명령 worker를 종료하고, hook 진입을 막은 후 다른 스레드의 현재 위치와 return address에서 DLL·hook 코드가 빠졌는지 Windows unwind API로 확인한 뒤 해제합니다. 해제 위치의 instruction에는 외부 CALL을 허용하지 않아 unwind 정보가 없는 trampoline으로 돌아오는 외부 호출을 만들지 않습니다.

2초 안에 안전한 해제를 확인하지 못하면 코드를 강제로 해제하지 않고 모듈 참조와 observer를 보존하며 로그에 정상 게임 종료 필요를 남깁니다. 이는 해제 성공이 아니며 DLL 교체 전에 게임을 정상 종료해야 합니다. 정상 해제는 실제 확인했고, timeout 경로를 의도적으로 유발하는 시험은 하지 않았습니다.

## 빌드

프로젝트 루트에서 PowerShell로 실행합니다.

```powershell
# 로컬 빌드 도구가 없을 때 한 번
py -3.13 -m pip install --disable-pip-version-check --target ot/.tools cmake==4.1.3 ninja==1.13.0
.\ot\build.cmd
```

현재 `build.cmd`는 이 PC의 VS 18 Build Tools 경로를 사용합니다. 다른 PC는 `OT_VS`를 수정하거나 x64 Native Tools 환경에서 CMake를 직접 실행합니다. CMake의 `SCS_SDK`, `ETS2_EXE` 캐시 인자로 경로를 바꿀 수 있습니다. SafetyHook 요구사항에 따라 0.3부터 C++23을 사용하며 정적 MSVC 런타임, x64, CFG/ASLR/NX를 유지합니다. 한국어 MSVC의 의존 파일 경로가 깨지지 않도록 UTF-8 콘솔에서 빌드합니다.

결과물:

- `dist/ot_loader.dll`: **게임용** 상주 SDK 플러그인. `plugins/`에 배치합니다.
- `dist/ot_core.dll`: 교체 가능한 기능 모듈. `plugins/ot_runtime/`에 배치합니다.
- `dist/ot_ipc.dll`: **Python 프로세스용** 공유 메모리 슬롯 복사 함수. 게임 plugins 폴더에 넣지 않습니다.
- `dist/ot_config.json`: DLL 시작 설정.
- `dist/ot_schema.json`: 컴파일된 스키마의 참고 사본. 수정해도 DLL의 주소/권한이 바뀌지 않습니다.
- `dist/ot_loader.pdb`, `dist/ot_core.pdb`, `dist/ot_ipc.pdb`: 디버그 심볼.

`schema/1.61.1.1/game.json`은 조사된 EXE 해시를 고정합니다. 새 EXE를 발견했다고 자동으로 갱신하지 않습니다. 구조를 다시 확인한 뒤 스키마와 함께 재빌드해야 합니다.

## 설치와 첫 연결

현재 설치 버전은 **0.7.0 로더+기능 모듈**, 기본 시작 상태는 Tier 0입니다. 직전 0.6.1과 설정은 `ot/backup/before-loader-0.7.0-20261008/`에 보존했습니다. 현재 설정 파일은 `plugins/ot_runtime/ot_config.json`입니다. 다음은 최초 설치부터의 이력입니다.

**0.1 DLL의 실제 SDK 연결을 확인한 뒤, 게임 종료 상태에서 0.2로 교체했습니다.** 실행 중인 게임에 원격 주입하거나 강제 종료하지 않았습니다. 이전 DLL·플러그인 설정·사용자 `config.cfg`는 `ot/backup/before-0.2/`에 보존했습니다. 개발 콘솔을 켰고, 설치된 플러그인 설정은 싱글플레이 내부 읽기를 허용하되 시작 Tier는 0입니다. `dist/ot_config.json`의 배포 기본값은 계속 내부 읽기 비활성입니다.

이후 게임 PID 34316을 유지한 채 `sdk unload` → 0.2.1 DLL 교체 → `sdk reload`까지 성공했습니다. 0.2 파일 백업은 `ot/backup/before-0.2.1/`입니다. 현재 패닉 키는 사용자 요청에 따라 F11이며 실제 키 누름 시험은 하지 않았습니다.

게임을 정상 종료한 다음 `dist/ot_core.dll`, `dist/ot_config.json` 두 파일을 아래 위치에 넣고 DX11로 시작합니다.

```text
D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins\
```

SDK 단계에서는 기존 플러그인과 vrperfkit을 유지했습니다. 이후 RenderDoc 캡처 준비 단계에서 사용자 정상 종료를 확인하고 `dxgi.dll`, `vrperfkit.yml`, `vrperfkit.log`, `plugins/ot_core.dll`을 `ot/backup/before-renderdoc-20261008-132657/`으로 옮겼습니다. 기존 FFB 플러그인과 `.scs` 파일은 변경하지 않았습니다. 14:29에는 게임 종료를 확인한 뒤 새 0.3.0 DLL·스키마를 plugins 폴더에 배치했습니다. 기존 설정 백업은 `ot/backup/before-0.3.0-20261008-142954/`입니다. 설치 설정의 `allow_render_probe`는 true지만 `initial_tier=0`이며 별도 on 명령 전에는 hook을 설치하지 않습니다. 0.2.1로 돌아갈 때는 게임 종료 후 앞의 DLL 백업을 사용합니다. vrperfkit 세 파일은 렌더 hook 개발 중 계속 분리해 둡니다.

RenderDoc 1.46 공식 portable을 `research/tools/renderdoc-1.46/`에 준비했습니다. 다운로드 직후 실행 파일의 유효한 Baldur Scott Karlsson Authenticode 서명을 확인했습니다. `research/open_renderdoc.cmd`를 직접 실행하면 `research/ets2-mirrors.cap`의 DX11 설정이 열립니다. 자동 게임 시작은 꺼져 있으며 호출 스택과 deferred command list 수집은 켜 두었습니다. 설정 형식은 [1.46 소스](https://github.com/baldurk/renderdoc/blob/v1.46/qrenderdoc/Code/Interface/QRDInterface.cpp)에 맞췄고 실제 UI 로딩도 확인했습니다. 자동 승인 심사가 초기 내장 Python 시험 실행 명령을 `blocked by policy`로 거부해 게임 실행과 캡처 트리거는 사용자가 직접 수행했습니다.

첫 사용자 실행에서는 설정이 UI에 정상 로딩되고 RenderDoc이 게임 PID 5732에 주입됐습니다. 그러나 이 프로세스의 연결이 끊어진 뒤 Steam이 별도 PID 32268을 실행했고, 그 게임에는 `renderdoc.dll`이 없었습니다. `research/live/2026-10-08-renderdoc/first-launch.log`와 `first-launch.cap`에 이 시도의 로그·실제 적용 설정을 보존했습니다. [공식 Steamworks 디버깅 방식](https://partner.steamgames.com/doc/sdk/api#SteamAPI_RestartAppIfNecessary)에 따라 게임 `bin/win_x64/steam_appid.txt`를 새로 만들고 설치 manifest에서 확인한 `227300`을 넣어 재실행했습니다. 그 결과 PID 28532에 RenderDoc이 유지되어 frame 3160 캡처에 성공했습니다. 임시 App ID 파일은 캡처 후 삭제했고 부재도 확인했습니다.

`research/live/2026-10-08-renderdoc/frame3160.rdc` (1.72 GB), 화면 썸네일, XML 렌더 명령을 보존했습니다. 패스 마커가 없고 두 세로형 G-buffer 구간이 같은 리소스를 재사용함을 확인했습니다. 실제 분석과 hook 후보는 [13번 M1 기록](../docs/13_game_operating_table.md)에 있습니다. `export_frame3160.py`를 RenderDoc 내장 Python으로 실행해 4뷰의 원시 텍스처 12개를 확보했고, `convert_pixels.py`를 Python 3.13으로 실행해 NPY와 비교 그림을 확인했습니다. 원시 Z는 음수 또는 0이며 두 세로형 시점의 색상은 서로 다릅니다. `pixels/export.log`, `images.json`, `pixel-statistics.json`에 실제 결과가 있습니다. `export_frame3160_constants.py`도 실제 실행해 16개 draw의 VS/PS·상수·viewport를 추출했습니다. `reconstruct_pixels.py`로 4개 카메라 공간 점군을 NPY·PLY로 저장했으며 fog ray와 geometry projection의 pixel-center 차이는 최대 0.000033 pixel 미만입니다. 후속 DLL에서 mirror5 단일·제한 시간 수집을 확인했으며, 4뷰 실시간 수집·월드 pose·미터 단위 검증은 아직입니다.

연구용 싱글플레이 프로필에서 사용합니다. Convoy/TruckersMP에는 플러그인을 빼고 실행합니다. `singleplayer_research`는 사용자가 설정하는 확인 값이며 멀티플레이 자동 감지 기능이 아닙니다.

```powershell
.\ot\ot.cmd ping
.\ot\ot.cmd version
.\ot\ot.cmd read truck.world.placement
.\ot\ot.cmd snapshot
.\ot\ot.cmd state
.\ot\ot.cmd watch --hz 10
# watch 종료: Ctrl+C
# 새 파일에 60초 수집 후 종료
.\ot\ot.cmd watch --hz 10 --duration 60 --output sdk-sample.jsonl
```

`version.channels`의 `0`은 채널 등록 성공입니다. 음수는 SDK 오류 코드이며 지원되지 않은 채널을 정상적인 0 값으로 대체하지 않습니다. pipe가 없다는 오류가 나면 아직 DLL이 로드되지 않았거나 초기화에 실패한 것입니다. 게임 `game.log.txt`와 위의 `ot_core.log`를 확인합니다.

내부 읽기를 허용하려면 설치한 `ot_config.json`의 `allow_tier1`과 `singleplayer_research`를 모두 `true`로 바꿉니다. 0.2부터는 `reload_permissions`로 이 두 설정을 다시 읽을 수 있습니다. 항상 Tier 0으로 내려간 뒤 권한을 다시 읽으며, `tier 1`을 별도로 요청해야 내부 읽기가 시작됩니다. 키·공유 메모리·초기 Tier 설정은 SDK 재초기화 때 적용합니다.

```powershell
.\ot\ot.cmd reload_permissions
.\ot\ot.cmd tier 1
.\ot\ot.cmd read vehicle.pose_physics
.\ot\ot.cmd read vehicle.origin_shift
.\ot\ot.cmd read 'mirror_camera[5].pose'
.\ot\ot.cmd read 'mirror_camera[5].projection'
.\ot\ot.cmd panic
```

EXE 해시가 다르거나 설정이 허용하지 않으면 `tier 1`은 실패합니다. 임의 주소 읽기와 Tier 2/3은 지원하지 않습니다. 패닉은 이후 내부 읽기를 끄며, 이미 클라이언트에 전달된 과거 표본을 삭제하지 않습니다. 완전한 DLL 제거는 게임 정상 종료 후 플러그인 파일을 빼는 방식입니다.

Python에서는 의존 패키지 설치 없이 사용할 수 있습니다.

```powershell
$env:PYTHONPATH = (Resolve-Path .\ot\py).Path
py -3.13
```

```python
from otpy import Client, StateReader
ot = Client()
print(ot.schema())                 # 실행 중인 DLL과 같은 스키마
print(ot.read("truck.speed"))
with StateReader() as state:       # ot_ipc.dll은 이 Python 프로세스에만 로드
    sample = state.read_latest()  # 새 표본이 없으면 None
```

수동 dump는 `.\ot\ot.cmd --timeout 30 dump`입니다. 게임 프로세스 상태를 파일로 쓰는 진단 명령이므로 주행 수집의 기본 동작에는 포함하지 않습니다.

## 시간과 좌표

`frame_id`는 플러그인이 센 **SDK frame** 번호입니다. `render_frame_id = null`, `render_coherent = false`로 발행합니다. 각 SDK 항목의 `available`, `observed_frame`을 함께 사용해야 합니다. 메뉴·로딩 중에는 값이 없을 수 있습니다.

미러 pose는 `sdk_frame_end`에서 읽은 카메라 객체의 값입니다. 이후 렌더 준비에서 갱신될 수 있으므로 같은 GPU 프레임의 6뷰 자세가 아닙니다. **이번 실행에서 미러 3·4·5에는 초기 로컬 좌표가 남아 있었습니다.** 미러의 `available`은 읽기 성공만 뜻합니다. 따라서 0.2.1은 미러 pose에 `coordinate_space: "unknown"`을 명시하며, 실제 렌더 pass와 연결하기 전에는 월드 자세로 사용하지 않습니다.

SDK world placement는 `position_m`과 `euler_rotations`(회전 수 단위)입니다. `vehicle.pose_physics`는 PhysX의 `mass_world * inverse(mass_local)`에 물리 세계 cell 원점과 SCS 차량 원점 이동을 반영한 월드 자세이며, `quaternion_wxyz`와 `coordinate_space: "world"`로 제공합니다. projection은 카메라 객체의 16개 원소이며 픽셀과 정합한 최종 GPU 상수로 검증하지 않았습니다.

## IPC 계약

pipe `\\.\pipe\ot`는 현재 Windows 사용자에게만 허용되고 원격 연결을 거부합니다. 요청은 UTF-8 JSON 한 줄, 응답도 한 줄이며 연결당 한 요청입니다. 응답을 읽은 뒤 클라이언트가 연결을 닫습니다. 예:

```json
{"cmd":"read","field":"truck.speed"}
```

응답은 `{"ok":true,"result":...}` 또는 `{"ok":false,"error":"..."}`입니다. `schema`는 컴파일된 필드 정의를 반환합니다. `watch`는 Python CLI 기능이며 서버 명령이 아닙니다.

공유 메모리 ABI 1은 little endian이며 [`native/ipc_layout.hpp`](native/ipc_layout.hpp)가 C++ 정의입니다.

| 영역 | 바이트 | 내용 |
| --- | --- | --- |
| 전역 헤더 | 64 | magic `OTSTATE1`; uint32 ABI, 슬롯 수, payload 용량, PID; int64 발행 번호와 drop 수 |
| 슬롯 헤더 | 각 64 | int32 state; uint32 길이; uint64 sequence; 예약 공간 |
| 슬롯 payload | 각 65,536 | UTF-8 JSON, NUL 종결 없이 길이로 읽음 |
| 전체 | 524,864 | 전역 헤더와 8개 슬롯 |

상태는 free=0, writing=1, ready=2, reading=3입니다. 생산자는 CAS로 free/ready를 획득하고 reading 슬롯을 덮어쓰지 않습니다. `ot_ipc.dll`은 ready→reading 획득 후에만 메타데이터와 payload를 읽고 즉시 free로 돌려줍니다. Python의 일반 메모리 대입을 원자적 교환으로 간주하지 않습니다.

`StateReader`는 단일 소비자용 최신 값 읽기입니다. 오래된 ready 슬롯은 덮어쓸 수 있으므로 손실 없는 녹화가 아니며, `dropped`는 큰 payload 또는 사용 가능한 슬롯이 없는 경우만 셉니다. 여러 독립 소비자가 모두 같은 프레임을 받는 broadcast 계약도 아닙니다. 클라이언트를 강제 종료하는 순간 reading 슬롯을 잡고 있었다면 그 슬롯은 생산자가 다시 시작될 때까지 남을 수 있습니다. 정상 종료는 context manager가 매핑과 DLL을 닫습니다.

## DLL 교체와 개발 콘솔

공식 SDK의 `research/sdk/readme.txt`는 `sdk reinit`, `sdk unload`, `sdk reload`를 제공합니다. 이는 **모든 SDK 플러그인**에 적용되므로 기존 FFB 플러그인도 함께 재초기화됩니다. 먼저 수집 클라이언트를 종료해 `OT_State` 매핑을 닫고, 주행을 멈춘 상태에서 사용합니다.

- `sdk reinit`: 설정을 다시 읽지만 DLL 파일은 교체하지 않습니다.
- `sdk unload`: SDK 종료 후 DLL을 해제합니다. 이 상태에서 새 파일을 복사합니다.
- `sdk reload`: 플러그인들을 다시 로드·초기화합니다.

개발 콘솔이 꺼져 있으면 게임을 정상 종료한 상태에서 사용자 `config.cfg`의 `g_developer`, `g_console`을 `1`로 설정하고 다시 실행해야 합니다. 변경 전 원본을 백업합니다. 게임 전체를 재시작하는 경우에는 **종료 → 파일 교체 완료 → 재실행** 순서입니다.

## 실제로 확인한 범위

2026-10-08, MSVC x64 RelWithDebInfo 빌드 성공. PE x64·SDK 함수 두 개 export·시스템 DLL 의존성 확인. 별도 Python 프로세스에서 `LoadLibrary`, 지원하지 않는 API 거절(`-1`), null SDK 인자 거절(`-2`), 미초기화 shutdown, `FreeLibrary` 성공을 확인했습니다. CLI 도움말과 플러그인이 없는 경우의 오류도 확인했습니다.

이후 실제 게임 PID 12332에서 SDK 초기화, 9채널 등록, pipe 왕복, 공유 메모리를 확인했습니다. `research/live/2026-10-08-ot-core/sdk-road-60s.jsonl`에는 597표본, SDK render clock 기준 59.932734초가 기록됐습니다. 모든 표본의 9채널이 available이었고 frame 번호 역행은 0, 생산자 dropped는 0이었습니다. 속력 범위는 약 `-0.0000654~0.0000510 m/s`로 **정차 실험**입니다. 60 Hz 발행 중 최신 값만 약 10 Hz로 읽었으므로 모든 frame을 보존한 기록은 아닙니다.

PID 12332 정상 종료 시 SDK 종료 로그에서 worker join과 pipe/mapping 해제를 확인했고 프로세스 소멸도 확인했습니다.

0.2 / PID 34316에서는 내부 읽기를 30초 켜서 298표본을 수집했습니다. PhysX 기반 차량 자세와 같은 SDK frame의 `truck.world.placement`를 비교한 위치 차이는 중앙값 `3.79e-6 m`, 최대 `8.40e-6 m`였고, 회전 차이는 최대 `1.92e-5°`였습니다. SDK heading/pitch/roll을 Y-X-Z 순서의 회전으로 변환해 비교했습니다. 현재 차량의 정차 대조이며 다른 차량·주행 전 구간을 검증한 수치가 아닙니다. 원본은 `tier1-initial-30s.jsonl`, 요약은 `physics-sdk-comparison.json`에 있습니다.

같은 실행에서 미러 0–5 객체와 pose·projection을 읽었고 6–8 슬롯은 비어 있었습니다. `panic` 명령 뒤 Tier 0과 snapshot의 engine 필드 제거를 확인했습니다. 실제 키 테스트는 사용자 요청으로 생략했으며 설정 키는 F11입니다. M0의 30분 주행, 오버레이, 자동 크래시 포착은 아직 남아 있습니다.

0.2.1 재로딩 뒤 새 `OT_State` 매핑과 9채널 수신, `reload_permissions`, 차량과 미러 필드 읽기, 명령 패닉을 다시 확인했습니다. 원본은 `0.2.1-reload.json`입니다. 이 후속 표본에서는 미러 5도 월드 위치 부근으로 갱신돼 있었으며, 최초의 로컬 값이 영구 장착 자세가 아니라 갱신 상태에 따라 달라진다는 점을 주의해야 합니다. 현재 API는 이 갱신을 특정 렌더 pass와 연결하지 않습니다.
