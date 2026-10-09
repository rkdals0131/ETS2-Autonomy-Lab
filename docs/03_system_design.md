# 시스템 구조

```text
ETS2 / Windows
  ot_loader → ot_core
  ├─ SDK·물리 상태 → OT_State
  └─ 독립 센서 pass → GPU pack → 공유 텍스처·fence + OT_Bundles
                         ↓
Windows C++ 릴레이
  ├─ GPU readback·RGB/CDR·JPEG
  ├─ 대용량 TCP: 영상·라이다·GT·렌더 TF·frame_info
  └─ 상태 TCP: SDK·clock·진단 / 역방향 구독·수집 제어
                         ↓ Ubuntu eth0 NAT 직접 주소
WSL ROS 2 Jazzy
  GenericPublisher → Fast DDS SHM → 소비자·rosbag2
                         └─ foxglove_bridge → Windows Foxglove

Windows 런처 → WSL ROS/Foxglove 소유 그룹 + Windows 릴레이
              ← 프로세스 종료·ROS 상태 수신·왕복 응답·수집 카운터
```

## 실행과 소유권

- 메타로더가 SDK 플러그인으로 상주하고 기능 DLL을 교체합니다.
- DLL의 lease는 한 클라이언트가 내부 읽기·리그·수집을 소유하게 합니다. 소유자가 사라지면 5초 뒤 Tier 0으로 복귀합니다.
- F11은 lease를 취소합니다. 재시작은 사용자 조작으로 수행합니다.
- 런처는 Windows Job Object와 WSL systemd unit으로 자신의 프로세스를 관리합니다. 창 종료·오류·실행 시간 만료 시 해당 그룹을 정리합니다.
- 설정은 로컬 JSON 한 곳에서 읽고 센서 프리셋을 상대 경로로 참조합니다. 프로세스·응답·수신 시각이 화면 상태의 기준입니다.

## 렌더와 전송

캡처 요청 하나를 다음 센서 선택 호출이 가져갑니다. 선택된 네 pass의 실제 Present ID를 대조하고 완성된 묶음만 발행합니다. 비수집 프레임은 센서 제출과 상세 관측을 건너뜁니다.

이미지는 공유 D3D11 텍스처에 pack합니다. 게임은 fence를 signal하고, 릴레이가 별도 device에서 staging으로 복사해 읽습니다. 소비 완료 전 슬롯은 재사용하지 않습니다. CPU 공유 슬롯은 소유 버퍼로 복사한 즉시 반환합니다. 혼잡 시 대기 중인 오래된 묶음을 통째로 버립니다.

상태와 영상의 송수신 worker·큐는 분리합니다. 상태 연결은 TCP_NODELAY를 사용합니다. WSL 주소는 연결 때 eth0에서 다시 조회합니다.

## ROS 소비자

Windows가 XCDRv1 메시지를 만들고 WSL은 GenericPublisher로 발행합니다. 센서 토픽은 best-effort, 상태는 reliable, 정적 TF는 transient-local입니다. DDS SHM participant 세그먼트는 128 MiB, 최대 메시지는 8 MiB입니다.

Foxglove는 JPEG·라이다·GT·TF·상태를 구독합니다. 원본 RGB·depth와 축소 perception 영상은 ROS 소비자가 선택합니다. 실제 구독이 GPU pack·readback 요구로 연결됩니다.

센서 확장·인지·제어는 [개발 순서](13_game_operating_table.md)를 따릅니다. 메시지 의미는 [ROS 계약](16_ros2_bridge.md)에 있습니다.
