# 지도 차로 기준선

설치 게임의 실제 지도 Road 곡선·road template 노면과 prefab 내비 곡선(`.ppd`)을 읽어, 현재 차량이 있는 차로의 주행 기준선을 생성합니다. 기본 실행은 `ot` 파이프에서 SDK `truck.world.placement`만 읽습니다. 게임 입력·Tier·설정은 변경하지 않습니다.

한 번 실행하면 현재 위치부터 `--distance-m`까지 한 파일을 만들고 끝납니다. `--follow`는 상주하면서 차량 앞에 항상 `--distance-m` 이상이 남도록 같은 파일을 이어 씁니다(장거리 고속도로용).

## 지원 도로와 차로 폭

- **단방향 template**(`ger16` 3차로, `ger13` 2차로, `ger17`/`ger18`/`ger19` 등): 실제 모델 `vis` 노면 X 범위와 좌우 shoulder로 차로 폭을 계산합니다. 예: `ger16`은 노면 -9~8m, shoulder 0/3.5m → `(17-3.5)/3` = 4.5m, 차로 중심 2.25/-2.25/-6.75m. 고정 차로폭을 가정하지 않습니다.
- **중앙분리 양방향 template**(한 Road에 양쪽 차도가 있는 `ger11` 2+2, `ger14` 3+3, `ger12`/`ger15` 등): 각 쪽 template이 중앙 끝(x=0)에서 바깥 shoulder까지이고 `road_offset`만큼 바깥으로 이동한다고 봅니다. 오른쪽 차도는 Road 방향, 왼쪽 차도(대칭)는 반대 방향 교통입니다. 예: `ger14` 중심 ±4.25/±8.75/±13.25m. 이 해석은 prefab 내비 곡선과의 접합 오차로 확인했습니다(아래 검증, 최대 0.038m). 중앙 끝에서 시작하지 않는 template이나 `lane_offsets_*`가 있는 look(`un22_*` 등)은 근거가 없어 지원하지 않습니다.
- 차량 진행방향과 같은 방향 교통을 가진 차로만 고릅니다. 단방향 Road를 역방향으로 따라가지 않습니다.

## 연결 규칙

**Road→Road.** 공유 Node에서 다음 Road의 같은 진행방향 차로 중, 지금 차로 끝과 0.5m·15° 이내로 이어지는 차로를 따라갑니다. 옆 차로가 생기거나 없어져도 이 차로가 이어지면 계속하고, 이어지는 차로가 없으면 `lane_ends`입니다. 차로 변경은 하지 않습니다.

**prefab 통과.** `def/world/prefab*.sii`의 `prefab_desc`로 `.ppd`를 열고(TruckLib.Models), origin Node 기준으로 prefab 좌표를 world로 옮깁니다. 진입 Node의 `InputLines` 중 시작점이 차로 끝과 0.5m·15° 이내인 곡선에서 출발해 `NextLines`로 다른 Node의 `OutputLines`에 닿는 모든 경로를 구합니다. 소형차 전용 곡선은 제외합니다.

1. **본선(through) 출구**: 진입 Node에서 닿는 출구 중 ① 그 Node를 떠나는 차로 수(합류 차로 포함)가 가장 많고 ② 직전 Road와 같은 road look이고 ③ 진행방향 변화가 가장 작은 출구입니다. 램프(1차로, 약 15–17°)·분기 도로는 여기서 빠집니다.
2. 최선 출구의 방향 변화가 30°를 넘거나, 다차로 진입의 유일한 계속이 1차로면 본선이 없다고 보고 `prefab_no_through`로 끝냅니다.
3. **같은 조건의 출구가 둘 이상(2° 이내)인 진짜 분기**는 이 차로가 결정합니다. 이 차로가 닿는 출구가 하나면 그쪽, 여럿이면 왼쪽(중앙분리대 쪽)부터 센 차로 순서를 유지하는 쪽입니다. 그래도 같으면 `prefab_ambiguous`입니다.
4. 이 차로가 본선 출구에 닿지 않으면(출구 전용 차로) `lane_leaves_through`입니다. prefab 안에서 이 차로가 갈라지면 왼쪽부터의 순서를 유지하는 가지를 고릅니다. 그래서 오른쪽에 붙는 보조차로로 옮겨 가지 않습니다.
5. 고른 경로에 차단기 신호가 있으면 `toll_gate`, 실제 신호등이면 `traffic_light`, 페리 연결 prefab이면 `ferry`로 끝냅니다.

이 차로 자체가 끝나는 합류 차로(연결로·보조차로)는 prefab 곡선이 옆 차로로 합류시키며, 이때 기준선은 짧은 S자(반경 약 90–130m, 30m 이내)가 됩니다.

## end_reason

| 값 | 의미 |
|---|---|
| `distance_limit` | 지정 거리에 도달(follow에서는 계속 연장 가능) |
| `road_end` | 다음 item이 없음(지도 데이터의 차도 끝) |
| `lane_ends` | 다음 Road에 이 차로가 이어지는 차로가 없음(중앙분리대 폭이 바뀌는 look 전환 포함) |
| `direction_change` | 다음 Road에 이 진행방향 차로가 없음 |
| `unsupported_road` | 지원하지 않는 road look(일반 양방향 도로 등) |
| `prefab_no_through` / `prefab_ambiguous` / `lane_leaves_through` | 위 prefab 규칙 2·3·4 |
| `prefab_unsupported` / `prefab_no_lane` / `prefab_lane_mismatch` / `prefab_no_exit` | 서술자 없음·노드 불일치 / 진입 곡선 없음 / 차로 끝과 진입 곡선 불일치 / 출구 없음 |
| `toll_gate` / `traffic_light` / `ferry` | 차단기·신호등·페리 |
| `loop` / `ambiguous` / `unsupported_item` / `map_unresolved` | 같은 item 재방문 / Node 연결 이상 / Road·prefab이 아닌 item / sector를 읽어도 item이 없음(DLC 지도 등) |

제어기는 모든 끝점에서 감속해야 합니다. 내비게이션 목적지 경로, 교차로 통과, 차로 변경, 교통 대응은 제공하지 않습니다.

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
```

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

지도는 현재 위치 주변 3×3 sector(4 km)만 먼저 읽고, 기준선이 아직 읽지 않은 sector의 item에 닿으면 그 주변 sector를 더하고 멀어진 sector를 빼서 지도를 다시 엽니다(약 1.3–3초). 전체 지도나 게임 메시를 추출하지 않습니다. prefab 서술자 경로표는 처음 prefab에 닿을 때 SII에서 읽고(약 5초) 빌드 폴더의 `prefab-desc-cache.json`에 저장합니다. 설치된 `def.scs`의 크기·시각이 바뀌면 다시 만듭니다. 캐시가 있을 때 1 km 실행은 약 3.9초, 30 km는 약 9.6초입니다.

## 출력과 follow 프로토콜

JSON은 `frame_id: "world"`, `lane_width_m`, `points`, `end_reason`, `revision`, `path_id`, `start_s`입니다. 점 좌표는 기존 ROS world와 같은 `[SCS X, -SCS Z]` 미터입니다. 약 1.5m 간격으로 실제 차로 곡선을 샘플링하며(조각 접합부의 0.5m 미만 중복점은 제거), 현재 위치의 차로 투영점에서 진행 방향으로 시작합니다. `start_s`는 `points[0]`의 경로 시작점 기준 누적 거리입니다. 한 번 실행은 `revision`·`path_id` 1, `start_s` 0입니다.

follow는 매 주기 placement를 기준선에 투영합니다.

- 차로 중심에서 `0.75 × lane_width_m`보다 멀어지면(차로 변경, 다른 도로, 끝 통과) 그 위치에서 새 경로를 만듭니다. 이때 `path_id`가 1 늘고, 상태 줄의 `reset`은 `true`입니다. prefab 안에서는 새 경로를 시작할 수 없어 도로에 나올 때까지 오류 상태로 재시도합니다.
- 계속 가능한 경로(`distance_limit`)에서 앞 남은 거리가 `--extend-below-m`보다 작으면 차량 앞 `--distance-m`까지 **뒤에 덧붙이고**, 차량 뒤 `--keep-behind-m`보다 오래된 점을 버립니다. 이미 낸 점은 바뀌지 않습니다.
- 바뀔 때마다 `revision`을 1 늘리고 같은 디렉터리의 임시 파일을 완성한 뒤 교체합니다. 소비자가 파일을 열고 있어 교체가 실패하면 25ms 간격으로 최대 1초 재시도합니다.
- stdout에 갱신마다 JSON 한 줄을 씁니다. 필드는 `revision`, `path_id`, `updated`, `reset`, `start_s`, `end_s`, `truck_s`, `ahead_m`, `offset_m`, `point_count`, `end_reason`, `prefabs`, `sectors`, `elapsed_ms`, `error`입니다. 오류(파이프 끊김·게임 정지 등)는 메시지가 바뀔 때만 출력하고, 마지막 파일은 유지한 채 재시도합니다. `--snapshot -`에서는 매 줄마다 한 줄씩 답합니다.

소비자는 `LanePath::extend(file)`(`ros2/src/ets2_autonomy/src/control/path_following.hpp`) 또는 C ABI `lat_extend(handle, file)`(`apps/sysid/lateral_capi.cpp`)로 이어 붙입니다. 이 함수들은 새 파일에서 자신의 마지막 점과 같은 누적 거리(`start_s + s`, 0.01m 이내)·위치의 점을 찾고, 그 뒤 점만 덧붙입니다. 기존 점의 `s`·heading·`progress`는 그대로입니다. revision이 같거나 작으면 0을 반환합니다. `path_id`가 다르거나 이어지는 점이 없으면 실패하고(`lat_extend`는 -1), 호출자가 파일을 새로 읽습니다. 런처와 ROS 노드(`gt_drive_node`)는 아직 한 번 실행 파일만 읽습니다.

차량이 지도 차로 밖에 있거나 지원하는 차로가 없으면 한 번 실행은 실패하고 기존 출력을 유지합니다. 호출자는 종료 코드 0을 확인한 뒤 새 파일을 사용해야 합니다.

## 오프라인 검증 (2026-10-20, 게임 실행 없음)

설치 지도 `base_map.scs`의 독일 고속도로 Road 위에 만든 저장 snapshot으로 실행했습니다. 증거는 `bridge/recordings/gt-path/offline/`(Git 제외)에 있습니다.

- **기존 기록 차로 재현**: 2026-10-10 실차 기록 `current-lane.json` 시작점의 snapshot에서 1 km를 다시 만들면, 666점 모두 기록과 최대 0.003m 차이입니다.
- **장거리 한 번 실행** 30 km 두 경로: `lane-e`는 prefab 91개(출구가 여럿인 prefab 23개)를 지나도 차로 순서 1→1을 유지했습니다. 최대 점 간격은 1.56m, 최대 꺾임 0.70°, 반경 150m 미만 구간은 1m(R149)입니다. `lane-f`는 prefab 95개를 지나고, 중앙분리 양방향 `ger11`/`ger14` 구간이 포함됩니다. 최대 접합 오차는 0.038m입니다. 연결로 합류 3곳에서 반경 95–98m S자가 생겼습니다.
- **후보 40개 일괄 실행**(독일 고속도로 임의 시작점, 각 최대 30 km, `candidates-summary.csv`): 총 364.6 km, prefab 1085개(출구 여럿 216개)를 통과했습니다. 5 km 이상은 23개, 10 km 이상은 14개입니다. 최대 점 간격은 1.63m, 최대 꺾임 1.10°, Road–prefab 접합은 최대 0.038m입니다. 종료 원인은 `road_end` 13, `lane_ends` 8, `lane_leaves_through` 6, `prefab_no_through` 5, `unsupported_road` 3, `traffic_light` 3, `distance_limit` 2입니다. 21개 경로는 반경 150m 미만 구간이 없습니다. 나머지의 급곡선은 합류 차로의 S자 또는 고속도로 끝·연결로 구간입니다. `road_end`를 4곳 확인한 결과, 모두 지도에서 그 차도가 실제로 끝나고 근처에서 반대 차도만 시작했습니다.
- **follow 재생**(`--snapshot -`, 가상 차량이 기준선을 따라 주기당 25m 전진, 횡 ±0.3m 흔들림, 매 revision을 `lateral_capi.dll`의 `lat_extend`로 이어 붙임): 8 km 21 revision, 12 km 31 revision 모두 이음 오류가 없었습니다. 남은 앞 거리는 최소 623m였습니다. 파일 점 수는 최대 884개로 유지됐습니다. 이어 붙인 경로는 같은 시작점의 한 번 실행 경로와 점 단위로 일치했습니다(차이 0). 갱신 시간은 지도 재오픈을 포함해 최대 2.6초였습니다. 차로를 왼쪽으로 옮긴 재생에서는 `path_id` 2 새 경로가 한 번 만들어졌습니다.

## 한계·미검증

- 실제 게임에서 follow 모드와 prefab 통과를 주행 확인하지 않았습니다. `ot` 파이프로 읽는 follow는 실행하지 않았고, snapshot 파일이 없을 때의 대기·재시도만 확인했습니다.
- `base_map.scs`만 읽습니다. DLC 지도(`dlc_east`/`dlc_fr`/`dlc_north`) 지역으로 넘어가면 `map_unresolved`로 끝납니다.
- 왼쪽(중앙분리대)부터 센 차로 순서 규칙은 우측통행 기준입니다. 좌측통행 지역은 검증하지 않았습니다.
- 중앙분리대 폭이 다른 양방향 look 사이(`ger11`→`ger12`, `ger14`→`ger15`)의 게임 내 전이 형상을 알 수 없어 `lane_ends`로 끝냅니다.
- 처음 위치가 prefab 안이면 시작하지 못합니다(도로에 나온 뒤 follow가 재시도).
- 이 provider는 GPL-2.0 구성요소(TruckLib)와 연결된 별도 프로세스로 유지합니다.

## 참고 소스·라이선스

- [TruckLib](https://github.com/sk-zk/TruckLib/tree/c42dbe4f2d6b): 지도 format 907(게임 1.59~1.61), Road Hermite 곡선, HashFS/모델/SII/PPD 의존성. 기존 소스를 직접 참조하며 수정하지 않습니다.
- [ETS2LA](https://github.com/ETS2LA/ETS2LA/tree/cf0fc395b0d0): `ETS2LA.Game/Data/Classes.cs`의 lane curve·prefab 좌표 변환·내비 곡선 탐색, `Data/Utils.cs`의 roadlook·내비 곡선 Hermite 해석을 참고했습니다. 4.5m 기본폭 코드는 복사하지 않았습니다.

TruckLib와 연결된 TruckLib.HashFs/Models/Sii는 GPL-2.0 라이선스 구성요소입니다. provider의 해당 라이선스 사본은 [LICENSE](LICENSE)에 보존합니다. 배포할 때 이 CLI와 연결된 라이브러리의 라이선스 및 해당 소스를 함께 보존해야 합니다. 다른 제품 프로세스에는 JSON 파일로 연결합니다.
