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

내비게이션 목적지 경로는 쓰지 않습니다. SDK telemetry에는 남은 거리·시간·제한속도만 있고 경로가 없습니다. 같은 Road나 prefab을 같은 방향으로 다시 지나면 `loop`로 끝냅니다. 반대 방향 재방문(막다른 길에서 되돌아옴)은 허용합니다.

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
| `--audit-looks` | — | 지도 전체 sector를 읽고 road look별 차로 모델을 prefab 내비 곡선 끝점과 비교한 CSV를 쓰고 끝냄 |

지도는 현재 위치 주변 3×3 sector(4 km)만 먼저 읽고, 기준선이 아직 읽지 않은 sector의 item에 닿으면 그 주변 sector를 더하고 멀어진 sector를 빼서 지도를 다시 엽니다(약 1–3초). archive 확인과 road look 읽기는 시작할 때 약 2.3초입니다. prefab 서술자 경로표는 처음 prefab에 닿을 때 SII에서 읽고(약 5초) 빌드 폴더의 `prefab-desc-cache.json`에 저장합니다. `def.scs`나 정의를 가진 DLC archive의 크기·시각이 바뀌면 다시 만듭니다.

## 출력과 follow 프로토콜

JSON은 `frame_id: "world"`, `lane_width_m`, `points`, `end_reason`, `revision`, `path_id`, `start_s`, `events`입니다. 점 좌표는 기존 ROS world와 같은 `[SCS X, -SCS Z]` 미터입니다. 약 1.5m 간격으로 실제 차로 곡선을 샘플링하며(조각 접합부의 0.5m 미만 중복점은 제거), 현재 위치의 차로 투영점에서 진행 방향으로 시작합니다. `start_s`는 `points[0]`의 경로 시작점 기준 누적 거리입니다. 한 번 실행은 `revision`·`path_id` 1, `start_s` 0입니다.

`events`(추가 필드)는 `[{"type": "traffic_light" | "toll_gate", "s": 경로 누적 거리}]`입니다. `s`는 `start_s`와 같은 기준이며, 해당 prefab에서 신호가 걸린 첫 곡선의 시작점(정지선 부근)입니다. follow에서는 버린 점보다 앞의 event를 함께 버립니다. 기존 소비자는 이 필드를 무시해도 됩니다.

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

## 한계·미검증

- 실게임 주행으로 확인한 곳은 프랑스 고속도로뿐입니다. 일반도로·교차로·DLC 지역은 오프라인 snapshot으로만 확인했습니다.
- 내비게이션 경로를 모릅니다. 교차로에서는 본선, 없으면 방향 변화가 가장 작은 쪽을 따라갑니다. 운전자가 다른 방향으로 꺾으면 차로에서 멀어진 뒤(약 3.4m) 새 경로를 만듭니다.
- 신호등은 위치만 `events`로 알립니다. 신호 상태와 정지는 제어기 몫입니다.
- 교차로 회전 곡선은 반경 5–10m입니다. 트럭이 그대로 따라갈 수 없는 곳이 있습니다.
- 좌측통행(영국) look은 감사로만 확인했고, 좌측통행 지역의 왼쪽부터 센 차로 순서 규칙은 검증하지 않았습니다.
- look 전환의 단차 흡수와 prefab 합류는 지도에 실제 형상이 없어 만든 S자입니다. 게임 안 차로 표시와 다를 수 있습니다.
- 내비 곡선이 없는 3노드 이상 prefab(지도에서 Road에 붙은 335개)은 가장 곧은 Node로 건넙니다. 실제 형상은 확인하지 않았습니다.
- 이 provider는 GPL-2.0 구성요소(TruckLib)와 연결된 별도 프로세스로 유지합니다.

## 참고 소스·라이선스

- [TruckLib](https://github.com/sk-zk/TruckLib/tree/c42dbe4f2d6b): 지도 format 907(게임 1.59~1.61), Road Hermite 곡선, HashFS/모델/SII/PPD 의존성. 기존 소스를 직접 참조하며 수정하지 않습니다.
- [ETS2LA](https://github.com/ETS2LA/ETS2LA/tree/cf0fc395b0d0): `ETS2LA.Game/Data/Classes.cs`의 lane curve·prefab 좌표 변환·내비 곡선 탐색, `Data/Utils.cs`의 roadlook·내비 곡선 Hermite 해석을 참고했습니다. 4.5m 차로 격자는 지도 전체 감사로 다시 확인했습니다. 양방향 비대칭 look의 중심 이동 같은 ETS2LA 규칙은 감사와 맞지 않아 쓰지 않습니다.

TruckLib와 연결된 TruckLib.HashFs/Models/Sii는 GPL-2.0 라이선스 구성요소입니다. provider의 해당 라이선스 사본은 [LICENSE](LICENSE)에 보존합니다. 배포할 때 이 CLI와 연결된 라이브러리의 라이선스 및 해당 소스를 함께 보존해야 합니다. 다른 제품 프로세스에는 JSON 파일로 연결합니다.
