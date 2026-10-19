# 레퍼런스 트럭 식별 주행

[시험장 맵](../../bridge/testground_map/README.md) 위에서 조향과 페달을 함께 잡고 정해진 시험 단계를 차례로 실행합니다. 모든 SDK 프레임은 `bridge/recordings/sysid-<시각>/frames.jsonl`에 기록하고, 진행 상태는 같은 폴더의 `status.json`에 씁니다.

```powershell
py -3 apps/sysid/sysid_drive.py --view               # 20,40,60,80 km/h 전체
py -3 apps/sysid/sysid_drive.py --speeds 60 --view   # 일부 속도만
```

`--view`를 주면 새 PowerShell 창에서 `sysid_view.py`(rich 사용)가 단계 목록, 현재 단계와 전체 진행률, 속도·조향·페달·패드 가장자리까지 거리를 보여 줍니다. 끝나면 Enter를 눌러 창을 닫습니다. `py -3 apps/sysid/sysid_view.py <run>/status.json`으로 따로 열 수도 있습니다.

## 단계

- 속도별(기본 20/40/60/80 km/h): 가속·정속 → 조향 속도 펄스 ±0.3(약 2 m/s² 횡가속 크기로 시간 조절) 후 명령 0 유지 2.5 s → 조향 위치 사인 0.2/0.5/1.0 Hz 3주기(약 1.5 m/s²) → 정상 선회 8 s(약 2 m/s²).
- 종방향: 타력 15 s, 60 km/h에서 브레이크 0.2/0.5/1.0 계단, 정지에서 스로틀 0.3/0.6/1.0 계단.
- 직진은 출발 방향의 heading 유지입니다. 앞쪽 여유가 부족하면 30 km/h, 반경 60 m로 유턴합니다.

## 실행 조건과 해제

- `ot_config.json`의 drive 권한과 `drive_controls_path`가 현재 프로필을 가리켜야 arm됩니다. 시동이 켜져 있고 주차 브레이크가 풀려 있어야 하며 자동 변속을 가정합니다.
- 게임 창이 2 s 이상 전경일 때 arm합니다. 수동 입력, 일시정지, F11, 패드 가장자리 250 m 안 진입 중 하나라도 생기면 멈추고, 아직 제어 중이면 브레이크로 세운 뒤 해제합니다.
- 패드 좌표(`PAD`)와 트럭 상수(`WHEELBASE`, `ANGLE_PER_UNIT`)는 스크립트 머리에 있습니다. 현재 값은 Scania S 4x2와 시험장 기본 위치입니다.
