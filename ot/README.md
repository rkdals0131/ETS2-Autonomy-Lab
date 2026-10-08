# ot 0.7.0 — 상주 로더·SDK·네 미러 GPU 캡처

> **확정:** SDK/IPC·내부 자차 pose 대조. 0.5.0 mirror5 5초·50표본·10 Hz 기록. 0.6.1은 미러 0·1·2·5를 같은 Present 구간에서 3묶음 수집하고 네 hook의 패닉 복원을 확인. RenderDoc 4뷰 픽셀·상수·카메라 점군.
> **미확정:** DLL 영상과 최종 카메라 상수 연결·GPU 실행/표시 시간·제한 없는 FPS 영향·월드 정합·30분 주행. 키 테스트는 사용자 요청으로 생략.
> **다음:** pass별 카메라 자세·투영·ray 상수를 영상에 연결합니다.

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
