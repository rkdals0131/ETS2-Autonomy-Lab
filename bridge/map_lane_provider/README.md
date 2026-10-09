# 지도 차로 기준선

설치 게임의 실제 지도 Road 곡선과 road template 노면을 읽어, 현재 차량이 있는 차로의 주행 기준선을 생성합니다. 기본 실행은 `ot` 파이프에서 SDK `truck.world.placement`만 읽습니다. 게임 입력·Tier·설정은 변경하지 않습니다.

현재 독일의 `ger16` 3차로 단방향 도로에서 실제 실행했습니다. PMD/PMG `vis` 노면의 X 범위는 -9~8m, roadlook의 좌우 shoulder는 0m/3.5m입니다. 실제 포장 차로 폭 `(17-3.5)/3`으로 4.5m를 계산합니다. 고정된 차로폭을 가정하지 않습니다. 템플릿 노면·shoulder·차로 수가 같은 단방향 layout을 지원하며, 다른 layout은 기준선을 만들거나 이어가지 않습니다.

## 빌드·실행

로컬 SDK와 기존 고정 TruckLib 소스가 필요합니다. SDK는 `research/tools/dotnet10`, TruckLib는 `research/sources/sk-zk_TruckLib-c42dbe4f2d6b`에 있습니다. 연구 자료는 Git에 포함되지 않습니다.

프로젝트 루트에서:

```powershell
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'
research/tools/dotnet10/dotnet.exe build bridge/map_lane_provider/map_lane_provider.csproj -c Release --disable-build-servers
research/tools/dotnet10/dotnet.exe bridge/build/map_lane_provider/Release/net10.0/map_lane_provider.dll --game 'D:/SteamLibrary/steamapps/common/Euro Truck Simulator 2' --out 'bridge/recordings/gt-path/current-lane.json' --distance-m 1000
```

`--snapshot`으로 기존 raw snapshot JSON 또는 `{"snapshot": ...}` 형식의 저장 표본도 읽을 수 있습니다. 기본값은 현재 SDK 위치, 기본 게임 설치 경로, 최대 1000m입니다. 현재 위치 주변 9개 sector만 직접 읽으며 전체 지도나 게임 메시를 추출하지 않습니다.

생성 JSON은 `frame_id: "world"`, `lane_width_m`, `points`, `end_reason`입니다. 점 좌표는 기존 ROS world와 같은 `[SCS X, -SCS Z]` 미터입니다. 약 1.5m 간격으로 실제 차로 곡선을 샘플링하고, 현재 위치의 차로 투영점에서 진행 방향으로 시작합니다.

연결 Road의 공유 Node와 동일한 차로 layout을 따라갑니다. 경로 선택이 필요한 prefab은 `prefab`, 차로 변경·지원하지 않는 layout·순환은 `ambiguous`, 연결 끝은 `road_end`, 지정 거리 끝은 `distance_limit`으로 종료합니다. 제어기는 모든 끝점에서 감속해야 합니다. 이 provider는 내비게이션 목적지 경로, 교차로 통과, 차로 변경, 교통 대응을 제공하지 않습니다.

차량이 지도 차로 밖에 있거나 지원하는 차로가 없으면 실패하고 기존 출력은 유지합니다. 호출자는 종료 코드 0을 확인한 뒤 새 파일을 사용해야 합니다. 파일은 같은 디렉터리에서 완성 후 교체합니다.

## 참고 소스·라이선스

- [TruckLib](https://github.com/sk-zk/TruckLib/tree/c42dbe4f2d6b): 지도 format 907(게임 1.59~1.61), Road Hermite 곡선, HashFS/모델/SII 의존성. 기존 소스를 직접 참조하며 수정하지 않습니다.
- [ETS2LA](https://github.com/ETS2LA/ETS2LA/tree/cf0fc395b0d0): `ETS2LA.Game/Data/Classes.cs`의 lane curve 및 `Data/Utils.cs`의 roadlook 해석을 참고했습니다. 4.5m 기본폭 코드는 복사하지 않았습니다.

TruckLib와 연결된 TruckLib.HashFs/Models/Sii는 GPL-2.0 라이선스 구성요소입니다. provider의 해당 라이선스 사본은 [LICENSE](LICENSE)에 보존합니다. 배포할 때 이 CLI와 연결된 라이브러리의 라이선스 및 해당 소스를 함께 보존해야 합니다. 다른 제품 프로세스에는 JSON 파일로 연결합니다.
