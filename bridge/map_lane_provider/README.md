# 지도 차로 기준선

설치 게임의 실제 지도(기본 지도와 설치된 지도 DLC)의 Road 곡선·road look과 prefab 내비 곡선(`.ppd`)을 읽어, 현재 차량이 있는 차로의 주행 기준선을 생성합니다. 기본 실행은 `ot` 파이프에서 SDK `truck.world.placement`만 읽습니다. 게임 입력·Tier·설정은 변경하지 않습니다.

한 번 실행하면 현재 위치부터 `--distance-m`까지 한 파일을 만들고 끝납니다. `--follow`는 상주하면서 차량 앞에 항상 `--distance-m` 이상이 남도록 같은 파일을 이어 씁니다.

## 읽는 지도

`base_map.scs`와 게임 폴더의 `dlc_*.scs` 중 지도 sector(`/map/europe`), 월드 정의(`/def/world`), prefab·road 모델을 가진 archive를 함께 엽니다. 현재 설치에서는 `dlc_east`·`dlc_fr`·`dlc_north`(지도)와 정의·모델이 있는 DLC 4개, 모두 7개입니다. 같은 경로 파일은 DLC가 기본보다 우선합니다. DLC sector는 기본 지도와 겹치지 않아 한 지도로 합쳐 읽습니다. road look·prefab 정의도 DLC 것을 포함합니다. 설치하지 않은 지도 DLC 지역으로 넘어가면 그 sector가 없으므로 `map_unresolved`로 끝납니다.

## 차로 위치

게임 AI 차로는 모든 road look에서 4.5m 격자에 놓입니다. template 노면 폭, shoulder, 구형 look의 `road_size`는 AI 차로 위치를 바꾸지 않습니다. 차로 중심(Road 기준선에서 오른쪽 +, 진행방향 기준)은 다음과 같습니다.

- **양방향**(`lanes_left`와 `lanes_right`가 모두 있거나 `template_left`가 있는 중앙분리 look): 오른쪽 i번째 차로(중앙에서 바깥으로) = `road_offset + 2.25 + 4.5·i + lane_offsets_right[i].x`. Road 방향 교통입니다. 왼쪽은 같은 식을 대칭으로 쓰며 반대 방향 교통입니다. 예: `ger1`·`un11`(1+1) ±2.25, `ger14` ±4.25/±8.75/±13.25, `un22_4m`(offset 6.75, lane offset -4.5) ±4.5/±9.0, 좁은 길 `ger4`(lane offset -1) ±1.25.
- **단방향**(한쪽만 차로): 차로들을 기준선 중심에 두고 홀수 차로를 왼쪽에 둡니다. `4.5·(k+0.5)`, k = -ceil(n/2) … floor(n/2)-1. 예: 1차로 -2.25, 2차로 ±2.25, 3차로 -6.75/-2.25/+2.25.
- **좌측통행** Road(`LeftHandTraffic`)는 위치를 그대로 두고 방향만 바꿉니다.
- `traffic_lane.rail.*`(철도·트램 궤도)은 주행 차로가 아닙니다. 차와 트램이 함께 쓰는 `traffic_lane.road.*.tram`, 버스 차로, AI가 없는 `no_vehicles` 차로는 주행 차로로 봅니다.
- 차로 폭(`lane_width_m`)은 가장 가까운 다른 주행 차로 중심까지 거리이며 3–4.5m로 제한합니다. 좁은 길은 3m, 그 밖에는 4.5m입니다.

이 규칙은 `--audit-looks`로 지도 전체에서 확인했습니다(아래 검증). 차로가 없는 look(보도·보행로·철도)만 지원하지 않습니다.

차량 위치에서 시작할 때는 진행방향과 같은 교통의 차로 중 가장 가까운 차로를 고릅니다. 양방향 도로에서는 자기 쪽 차로가 선택됩니다. 차로 중심에서 폭의 절반 안에 Road 차로가 없으면 prefab 내비 곡선(45° 이내 방향)을 찾습니다. 그래서 교차로·회전교차로 안에서도 시작합니다. 여러 이동 곡선이 겹치는 곳(교차로 진입부)에서는 0.5m 이내 후보 중 도달 가능한 출구의 방향 변화가 가장 작은 곡선을 고릅니다. 직진 중인 차량을 회전 곡선에 올리지 않기 위해서입니다.

## 연결 규칙

**Road→Road.** 공유 Node에서 다음 Road의 같은 진행방향 차로 중 지금 차로 끝과 0.5m·15° 이내로 이어지는 차로를 따라갑니다. 0.5m 이내 차로가 없으면(look 전환에서 lane offset이나 중앙분리대 폭이 바뀜) 왼쪽부터 같은 순위의 차로가 6m 이내일 때 이어 갑니다. 이 단차는 다음 조각의 앞부분에서 smoothstep으로 흡수합니다. 흡수 길이는 단차 1m당 25m이며 15–150m입니다(조각 길이 이내). 차로 변경은 하지 않습니다.

**prefab 통과.** prefab 서술자의 노드·곡선을 origin Node 기준으로 world로 옮깁니다. 진입 Node의 `InputLines`에서 출발해 `NextLines`로 다른 Node의 `OutputLines`에 닿는 모든 경로를 구합니다. 소형차 전용 곡선은 제외합니다.

1. **진입 곡선**: 차로 끝에서 가장 가까운 곡선입니다. prefab에 이 차로의 곡선이 없거나(차로가 prefab에서 끝남) 그 곡선이 어느 출구에도 닿지 않으면, 출구에 닿는 가장 가까운 곡선이 4.6m(한 차로) 이내일 때 이어 받고 단차를 흡수합니다(합류).
2. **본선(through) 출구**: 진행방향 변화 30° 이내 출구 중 ① 그 Node를 떠나는 차로 수가 가장 많고 ② 직전 Road와 같은 road look이고 ③ 방향 변화가 가장 작은 출구입니다. 같은 조건의 출구가 2° 이내로 여럿이면 분기(split)입니다. 이 차로가 본선 또는 분기 가지에 닿으면 왼쪽부터의 차로 순서를 유지하는 가지를 고릅니다.
3. **본선이 없을 때**: T자 교차로의 줄기, 본선이 크게 꺾이는 교차로, 출구 전용 차로, prefab 안에서 시작한 경우입니다. 이 차로가 닿는 출구 중 방향 변화가 가장 작은 쪽을 따라갑니다. 좌우 대칭 T자에서는 짧은 경로, 보통 우회전입니다.
4. 신호등·차단기(톨게이트)가 있어도 경로는 계속됩니다. 고른 곡선의 첫 신호 지점을 `events`로 냅니다. 페리 연결 prefab에서는 `ferry`로 끝냅니다.
5. 내비 곡선이 없는 prefab(건널목·다리·장식 조각 등, 지도에서 Road에 붙은 것 790개)은 진입 Node의 횡 위치를 유지해 방향 변화가 가장 작은 다른 Node로 Hermite 곡선으로 건넙니다(60° 이내, 200m 이내).

`--route <경로 파일>`이 없으면 내비게이션 목적지 경로를 쓰지 않습니다(SDK telemetry에는 남은 거리·시간·제한속도만 있고 경로가 없습니다). `--route`를 주면 경로 위 prefab에서 경로의 출구를 고릅니다(아래 "SD 지도 경로"). 같은 Road나 prefab을 같은 방향으로 다시 지나면 `loop`로 끝냅니다. 반대 방향 재방문(막다른 길에서 되돌아옴)은 허용합니다.

## end_reason

| 값 | 의미 |
|---|---|
| `distance_limit` | 지정 거리에 도달(follow에서는 계속 연장 가능) |
| `road_end` | 다음 item이 없음(지도의 막다른 길·지도 가장자리·회사 부지 입구) |
| `lane_ends` | 다음 Road에 이 차로를 잇는 차로가 없음(같은 순위 차로가 6m보다 멂, 또는 15° 넘는 꺾임) |
| `direction_change` | 다음 Road에 이 진행방향 차로가 없음(일방통행 역방향) |
| `unsupported_road` | 주행 차로가 없는 look(보도·철도) |
| `prefab_unsupported` / `prefab_no_lane` / `prefab_lane_mismatch` / `prefab_no_exit` | 곡선 없는 prefab을 건널 수 없음 / 진입 곡선 없음 / 진입 곡선이 4.6m·15°를 넘음 / 출구 없음 |
| `ferry` | 페리 |
| `loop` / `ambiguous` / `unsupported_item` / `map_unresolved` | 같은 방향 재방문 / Node 연결 이상 / Road·prefab이 아닌 item / 설치되지 않은 지도 지역 |

이전 버전의 `traffic_light`·`toll_gate`·`prefab_no_through`·`prefab_ambiguous`·`lane_leaves_through`는 더 이상 내지 않습니다. 제어기는 모든 끝점과 `events`의 신호 지점에서 감속해야 합니다. 신호 상태, 교통 대응, 차로 변경은 제공하지 않습니다.

## 빌드·실행

로컬 SDK와 기존 고정 TruckLib 소스가 필요합니다. SDK는 `research/tools/dotnet10`, TruckLib는 `research/sources/sk-zk_TruckLib-c42dbe4f2d6b`에 있습니다. 연구 자료는 Git에 포함되지 않습니다.

프로젝트 루트에서:

```powershell
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'
research/tools/dotnet10/dotnet.exe build bridge/map_lane_provider/map_lane_provider.csproj -c Release --disable-build-servers
$env:ETS2_DIR = '<게임 설치 폴더>'  # 또는 --game '<게임 설치 폴더>'
research/tools/dotnet10/dotnet.exe bridge/build/map_lane_provider/Release/net10.0/map_lane_provider.dll --out 'bridge/recordings/gt-path/current-lane.json' --distance-m 1000
# 상주 모드
research/tools/dotnet10/dotnet.exe bridge/build/map_lane_provider/Release/net10.0/map_lane_provider.dll --follow --out 'bridge/recordings/gt-path/current-lane.json' --distance-m 1000
# 지도 전체 look 감사(약 1.5분)
research/tools/dotnet10/dotnet.exe bridge/build/map_lane_provider/Release/net10.0/map_lane_provider.dll --audit-looks 'bridge/recordings/gt-path/offline/audit-looks.csv'
```

provider가 실행 중이면 빌드 폴더의 DLL이 잠깁니다. 이때는 `-p:OutDir=<다른 폴더>/`로 따로 빌드합니다.

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `--game` | `ETS2_DIR` | 게임 설치 폴더. 런처는 설치된 loader 위치에서 찾아 전달 |
| `--out` | `bridge/recordings/gt-path/current-lane.json` | 출력 JSON |
| `--distance-m` | 1000 | 한 번 실행: 생성 길이. follow: 차량 앞에 확보할 길이 |
| `--snapshot` | (`ot` 파이프) | 저장 snapshot JSON(raw 또는 `{"snapshot": ...}`). follow에서는 매 주기 다시 읽음. `-`는 stdin 한 줄당 snapshot 하나(오프라인 재생) |
| `--follow` | 끔 | 상주 모드 |
| `--extend-below-m` | min(600, distance) | follow: 앞 남은 거리가 이보다 작으면 연장 |
| `--keep-behind-m` | 300 | follow: 차량 뒤로 남길 길이 |
| `--period-s` | 0.5 | follow: placement 읽기 주기(약 2 Hz) |
| `--scene-radius-m` | 180 | follow: 주변 지도 파일(`<out>.scene.json`)의 반경. 0이면 쓰지 않음 |
| `--game-route` | 끔 | follow: 게임 GPS 경로를 실행 중인 게임에서 읽어 `<out>.route.json`에 씀(값 없는 옵션, 오프라인 재생에서는 안 함). 런처가 켬 |
| `--audit-looks` | — | 지도 전체 sector를 읽고 road look별 차로 모델을 prefab 내비 곡선 끝점과 비교한 CSV를 쓰고 끝냄 |

지도는 현재 위치 주변 3×3 sector(4 km)만 먼저 읽고, 기준선이 아직 읽지 않은 sector의 item에 닿으면 그 주변 sector를 더하고 멀어진 sector를 빼서 지도를 다시 엽니다(약 1–3초). archive 확인과 road look 읽기는 시작할 때 약 2.3초입니다. prefab 서술자 경로표는 처음 prefab에 닿을 때 SII에서 읽고(약 5초) 빌드 폴더의 `prefab-desc-cache.json`에 저장합니다. `def.scs`나 정의를 가진 DLC archive의 크기·시각이 바뀌면 다시 만듭니다.

## 출력과 follow 프로토콜

JSON은 `frame_id: "world"`, `lane_width_m`, `points`, `end_reason`, `revision`, `path_id`, `start_s`, `events`입니다. 점 좌표는 기존 ROS world와 같은 `[SCS X, -SCS Z]` 미터입니다. 약 1.5m 간격으로 실제 차로 곡선을 샘플링하며(조각 접합부의 0.5m 미만 중복점은 제거), 현재 위치의 차로 투영점에서 진행 방향으로 시작합니다. `start_s`는 `points[0]`의 경로 시작점 기준 누적 거리입니다. 한 번 실행은 `revision`·`path_id` 1, `start_s` 0입니다.

`events`(추가 필드)는 `[{"type": "traffic_light" | "toll_gate", "s": 경로 누적 거리}]`입니다. `s`는 `start_s`와 같은 기준이며, 해당 prefab에서 신호가 걸린 첫 곡선의 시작점(정지선 부근)입니다. follow에서는 버린 점보다 앞의 event를 함께 버립니다. 기존 소비자는 이 필드를 무시해도 됩니다.

follow는 따라가는 차로 양옆의 같은 방향 차로도 `<out>.left.json`·`<out>.right.json`(예: `follow-lane.left.json`)에 같은 형식으로 씁니다. 따라가는 차로 중심에서 차로 폭만큼 옆 점으로 시작하고, 따라가는 차로와 함께 연장·정리합니다. 따라가는 차로가 reset되거나 옆 차로가 트럭 옆을 벗어나면(차로 감소·추가) 다시 시작하며, 없으면 파일을 지웁니다(재시도 2 s마다). `path_id`는 세 파일이 한 카운터를 나눠 씁니다. 상태 줄에 `left_path_id`·`right_path_id`가 붙습니다. 용도는 방향지시등 차로 변경과 운전자 차로 변경 뒤 바로 다시 잡기입니다. ✔ 2026-10-21 고속도로 기록(왕복 2+2차로) 재생에서 옆 차로가 트럭 기준 4.5 m에 나왔고, 운전자가 차로를 바꾸자 반대쪽 차로로 바뀌었습니다.

### 지도 구역 읽기와 차로 찾기 (2026-10-21 저녁)

- **백그라운드 구역 읽기**: 지도 구역(sector)이 모자라면 예전에는 그 주기 안에서 지도를 다시 열어 주기가 0.7–2.4 s 멈췄습니다. 차로 변경 뒤 재시작과 겹치면 새 차로가 그만큼 늦었습니다. 이제 follow는 매 주기 끝에 다음 주기들이 쓸 구역을 계산해, 없는 구역이 있으면 지도 아카이브를 따로 연 리더로 백그라운드에서 지도를 엽니다. 다음 주기 시작에 그 지도로 바꿔 끼웁니다. 항목은 UID로 다시 찾으므로 바꿔 끼우기는 예전 재오픈과 같습니다. 필요한 구역: 트럭과 진행 방향 1.2 km 지점의 구역 하나 여유(차로 찾기 기준), 진행 방향 600·1200 m와 경로 끝, 끝에서 마지막 방향으로 600 m의 500 m 여유(경로 연장 기준). 그래도 주기 안에서 열게 되면 stderr에 `map: synchronous open for ...`(요청 위치, 그때 읽던 구역)를 남깁니다. 구역이 한 번에 최대 21개쯤 열려 메모리가 조금 늘었습니다.
- **차로 찾기 공간 색인**: 트럭에서 가장 가까운 차로를 찾을 때 열린 지도의 모든 도로를 2 m 간격으로 훑었습니다(25 ms 안팎, 차로 밖이면 0.5 s마다, 옆 차로 시작마다 두 번 더). 이제 지도를 열 때 도로(16 m 간격 표본)와 prefab(노드)을 64 m 격자에 넣어 두고 주변 칸만 봅니다. 주변에 색인된 도로가 없으면 예전처럼 전부 훑습니다.
- 검증(독일 고속도로 12 km 가상 주행, 0.5 s마다 실시간으로 넣음, 기존 provider와 비교): 차로 파일 revision 33개가 점 단위로 같았습니다. 주행 중 지도 열기 5번이 모두 백그라운드였습니다(예전: 3번 모두 주기 안, 1.0–1.7 s). 주기 p95는 41.6 → 7.5–15 ms(백그라운드 읽기 중 CPU를 나눔)이고, 4 km 실제 주행 기록의 차로 재시작은 56 → 6 ms, 86 s 고속도로 기록에서는 112 → 27 ms였습니다. 시작할 때 첫 지도 열기(1–2 s)는 그대로입니다.

### 주변 지도(scene)

follow는 Foxglove 표시용으로 트럭 주변 `--scene-radius-m`(기본 180 m, 지면 평면 거리) 안의 지도도 `<out>.scene.json`(예: `follow-lane.scene.json`)에 씁니다. 자율주행 화면처럼 내 차 주변의 차선·도로 경계·교차로·신호등을 보여 주기 위한 것이고, 제어에는 쓰지 않습니다.

```json
{"frame_id": "world", "revision": 12, "center": [x, y, z], "radius_m": 40,
 "lines": [{"kind": "edge" | "lane" | "centre" | "junction", "s0": 14.2, "points": [[x, y, z], ...]}, ...],
 "signals": [{"type": "traffic_light", "position": [x, y, z], "height_m": 6.44, "heading": 1.57}, ...]}
```

- 좌표는 다른 파일과 같은 ROS world `[SCS X, -SCS Z]`에 높이 `SCS Y`를 더한 `[x, y, z]` 미터입니다(소수 둘째 자리). `center`는 그 주기의 트럭 위치입니다.
- **Road**(양방향 모두): 차로 모델(위 "차로 위치")의 주행 차로마다 중심 ± 폭/2를 경계로 두고, 이웃 차로의 마주 보는 경계가 0.75 m 안이면 한 선으로 합칩니다. `lane`은 같은 방향 차로 사이, `centre`는 반대 방향 사이, `edge`는 그 밖(도로 바깥 가장자리, 중앙분리대 양쪽)입니다. 철도·트램 궤도 차로와 차로가 없는 look(보도)은 그리지 않습니다.
- **prefab**(교차로·회전교차로·램프 조각 등): 내비 곡선(소형차 전용 제외)을 `junction` 선으로 냅니다. prefab 안의 도로 경계는 내지 않습니다(아래 한계).
- 선은 약 2 m 간격으로 샘플링하고 반경 안의 연속 구간만 냅니다. `s0`는 첫 점의 그 선(Road·곡선 하나) 시작부터의 거리로, 점선 무늬를 땅에 고정하는 데 씁니다.
- **신호등**: prefab 서술자의 semaphore 중 신호등 type이거나, profile(`/def/world/semaphore_profile*.sii`)에 `traffic_light*` 칸이 있는 것만 냅니다. 차단기(톨게이트·회사 부지), 주유소(`gas_*`), 철도 건널목(`crossing*`), 논리 전용(virtual)은 뺍니다. 지면 평면 1 m 안의 locator(기둥 밑동과 그 등)는 하나로 합칩니다. `position`의 z는 가장 가까운 내비 곡선 점의 높이(지면), `height_m`은 그 위 가장 높은 등의 높이입니다(0은 지면에 놓인 기둥 모델, 약 1.9 m는 기둥 등, 4.5 m 이상은 차로 위 가로대 등). `heading`은 locator의 -Z 축으로, 그 신호가 통제하는 교통의 진행 방향입니다(등은 반대쪽을 봅니다). ✔ 독일 신호 교차로 3곳에서 각 진입로의 오른쪽 정지선 부근에 진행 방향으로 나왔습니다.
- **신호 상태는 없습니다.** 게임 SDK가 신호등 상태를 내보내지 않으므로 위치와 방향만 냅니다.
- 타이밍: 매 주기(0.5 s) 계산하고, 트럭이 반경의 1/10(기본 18 m) 넘게 움직였거나 반경 안 item이 바뀌었을 때만 파일을 원자적으로 교체합니다(`revision` 1씩 증가). 넓게 쓰는 이유: hud는 파일이 바뀔 때만 읽고 매 프레임 트럭 앞 `scene_ahead_m`(50 m) 지점 주변 `scene_view_m`(70 m)만 그리므로(앞 120 m, 뒤 20 m), 다음 교체까지 트럭이 움직여도 주변이 비지 않습니다. 2026-10-22 전에는 반경 40 m를 트럭 위치 중심으로 쓰고 hud가 0.5 s마다 통째로 그려서, 90 km/h에서 앞쪽이 약 15–27 m만 나왔습니다. Road·prefab마다 형상을 캐시하므로 보통 주기는 2 ms 안팎입니다. 처음 보는 prefab 모델의 서술자를 읽는 주기, 지도 재오픈 뒤 색인을 다시 만드는 주기는 30–50 ms, 시작 첫 주기는 0.1–0.5 s입니다. 30 ms를 넘은 주기는 stderr에 `scene: ...` 줄로 남깁니다. 상태 줄에는 `scene_written`·`scene_ms`·`scene_items`가 붙습니다.
- scene 실패는 차로 파일에 영향을 주지 않습니다. scene은 차로 추종과 따로 계산하므로 지도 차로 밖(회사 부지 등)에서도 나옵니다.
- ✔ 2026-10-21 오프라인 재생(`--snapshot -`): 고속도로 기록 주행 80 s(161주기), 독일 일반도로 8 km(신호 교차로 포함, 1001주기), 북유럽 도시 8 km(1001주기). 보통 주기 중앙값 1.5–2.5 ms, p95 2.6–8.7 ms(같은 PC에서 MATLAB이 돌던 중). 따라가는 차로를 가로지르는 경계는 확인한 1,965주기 중 1,902주기에서 차로 중심 ±폭/2와 0.05 m 안(대부분 0.02 m 안)이었습니다. 나머지는 트럭이 prefab 안에 있거나(경계 없음) 따라가는 차로가 look 전환 단차를 흡수하는 S자 구간입니다. 파일 크기는 중앙값 3–7 KB, 최대 16 KB(점 613개)입니다(반경 40 m 때).
- ✔ 2026-10-22 반경 120 m 오프라인 재생(고속도로 기록 1040주기, 90 km/h, 실시간 0.5 s): 매 위치에서 그 전 주기까지 쓴 파일이 트럭 주변 60 m를 모두 덮었습니다(최소 60 m; 같은 재생에서 예전 40 m는 27.5 m, hud의 0.5 s 읽기 지연 제외). 주기 중앙값 2.0 ms·p95 3.1 ms(40 m: 1.5·2.3 ms), 파일 중앙값 25 KB·최대 89 KB입니다. 반경 안 item이 자주 바뀌어 교체는 거의 매 주기입니다.
- ✔ 2026-10-22 반경 180 m 같은 재생(실시간 대기 없이, 게임 실행 중): 그 전 주기까지 쓴 파일이 트럭 앞 50 m 지점 주변 70 m를 매 위치에서 덮었습니다(여유 최소 35 m, 90 km/h 0.5 s 지연 12.5 m보다 큼). 교체 718/1040회, 주기 중앙값 2.8 ms·p95 13.9 ms, 파일 중앙값 44 KB·최대 153 KB입니다.

한계: 교차로·램프 prefab 안의 도로 가장자리는 없습니다(서술자의 지도용 `MapPoints`로 만들 수 있지만 단순하지 않아 하지 않았습니다). 내비 곡선이 없는 prefab(건널목·다리 조각 등) 구간에서는 선이 끊깁니다. look 전환에서 차로 위치가 바뀌는 곳은 경계도 계단처럼 끊깁니다(차로 기준선만 S자로 흡수). 경계는 AI 차로 격자(4.5 m, 좁은 길 3 m) 기준이라 실제 노면 도색과 폭이 다를 수 있고, 점선·실선 구분은 지도에 없어 종류(`lane`/`centre`/`edge`)로만 나눕니다. Road 위 신호등(prefab 밖)과 신호 상태는 내지 않습니다.

### 게임 GPS 경로(`--game-route`)

follow는 게임 GPS에 잡힌 경로를 실행 중인 게임 메모리에서 읽어 `<out>.route.json`(예: `follow-lane.route.json`)에 씁니다. 판단 계층이 언제 어느 차로로 갈지 정하는 전역 경로입니다. 읽기만 합니다(`PROCESS_VM_READ`, 주입·쓰기 없음).

- 찾는 길: 설치된 실행 파일 `.text`에서 ETS2LA 플러그인의 두 패턴(`research/sources/ETS2LA_plugin-3b01d90b5be2`, `src/patterns.win32.hpp`)으로 base_ctrl 정적 포인터와 gps_manager 멤버 오프셋을 찾고(1.61.1.9: RVA `0x36AF7C8`, `+0x4128`; 1.61.1.1은 `0x36AE6D8`, 같은 오프셋), base_ctrl이 `game_ctrl` 유닛일 때(주행 중) gps_manager → simple route source(+8) → route task(+0x20) → route items(+0x78, 0x30 바이트 항목)를 읽습니다. 항목은 경로 순서의 node(종류 0)와 그 사이 prefab(종류 1)이고, node마다 끝까지 남은 게임 거리·시간이 있습니다.
- 도로·prefab 사슬: 이웃한 두 node가 함께 닿는 item(node의 backward/forward item, 종류 3 도로·4 prefab)이 그 구간입니다. 도로는 앞 node의 forward item이면 도로 방향(`forward` true)입니다. 지도 없이 메모리만으로 만들므로 아직 읽지 않은 sector의 경로도 나옵니다. 둘이 함께 닿는 item이 없으면 `gap`(페리·열차: 게임이 항구 node를 바로 잇습니다).
- 쓰는 때: 매 주기(0.5 s) route task 머리(포인터·배열 주소·크기)만 읽고, 바뀌었을 때만 전체를 읽어 원자적으로 교체합니다(`revision` 1씩 증가). 읽는 중에 task가 바뀌면 그 주기를 버리고 다음 주기에 다시 읽습니다. 경로가 없거나 주행 중이 아니면 `{"source": "game", "unavailable": "no route" | "not driving"}`로 바꿉니다. 상태 줄에 `route_written`·`route_nodes`가 붙고, 오류는 stderr `game route: ...`로 바뀔 때만 남깁니다.

```json
{"frame_id": "world", "source": "game", "revision": 1, "created": "ISO 시각",
 "start": [x, y], "goal": [x, y], "length_m": 9581.5, "eta_s": null,
 "game_distance_m": 145909.6, "game_time_s": 8689.6,
 "items": [{"kind": "road" | "prefab" | "gap", "uid": "16진 uid", "entry_node": "16진 uid", "exit_node": "16진 uid",
            "forward": true, "exit_curves": [], "lanes_ok": [], "s_start_m": 0, "length_m": 64.3, "game_distance_m": 198.8}, ...],
 "polyline": [[x, y, z], ...]}
```

- 좌표는 ROS world입니다. `polyline`은 경로 node 위치(도로 곡선이 아닌 node 사이 직선), `length_m`·`s_start_m`·항목 `length_m`은 그 직선 길이입니다. `game_*`는 게임이 GPS에 보여 주는 지도 축척 거리·시간(도시 밖 약 19배, 도시 안 3배)이며 참고용입니다.
- 이 형식은 자체 경로 탐색(B, 같은 파일 형식에 `"source": "sdmap"`)과 같습니다. `exit_curves`(그 prefab에서 경로를 따르는 내비 곡선)와 `lanes_ok`(차로 변경 없이 다음 출구로 갈 수 있는 차로)는 빈 배열이고, `--enrich-route`가 SD 지도로 채웁니다. walker는 이 파일을 `--route`로 받아 교차로 출구를 고릅니다(아래 "SD 지도 경로", ✔ 2026-10-22 이 절의 실게임 경로 파일로 확인).
- 경로는 게임이 마지막으로 경로를 계산한 지점에서 시작합니다(달려도 앞 항목이 줄지 않음). 트럭의 진행 위치는 소비자가 `polyline`에 투영해 구합니다.
- ✔ 2026-10-22 실행 중인 1.61.1.9 게임: 패턴이 각각 두 곳(같은 값)에서 맞았습니다. 첫 읽기 경로는 node 688개(prefab 항목 70개), 직선 81 km·게임 거리 1,507 km(구간 비율 중앙값 19.0, 18.5 km 한 구간은 페리)였고, 다시 읽었을 때 경로는 node 176개(도로 134·prefab 41·gap 0), 직선 9.6 km였습니다. 트럭은 그 폴리라인에서 2.7 m(30번째 항목, prefab 안)였고, 그 항목 시작의 남은 게임 거리 137,461이 SDK `truck.navigation.distance` 136,216과 항목 안 진행만큼 차이 났습니다. 1.61.1.1(2026-10-08, `research/live/2026-10-08-readonly`)에서도 같은 경로가 읽혔습니다.
- 한계: 차로 단위 안내(게임 route advisor의 차로 화살표, simple route source의 `lane_routing_task`)는 아직 읽지 않습니다. 게임 업데이트로 패턴이 안 맞으면 `game route: game GPS patterns not found`를 남기고 경로 파일 없이 계속 돕니다. 구조체 오프셋(+8, +0x20, +0x78, node +0x20/+0x28/+0x30, item +0xA/+0x40)이 바뀌면 읽기 검사(배열 크기·uid 0·읽는 중 변경)에서 걸러지지만 엉뚱한 값을 걸러 내지 못할 수 있어, 업데이트 뒤에는 위 검증(트럭이 폴리라인 위, 남은 거리)을 다시 해야 합니다.

follow는 매 주기 placement를 기준선에 투영합니다.

- 차로 중심에서 `0.75 × lane_width_m`보다 멀어지면(차로 변경, 교차로에서 다른 방향, 다른 도로, 끝 통과) 그 위치에서 새 경로를 만듭니다. 이때 `path_id`가 1 늘고, 상태 줄의 `reset`은 `true`입니다. prefab 안에서도 새 경로를 시작합니다.
- 계속 가능한 경로(`distance_limit`)에서 앞 남은 거리가 `--extend-below-m`보다 작으면 차량 앞 `--distance-m`까지 **뒤에 덧붙이고**, 차량 뒤 `--keep-behind-m`보다 오래된 점을 버립니다. 이미 낸 점은 바뀌지 않습니다.
- 바뀔 때마다 `revision`을 1 늘리고 같은 디렉터리의 임시 파일을 완성한 뒤 교체합니다. 소비자가 파일을 열고 있어 교체가 실패하면 25ms 간격으로 최대 1초 재시도합니다.
- stdout에 갱신마다 JSON 한 줄을 씁니다. 필드는 `revision`, `path_id`, `updated`, `reset`, `start_s`, `end_s`, `truck_s`, `ahead_m`, `offset_m`, `point_count`, `end_reason`, `prefabs`, `sectors`, `elapsed_ms`, `error`입니다. 오류(차로 밖·파이프 끊김·로딩·메뉴 등)는 메시지가 바뀔 때만 출력하고, 마지막 파일은 유지한 채 재시도합니다. 차로에 다시 올라오면 자동으로 새 경로를 냅니다. `--snapshot -`에서는 매 줄마다 한 줄씩 답합니다.

소비자는 `LanePath::extend(file)`(`ros2/src/ets2_autonomy/src/control/path_following.hpp`) 또는 C ABI `lat_extend(handle, file)`(`apps/sysid/lateral_capi.cpp`)로 이어 붙입니다. 이 함수들은 새 파일에서 자신의 마지막 점과 같은 누적 거리(`start_s + s`, 0.01m 이내)·위치의 점을 찾고, 그 뒤 점만 덧붙입니다. 기존 점의 `s`·heading·`progress`는 그대로입니다. revision이 같거나 작으면 0을 반환합니다. `path_id`가 다르거나 이어지는 점이 없으면 실패하고(`lat_extend`는 -1), 호출자가 파일을 새로 읽습니다. 런처와 ROS 노드(`gt_drive_node`)는 아직 한 번 실행 파일만 읽습니다.

차량이 지도 차로 밖(회사 부지, 주차장, 갓길 바깥)에 있으면 한 번 실행은 실패하고 기존 출력을 유지합니다. 호출자는 종료 코드 0을 확인한 뒤 새 파일을 사용해야 합니다. 한 번 실행의 stdout 요약에는 시작 item(`start_item`: road/prefab), `junction_choices`(출구가 여럿인 prefab 수), `events`, `blends`·`max_blend_m`(흡수한 단차)도 나옵니다.

## 검증 (2026-10-21)

증거는 `bridge/recordings/gt-path/offline/v2/`(Git 제외)에 있습니다. 도구는 `offline/tools/`의 `check_lane.py`, `follow_replay.py`(`PROVIDER` 환경변수로 DLL 지정), `snap2`(임의 Road·prefab 위 snapshot), `many2`(지도 전체 무작위 snapshot)입니다.

- **차로 모델 감사**(`--audit-looks`, 기본+DLC 486 sector, Road 114,663개 10,800 km): 주행 차로가 있는 look 244개(9,578 km)를 모두 모델링했습니다. 지원하지 않는 1,222 km는 모두 보도·보행로·철도 look입니다. Road 끝에 붙은 prefab 내비 곡선 끝점 152,179개 중 151,722개(99.7%)가 같은 방향 모델 차로와 0.3m 이내입니다. 일치한 끝점의 최대 오차는 0.25m, look별 중앙값은 0.001–0.016m입니다. 양방향 look 169개(6,931 km)는 99.9%, 단방향 75개는 99.4%, 좌측통행 22개(510 km)는 98.3%가 일치합니다. 90% 미만은 표본이 적은 4개 look입니다(`look9`, `blke_40`, `blke41`, `look27:lht`).
- **기존 경로 재현**: 2026-10-20 snapshot 4개(독일 고속도로)의 출력이 이전과 최대 0.002m 차이입니다. 기존 기록 차로 1 km는 차이가 없습니다. 이전에 29.4 km에서 `prefab_no_through`로 끝나던 `lane-a`는 고속도로가 1차로로 줄어드는 곳을 지나 30 km까지 이어집니다.
- **양방향 일반도로·DLC**(독일·북유럽·프랑스·발칸·발트 일반도로, 좁은 길, 도시 4차로, `dlc_east`·`dlc_fr`·`dlc_north` 각 3곳, 10 km 요청): 모두 시작했습니다. 최대 점 간격은 1.88m, 접합 오차는 최대 0.04m입니다. 교차로 회전(R 5–10m)과 도시 T자 우회전에서 최대 꺾임 21°가 생깁니다. 이는 내비 곡선 자체의 반경입니다. 중앙분리대 폭이 바뀌는 `un22_4m`↔`un22_10m` 전환에서는 3m 단차를 흡수했습니다.
- **prefab 안 시작** 5곳(T자, 도시 교차로, 좁은 길 교차로, 동유럽 교차로): 모두 시작해 3 km 이어지거나 실제 막다른 길에서 끝났습니다.
- **무작위 80곳**(`many2`, Road 길이 가중, 10 km): 80곳 모두 시작했고 353 km를 만들었습니다. 10 km를 채운 경로가 23개, 5 km 이상이 30개입니다. prefab 947개(출구가 여럿인 곳 206개)를 지났고 신호 event는 117개입니다. 종료 원인은 `road_end` 52, `distance_limit` 23, `map_unresolved` 3(설치하지 않은 지역), `loop` 2입니다. `road_end` 14곳을 확인한 결과 모두 다른 도로나 prefab이 80m 안에 없는 실제 막다른 길이었습니다. 무작위 표본에는 연결망에서 떨어진 풍경용 도로가 많이 섞입니다. 최대 점 간격은 2.02m, 흡수 없이 이은 접합은 최대 0.5m·2.6°입니다. 단차 흡수는 37번 있었고 최대 5.0m입니다(좌측통행 고속도로 look 전환). 꺾임 10° 초과가 15개 경로에 있으며 모두 교차로 회전 곡선입니다.
- **follow 재생**(가상 차량 주기당 25m, 횡 ±0.3m, `lat_extend`로 이어 붙임): 일반도로 8 km(ger1, 신호 교차로 포함), 북유럽 도시 8 km(prefab 40개), 발트 일반도로 8 km, 교차로 안 시작 3 km, 중앙분리대 전환 7 km, 독일 고속도로 12 km(prefab 50개)에서 이음 오류가 없었습니다. 이어 붙인 경로는 한 번 실행 경로와 점 단위로 같았습니다(차이 0). 남은 앞 거리는 최소 623m, 파일 점 수는 최대 889개, 갱신 시간은 지도 재오픈을 포함해 최대 2.1초였습니다.
- **실게임**(`ot` 읽기 전용, 프랑스 고속도로 `template15`, `dlc_fr`): 한 번 실행 2 km가 시작 오차 0.56m로 만들어졌습니다. 45초 follow에서 revision 2까지 정상 연장했습니다(차량 앞 999m 유지, 운전자 주행 중 횡 오차 0.8–1.25m). 게임이 로딩 중일 때는 placement 없음 오류를 내고 재시도했습니다. 램프 옆(차로 바깥 5.8m)에서는 차로 밖 오류를 냈습니다. 최종 빌드의 30초 follow도 같은 고속도로에서 시작 오차 0.63m로 경로를 냈습니다(정차 중).

## SD 지도 경로(`--plan-route`, `--route`)

설치된 지도 전체(기본+지도 DLC)를 한 번 읽어 경로 탐색용 그래프(SD 지도)를 로컬 캐시로 만들고, 그 위에서 목적지까지 시간 최소 경로를 찾아 `<out>.route.json`(게임 GPS 경로와 같은 형식)을 씁니다. follow나 한 번 실행에 `--route <파일>`을 주면 walker가 교차로(prefab)에서 그 경로의 출구를 고릅니다. 판단 계층(차로 변경 계획)은 아직 없고, 이 기능은 경로와 "어느 차로여야 하는지"를 그 계층에 넘기는 데까지입니다. 코드는 `SdMap.cs`(캐시), `RoutePlanner.cs`(탐색·경로 파일·`--route`·CLI)이고, `Program.cs`에는 옵션 처리, walker 출구 선택 훅, 경로 event 전달만 더했습니다.

### 캐시(SD 지도)

- **그래프**: 정점은 item 통과 하나입니다. 도로는 진행 방향마다(주행 차로가 그 방향에 있을 때) 하나, prefab은 진입 node → 출구 node마다 하나(내비 곡선 경로가 있을 때, 소형차 전용 곡선 제외, 같은 node로 돌아오는 출구 제외)입니다. 통과는 출구 node에서 그 node 반대편 item의 통과로 이어집니다(walker의 한 걸음과 같은 연결). 내비 곡선 없는 prefab(건널목·다리 조각 등)은 walker의 PassThrough처럼 진입 node마다 가장 곧은 node 하나로 건넙니다(60°·200 m 이내). 페리 prefab은 통과를 만들지 않습니다.
- **통과마다 저장**: 길이, 비용(초), 속도, 진입→출구 방향 변화(`turn_deg`, 왼쪽 +), 폴리라인(도로: 그 방향 주행 차로들의 중심선, prefab: 가장 짧은 곡선 경로; `--sdmap-step-m` 간격, 기본 10 m), 도로의 진행 방향 차로 수와 왼쪽부터 순위별 차로 양 끝점, prefab의 진입 곡선(시작점·왼쪽 오프셋·출구 도달 여부)과 이 출구로 가는 모든 경로의 곡선 번호. 회사(company·city 토큰, depot prefab, 위치) 807개와 도시 구역 463개도 넣습니다.
- **속도·비용**: 주행 차로의 traffic lane 종류로 속도를 정합니다(motorway 85, expressway 80, local 65, slow_road 등 55, no_vehicles 30 km/h; 도로 가운데가 지도의 도시 구역(CityArea 사각형, node에서 폭·높이만큼) 안이면 최대 45 km/h). prefab은 붙은 도로 중 빠른 쪽 속도이고, 꺾이면 곡선 길이와 방향 변화로 구한 반경에서 횡가속 1.5 m/s²로 제한합니다. 벌점은 우측통행 기준 좌회전(45° 초과) 8 s, 우회전 3 s, U턴(150° 초과) 60 s, 신호등이 있는 곡선 10 s입니다(좌측통행 prefab은 좌우를 바꿈). 게임 GPS가 피하는 도로(`GpsAvoid`)는 시간 3배입니다. 이 값들은 손으로 정했고 게임 GPS 비용과 맞춘 것이 아닙니다.
- **형식**: 이진 파일 하나입니다. 머리(형식 번호, 키, 간격) 뒤에 노드·item·통과·폴리라인 점·차로 끝점·진입 곡선·곡선 번호를 고정 크기 구조체 배열 그대로(리틀 엔디언) 쓰고, 회사·도시 표를 붙입니다. 읽기는 블록 복사뿐이라 65 MB를 55–73 ms에 읽습니다. 같은 내용을 JSON으로 쓰면 몇 배 크고 파싱에 수 초가 걸려서 이진으로 했습니다. 압축은 하지 않았습니다(읽기 시간이 늘어남).
- **키·재생성**: 캐시 키는 형식 번호, 폴리라인 간격, 게임 실행 파일 버전, `def.scs`·`base_map.scs`와 지도·월드 정의를 가진 `dlc_*.scs`의 이름·크기·수정 시각입니다. 게임 업데이트나 지도 DLC 설치·제거로 키가 바뀌면 다음 실행에서 자동으로 다시 만듭니다(`--sdmap-build`는 강제). 기본 위치는 빌드 폴더의 `sdmap-cache.bin`(`--sdmap-cache`로 변경)입니다. 키 계산은 DLC archive를 열어 보므로 0.25–0.55 s 걸립니다.
- ✔ 측정(2026-10-22, 1.61.1.9, 지도 DLC 3개): 486 sector, node 156,940, item 134,518(도로 102,871, prefab 31,647), 통과 278,785. 도시 구역 안 도로 13,175개. 10 m 간격: 만들기 72 s(지도 열기 32 s 포함; 다른 재생과 함께 돌 때 87–97 s), 메모리 최대 작업 집합 2.0 GB(프로세스 측정; 처음 빌드 중 tasklist로는 4.0 GB를 봄), 파일 65.5 MB(점 2,664,378), 읽기 55–73 ms. 25 m 간격: 만들기 78 s, 51.2 MB(점 1,473,112), 읽기 60 ms.
- **차로 없는 도로**(형식 4, 2026-10-22): AI 차로가 하나도 없는 look(`lanes_left`·`lanes_right` 둘 다 빈 것: depot·부지 진입로, 비포장길; 10종, 도로 5,383개)은 기준선 위 양방향 20 km/h 통과로 넣습니다(`NoLanes`, 차로 수 0, `lanes_ok` 없음). 철도·트램 선로뿐인 look은 넣지 않습니다. walker는 이런 도로를 따라가지 못합니다(차로 모델 없음).
- **연결성**(✔ `--route-selftest`, 형식 4): 가장 큰 강연결 성분(주 연결망)이 방향별 도로 길이 17,297 km의 40.8%입니다. 주 연결망에서 들어가기만 하는 막다른 부분 10.8%, 나오기만 하는 부분 10.3%, 어느 쪽도 아닌 고립 도로 38.2%입니다. 고립 도로의 80%는 UI 지도에 보이지 않는 도로(전체의 52.8%)로, 풍경용 도로·연결 없는 도로 조각입니다. 두 번째 성분(4.0%)은 영국입니다. 막다른 길에서 되돌아오는(U턴) 통과는 없습니다.
- **본토는 한 연결망**(✔ 2026-10-22 진단): 도시 구역 중심 1.5 km 안의 보이는 도로로 보면 도시 182곳 중 133곳이 주 연결망이고, 밖의 49곳은 페리 너머(영국, 코르시카, 사르데냐)와 설치하지 않은 지도 DLC 지역(서발칸, 러시아·발트, 핀란드 동부, 이탈리아 DLC의 제노바·파르마, 루마니아)뿐입니다. 이 지역의 도로 조각은 기본 지도에 있지만 이어지는 item이 DLC에만 있어(node가 가리키는 item이 설치된 지도 archive 어디에도 없음, TruckLib `UnresolvedItem`) 수백 개의 작은 성분으로 쪼개집니다. 이 지역 고속도로 661 km가 주 연결망 밖 성분의 대부분입니다.

### 경로 탐색

- **A\***: 통과를 정점으로 하는 A\*입니다. 휴리스틱은 출구 node에서 목적지까지 직선거리를 가장 빠른 속도(25 m/s)로 나눈 값이라 과대평가하지 않아, 결과는 Dijkstra와 같습니다(✔ 자체 점검 40쌍에서 비용 일치). ALT·contraction hierarchy는 쓰지 않았습니다. 가장 긴 경로도 약 50 ms 이하라 한 번 계획에 전처리·메모리를 더 쓸 이유가 없었습니다.
- **시작**: `--from x,y,heading_deg`(ROS world, heading은 +x에서 반시계 도) 또는 `--snapshot`(파일·`-`)이나 `ot` 파이프의 트럭 placement입니다. 시작점 15 m 안에서 진행 방향 45° 안의 통과를 모두 출발 후보로 두고(없으면 40 m·60°), 각 후보는 남은 구간 시간에 벗어난 거리 1 m당 2 s를 더해 시작합니다. 교차로 안에서도 그 prefab 통과에서 시작합니다.
- **목적지**: `--to x,y`(가까운 도로 8개, 50 m → 500 m → 3 km로 넓혀 찾음), `--job company,city`, 또는 작업 목적지입니다. 작업 목적지는 SDK job 구성의 `destination.company.id`·`destination.city.id`이고, `--snapshot` 파일에 `"configs": {"job": ...}`(ot 파이프 `configs` 명령의 모양)가 있으면 그것을, 실행 중이면 파이프의 `configs`를 읽습니다(snapshot에는 job이 없음). 회사는 지도의 Company item(같은 회사·도시의 depot 전부)으로 찾고, 그 depot prefab으로 들어가는 통과의 끝에 닿으면 도착입니다. 들어가는 통과가 없는 depot(AI가 들어가지 않는 부지, 도로에 이어지지 않은 표지용 회사 prefab)은 depot 300 m 안의 도로 64개가 목적지이고, 거기에 닿는 경로가 없으면 1 km·2 km 안 도로 512개까지 넓혀 다시 찾습니다. 가까운 도로가 아무 데도 이어지지 않은 부지 길 조각인 경우가 많아서입니다(가까운 8개만 쓰던 처음 구현에서 본토 depot 45곳이 닿지 않았습니다). 회사 없이 도시만 있으면 도시 구역 중심 근처 도로입니다(△ 확인 안 함).
- ✔ 탐색 시간(`--route-bench`, 주 연결망 위 무작위 쌍 구간별 30개, 모두 경로 있음). 월드 직선거리 기준이며, 게임 지도 축척으로는 도시 밖 약 19배입니다.

| 직선거리(월드) | 경로 길이 중앙값(최대) | 탐색 중앙값 / p95 / 최대 | 펼친 정점 중앙값 |
|---|---|---|---|
| 4–6 km | 11.0 km (37.9) | 1.3 / 10.8 / 12.1 ms | 1,306 |
| 40–60 km | 80.6 km (131.8) | 13.0 / 30.4 / 51.0 ms | 44,089 |
| 80 km 이상(게임 축척 약 1,500 km 이상) | 130.7 km (159.0) | 24.0 / 31.1 / 33.7 ms | 103,410 |

### 경로 파일(`<out>.route.json`)

`--plan-route --out <파일>`은 `<파일 이름>.route.json`을 씁니다(`--out`이 이미 `.route.json`으로 끝나면 그대로). follow의 `--game-route`도 `<out>.route.json`을 쓰므로, 같은 `--out`을 주면 게임 경로를 덮어씁니다. 게임 GPS 경로(A)와 SD 지도 경로(B)가 같은 형식을 씁니다.

```json
{"frame_id": "world", "source": "sdmap" | "game", "revision": 1, "created": "ISO 시각",
 "start": [x, y], "goal": [x, y], "length_m": 10686.3, "eta_s": 734.1,
 "items": [{"kind": "road" | "prefab" | "gap", "uid": "16진", "entry_node": "16진", "exit_node": "16진",
            "exit_curves": [0, 2], "lanes_ok": [1], "s_start_m": 0, "length_m": 64.3,
            "lanes": 2, "entry_curves": [3], "turn_deg": -90.0, "traffic_light": true, "curveless": true}, ...],
 "polyline": [[x, y, z], ...]}
```

| 필드 | 뜻 |
|---|---|
| `source` | `sdmap`(이 탐색), `game`(게임 GPS, 위 절) |
| `revision` | 파일이 바뀔 때마다 1 증가(B는 한 번 쓰므로 1) |
| `start`, `goal` | 요청한 출발·목적지(ROS world 평면) |
| `length_m`, `eta_s` | 폴리라인 길이, 비용 모델의 예상 시간(벌점 포함; A는 `null`) |
| `items[].kind` | `road`, `prefab`, A의 `gap`(페리·열차, `uid` null) |
| `uid`, `entry_node`, `exit_node` | 지도 item과 들어가는·나오는 node(16진 문자열; B는 앞 0 없이, A는 16자리). walker는 prefab의 이 셋만 씁니다 |
| `exit_curves` | prefab: 진입 곡선에서 이 출구로 가는 모든 경로의 내비 곡선 번호(ppd 순서). 도로는 빈 배열 |
| `lanes_ok` | 도로: 진행 방향 차로 중 왼쪽부터 센 순위(0 = 가장 왼쪽)로, 차로 변경 없이 경로를 이어 갈 수 있는 차로. 다음 item이 곡선 있는 prefab이면 walker 규칙(가장 가까운 진입 곡선, 출구에 닿는 곡선 우선, 4.6 m 이내)으로 고른 진입 곡선이 경로 출구에 닿는 차로, 다음이 도로면 그 도로의 `lanes_ok`로 이어지는 차로(0.5 m 이내, 아니면 같은 순위 6 m 이내), 그 밖(목적지, 곡선 없는 prefab, gap)은 모든 차로. prefab: 경로 출구에 닿는 진입 곡선의 왼쪽부터 순위(1 m 안 곡선은 한 차로) |
| `s_start_m`, `length_m` | 경로 시작부터 그 item까지, 그 item 안에서 경로가 지나는 길이(첫·끝 item은 일부) |
| `lanes` | 도로: 진행 방향 차로 수(B·보강만) |
| `entry_curves` | prefab: 경로 출구에 닿는 진입 곡선 번호(B·보강만) |
| `turn_deg`, `traffic_light`, `curveless` | prefab: 진입→출구 방향 변화(왼쪽 +), 신호등 곡선 여부, 곡선 없는 prefab(B만) |
| `forward`, `game_distance_m`, `game_time_s`, `unavailable` | A만(위 절). 읽는 쪽은 모르는 필드를 무시합니다 |
| `enriched` | `--enrich-route`가 채운 파일이면 `"sdmap"` |
| `polyline` | ROS world `[x, y, z]`. B: 도로는 진행 방향 차로들의 중심선, prefab은 곡선(차로 중심에서 최대 차로 폭 절반 남짓 떨어짐), 간격은 캐시 간격. A: node 위치 |

`--enrich-route <파일> [--out <파일>]`은 어느 출처의 경로 파일이든 SD 지도로 `exit_curves`·`entry_curves`·`lanes_ok`·`lanes`와 지도 형상 폴리라인을 채웁니다(지도에 없는 item·`gap`은 그대로, 나머지 필드 유지). `--out`이 없으면 그 파일을 고쳐 씁니다. ✔ 실게임 경로 파일(175 item)에서 174개를 찾아 36 ms에 채웠습니다. 못 찾은 1개는 트럭 내비 곡선으로는 진입 node에서 GPS 출구로 가는 경로가 없는 prefab입니다(아래).

### `--route`(walker 출구 선택)

- follow와 한 번 실행에 `--route <경로 파일>`을 주면, walker가 prefab에 들어갈 때 경로 파일에서 그 prefab과 진입 node(prefab 안에서 시작하면 prefab만)로 경로의 출구 node를 찾습니다. 지금 차로의 진입 곡선이 그 출구에 닿으면 그 출구를 따라갑니다(같은 출구로 가는 길이 여럿이면 차로 순위 유지, 짧은 길 순). 기본 규칙(본선·분기·가장 작은 꺾임)으로 골랐을 출구와 다르면 `route_turn` event를 냅니다.
- 지금 차로의 곡선이 경로 출구에 닿지 않으면 기본 규칙의 출구를 그대로 따라가고 `route_lane_mismatch` event를 냅니다. event에는 그 출구에 닿는 진입 차로(`lanes_ok`, 왼쪽부터 순위)와 지금 차로 순위(`lane`)가 있어, 판단 계층이 그 전에 차로 변경을 계획할 수 있습니다. walker는 차로 변경을 하지 않습니다. 그 뒤 경로를 벗어난 prefab에서는 경로 항목이 없으므로 기본 규칙입니다(경로로 돌아오면 다시 따릅니다).
- event는 차로 파일 `events`에 `{"type": "route_turn" | "route_lane_mismatch", "s": 누적 거리, "prefab": "16진", "exit_node": "16진", "lanes_ok": [..], "lane": n}`로 신호등 event와 함께 s 순서로 들어가고(경로 event가 없으면 `events`는 예전과 같음), follow 상태 줄(과 한 번 실행 요약)에는 `route_revision`, `route_taken`(이 경로에서 경로 출구를 고른 prefab 수), `route_events`(지금 파일에 남은 경로 event)가 붙습니다. 옆 차로(`.left/.right`)도 같은 경로를 따릅니다.
- 경로 파일은 최대 1초에 한 번 수정 시각을 보고 바뀌었으면 다시 읽습니다(이미 낸 점은 그대로, 앞으로 지날 prefab부터 적용). 처음에 파일이 없어도 시작하고 생기면 읽습니다. `unavailable`만 있는 파일은 경로 없음으로, 읽기 오류는 직전 경로 유지로 처리합니다. 그래서 follow에 `--game-route --route <out>.route.json`을 함께 주면 게임 GPS 경로를 따라갑니다(△ 런처에는 아직 넣지 않음).
- `--route`가 없으면 출력은 예전과 바이트 단위로 같습니다(아래 검증).

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `--route` | — | follow·한 번 실행: 이 경로 파일의 출구를 prefab에서 고름 |
| `--plan-route` | — | 경로를 계획해 `--out` 옆 `.route.json`을 쓰고 끝냄(값 없음). 시작 `--from x,y,heading_deg` 또는 `--snapshot`/파이프, 목적지 `--to x,y`·`--job company,city`·작업 목적지 |
| `--enrich-route` | — | 경로 파일을 SD 지도로 채움(위) |
| `--sdmap-cache` | 빌드 폴더 `sdmap-cache.bin` | 캐시 파일 |
| `--sdmap-step-m` | 10 | 폴리라인 간격(0.5–100 m, 키에 포함) |
| `--sdmap-build` | — | 캐시를 강제로 다시 만듦(값 없음) |
| `--route-selftest` | — | 캐시·탐색 자체 점검(값 없음, 실패 시 종료 코드 1) |
| `--route-bench <n>` | — | 거리 구간별 n개 무작위 경로 탐색 시간 |
| `--route-compare <포즈 jsonl>` | — | 기록 주행의 첫·끝 포즈로 경로를 계획하고 주행 위치와 비교(`--out`에 경로 저장) |

### 검증(2026-10-22)

증거와 도구는 세션 scratchpad(`route/`: `replay.py`·`diff_runs.py`·`follow_check.py`·`route_walk_check.py`·`synth_drive.py`)에 있습니다.

- ✔ **자체 점검**(`--route-selftest`, 11항목 통과): 캐시 왕복(쓰기→읽기 배열 동일), 통과 278,785개의 node·비용(길이/최고 속도 이상)·폴리라인, 도로 폴리라인 끝이 node에서 21.8 m 이내, 막다른 통과 18,751개(6.7%), 주 연결망 하나(42.7% 대 다음 4.2%), A\* = Dijkstra(40쌍), 경로 item이 공유 node로 이어짐, 폴리라인 점 간격 최대 13.5 m, prefab 직전 도로 319개 모두 `lanes_ok` 있음, 회사 depot 807개 중 651개 도달(진입 통과 504, 주변 도로 147; 나머지 156개는 모두 2 km 안에 주 연결망에서 닿는 도로가 없는 페리 너머·설치하지 않은 DLC 지역. 처음 구현은 606개: 본토 depot 45곳을 놓침), walker가 경로 파일에서 prefab 134개의 출구를 모두 읽음.
- ✔ **기록 주행과 비교**(`--route-compare`, 첫 포즈 → 끝 포즈): 독일 일반도로 12.99 km(poses_f)는 주행 위치 100%가 경로 10 m 안(중앙값 0.02 m, 탐색 8 ms, 예상 629 s·실제 520 s), 고속도로 1.87 km(bag2) 100%(중앙값 2.25 m, 예상 110 s·실제 82 s), 일반도로 4.33 km(lcc_bag) 100%(2.25 m, 예상 221 s·실제 224 s). 2.25 m는 2차로 중심선과 주행 차로의 차이입니다. `bridge/recordings`의 다른 기록 34개는 지도 차로가 없는 시험 구역이라(provider도 "No drivable lane") 비교할 수 없었습니다.
- ✔ **같은 출발·도착의 게임 GPS 경로와 비교**(코르시카 9.7 km): SD 경로 9.97 km, 게임 경로의 87%가 SD 경로 10 m 안, 게임 경로 prefab 41개 중 32개가 같음. 나머지는 비용 모델 차이로 다른 길을 고른 구간입니다.
- ✔ **walker `--route`, 기록 주행 재생**(`--snapshot -`): 세 기록 모두 경로가 기본 규칙과 같은 출구라 차로 파일이 `--route` 없을 때와 같았고(경로 출구 선택 42·9·8번, mismatch 0, reset 수 같음), 운전자가 다르게 꺾은 교차로가 기록에 없어 경로가 바꾼 출구는 아래로 확인했습니다.
- ✔ **walker `--route`, 실게임 경로 파일**(`source: game`, 1.61.1.9에서 읽은 175 item): 한 번 실행에서 경로 9.72 km 중 9.08 km를 따라갔습니다(차로 점의 93.6%가 경로 10 m 안, 경로 출구 선택 34번, `route_turn` 3번). `--route` 없이는 501 m 뒤 막다른 길(`road_end`)로 갔습니다(39%). 9,066 m의 prefab(`2f99e81849c71c9d`, 목적지 근처)은 트럭 내비 곡선으로 진입 node에서 GPS 출구로 가지 못해 `route_lane_mismatch`(`lanes_ok` [])를 내고 기본 출구로 갔습니다.
- ✔ **walker `--route`, SD 경로**(10.70 km, prefab 48개, 탐색 6.7 ms): 차로 점의 91.6%가 경로 10 m 안이었고(출구 선택 41번, `route_turn` 4번), `route_lane_mismatch`가 두 번 나왔습니다. 10,220 m는 우회전 출구가 오른쪽 차로에서만 닿는 곳(`lanes_ok` [1], `lane` 0)으로 계획 파일의 직전 도로 `lanes_ok`도 [1]이었습니다. 7,957 m는 차로가 늘어나는 prefab 바로 뒤의 분기 prefab(`2f99e818068740a7`, `lanes_ok` [2])이라 계획 파일의 도로 `lanes_ok`에는 나타나지 않았습니다(아래 한계). walker는 거기서 기본 출구로 갔다가 경로로 돌아왔습니다.
- ✔ **follow 가상 주행**(위 두 경로의 차로를 주기당 12.5 m로 재생): 차로 재시작(reset)이 게임 경로 4 → 1번, SD 경로 5 → 1번(처음 시작만)으로 줄었고(SD 경로는 mismatch 2번 포함), 앞 300 m 차로가 실제 지나간 길과 맞은 비율이 89.9 → 96.1%, 94.6 → 96.0%였습니다.
- ✔ **`--route` 없을 때 동일성**(main 4dca51c 빌드와 비교): poses_f 1,040주기(revision 33), bag2 161주기(6), lcc_bag 440주기(12, 실시간 속도)에서 차로 파일이 모든 revision에서 바이트 단위로 같았고, 상태 줄도 시간 필드를 빼고 같았습니다. lcc_bag를 대기 없이 재생하면 `path_id`만 달라지는데, main 빌드끼리도 똑같이 달라집니다(옆 차로 재시도가 2 s 벽시계 기준).
- ✔ **작업 목적지**: snapshot의 `configs.job`(itcc, ostrava)으로 47.6 km(24 ms), `--job eco,marseille`로 67.5 km(40 ms) 경로를 냈습니다. △ 실제 게임의 job 구성 읽기(파이프 `configs`)는 job이 있는 상태에서 해 보지 않았습니다.

### 한계

- 페리·열차를 경로에 넣지 않습니다. 영국·코르시카·사르데냐와 본토 사이는 "No route"입니다(게임 경로 파일은 `gap`으로 표시). 설치하지 않은 지도 DLC 지역도 없습니다.
- 비용 모델(차로 종류별 속도, 벌점)은 손으로 정했습니다. 게임 GPS와 87% 겹쳤지만 다른 길을 고를 수 있고, 예상 시간은 맞지 않을 수 있습니다(도시 구역을 지나는 고속도로 110 s 대 실제 82 s, 일반도로 221 s 대 224 s).
- `lanes_ok`는 다음 경로 prefab 하나까지만 봅니다. prefab이 이어진 곳(차로가 늘어나는 조각 바로 뒤의 분기 등)에서는 뒤 prefab이 요구하는 차로가 앞 도로의 `lanes_ok`에 나타나지 않습니다. 캐시에 진입 곡선별 경로 끝점이 없어서이고, 그때는 walker의 `route_lane_mismatch`(follow에서 약 0.6–1 km 앞)가 알립니다. 여러 교차로에 걸친 차로 계획, 차로 변경 위치·시점은 판단 계층 몫입니다.
- 곡선 없는 prefab은 가장 곧은 node로만 건넙니다(walker와 같음). 그런 prefab에서 꺾는 경로는 없습니다.
- walker는 경로의 (prefab, 진입 node)로만 경로를 압니다. 경로를 벗어나도 따로 알리지 않고(경로 event가 더 나오지 않음), 경로 위 도로에서의 진행 위치는 소비자가 `polyline`에 투영해야 합니다.
- 시작점은 캐시 폴리라인(차로들의 중심선)에 15 m·45°로 붙입니다. 옆 평행 도로가 가까우면 다른 도로에서 시작할 수 있습니다(1 m당 2 s 벌점으로 덜 고름).
- 도시만 있는 목적지, 실게임 job 구성 읽기, 실게임 주행 중 `--route`는 확인하지 않았습니다.

## 한계·미검증

- 실게임 주행으로 확인한 곳은 프랑스 고속도로뿐입니다. 일반도로·교차로·DLC 지역은 오프라인 snapshot으로만 확인했습니다.
- `--route`가 없으면 내비게이션 경로를 모릅니다. 교차로에서는 본선, 없으면 방향 변화가 가장 작은 쪽을 따라갑니다. 운전자가 다른 방향으로 꺾으면 차로에서 멀어진 뒤(약 3.4m) 새 경로를 만듭니다. `--route`의 한계는 "SD 지도 경로"에 있습니다.
- 신호등은 위치만 `events`(와 follow의 scene 파일)로 알립니다. 신호 상태와 정지는 제어기 몫입니다.
- 교차로 회전 곡선은 반경 5–10m입니다. 트럭이 그대로 따라갈 수 없는 곳이 있습니다.
- 좌측통행(영국) look은 감사로만 확인했고, 좌측통행 지역의 왼쪽부터 센 차로 순서 규칙은 검증하지 않았습니다.
- look 전환의 단차 흡수와 prefab 합류는 지도에 실제 형상이 없어 만든 S자입니다. 게임 안 차로 표시와 다를 수 있습니다.
- 내비 곡선이 없는 3노드 이상 prefab(지도에서 Road에 붙은 335개)은 가장 곧은 Node로 건넙니다. 실제 형상은 확인하지 않았습니다.
- 이 provider는 GPL-2.0 구성요소(TruckLib)와 연결된 별도 프로세스로 유지합니다.

## 참고 소스·라이선스

- [TruckLib](https://github.com/sk-zk/TruckLib/tree/c42dbe4f2d6b): 지도 format 907(게임 1.59~1.61), Road Hermite 곡선, HashFS/모델/SII/PPD 의존성. 기존 소스를 직접 참조하며 수정하지 않습니다.
- [ETS2LA](https://github.com/ETS2LA/ETS2LA/tree/cf0fc395b0d0): `ETS2LA.Game/Data/Classes.cs`의 lane curve·prefab 좌표 변환·내비 곡선 탐색, `Data/Utils.cs`의 roadlook·내비 곡선 Hermite 해석을 참고했습니다. 4.5m 차로 격자는 지도 전체 감사로 다시 확인했습니다. 양방향 비대칭 look의 중심 이동 같은 ETS2LA 규칙은 감사와 맞지 않아 쓰지 않습니다.

TruckLib와 연결된 TruckLib.HashFs/Models/Sii는 GPL-2.0 라이선스 구성요소입니다. provider의 해당 라이선스 사본은 [LICENSE](LICENSE)에 보존합니다. 배포할 때 이 CLI와 연결된 라이브러리의 라이선스 및 해당 소스를 함께 보존해야 합니다. 다른 제품 프로세스에는 JSON 파일로 연결합니다.
