# 좌표·시각·센서 데이터

## 좌표계

`base_link`는 구동 후축 중심 아래의 명목 지면점입니다. +x 전방·+y 좌측·+z 위이며, 도로 요철에 따라 원점을 다시 잡지 않습니다. 현재 FH4 4x2의 SDK/model 원점 보정은 `[0, -0.0041681, 2.0315917]` m입니다.

```text
p_model = base_origin + (-base_y, base_z, -base_x)
world → base_link → cabin → camera / lidar
```

카메라 장착값은 neutral chassis 기준이며, 렌더 때 head 회전 전의 cabin parent로 변환합니다. 캐빈 서스펜션과 렌더 보간이 TF에 반영됩니다. 카메라 optical 축은 +x 오른쪽·+y 아래·+z 전방이고, 라이다는 +x 전방·+y 좌측·+z 위입니다.

게임 길이 단위는 SDK 미터 좌표와 일치합니다. SDK·물리 자세와 리그 위치를 같은 좌표로 대조했습니다. SDK 속도의 측정점은 질량중심이므로 회전 중 다른 장착점 속도에 `omega × r` 보정이 필요합니다.

## 시각

- 카메라·깊이·라이다·GT·렌더 TF는 해당 pass의 센서 stamp와 render_frame_id를 공유합니다.
- SDK 상태는 자체 SDK 관측 시각을 유지합니다.
- `/clock`은 pause를 반영한 시뮬레이션 시각입니다. pause 동안 상태 전송은 계속되고 센서 발행은 멈춥니다.
- SDK frame_end 뒤에 렌더 보간·카메라·가시성 준비가 이어집니다.
- 네 pass가 다른 Present 구간에 속하면 묶음을 폐기합니다.

## 영상·깊이

RGB8은 GPU 노출·Reinhard·sRGB 변환 결과입니다. 자동 노출 gain은 `/ets2/frame_info/exposure`에 기록합니다. CameraInfo는 실제 projection·viewport에서 계산하며 축소 영상은 보정값도 함께 축소합니다.

깊이는 DSV를 실제 projection과 viewport 깊이 범위로 복원한 광축 거리(m), 32FC1입니다. 유효 깊이가 없는 픽셀은 NaN입니다. 라이다 range는 광축 깊이를 빔 방향 거리로 변환한 값입니다. [DSV와 재질 Z](12_dx11_mirror_render_path.md#잎-billboard의-z-보정과-dsv-차이).

GT 차량 박스는 해당 pass 준비 시점의 렌더 모델 자세를 사용합니다. 제출 목록의 차량은 최종 영상에서 다른 물체에 가려질 수 있습니다.

## 기록

ROS bag은 수신 시각으로 기록하고 `/clock`·TF·센서·frame_info를 함께 보존합니다. 기록에 `--use-sim-time`, 재생에 `--clock`을 넣지 않습니다. 재생 소비자는 `use_sim_time=true`로 설정하고 실시간 브리지는 중지합니다.

런처의 차량 상태 기록은 기본 `bridge/recordings/<run>/bag`에 저장합니다. 영상·라이다 기록은 가용 공간을 확인한 `sensor_recording_root`에 저장합니다. 경로는 현재 PC의 저장 장치에 맞춰 지정하며 기록은 Git에서 제외합니다. `/tmp`는 일회성 staging에 사용합니다. 기존 로컬 연구 표본은 `research/live`에 남아 있습니다.
