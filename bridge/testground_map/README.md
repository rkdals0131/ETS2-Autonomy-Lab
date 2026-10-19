# 동역학 시험장 맵

레퍼런스 트럭의 종·횡 동역학 시험용 평탄 아스팔트 패드를 생성합니다. 기본 크기는 폭 5.1 km(동서) × 길이 8 km(남북)이고, 지형 재질은 `terrain_mat.share_28`(asphalt 01), 프로필은 `profile0`(plain)입니다. 계수는 0, 노이즈 없음, 높이는 europe 판 y=20, 독립 맵 y=0입니다. 차선·도로·교통은 없습니다.

하나의 모드(`ot_testground.scs`)에 두 형태를 함께 넣습니다.

- **europe external map**: 보유 지도 격자 x −16..16, z −16..13 안쪽의 빈 북해 sector (−7, −12) 주변입니다. 중심은 SCS (−26000, 20, −46000)입니다. 게임은 `map/europe/`에 sector만 추가하면 읽지 않고, `map/europe.external.sii`의 `external_map_data` 유닛 **하나**가 가리키는 폴더를 europe에 붙입니다(핼러윈 이벤트 맵 방식). 그래서 `map/ot_testground_ext/`에 패드 sector와 기본 게임 `base_map.scs`의 `/map/halloween_ets2` sector를 함께 넣고, 이 파일을 그 폴더로 바꿉니다. 유닛을 둘 넣거나 유닛 이름 조각이 12자를 넘으면 파일 전체가 거부됩니다(2026-10-20 로그). 모드를 켠 프로필에서 콘솔 `goto -26000;25;-46000`으로 카메라를 옮긴 뒤 Ctrl+F9로 트럭을 옮깁니다.
- **독립 맵**: `map/ot_testground.mbd`이며, 중심은 원점이고 시작 위치는 (0, 0.5, 0)입니다. 콘솔 `preview ot_testground`로 엽니다.

## 빌드·실행

프로젝트 루트에서:

```powershell
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
research/tools/dotnet10/dotnet.exe build bridge/testground_map/testground_map.csproj -c Release --disable-build-servers
research/tools/dotnet10/dotnet.exe bridge/build/testground_map/Release/net10.0/testground_map.dll --game '<게임 설치 폴더>'  # 또는 ETS2_DIR
```

기본 출력은 `문서/Euro Truck Simulator 2/mod/ot_testground.scs`입니다. `--out`, `--width-m`(100–5156), `--length-m`(100–50000), `--europe-x`, `--europe-y`, `--europe-z`로 바꿀 수 있습니다.

패드는 동쪽 가장자리를 따라 `Append`로 이은 1 km terrain 아이템 사슬이고, 지형은 오른쪽(+Z 진행 시 −X)으로만 펼칩니다. 노드선을 패드 가운데 두고 양쪽으로 펼친 첫 판과, 아이템을 따로 추가한 판에서는 트럭을 그 이음매 위에 놓으면 아래로 떨어졌습니다(2026-10-20). 기본 지도의 terrain 면(1.61, 198,836개)은 모두 쿼드 63행 이하이고, 77행(6500 m)인 판은 게임이 `Terrain item quad info mismatch`로 거부했습니다. plain 프로필 행 간격으로 63행은 5156 m라 폭을 그 이하로 제한합니다. 열은 16 m 간격(1 km당 63열)입니다.

TruckLib은 sector `.desc`를 버전 2(경계 0..4000 고정)로 씁니다. 1.61 지도의 `.desc`는 버전 3이고, 내용물의 월드 경계를 1/256 m 단위로 담습니다(min x, min z, max x, max z, 플래그, 토큰 2개). 생성기는 저장 뒤 모든 패드 sector의 `.desc`를 패드 전체 경계로 다시 씁니다.

TruckLib은 아이템 k-DOP 경계를 계산하지 않습니다. 생성기는 기본 지도 terrain에서 관찰한 축 (x, y, z, (x+z)/2, (x−z)/2)으로 직접 채웁니다.

## 참고·라이선스

[TruckLib](https://github.com/sk-zk/TruckLib/tree/c42dbe4f2d6b)(지도 format 907, 게임 1.59–1.61)을 직접 참조합니다. TruckLib은 GPL-2.0이며 라이선스 사본은 [LICENSE](LICENSE)에 보존합니다.
