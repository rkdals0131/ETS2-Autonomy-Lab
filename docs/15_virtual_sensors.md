# 깊이 영상에서 만드는 Phase 1 가상 라이다

현재 FH5의 전방·좌우 포드 카메라와 **같은 광학 원점**에서 빔을 샘플링합니다. 게임을 실행하지 않고 디렉터리·ZIP·TAR.ZST 묶음에서 세 라이다의 점군을 만들 수 있습니다. 실제 라이다의 반사·재질 반응을 재현한 센서는 아니며, 노이즈 없는 순간 기하 관측입니다.

```powershell
.\ot\ot.cmd lidar '<frame.tar.zst>' --config .\ot\presets\phase1-lidar.json --output lidar.npz
```

출력 파일은 덮어쓰지 않습니다. Python에서는 `load_lidar_profile()`로 설정을 한 번 읽고, 메모리의 기존 `OT_Bundles`와 `sample_lidars(bundle, profile)`을 사용합니다. 게임에 새 hook이나 DLL 변경은 필요 없습니다.

## RGB-D·차량 메타데이터와 함께 기록

```powershell
.\ot\ot.cmd record_bundles --config .\ot\presets\phase1-highway.json --lidar-config .\ot\presets\phase1-lidar.json --vehicles --hz 10 --duration 5 --output .\research\live\my-lidar-run
```

각 프레임의 TAR.ZST 또는 ZIP 안에 같은 프레임의 `lidar.npz`가 추가됩니다. 라이다 생성과 저장은 기존의 제한된 frame worker에서 처리하며, 기본 worker 수는 라이다 사용 시 4개·미사용 시 기존 2개입니다. `--workers`로 직접 바꿀 수 있습니다. `--vehicles`가 같은 pass의 차량 모델 자세·actor bounds·draw 상수 기록을 켭니다. 기록 중 카메라 배치는 고정되며 종료·오류·Ctrl+C에서 writer를 합류시키고 Tier 0으로 돌아갑니다.

```python
from otpy.bundles import load_bundle
from otpy.lidar import read_lidar

bundle = load_bundle("frame-00000042.tar.zst")
arrays, metadata = read_lidar(bundle)
points = arrays["xyz_world"][arrays["status"] == 0]
```

내부 NPZ에는 **range·결측 상태·소스 카메라/픽셀·각도 오차**와 빔 패턴·센서 자세를 저장합니다. 반복되는 XYZ와 각도 배열은 `read_lidar()`가 복원하여 아래의 전체 배열 인터페이스를 반환합니다. XYZ까지 저장한 초기 묶음도 읽습니다. 독립 `lidar --output` 명령의 NPZ는 기존처럼 모든 배열을 담습니다. 원시 카메라 파일·`images.json` 형식은 유지하며 이전 기록에도 라이다 필드는 필수가 아닙니다. `bundle.json`의 `lidar_file`이 동봉 파일을 가리킵니다.

내부 NPZ를 다시 압축한 뒤 TAR.ZST에 넣지 않습니다. 외부 Zstandard가 한 번만 무손실 압축합니다. `index.jsonl`에는 프레임별 `lidar_seconds`, `archive_seconds`, `lidar_bytes`를 남기고, `run.json`에는 실제 worker 수·빔 설정·누락·종료 결과를 저장합니다. MCAP 발행, 레이더·IMU·GNSS 결합은 아직 없습니다.

## 배치와 빔

장착 위치는 [FH5 외판 보정](../ot/README.md#phase-1-고속도로-4뷰)을 그대로 사용하며 라이다용 위치를 따로 이동하지 않습니다. 설정은 [phase1-lidar.json](../ot/presets/phase1-lidar.json)에 있습니다.

| 센서 | 자세 기준·깊이 소스 | 수평 빔 | 수직 빔 | 최대 range |
| --- | --- | --- | --- | --- |
| L_F | mirror1 광축·원점, mirror0 협각 우선 / mirror1 광각 | −60°…+60°, 0.2° | −5°…+21°, 71개 비균일 채널 | 250 |
| L_PL | mirror2 광축·원점·깊이 | −50°…+50°, 0.2° | −13°…+27°, 64개 균일 채널 | 150 |
| L_PR | mirror5 광축·원점·깊이 | −50°…+50°, 0.2° | −13°…+27°, 64개 균일 채널 | 150 |

각도는 **각 카메라의 광축 기준**으로 방위각은 좌측, 고각은 위쪽이 양수입니다. 센서 좌표는 x 전방·y 좌측·z 위이며 `base_link`와 원점·회전이 다릅니다. 전방 광각의 하향 8°를 고려해 +3°…+13° 구간을 0.2°로 촘촘하게 했습니다. 양 끝은 0.8° 간격입니다. 측면은 하향 12° 카메라에 맞춰 광축보다 위쪽으로 범위를 옮겼습니다. 실물 업체의 빔 배열을 복제한 값은 아닙니다.

거리와 월드 좌표는 원본 `world_units`를 그대로 따릅니다. 현재 캡처는 `game_length_units`로 표시합니다. 최대 range는 새로운 실거리 검증 주장이 아닙니다. 요청한 빔이 실제 pinhole 시야 밖이면 결측으로 남기므로, 좌우 센서는 360° 라이다가 아닙니다.

## 샘플링과 결측

1. 캡처한 카메라 회전·projection·viewport로 빔을 원본 픽셀 좌표에 투영합니다.
2. `sources` 순서에서 시야에 들어오는 첫 카메라를 선택합니다. 전방 협각의 깊이가 없다고 광각의 다른 표면으로 메우지 않습니다.
3. 가장 가까운 픽셀의 geometry DSV를 기존 역투영 경로로 복원합니다. compact RGB-D의 R32F와 원시 DSV를 모두 지원합니다. 물체 경계에서 bilinear 보간하지 않습니다.
4. 그 픽셀의 카메라 Z를 빔 방향의 range로 바꿉니다. 픽셀 면적 안에서 Z가 일정하다는 근사이며, 빔 방향 자체를 픽셀 중심으로 꺾지는 않습니다. `source_ray_error_deg`에 원본 픽셀 광선과의 각도 차이를 남깁니다.
5. 유효한 range만 센서·월드 점으로 변환합니다. 나머지 좌표와 range는 NaN입니다.

전방 소스는 같은 원점을 가져야 합니다. 서로 다른 프레임·관측 세션의 소스를 섞거나 다른 장착점의 영상을 합치는 입력은 거절합니다. 원점 비교의 1e−4 게임 단위 허용치는 FP32 셀 내부 좌표 반올림용이며 다른 포드를 허용하는 기준이 아닙니다.

독립 NPZ와 `read_lidar()` 결과는 모든 요청 빔을 센서별로 이어 표현합니다. `metadata_json` 또는 반환 metadata의 각 센서에 시작 index·개수·채널×열 shape·실제 각도 배열·월드 원점과 회전·소스별 카메라/복사 QPC가 있습니다.

| 배열 | 의미 |
| --- | --- |
| `range`, `xyz_sensor`, `xyz_world` | 유효 반환 거리, 센서 FLU 좌표, 게임 월드 좌표 |
| `sensor_index`, `azimuth_deg`, `elevation_deg` | 빔의 센서와 방향 |
| `source_camera_index`, `source_pixel_xy` | 사용한 원본 카메라와 정수 픽셀 XY; 소스가 없으면 −1 |
| `source_ray_error_deg` | 원본 픽셀 중심 광선과 요청 빔의 각도 차이 |
| `status` | 0 반환, 1 시야 밖, 2 유효 깊이 없음, 3 최대 거리 초과 |

결측은 빈 공간의 증거가 아닙니다. 엔진의 미러 제외 규칙·LOD로 렌더되지 않은 물체는 이 방식에서도 빠집니다. intensity, 클래스, 노이즈·드롭아웃, rolling scan, 실제 raycast는 생성하지 않습니다. 레이더·IMU·GNSS도 이 명령의 범위가 아닙니다.

## 실제 기록 결과

![실제 4뷰에서 만든 세 라이다](images/phase1-virtual-lidar.png)

0.17.0 FH5 정차 기록의 frame 42에서 총 106,799개 빔 중 95,457개가 유효했습니다. 전방의 8,925개 빔은 협각, 33,746개는 광각을 선택했으며 두 원점 차이는 0이었습니다.

| 센서 | 반환 | 시야 밖 | 깊이 없음 | 거리 초과 |
| --- | ---: | ---: | ---: | ---: |
| L_F | 38,225 | 0 | 4,206 | 240 |
| L_PL | 28,167 | 320 | 3,247 | 330 |
| L_PR | 29,065 | 320 | 2,402 | 277 |

실제 네 카메라에서 골라 읽은 9,652개 픽셀의 역투영은 기존 전체 이미지 복원과 같았습니다. 반환 점을 원본 영상으로 다시 투영하면 선택 픽셀 중심에서 축별 최대 약 0.5픽셀 이내였고, 카메라 Z 차이는 최대 7.3e−6 게임 단위였습니다. 원시 DSV 디렉터리·ZIP·TAR.ZST 입력을 실행했고, 원시 material/color의 선택 픽셀 복원도 기존 경로와 대조했습니다. 다른 원점·다른 프레임을 섞는 입력은 실제로 거절됐습니다.

저장된 서로 다른 10개 프레임에서 순수 빔 생성은 중앙값 **69ms**였습니다(NumPy, BLAS 1스레드). TAR.ZST 읽기·해제·JSON 해석은 별도로 0.58–0.81초였고, 출력 압축 저장은 이 수치에 포함하지 않았습니다. 따라서 저장 파일 재생 전체가 10Hz라는 의미는 아닙니다. frame 42의 압축 NPZ는 약 3.80MB입니다. 원본은 로컬 `research/live/2026-10-08-camera-rig/lidar/`입니다.

후속 라이브 결합 기록에서는 **5초·50수집·50저장·누락/수집 오류 0회**, 총 압축 크기 **718,268,275B**를 기록했습니다. 4뷰 RGB-D·차량 메타데이터와 세 라이다를 같은 파일에 저장했습니다. 기본 옵션을 쓰는 CLI도 1초·10묶음·누락 0회였습니다. 정차·짧은 기록이며 장시간 주행이나 전경 FPS 측정을 대신하지 않습니다.

50개 파일을 모두 다시 읽어 200뷰·라이다의 프레임/세션 연결과 유효·결측 좌표를 확인했습니다. 첫·중간·마지막 묶음의 네 카메라에서 반환점을 원본 깊이와 대조했으며 카메라 Z 차이는 최대 6.04e−6 게임 단위였습니다. 복사 시각 기준 처리율은 **9.960Hz**, 간격 중앙값은 102.25ms였습니다. 네 worker의 프레임별 처리 시간 중앙값은 라이다 생성·NPZ 구성 126.6ms, 압축·저장 250.9ms로 겹쳐 실행됩니다. 이는 제어 입력까지의 종단 지연 측정이 아닙니다.

초기 방식은 XYZ를 중복 저장해 worker 2개에서 18/50개, 4개에서 3/50개가 누락됐고 6개로 늘려도 해결되지 않았습니다. 같은 표본의 라이다 NPZ를 7,098,515B → 2,184,491B로 줄였으며, 읽어 복원한 모든 배열이 이전 전체 XYZ 출력과 일치했습니다. 프레임 파일의 원본 RGB-D와 라이다는 디렉터리·ZIP·TAR.ZST 모두 저장 후 다시 읽어 대조했고, 다른 프레임의 라이다를 붙인 묶음은 거절했습니다. 결과는 로컬 `lidar-recording-offline/`, `stream-lidar-compact/`, `stream-lidar-cli/`에 있습니다. 세 경로 모두 `research/live/2026-10-08-camera-rig/` 아래입니다.
