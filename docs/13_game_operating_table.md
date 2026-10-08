# 13번 계획의 구현 상태 — 2026-10-08

> **확정:** SDK 9채널·IPC·내부 자차 pose 대조. RenderDoc 4뷰 픽셀·상수·점군. 0.5.0 mirror5 5초·50표본·10 Hz 기록. 0.6.1에서 같은 Present 구간의 미러 0·1·2·5 세 묶음, 공유 텍스처 0·2의 서로 다른 픽셀, 네 hook 패닉 복원·완전한 SDK 언로드 확인.
> **남은 검증:** draw별 상수 변경 범위·GPU 실행/표시 시간·제한 없는 FPS 영향·30분 주행·오버레이·독립 6뷰. 월드 정합·미터 단위·AI 박스도 남아 있습니다.
> **추가 구현:** 0.7.0 메타로더로 콘솔 없는 기능 DLL 교체. 0.7.1–0.7.3 hook 본문 시간 계측·내부 읽기 최적화. 0.8.0은 영상과 pass 기본 카메라 상태 연결·기본 월드 점군 확보. 0.8.1은 같은 영상의 GPU geometry 상수를 수집해 CPU/GPU 회전·투영·기준 원점의 일관성 대조.
> **다음:** 개별 draw의 최종 GPU 상수 대조·월드 정합·주행·연속 데이터 전달. vrperfkit은 백업·분리 상태입니다. 패닉 키는 F11이며 키 테스트는 생략.

아래는 사용자가 제공한 작업 계획 원문입니다. 계획에 적힌 완료 기준을 이미 달성한 것으로 해석하지 않습니다. 실제 구현과 명령은 [ot 사용법](../ot/README.md)에 기록합니다.

| 마일스톤 | 현재 상태 |
| --- | --- |
| M0 | 부분 완료. SDK/IPC·내부 자차 pose·패닉·SDK 언로드/재로딩 확인. 0.7.0 상주 로더로 콘솔 없는 기능 DLL 교체·실패 복구 확인. 30분 주행, ImGui, 자동 크래시 포착은 남음. 키 테스트는 사용자 요청으로 생략 |
| M1 | 완료. mirror5 종료 지점을 실제 GPU 복사에 사용. 후속 0.6.0은 pass 명령 구간으로 0·1·2·5 식별, 0.6.1은 네 hook 구성으로 단순화. 패닉 복원·완전한 SDK 언로드 확인 |
| M2 | 네 뷰 첫 픽셀 확보. 0.6.1에서 프레임 구간 32·41·50, 각각 33 MiB. 0·2의 RGB·Z 차이와 윤곽 대응 확인. 0.5.0 mirror5는 5초·50표본·10 Hz. 네 뷰 FPS 비용의 대조 측정·장시간 수집은 아직 |
| M3 | DLL에서 네 영상의 CPU/GPU 상수를 연결해 회전·투영·기준 원점 대조. DSV와 shader Z를 구별하는 오프라인 점군 명령 구현. 근거리 FP16 절삭 대조 및 잎 billboard의 별도 Z 보정 경로 확인. 개별 픽셀의 draw 연결·월드 정합·AI 박스·미터 검증은 아직 |
| M4–M6 | 미착수. 독립 리그·6뷰·누락 측정·공유 메모리/MCAP 센서 묶음 출력 없음. 네 미러 단일 묶음의 파일 저장은 M2에서 구현 |

현재 구현의 차이: 스키마는 추가 YAML 의존성 없이 JSON으로 저장·컴파일합니다. Tier 0에서도 공식 SDK 수신은 유지합니다. Tier 1은 `sdk_frame_end`에서 미러와 물리 차량 자세를 읽는 초기 경로이며, 렌더 완료 스냅샷이 아닙니다. 공유 메모리의 슬롯 소유권 교환은 Python 프로세스에 로드하는 작은 `ot_ipc.dll`이 담당합니다. RenderDoc 캡처용 백업은 `ot/backup/before-renderdoc-20261008-132657/`이며, `dxgi.dll`, `vrperfkit.yml`, `vrperfkit.log`, 구버전 `plugins/ot_core.dll`이 보존돼 있습니다. 캡처 후 게임 정상 종료를 확인하고 14:29에 새 0.3.0 DLL을 plugins 폴더에 배치했습니다. vrperfkit은 계속 분리 상태입니다. 첫 캡처 시도는 Steam 재실행으로 연결이 빠졌으나 임시 `steam_appid.txt`로 해결했고, 실제 캡처 후 이 파일은 제거했습니다. 이후 저장된 RDC에서 4개 뷰의 픽셀을 추출했습니다. 이는 저장된 캡처의 추출 이력이며, 후속 DLL의 mirror5 10 Hz 실측은 아래에 따로 기록합니다.

### M1의 실제 캡처 결과

후속 0.6.1에서는 미러 0·1·2·5를 같은 Present 구간에서 세 번 수집했습니다. 프레임 구간은 32·41·50이며, 각 묶음의 네 복사 제출 시각이 해당 Present 반환 사이에 있었습니다. 미러 0·2는 원본 Texture2D를 재사용하지만 색상은 100%, 첫 Z는 99.9128%의 픽셀이 달랐습니다. [네 뷰 RGB/Z/재질](../research/live/2026-10-08-render-probe/mirrors-first/comparison.png)과 raw·NPY를 보존했습니다. 이는 렌더 구간을 맞춘 결과이며 자차·AI까지 동기화한 스냅샷은 아닙니다. `sdk unload`에서 DLL·pipe 부재와 worker·매핑·hook 임시 코드의 해제를 확인했습니다. 실행 원본은 `0.6.1-first-bundles.json`, 수치 요약은 `0.6.1-bundle-summary.json`입니다.

후속 0.3.0은 이 호출 스택의 인수 준비 명령 RVA `0x2B3193`를 유일한 signature로 찾고 렌더 호출을 기록합니다. RenderDoc 없는 일반 DX11 PID 24748에서 10초간 20,824회, 기록 실패 0회를 확인했습니다. 패닉 후 호출 수 정지·게임 코드 복원·SDK 수신 유지, SDK 언로드와 재로딩도 확인했습니다. 0.4.1은 이미지 생성 RVA `0x21F09D`의 이름 연결을 추가해 실제 GPU 픽셀 수집까지 진행했습니다. 구체적인 권한·명령·해제 동작은 [ot 사용법](../ot/README.md)에 있습니다.

### DLL에서 확보한 mirror5 첫 픽셀

0.4.1의 첫 캡처는 렌더 스레드 18012에서 G-buffer binding 28 → 색상 타깃 이탈 binding 31을 연결했습니다. `mirror5/attributes_0`, `attributes_3`, `composition_raw`의 이름·image ID를 생성 시점에 기록하고 실제 RTV의 Texture2D와 대조했습니다. 512×256 raw 3장(3 MiB)을 비동기 GPU copy 뒤 readback했습니다. [RGB/Z/재질 비교 그림](../research/live/2026-10-08-render-probe/mirror5-first/comparison.png)과 raw·NPY가 같은 폴더에 있습니다. Z 범위는 `-6.03515625~-0.26806640625`, 유한·비영 표본 100%이며 차체·바닥·배수구의 윤곽을 RGB와 대조했습니다. 원시 행 방향을 유지한 그림입니다.

추가 세 번까지 총 4회 수집에 성공했습니다. 캡처 순번은 1–4이고 SDK 힌트는 3755·9672·9706·9742입니다. 이 번호들은 아직 실제 GPU frame ID가 아닙니다. 각 수집 후 패닉에서 두 hook의 실제 코드 복원·활성 수 0을 확인했고, 최종 SDK 언로드에서도 DLL·pipe 부재와 임시 코드 해제 로그를 확인했습니다. 원본은 `research/live/2026-10-08-render-probe/0.4.1-*.json`입니다. 미터 단위·월드 정합·연속 수집 FPS는 이 정차 실험으로 확정하지 않습니다.

후속 프레임 경계 조사에서는 저장된 RenderDoc `Present` chunk 32564의 호출 스택을 `0x2BFEDA ← 0x2B07FF`로 연결했습니다. 실제 `IDXGISwapChain::Present` 호출은 `0x2BFED7`이며, 호출자의 `0x2B07DD → 0x2F0670`은 renderer `+0x858`의 미처리 명령 수가 0이 될 때까지 기다립니다. 0.5.0은 이 반환 지점을 세 번째 hook으로 관측해 Present 구간과 간격을 기록합니다. GPU 실행 완료나 실제 모니터 표시 시각을 뜻하지 않습니다. 근거는 `present-chunk.xml`, `dxgi-present-call.txt`, `dxgi-present-caller.txt`, `render-queue-drain.txt`입니다.

0.5.0의 실제 정차 실험은 5.0002초에 50표본(9.9996 Hz), raw 150 MiB, 요청 슬롯 누락 0회였습니다. 프레임 구간 번호는 153–300에서 3씩 증가했고 50개 모두 복사 제출 시각이 해당 Present 반환 사이에 있었습니다. Present 기록 손실은 0, HRESULT는 모두 S_OK였습니다. 같은 hook을 켜고 픽셀 수집 없이 관측한 5초는 29.996회/s, 수집 중에는 30.001회/s였습니다. 현재 장면의 약 30 Hz 제출 속도에서 차이가 관측되지 않았지만 hook 자체 비용·제한 없는 FPS·장시간 성능까지 확정하지 않습니다. 복사 준비·제출과 readback의 CPU 시간 중앙값은 각각 0.341/0.414 ms였습니다.

마지막 50번 표본의 RGB/Z/재질을 확인했습니다. 세 hook의 코드 복원·Tier 0 복귀에 이어 사용자 `sdk unload` 후 DLL·pipe 부재와 임시 코드 해제 로그도 확인했습니다. 실제 기록은 `research/live/2026-10-08-render-probe/0.5.0-*.json*`, 대표 픽셀은 `mirror5-record-sample50/`입니다. `record_mirror5`는 단일 캡처를 순차 요청하는 제한 시간 CLI이며, 다중 뷰 GPU ring·공유 센서 묶음·MCAP 구현은 아닙니다.

사용자가 RenderDoc에서 실행한 게임 PID 28532에 `renderdoc.dll`이 로드됐으며, frame 3160의 저장 완료를 로그에서 확인했습니다. 파일은 `research/live/2026-10-08-renderdoc/frame3160.rdc` (1,723,327,850 bytes)입니다. `renderdoccmd thumb`와 `convert -c xml`로 화면 썸네일과 명령 XML을 추출했습니다. 화면은 차고 내 정차·운전석 시점이며 우측 HUD 미러가 보입니다. 이것만으로 개별 미러의 RGB/Z readback 완료를 주장하지 않습니다.

- XML chunk 32,566개 중 BeginEvent/EndEvent/marker는 없었습니다. 리소스 debug name은 backbuffer 1개였습니다.
- Deferred context 35는 생성됐지만, 이 캡처의 context를 가진 API 명령 29,062개는 모두 immediate context 7이었습니다. `FinishCommandList`·`ExecuteCommandList`는 기록되지 않았습니다. 전체 게임에서 deferred context를 사용하지 않는다는 뜻은 아닙니다.
- `OMSetRenderTargets` 74회의 호출 스택은 동일한 게임 RVA `0x2B31AA ← 0x2F05D7 ← 0x11C06F ← 0x1D53E6B`로 이어졌습니다. 실제 COM 호출 명령은 `0x2B31A4`, vtable slot `0x108`입니다. 일반 명령 소비 경로이므로 이 주소만으로 특정 미러를 식별할 수는 없습니다.
- 미러 해상도와 일치하는 G-buffer 묶음은 512×256 → 512×1024 → 512×512 → 512×1024 순서였습니다. 세로형 두 묶음은 attribute0 ID 9728, attribute3 ID 9737, depth ID 9740, composition 후보 ID 9748을 재사용합니다. 단순 해상도 일치만으로 미러 번호를 확정하지 않고 픽셀과 대조합니다.

| 후보 구간 | G-buffer 마지막 draw chunk | color 후보 마지막 draw chunk | attribute0 / attribute3 / color 리소스 |
| --- | --- | --- | --- |
| wide | 11318 | 11979 | 57809 / 57818 / 57829 |
| tall_first | 14840 | 15748 | 9728 / 9737 / 9748 |
| square | 19616 | 20839 | 10006 / 10015 / 10026 |
| tall_second | 23298 | 24104 | 9728 / 9737 / 9748 |

위 번호는 RenderDoc XML chunk index이며 replay event ID가 아닙니다. `export_frame3160.py`는 replay의 APIEvent에서 대응 event ID를 찾아 각 시점의 원시 텍스처를 꺼냅니다. 호출 스택·바인딩 원본 및 disassembly도 같은 자료 폴더에 보존했습니다.

### 프레임 3160의 실제 픽셀

2026-10-08 14:01, 사용자 명령으로 RenderDoc 내장 Python에서 추출을 완료했습니다. 처음에는 시작 스크립트에도 `pyrenderdoc`가 제공되는 점을 잘못 처리해 빈 UI가 열렸습니다. `IsCaptureLoaded()`를 함께 판별하도록 수정한 뒤 raw 파일 12개와 `images.json`이 생성됐고, 추출용 프로세스는 종료됐습니다. 일반 Python 2.7 또는 Python 3의 모듈 검색 경로를 변경할 필요는 없습니다.

| 뷰 후보 | G-buffer / color event ID | Z 최솟값 / 최댓값 | Z=0 비율 |
| --- | --- | --- | --- |
| wide | 7815 / 8476 | -6.03515625 / -0.26806640625 | 0% |
| tall_first | 11337 / 12245 | -213.5 / 0 | 2.036% |
| square | 16113 / 17336 | -188.625 / 0 | 2.141% |
| tall_second | 19795 / 20601 | -236.25 / 0 | 2.198% |

- 모든 Z 표본은 유한했고, 0이 아닌 Z는 음수였습니다. 이는 해당 shader 경로의 카메라 공간 Z이며 광선 거리나 미터 단위 거리의 실측 검증이 아닙니다. Z=0은 거리 0으로 해석하지 않습니다.
- 같은 리소스를 재사용한 세로형 두 시점의 color 픽셀은 전부 달랐고, attributes0 픽셀은 99.9998% 달랐습니다. 이미지에도 서로 다른 차고 방향이 보입니다. 따라서 Present에서 한 번만 읽으면 두 논리 뷰의 중간 결과를 모두 얻을 수 없다는 점을 실제 픽셀로 확인했습니다.
- 표시한 mask는 `((w >> 13) & 7) | ((z & 3) << 3)`의 packed material bits입니다. 관측 값은 0·2·4·8·10이며 객체 의미 분할 클래스가 아닙니다.
- `pixels/*.bin`과 대응 NPY는 원시 RGBA16F / RGBA16UINT를 보존합니다. `*_camera_z.npy`는 `.w`의 float32 변환입니다. 총 raw 텍스처 크기는 33 MiB입니다.
- [비교 그림](../research/live/2026-10-08-renderdoc/pixels/comparison.png)은 네 뷰에 공통 노출 배율 약 368.4와 Reinhard·sRGB 변환을 적용했습니다. 원본 행 순서를 유지했으므로 장면이 상하 반전돼 보입니다. 이 표시용 배율·행 방향을 센서 보정값으로 사용하지 않습니다.

### 같은 캡처의 상수와 카메라 공간 점군

`export_frame3160_constants.py`도 사용자 실행으로 완료됐습니다. G-buffer 마지막 draw, 색상 구간의 두 full-screen triangle, 마지막 color draw, 총 16개 시점의 VS/PS disassembly·상수 buffer·viewport를 확보했습니다. 원본은 `constants/draw-state.json`, `*_cb0.bin`, `shader_*.txt`입니다. stripped shader의 이름 없는 상수 lane 중 4개는 NaN bit pattern이었으며 실제 fog 연산에서 사용하지 않는 lane입니다. JSON에서는 `"nan"` 문자열로 표시하고 raw buffer는 보존했습니다. 상수 전체를 유효한 행렬 또는 float 벡터라고 해석하지 않습니다.

두 번째 full-screen triangle은 정적 `def.fog` 분석과 같은 연산을 수행했습니다. 해당 draw의 PS 입력은 추출한 attributes0·attributes3 리소스 ID와 일치했습니다. PS `cb0[3]`의 ray 상수는 다음과 같습니다.

| 뷰 | fog event | ray A=(Ax, Ay, Az, Aw) | 저장한 유효 점 |
| --- | --- | --- | ---: |
| wide | 8243 | (1.7876297, -0.8938152, -3.5752594, 1.7876304) | 131,072 |
| tall_first | 12008 | (0.5205670, -1.0411340, -1.0411340, 2.0822680) | 513,615 |
| square | 17108 | (1, -1, -2, 2) | 256,531 |
| tall_second | 20427 | (0.5205670, -1.0411340, -1.0411340, 2.0822680) | 512,762 |

이 캡처에서 full-screen VS와 PS의 viewport 변환은 `u=(x+0.5)/width`, `v=(y+0.5)/height`로 정리됩니다. `xyz = Z * (u*Az+Ax, v*Aw+Ay, 1)`로 복원했습니다. 원본 행 방향·음의 Z를 유지합니다. Z=0·비유한 Z와 bit16은 제외하며 무효 XYZ는 NaN으로 저장합니다. 이는 zfetch의 제외 의미를 따른 초기 범위이고 모든 투명 표면을 해석했다는 뜻은 아닙니다. 이 4뷰에는 bit16 픽셀이 없었습니다.

독립적으로 geometry VS의 `cb0[0..3]` 변환과 `cb0[4..7]` clip 변환에서 투영행렬을 분리했습니다. 이 행렬로 fog ray에서 복원한 점을 투영한 결과, 원래 pixel center와의 최대 차이는 **0.000033 pixel 미만**이었습니다. geometry viewport의 깊이 범위는 0.01–0.9, fog draw는 0–1입니다. 이 비교는 두 상수 경로의 좌표 일관성을 확인하며, 깊이 정확도·미터 단위·월드 pose 검증을 대신하지 않습니다.

`reconstruct_pixels.py`를 실제 실행해 `reconstruction/*_xyz_camera.npy`, `*_valid.npy`, 뷰별 PLY 4개, [점군 그림](../research/live/2026-10-08-renderdoc/reconstruction/pointclouds.png)을 저장·확인했습니다. 그림만 15 게임 길이 단위 이내의 일부 점을 표시하며 원본 NPY·PLY는 전체 유효 점을 보존합니다. PLY 색상은 앞의 노출 조정 미리보기입니다. 각 점군은 별도 카메라 좌표계이며 아직 하나의 월드 점군으로 합치지 않았습니다. `mirrorN` 번호와 좌우의 확정 연결, 상대 모델/월드 원점, AI 박스 정합, 연속 frame 수집은 남아 있습니다.

---
# 13. 다음 단계 지시: 게임 수술대

> **상태 (2026-10-08)**
> - **확정**: 미러 카메라 6개와 9슬롯 제출 경로. 미러 0·1·2·5의 DX11 색상·depth·attributes 리소스. `attributes_0.w` = 카메라 공간 Z. SDK와 ETS2LA의 좌표·시간 규약.
> - **미확보**: GPU 픽셀. pass 상수와 픽셀의 연결. 미러 강제 갱신. 운전석과 독립된 센서 자세.
> - **이번 단계 목표**: 읽기 전용 외부 관측을 끝냅니다. 게임 프로세스 안에서 관측하고 개입하는 도구, 즉 "수술대"를 만들어 첫 픽셀과 같은 프레임의 6뷰를 얻습니다.

01–12번은 조사 문서이고, 13번은 실행 지시입니다. 이후 구현 기록은 이 문서의 마일스톤 번호(M0–M6)를 따릅니다.

---

## 1. 왜 방식을 바꾸는가

지금까지는 외부 프로세스에서 `ReadProcessMemory`로 읽기만 했습니다. 구조를 파악하는 데는 충분했지만, 다음 세 가지 한계 때문에 더 나아갈 수 없습니다.

| 한계 | 결과 |
| --- | --- |
| 표본이 원자적이지 않음 | 같은 frame의 자세·카메라·차량 목록이라고 주장할 수 없음 (12번 문서에 명시됨) |
| GPU 메모리 접근 불가 | 픽셀이 한 장도 없음 |
| 쓰기 불가 | "이 값을 바꾸면 무엇이 바뀌는가"라는 인과 실험을 할 수 없음 |

따라서 게임 프로세스 안에 상주하는 DLL을 둡니다. 이 DLL은 **정해진 엔진 시점에서 읽고, 허용된 대상만 쓰며, 언제든 원상 복구합니다.** 이것이 수술대입니다.

---

## 2. 원칙

1. **버전 고정.** `eurotrucks2.exe` 1.61.1.1의 SHA-256을 기록합니다. DLL은 시작할 때 해시를 비교하고, 다르면 hook 없이 SDK 텔레메트리 모드로만 동작합니다. 설치 폴더는 백업해 두고, 자동 업데이트를 막을 수단(SCS의 이전 버전 beta 브랜치 등)을 확인합니다.
2. **싱글플레이 전용.** 연구용 프로필을 따로 만들고, Convoy나 TruckersMP 세션에서는 플러그인을 빼고 실행합니다.
3. **모든 개입은 되돌릴 수 있어야 합니다.** hook을 해제하고, 덮어쓴 값을 복원하고, 패치한 바이트를 원래대로 돌립니다. 패닉 키 하나로 Tier 0까지 복귀해야 합니다.
4. **쓰기 권한은 단계적으로 엽니다.** 아래 Tier 표를 따릅니다.
5. **범용 도구는 다시 만들지 않습니다.** 탐색에는 기존 도구를 쓰고, 이 게임에 특화된 부분만 직접 만듭니다(3.12절).
6. **게임 아카이브(.scs) 원본은 수정하지 않습니다.** 데이터 변경은 별도 mod 패키지로, 코드 변경은 DLL로만 합니다.

### 개입 단계 (Tier)

| Tier | 허용 범위 | 예 | 켜는 방법 |
| --- | --- | --- | --- |
| 0 | 외부 읽기 전용 (현재) | `read_render_memory.py` | 기본값 |
| 1 | 프로세스 내부에서 hook 시점에 읽기와 GPU 복사 | frame 스냅샷, 렌더 탭 | 설정 플래그 |
| 2 | 스키마에서 `writable`로 지정한 필드만 쓰기, 해제 시 자동 복원 | 미러 카메라 pose·projection, 선택 bit | 설정 플래그 + 오버레이 확인 |
| 3 | 코드 패치와 분기 변경 | 미러 제외 검사 무시, LOD 기준 카메라 변경, 슬롯 확장 | 패치별 개별 플래그 |

패닉 키를 누르면 Tier 3, 2, 1을 역순으로 해제합니다.

---

## 3. 수술대 구성

```text
ETS2 프로세스
 ├─ ot_core.dll  (SCS SDK 플러그인 경로로 로드)
 │   ├─ 버전 게이트 / 로그 / 크래시 덤프
 │   ├─ hook 관리자 (signature → 주소, hook별 Tier)
 │   ├─ 스키마 레지스트리 (타입 있는 읽기/쓰기, writable 화이트리스트)
 │   ├─ frame 스냅샷 (정해진 엔진 시점에 필드를 한 번에 복사)
 │   ├─ 렌더 탭 (pass 종료 직후 GPU→GPU 복사 → 지연 readback)
 │   ├─ 카메라 리그 (mirror_camera를 센서 자세로 덮어쓰기)
 │   ├─ 인게임 오버레이 (ImGui) + 패닉
 │   └─ 명령 서버 (named pipe)
 │        │ 제어 평면: \\.\pipe\ot
 │        │ 데이터 평면: Local\OT_State, Local\OT_Bundles
 ▼
Python (otpy)
 ├─ 스키마 기반 클라이언트 / REPL / Jupyter
 ├─ 링 버퍼 reader → numpy
 └─ MCAP 기록 / Foxglove
```

### 3.1 진입: SCS SDK 플러그인을 로더로 사용

- `bin/win_x64/plugins/`에 넣은 DLL은 게임이 공식 경로로 로드합니다. 별도 injector가 필요 없고, 백신 오탐도 줄어듭니다.
- `scs_telemetry_init`에서 버전 게이트, 로그, 명령 서버를 시작합니다. hook 설치는 나중으로 미룰 수 있습니다.
- **D3D11 device 획득 방법은 플러그인 로드 시점에 따라 다릅니다.** 먼저 플러그인 로드가 device 생성보다 앞인지 뒤인지 로그로 확인합니다.
  - 뒤라면: 이미 위치를 아는 미러 텍스처의 `ID3D11Texture2D*`에서 `ID3D11DeviceChild::GetDevice`로 device와 immediate context를 얻습니다.
  - 앞이라면: `D3D11CreateDevice`와 `CreateSwapChain` 경로를 hook합니다.
- 프록시 `dxgi.dll` 방식(vrperfkit과 같은 방식)은 쓰지 않습니다.
- **선결 작업 (문제 5):** 설치 폴더의 vrperfkit 잔재인 `dxgi.dll`, `vrperfkit.yml`, `vrperfkit.log`를 별도 백업 폴더로 옮깁니다. hook DLL이나 RenderDoc과 충돌할 1순위 후보입니다.

### 3.2 Hook 관리자

- 라이브러리는 [SafetyHook](https://github.com/cursey/safetyhook)을 권장합니다. x64 inline hook과 mid-function hook을 지원하며, 함수 중간 지점 hook이 많이 필요하므로 이게 유리합니다. 대안은 MinHook입니다.
- 주소는 RVA를 하드코딩하지 않고 signature(바이트 패턴)로 찾습니다. 찾은 RVA를 10–12번 문서의 RVA와 대조해서, 다르면 그 hook만 비활성화합니다.
- hook마다 이름, Tier, 활성 상태, 호출 횟수, 마지막 호출 frame id를 명령 서버와 오버레이에 노출합니다.

### 3.3 스키마 레지스트리 = 우리 API

10–12번 문서에 산문으로 흩어진 offset과 구조를 기계가 읽는 형식으로 옮깁니다. 이것이 "우리만의 API"의 실체입니다.

```yaml
# schema/1.61.1.1/mirror_camera.yaml
type: mirror_camera
owner: visual_interior + 0x13A8   # 9슬롯 배열
fields:
  - name: pose_render
    offset: 0x???          # 문서 12에서 확정한 값으로 채움
    type: transform_q      # 위치 float3 + quaternion wxyz
    verified: rpm-2026-10-08
    evidence: 12_dx11_mirror_render_path.md#미러-자세와-운전석-시점-의존성
    writable: tier2
  - name: projection
    offset: 0x???
    type: mat4_row_major
    verified: rpm-2026-10-08
    writable: tier2
```

- DLL과 Python 클라이언트가 같은 스키마를 읽습니다. 예: `ot.read("mirror_camera[5].projection")`
- 쓰기는 `writable` 필드만 허용합니다. 쓰기 전 값을 저장해 두었다가 해제할 때 복원합니다.
- `verified`가 없는 필드는 읽기만 허용합니다.
- ReClass.NET에서 재구성한 구조체를 이 형식으로 내보내는 작은 변환기를 둡니다.

### 3.4 명령 서버와 데이터 평면

- **제어 평면:** named pipe `\\.\pipe\ot`, JSON 요청과 응답.
  - 명령: `ping`, `version`, `hooks`, `read`, `write`, `watch`, `snapshot`, `patch`, `unpatch`, `tier`, `panic`.
- **데이터 평면:** 공유 메모리 링 버퍼.
  - `Local\OT_State`: frame 스냅샷.
  - `Local\OT_Bundles`: 이미지, Z, mask, pass 상수.
  - 슬롯 헤더에 `seq`, `frame_id`, 상태(`writing`/`ready`/`reading`)를 둡니다. 생산자는 `reading` 상태인 슬롯을 덮어쓰지 않습니다(03번 원칙).
- 네트워크 소켓은 열지 않습니다. Foxglove 연결은 Python 쪽에서 합니다.

### 3.5 frame 스냅샷

- 외부 읽기가 원자적이지 않은 문제를 여기서 해결합니다. 엔진의 정해진 지점에서 hook이 스키마 필드 목록을 한 번에 복사하고, frame id와 함께 `OT_State`에 기록합니다. 지점 후보는 12번 문서의 "프레임 안의 가시성·모델 선택·미러 제출 순서"에서 미러 준비가 끝난 직후입니다.
- 이 스냅샷에 담긴 자차 렌더 pose, AI 목록, 신호, 미러 카메라 행렬이 센서 묶음의 메타데이터가 됩니다.
- SDK 값은 별도 필드로 함께 저장합니다. SDK `frame_end`는 렌더 완료 시점이 아니므로 시각 기준으로 쓰지 않습니다(04번).

### 3.6 인게임 오버레이

- Dear ImGui DX11 backend를 Present hook에 붙입니다.
- 표시 항목: 현재 Tier, hook 상태, frame id, 캡처 Hz, 링 점유율, 미러·센서 썸네일, 패닉 버튼.
- 운전용 상태 UI와 디버그 UI는 분리합니다(03번 원칙).

### 3.7 렌더 탭

목표는 각 mirror pass가 끝난 직후, 리소스가 재사용되기 전에 복사하는 것입니다. 미러 0과 2는 같은 depth와 attributes 리소스를 씁니다.

**hook 지점 찾는 순서**

1. DLL을 끈 상태에서 RenderDoc으로 캡처하고, 엔진이 debug marker(`ID3DUserDefinedAnnotation::BeginEvent`나 PIX marker)를 내보내는지 확인합니다. 내보낸다면 marker 경계가 곧 pass 경계이므로 가장 싸고 견고한 hook 지점입니다.
2. marker가 없으면 RenderDoc의 호출 스택에서 실행 파일 주소를 찾고, Ghidra에서 render graph의 pass 실행 함수를 추적해 pass 종료 지점을 hook합니다. 그래프 구성 함수 `0x4D4450`의 소비자부터 봅니다.
3. 마지막 수단은 `OMSetRenderTargets` hook에서 알려진 RT 포인터가 언바인드되는 시점을 감지하는 방식입니다. 0과 2의 리소스 공유 때문에 바인딩 순서를 추적해야 합니다.

**deferred context 확인:** 엔진이 `FinishCommandList`와 `ExecuteCommandList`를 쓰는지 먼저 봅니다. 쓴다면 복사 명령을 같은 command list에 넣거나, Execute 직후 immediate context에 넣어야 합니다.

**복사 방식**

- pass 종료 시점에 `CopyResource`로 자체 DEFAULT 텍스처 링에 복사합니다(GPU→GPU).
- event query로 완료를 확인한 뒤, N+2 frame 이후 STAGING으로 옮겨 `Map`합니다. `D3D11_MAP_FLAG_DO_NOT_WAIT`를 써서 렌더 스레드를 막지 않습니다.
- 복사 대상:
  - `composition_raw`: RGBA16F
  - `attributes_0`: RGBA16F, `.xyz` = 법선, `.w` = Z
  - `attributes_3`: RGBA16UI mask
  - 비교용 DSV: `D32_FLOAT_S8X24_UINT`. typeless 복사와 readback 경로는 실제 device에서 확인해야 합니다.
- 같은 callback에서 그 pass의 view, projection, viewport, ray 상수, frame id를 함께 기록합니다.

### 3.8 카메라 리그 (문제 2)

- 새 카메라를 생성하지 않습니다. 기존 `mirror_camera` 6개를 센서로 전용합니다.
- **쓰는 시점:** 엔진은 매 frame pose를 다시 계산하므로, 그 계산 뒤이면서 가시성·LOD 계산 전에 덮어써야 합니다. 후보는 선택된 미러 카메라 갱신(`0x538860`의 vtable `+0x68` 호출) 직후입니다. 이 지점이 맞는지 M4 첫 단계에서 검증합니다.
- **쓰는 값:**
  - pose: 렌더 시각의 본체 자세(12번 "frame 버퍼") × base_link 기준 센서 extrinsic.
  - projection: 반전 없는 pinhole과 지정 FOV. 미러 projection에 좌우 반전이 있는지부터 확인합니다.
  - 해상도: `r_mirror_scale`과 mod 설정.
- **기준 좌표:** 캐빈 서스펜션을 제외한 샤시 기준을 기본으로 합니다.
- **강제 갱신:** 미러 선택은 시야 검사와 HUD 요청의 합집합입니다. 센서 슬롯은 선택 bit를 항상 켭니다.
- 이 단계에서는 사용자 미러를 잃는 것을 감수합니다. 미러 확인은 디지털미러 UI나 오버레이 썸네일로 대신합니다.

### 3.9 커스텀 mod 패키지

`ot_sensor_mod`를 최상위 load order에 둡니다. 데이터만 바꾸고, 코드 변경은 DLL에서만 합니다.

- mirror_data와 카메라 정의: 필요한 슬롯(`cam_m_h` 등)을 활성화해 mirror_camera 객체를 최대한 확보합니다.
- 설정: `r_mirror_scale_x/y`, `r_mirror_view_distance`, `r_deferred_mirrors`, 그리고 motion blur, TAA 등 기하 검증을 방해하는 효과를 끕니다. 설정은 연구 프로필별로 파일로 남깁니다.
- 처음에는 다른 mod 없이 이 mod만 켭니다. mirror_data를 덮어쓰는 mod와 충돌한 사례가 보고되어 있습니다(08번).

### 3.10 누락 수술 (문제 3)

LOD, Visibility area, cut plane이 모두 메인 게임 카메라를 기준으로 동작한다는 점은 이미 확인했습니다. **먼저 M5에서 측정하고, 누락이 실제로 확인될 때만 수술합니다.** 효과 대비 위험이 낮은 순서로 시도합니다.

1. 설정으로 `r_mirror_view_distance`를 확장합니다.
2. 미러 제외 bit 검사 분기를 센서 슬롯에서만 무시합니다(Tier 3).
3. 차량 LOD 선택 기준을 센서 카메라로 바꾸거나 최고 LOD를 강제합니다(Tier 3).
4. Visibility area 판정 기준을 바꿉니다(Tier 3, 위험이 가장 큽니다).

### 3.11 입력

M6 이후 작업입니다.

- 입력 소유자는 하나로 둡니다. 먼저 우리 DLL 안에서 SCS Input SDK의 가상 입력 장치(공식 경로)를 검토합니다. 부족하면 ETS2LA 방식의 메모리 override를 스키마 필드로 옮겨 옵니다.
- 명령 유효기간은 0.2 s로 하고, 패닉이나 프로세스 종료 시 즉시 해제합니다.

### 3.12 기존 도구와의 분담

| 도구 | 용도 |
| --- | --- |
| Cheat Engine | 값 검색, 변화 추적, 새 필드 탐색 |
| ReClass.NET | 구조체 재구성 → 스키마로 내보내기 |
| x64dbg + Prism3D Unit Resolver | 런타임 호출 추적, 객체 클래스 확인 |
| Ghidra | 정적 분석, 함수 이름 DB |
| RenderDoc | pass, 리소스, marker, 호출 스택 |

**직접 만드는 것:** 스키마 레지스트리, hook 시점 스냅샷, 렌더 탭, 카메라 리그, 패닉과 복구, 명령 서버.

RenderDoc 캡처는 hook 충돌을 피하려고 우리 DLL을 끈 상태에서 합니다.

---

## 4. 마일스톤

### M0. 수술대 설치

- **작업**
  - vrperfkit 잔재 이동, exe 해시 기록, 버전 게이트.
  - SDK 플러그인 골격: C++20, CMake, MSVC x64.
  - 로그, 크래시 덤프(`MiniDumpWriteDump`).
  - 명령 서버의 `ping`, `version`, `read`.
  - ImGui 오버레이, 패닉 키.
- **완료 기준**
  - 30분 주행 중 충돌이 없습니다.
  - 패닉 후 활성 hook이 0개입니다.
  - 게임 종료 시 깨끗하게 언로드됩니다.
  - `read`로 읽은 자차 pose가 SDK 값과 일치합니다(04번의 변환 규약 적용).
- **막히면:** 플러그인 로드 순서와 device 생성 순서부터 로그로 확인합니다.

### M1. hook 지점 확정

- **작업:** DLL을 끈 상태로 RenderDoc 캡처를 뜨고 다음을 확인합니다.
  - marker 유무
  - deferred context 사용 여부
  - mirror pass 실행 순서와 0·2 재사용
  - 호출 스택 → 실행 파일 함수
- **완료 기준:** mirror5 pass 종료 지점 1개를 signature와 이름으로 hook 목록에 등록합니다.
- **막히면:** RenderDoc이 현재 빌드에 붙지 않을 수 있습니다. 그 경우 PIX나 Nsight Graphics로 시도하고, 그래도 안 되면 3.7절의 3번 방식(`OMSetRenderTargets`)으로 갑니다.

### M2. 첫 픽셀

- **작업**
  - mirror5의 `composition_raw`, `attributes_0`, `attributes_3`를 저해상도로 readback합니다.
  - 원시 데이터는 EXR이나 npy로 저장하고, 확인용 PNG도 만듭니다.
  - 그다음 mirror 0, 1, 2로 넓힙니다. 0과 2의 depth가 서로 다르게 나오는지가 핵심 검증입니다.
- **완료 기준**
  - RGB 윤곽과 Z 윤곽이 일치합니다.
  - frame id가 단조 증가합니다.
  - 사용자 화면 FPS 변화를 측정값으로 남깁니다.
- **막히면:** 06번 문서의 "막힌 지점별 다음 조사" 표를 따릅니다.

### M3. 정합과 단위 (문제 4 포함)

- **작업**
  - 같은 callback에서 얻은 view, projection, ray 상수로 Z를 카메라 좌표 점군으로 복원합니다.
  - frame 스냅샷의 AI 차량 박스를 이미지에 투영합니다.
- **완료 기준**
  - 투영한 박스가 RGB 속 차량과 겹칩니다.
  - 거리 구간(0–25, 25–50, 50–100, 100 m 이상)별로 박스 영역 Z와 박스 거리의 오차 표를 만듭니다.
  - 게임 길이 단위가 미터인지 확정합니다.
  - FP16 Z와 D32 DSV를 비교해 원거리 정밀도를 판단합니다.
  - `r_mirror_scale`을 1, 2, 4로 바꿔 가며 비용 표를 만듭니다.

### M4. 카메라 리그: 1뷰 → 4뷰 → 6뷰

- **작업:** 3.8절의 덮어쓰기 시점을 검증하고, 센서를 1개, 4개, 6개 순서로 적용합니다(04번 배치 후보).
- **완료 기준**
  - 운전석에서 고개를 돌리거나 FOV를 바꿔도 센서 이미지가 변하지 않습니다.
  - 6뷰가 같은 frame id로 묶입니다.
  - 뷰 수 × 해상도 × FPS 비용 표를 만듭니다.
- **막히면:** 센서가 고개를 따라 움직이면, 덮어쓰기 시점이 pose 재계산보다 앞에 있는 것입니다. 이미지가 갱신되지 않으면 선택 bit를 확인합니다.

### M5. 누락 측정과 수술 (문제 3)

- **지표:** 센서 frustum 안에 있는 AI 차량 중 박스 영역에 Z가 실제로 채워진 비율을, 거리별·뷰별로 냅니다. 후방 뷰를 중점으로 봅니다.
- **완료 기준:** 누락률 표를 만들고, 필요하면 3.10절의 수술을 적용한 뒤 다시 측정합니다.

### M6. 묶음 출력

- **작업:** 4–6뷰 묶음을 10 Hz로 `OT_Bundles`에 내보내고, otpy가 이를 MCAP 기록과 Foxglove로 연결합니다.
- **완료 기준:** 10분 주행을 기록하고, 다시 재생했을 때 뷰, Z, 박스, 자차 pose가 같은 시각에 정렬됩니다.
- **이후 판단:** 엔진의 9슬롯 너머로 슬롯을 확장할지, 해상도를 올릴지, 입력 연결(3.11절)로 넘어갈지 정합니다.

---

## 5. 저장소 구조

```text
ot/
  native/                 # ot_core.dll (C++20, CMake, MSVC x64)
    loader/  gate/  hooks/  schema/  snapshot/
    rendertap/  rig/  overlay/  server/  panic/
  schema/1.61.1.1/*.yaml  # 확정 구조 = 우리 API
  signatures/1.61.1.1.yaml
  py/otpy/                # 클라이언트, 스키마 접근, 링 reader, recorder
  mod/ot_sensor_mod/
  tools/  reclass/  x64dbg/  renderdoc/  schema_export/
  docs/                   # 01–13 + 마일스톤 기록
  research/               # 기존 원시 자료 유지
```

---

## 6. 문서 규칙 (문제 6)

- 모든 문서는 첫머리에 **상태 3줄(확정 / 미확정 / 다음)**을 둡니다.
- 확정된 offset과 구조는 **스키마 파일에** 기록합니다. 문서에는 근거와 해석만 남깁니다.
- 불확실성은 문서 끝 **"미확정" 표 하나에** 모읍니다. 문단마다 "단정하지 않습니다"를 반복하지 않습니다.
- 실험 기록은 수치와 이미지를 먼저, 해석을 그다음에 둡니다.
- 마일스톤이 끝날 때마다 08번 표를 갱신합니다.

---

## 7. 하지 않을 것

- 범용 메모리 스캐너나 디버거를 다시 만드는 일.
- 시간 감속이나 순차 시점 전환(01번 결정 유지).
- 멀티플레이 세션에서의 사용.
- M4 이전의 엔진 슬롯 확장.
- M6 이전의 모델 학습.
- 별도의 테스트 프레임워크(06번 원칙 유지). 완료 판정은 실제 게임 영상과 수치로 합니다.

---

## 8. 리뷰 문제점과 대응

| # | 문제 | 대응 위치 |
| --- | --- | --- |
| 1 | 분석이 구현보다 앞서 있고 픽셀이 없음 | 1절 전환, 3.7절 렌더 탭, M1–M2 |
| 2 | 기존 미러 슬롯을 활용한 6뷰가 뒤로 밀려 있음 | 3.8절 카메라 리그, M4 |
| 3 | 메인 카메라 기준 culling과 LOD로 인한 누락 | 3.10절, M5 |
| 4 | 미러 해상도와 FP16 Z 정밀도 | M3 비용 표와 Z/D32 비교 |
| 5 | 설치 폴더의 vrperfkit `dxgi.dll` | 3.1절 선결 작업, M0 |
| 6 | 문서가 방어적이라 결론이 묻힘 | 6절 문서 규칙, 3.3절 스키마 이동 |
