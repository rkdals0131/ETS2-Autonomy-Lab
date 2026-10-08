# DX11 미러 렌더 경로와 실제 리소스 연결

이 문서의 외부 읽기 기록은 DLL 도입 전 조사다. 이후 DLL 0.8.2에서 네 미러의 픽셀·CPU/GPU 카메라 상수·DSV 수집까지 진행했으며 실제 구현 상태는 [ot 문서](../ot/README.md)를 따른다. 후속 재질 분석은 맨 아래 [잎 billboard의 Z 보정과 DSV 차이](#잎-billboard의-z-보정과-dsv-차이)에 추가했다.

**미러 0·1·2·5의 이름, 렌더 그래프 항목, 실제 DX11 색상·깊이 텍스처를 외부 메모리 읽기로 연결했다. 셰이더에서는 일반 색상 텍스처 `attributes_0.w`에 카메라 좌표계 Z가 들어가는 경로도 확인했다. 실제 복원 상수에 이어 미러별 준비 작업의 투영행렬·viewport도 읽었다. 미러 장면의 viewport 깊이 범위는 약 0.01–0.9이며, 메인 실내는 0.9–1.0이었다. 픽셀과 같은 프레임으로 연결한 결과는 아직 없다.** 미러 0과 2가 같은 깊이·중간 색상 리소스를 재사용하므로, 후속 수집기는 각 미러의 렌더 완료 시점에 복사해야 한다. 화면 Present 시점의 일괄 복사만으로 네 미러의 depth를 확보할 수 있다고 가정하면 안 된다.

관측 대상은 2026-10-08의 사용자 소유 ETS2 1.61.1.1 프로세스 PID 24940이다. 게임 함수 호출, 디버거 연결, 메모리 쓰기, DLL 설치·주입, 입력, 설정 변경은 수행하지 않았다. 아래 offset은 이 EXE에만 해당하며 주소는 재시작 후 달라진다. CPU 자료구조를 여러 번 읽은 결과이므로 한 게임 tick 또는 GPU frame의 원자적 스냅샷은 아니다.

## 실제로 연결한 색상과 깊이

렌더 그래프에는 미러 번호를 명시한 `mirror0`, `mirror1`, `mirror2`, `mirror5` namespace가 있었다. 각 namespace에 `attributes_0..3`, `depth_stencil`, `diffuse_lighting`, `specular_lighting`, `composition_raw`가 있다. 최종 미러 출력은 `drawable` namespace에서 reflection TOBJ 이름으로 식별된다. 크기만 보고 미러라고 추정한 결과가 아니다.

| 미러 | 최종 reflection 출력 | 최종 출력 image ID | 중간 색상 / depth 크기 | 중간 색상 ID | depth ID |
| --- | --- | ---: | --- | ---: | ---: |
| 0 close | 256×512 | 893 | 512×1024 | 1886 | 2108 |
| 1 close_s | 256×256 | 1219 | 512×512 | 1983 | 2951 |
| 2 far | 256×512 | 2224 | 512×1024 | **1886** | **2108** |
| 5 front | 256×128 | 2378 | 512×256 | 1888 | 2038 |

- 최종 출력 format 26: `DXGI_FORMAT_R11G11B10_FLOAT`.
- `composition_raw` format 10: `DXGI_FORMAT_R16G16B16A16_FLOAT`.
- `depth_stencil` format 20: `DXGI_FORMAT_D32_FLOAT_S8X24_UINT`, bind `0x40`（depth-stencil）.
- 색상 bind `0x28`은 render-target + shader-resource다. 위 리소스 모두 sample count 1, CPU access 0, 공유 관련 misc flags 0이었다.
- `attributes_0..2`는 format 10, `attributes_3`은 format 12였다. 후속 shader 분석에서 기본 diffuse 재질의 `attributes_0.xyz`는 법선, `.w`는 카메라 공간 Z임을 확인했다. 모든 재질·투명도 변형을 해석한 것은 아니다. 자세한 근거는 다음 절에 있다.

현재 설정 `r_mirror_scale_x/y = 2`와 중간 버퍼의 2배 크기가 대응한다. 다만 최종 출력은 기본 크기이므로 "미러 텍스처 해상도"를 한 숫자로 기록하면 중간 렌더와 출력 해상도를 혼동하게 된다. `main0`의 중간 색상·depth는 3840×2160, backbuffer는 1920×1080이었다. `ymirror0..2`에서는 dashboard/map/GPS용 별도 출력도 관측했다. 이들을 도로를 바라보는 추가 미러 카메라로 세지 않는다.

미러 0과 2의 depth ID와 실제 `ID3D11Texture2D*`가 같은 렌더 그래프 버퍼 내에서 동일한 사례를 40회 기록했다. 중간 색상도 같은 ID를 쓴다. 이는 서로 다른 논리적 이미지의 물리 리소스 재사용을 확인한 것이다. GPU 명령 실행 순서와 픽셀 덮어쓰기 자체는 아직 캡처하지 않았다. **따라서 수집 위치를 잘못 고르면 두 카메라에 같은 depth를 붙이는 실제 설계 위험이 있다.** 후속 구현에서 pass별 복사와 카메라 연결을 먼저 해결해야 한다.

## 셰이더에서 확인한 Z·법선과 우선 수집 경로

`effect.scs`의 deferred 효과 정의·SM5X 셰이더 1,316개를 선택 추출한 뒤, 비교할 GLSL 및 기본 diffuse 재질을 추가했다. 현재 선택 추출본은 1,329파일, 12,204,311 bytes다. 원본 게임 파일은 변경하지 않았다. `.rfx`는 효과·pass·shader 파일의 연결을 제공한다. SHDO 안의 DXBC를 분리하고 Windows SDK `fxc /dumpbin`으로 해석했다. 같은 효과의 GLSL 텍스트는 의미를 대조하는 보조 근거로 사용했으며, 실행 중인 API를 OpenGL로 바꾸지 않았다.

확인한 경로는 다음과 같다.

- `eut2.dif`의 **defattr pass**: DX11 pixel shader가 `SV_Target0.xyz = normalize(TEXCOORD0.xyz)`, `SV_Target0.w = TEXCOORD1.z`를 기록한다. 같은 vertex shader의 TEXCOORD0은 변환된 normal, TEXCOORD1은 카메라 공간 위치다. clip-space position과는 다른 계산이다. GLSL의 출력 및 후속 소비 셰이더도 이를 뒷받침한다.
- `def.ssao.zfetch`: `attributes_0.w`를 Z로 읽는다. 0 또는 `attributes_3`의 특정 mask bit에 해당하면 `-65504`를 출력한다. 일반 D3D의 비선형 depth 값에 near/far 공식을 적용하는 경로가 아니다.
- `def.fog`: `attributes_0.w × (ray_x, ray_y, 1)`로 카메라 공간 위치를 복원한 후 벡터 길이를 계산한다. ray는 `deferred_param_viewport_coord_to_es_ray`와 viewport 관련 상수에서 계산된다. 따라서 `.w`는 광선 방향의 유클리드 거리가 아니라 **Z 성분**이다.
- `attributes_3`에서 얻는 bit mask는 `((w >> 13) & 7) | ((z & 3) << 3)`다. bit 16이 켜진 경우 fog는 interleaved fragment layer에 맞춰 이웃 pixel을 읽는 별도 분기를 사용한다. 이것이 모든 투명 물체를 어떻게 표현하는지는 아직 미해석이다.

실제 메모리에서 연결한 `attributes_0` ID는 mirror0/2 = **1136**, mirror1 = **1015**, mirror5 = **386**이다. format은 `R16G16B16A16_FLOAT`, bind는 RT + SRV다. `attributes_3` ID는 각각 **1191, 2407, 1620**이며 format은 `R16G16B16A16_UINT`다. 0·2의 attributes도 물리 리소스를 재사용한다.

이에 따라 **첫 depth 수집은 `attributes_0`를 원시 RGBA16F로 복사하고 `attributes_3`를 함께 보관하는 방법을 우선한다.** DSV 복사와 depth SRV 생성에 앞서 이미 존재하는 Z·법선 경로를 활용할 수 있다. 같은 pass의 ray·view 변환·viewport가 필요하며 픽셀을 실제로 받기 전에는 거리 단위·부호·화면 방향을 확정하지 않는다. FP16에 저장된 Z의 정밀도도 D32와 같지 않다. 예를 들어 크기 100 부근의 표현 간격은 0.0625, 400 부근은 0.25 게임 길이 단위다. 이는 저장 형식에서 계산한 간격이며 실제 거리 오차 측정값은 아니다.

shader 연결 목록은 `research/findings/deferred_shader_mapping.json`, 원본은 `research/extracted/effect/`, 분리한 DXBC·disassembly·GLSL 텍스트는 `research/extracted/effect-analysis/`에 있다. 추출된 게임 자료는 기존 `.gitignore` 범위 안에 둔다. 이 결과는 정적 shader 해석이며 해당 draw가 관측 순간에 실행됐다는 GPU trace는 아니다.

## CPU 메모리에서 GPU 객체까지의 경로

```text
base_ctrl + 0xA8 array[mirror index]
  → drawable_data_t (0x168 bytes)
       +0x38 : 설명 문자열 / reflection 이름
       +0x160: r_texture_id_t
  → rendergraph_image_t (0x7F0 bytes)
       이름·namespace로 위 drawable 및 mirrorN에 연결
       +0x740: r_image_id_t
  → renderer + 0x363FE58의 dx11_image_object_t pool
       image ID × 0xA8
       +0x58: ID3D11Resource* / 관측된 Texture2D 객체
```

실행 파일 RVA `0x3681860`의 전역 포인터가 renderer를 가리킨다. renderer `+0x363FD18`은 DX11 device 포인터다. `dx11_device_t::image_create_rtv`의 RVA `0x2B8FD0`에서 image pool, stride, `+0x58` 필드가 `ID3D11Device::CreateRenderTargetView`로 전달되는 것을 확인했다.

렌더 그래프 manager는 EXE RVA `0x304FC50`에 있다. 세 버퍼의 간격은 `0x2A8`, 각 버퍼의 첫 필드는 표준 array이며 `rendergraph_image_t` stride는 `0x7F0`이다. 현재 버퍼 index는 RVA `0x3050448`이다. accessor `0x21CEC0`의 인덱싱 및 assert의 실제 타입 이름으로 구조를 확인했다. image ID의 `+0x740` 의미는 생성 함수 `0x21D460`과 외부 텍스처 연결 함수 `0x21D100`에서 확인했다.

그래프 항목의 이름 문자열 포인터는 `+0x08`, namespace 문자열 포인터는 `+0xA8`, format은 `+0x1E4`, width/height는 `+0x1EC/+0x1F0`이다. `+0x730`에는 그래프 index가 있다. 서로 다른 프레임에 재사용되므로 index·포인터를 영구 카메라 ID로 저장하면 안 된다.

### 이름 있는 정적 텍스처와 렌더 출력의 차이

renderer `+0x1A75F98`은 일반 문자열 테이블이 아니라 `array_t<r_texture_object_t>`다. stride `0x80`, 이름 포인터 `+0x18`, image ID `+0x7C`다. drawable의 `+0x160`은 이 풀의 texture ID이며 하위 14비트가 index다. 상위 비트 의미 전체는 아직 해석하지 않았다.

reflection TOBJ 이름으로 이 풀만 조회하면 64×128 / 128×128 / 128×64의 작은 BC3/BC1 텍스처가 나온다. 이것은 위 표의 동적 렌더 결과가 아니다. 미러 렌더 준비 함수 `0x4D4450`이 drawable 출력의 그래프 ID를 구한 뒤, `0x3279C0`을 통해 **texture ID → graph image ID** alias를 등록한다. 전역 alias array는 RVA `0x2D2B938`, stride 16이며 texture ID `+0`, graph ID `+4`, 적용 mask `+8`이다. 이를 따라가야 실제 출력에 도달한다.

기존 조사에서 `name_handle`, 이름 테이블 또는 descriptor `type_byte`로 남긴 원시 필드는 이 후속 해석과 함께 읽어야 한다. descriptor 첫 byte의 관측값 26은 출력 format 26과 대응한다. raw JSON의 과거 키 이름 자체를 타입의 근거로 사용하지 않는다.

## 렌더 제출과 가시성

`base_ctrl` 함수 `0x4DB6D0`은 `visual_interior`의 미러 카메라와 `base_ctrl +0x3AC0` 제출 배열을 `0x538860`에 전달한다. 제출 레코드 stride는 `0x98`, 레코드 `+0x10`은 미러 index, `+0x08`은 렌더용 카메라 포인터다.

`0x538860`은 9개 슬롯을 순회하며 여러 조건에서 만든 bit mask로 제출 대상을 고른다. `0x537C20`, `0x537FB0`의 결과와 호출자 mask가 합쳐진다. camera index 2/12, 미러 설정, 인테리어 상태가 조건에 사용된다. 각 조건의 제품 설정 이름과 F2·고개 회전 효과 전체를 아직 매핑하지 않았다.

후속 추적에서 두 함수가 읽는 설정은 **`r_mirror_group`**임을 확인했다. EXE RVA `0x2D3CB60`의 설정 객체 `+0x08`에 그 이름이 있고, 정수 getter `0x1CBD70`은 연결 객체 `+0x120`을 따라간 뒤 `+0x116`이 유효하면 `+0x118`의 값을 반환한다. 외부 관측에서 연결 객체는 없었고 캐시가 유효했으며 값은 3이었다. 함수를 원격 호출한 것은 아니다.

미러 선택 경로는 다음 세 mask의 합집합이다. bit 번호는 미러 slot 번호다.

| 입력 | 확인한 조건 | 해석 범위 |
|---|---|---|
| `0x537C20`의 결과 A | 활성 camera index가 2 또는 12이고 `r_mirror_group != 0`. 실내의 별도 활성 byte·부품 포인터 또는 특수 slot 상태를 검사 | 기하학적 시야 검사 없이도 선택하는 경로. 해당 byte와 UI 설정의 연결은 미확인 |
| `0x537FB0`의 결과 B | 같은 camera index·그룹 조건 후 현재 카메라와 미러 배치를 이용한 시야 검사 | 반경 약 0.3의 구와 시야 영역을 대조하는 보수적 검사. 화면 픽셀이나 가림 여부의 측정이 아님 |
| `0x538860` 인수 R8D의 C | `*(base_ctrl +0x31B0)`의 `+0x30 → game_actor_hud`, 그 객체의 `+0x13C` | A/B와 별개인 HUD 미러 요청. 아래 후속 추적에서 `showmirrors` 입력과 `g_mirrors` 모드에 연결 |

`0x5389C7`과 `0x5389CA`가 `B | A | C`를 만든다. 따라서 시야 검사 B에서 제외됐다는 사실만으로 해당 미러가 제출되지 않는다고 판단하면 안 된다. 반대로 C의 bit가 켜져도 상위 객체·카메라 존재 검사와 다른 제출 조건을 통과해야 하므로 GPU 갱신 보장은 아니다.

그룹 값은 A/B에 다르게 적용된다. B는 값 1에서 slot 0·2만 검사하고, 값 2에서 slot 1·3을 건너뛴다. 값 3은 그 두 제한에 걸리지 않는다. A의 slot 1·3은 값 >2, slot 4·5·6은 값 >1을 추가로 요구한다. 두 함수 모두 값 0이면 0을 반환하지만, 최종 선택에는 별도 C가 합쳐진다. 이 코드 해석을 실제 F2 동작이나 사용자 설정 조합의 실험 결과로 간주하지 않는다.

2026-10-08의 후속 표본은 active camera index 2, `r_mirror_group = 3`, C=`0x4`（slot 2）였다. `visual_interior +0x14C1..0x14C4`의 활성 byte와 A가 검사하는 부품 62–65의 포인터는 모두 0, `+0x15D8/+0x15DC`는 -1이었다. 그 입력으로 A의 코드를 해석하면 0이다. B의 중간 기하 자료나 최종 반환값을 같은 시점에 포착하지는 않았으므로 B 또는 최종 제출 mask의 값을 역으로 단정하지 않는다. 현재 가시성을 무시하고 추가 센서를 유지하려면 C를 만드는 경로와 상위 제출 조건이 조사·구현 지점이 된다. 이번 조사에서는 그 값을 변경하지 않았다.

시야 검사 함수 `0x13D2120`은 cell 상대 위치, 거리 제한, 전후·가로·세로 경계를 대조한다. 호출의 radius는 EXE RVA `0x251C10C`의 float `0.3000000119`였다. 보조 시야 목록도 검사하는 분기가 있어 단일 화면의 단순 사각형 판정으로 축약하지 않았다. 근거는 `mirror-mask-source-a.txt`, `mirror-mask-source-b.txt`, `mirror-submission-entry.txt`, `game-actor-submit-mirrors.txt`, `cvar-integer-getter.txt`, `mirror-visibility-test.txt`다. 입력 관측은 `research/live/2026-10-08-render-path/mirror-selection-inputs.json`에 있으며 15회, 4,508 bytes, reader 측 약 0.535 ms였다. observer PID 5064의 handle 및 프로세스 종료를 확인했다.

### HUD 요청의 생성과 숨김 설정

**C는 임의 센서용 목록이 아니라 기본 HUD 미러 두 개의 요청 mask였다.** 객체의 타입 descriptor는 `game_actor_hud`, 실행 vtable RVA는 `0x228FEF0`이었다. `0x68A130`은 매번 `HUD +0x13C`를 0으로 초기화한 뒤 slot 0·2의 bit만 설정한다. 일반 모드의 대응은 다음과 같다.

| `g_mirrors` 모드 | 요청 slot | C |
|---:|---|---:|
| 0 | 없음 | `0x0` |
| 1 | 2 | `0x4` |
| 2 | 0, 2 | `0x5` |
| 3 | 0 | `0x1` |

생성자 `0x686AB0`은 입력 token `0x0297AB3AD4F296B5`를 조회해 `HUD +0x1A8`에 보관한다. 이 값은 SCS의 base-38 문자열 표현에서 `showmirrors`이며, live 입력 객체의 `+0x08/+0x28`에서도 같은 값을 읽었다. 추출한 게임 `def/color_feedback.sii`는 `showmirrors`에 기본 `keyboard.f2` 주석을 붙인다. `0x689480`은 이 입력 객체의 상태 `+0xF8 == 2`일 때 `0x689FF0`을 호출하고, 그 함수가 `g_mirrors = (g_mirrors + 1) & 3`을 기록한 뒤 `0x68A130`으로 이어진다. 사용자의 현재 키 재지정이나 F2 입력 결과를 실험한 것은 아니다.

모드는 EXE RVA `0x2D368A0`의 `g_mirrors`, 별도 기억값은 `0x2D369E0`의 `g_mirrors_visibility_setup`이다. 순환 함수는 active camera index가 2일 때 기억값의 하위 2 bits, 다른 index일 때 bits 2–3을 갱신한다. `0x68A130`에는 카메라의 별도 boolean이 켜졌을 때 모드 2→1, 3→0으로 바꾸는 분기도 있으므로 위 표는 그 보정이 없는 경우다.

**index 2에서 사용하는 boolean은 운전석 카메라의 유효 `outside` 상태였다.** `0x8AF2D0`은 선택된 `camera_azimuth_range +0x18`을 운전석 카메라 `+0x578`에 복사하고, 범위가 없으면 카메라 자체의 `+0x548`을 복사한다. EXE의 reflection 등록 자료는 두 원본 필드 모두를 `outside`로 명명한다. 따라서 단순히 고개를 조금 돌렸다는 조건이 아니라, 선택된 시점 범위 또는 카메라 설정이 바깥 시점으로 지정된 경우의 보정이다. index 12가 사용하는 별도 getter의 의미까지 이 결과로 일반화하지 않는다. 근거는 `interior-reference-offset-update.txt`와 `camera-outside-reflection.json`이다.

**`g_mirrors_keep_hidden`은 이 함수 안에서 HUD 창 분기와 렌더 요청 mask를 분리한다.** 설정 객체는 EXE RVA `0x2D36B20`이다. 요청된 slot이라도 이 값이 0이 아니면 비활성 모드와 같은 UI 함수를 실행하지만, 뒤의 `+0x13C` bit 설정에는 숨김값을 사용하지 않는다. 따라서 창을 숨기면서 기존 미러 요청을 남기는 코드 경로가 있다. 이는 GPU 갱신·픽셀 확보 실험의 성공을 뜻하지 않는다. 또한 `showmirrors` 입력 처리 경로는 숨김값을 0으로 되돌리므로 F2 조작과 독립된 영구 설정으로 가정하면 안 된다.

후속 관측은 `g_mirrors`의 실제 문자열 값 `"1"`, `g_mirrors_visibility_setup = 1`, `g_mirrors_keep_hidden = 0`, HUD의 보정 boolean 캐시 0, C=`0x4`였다. `g_mirrors`의 정수 캐시 유효 byte는 0이어서, 남아 있던 정수 1을 근거로 삼지 않고 getter가 선택하는 `+0xB1`의 문자열을 해석했다. 서로 다른 시점의 짧은 읽기이며 설정 전환을 실험하지 않았다.

센서 구현에 미치는 제약은 분명하다. 이 HUD 함수를 그대로 재사용하면 0·2 외 slot을 요청할 수 없으며, HUD 필드에 일회성으로 다른 bit를 넣어도 다음 갱신에서 지워질 수 있다. 후속 구현에서는 사용자 HUD 요청을 보존하면서 별도의 센서 요청을 렌더 제출에 합치는 방식을 검토해야 한다. 이 설계 방향은 이번 코드 흐름에서 도출한 제안이며, hook·필드 쓰기·설정 변경을 구현하거나 실행하지 않았다.

근거는 `hud-constructor.txt`, `hud-update.txt`, `hud-mirror-toggle.txt`, `hud-mirror-mode-setter.txt`, `hud-mirror-request-update.txt`다. live 자료는 `mirror-request-owner.json`, `hud-mirror-mode-cvars.json`, `hud-visibility-cvar.json`, `hud-mirror-input.json`에 보존했다. 마지막 입력 객체는 Prism unit과 다른 vtable이어서 일반 `unit_name` 조회가 실패했으며, 그 오류를 타입 판정 근거로 사용하지 않았다. 입력의 식별은 생성자와 token 값을 사용했다. observer PID 4148·9376·30364·23616은 모두 handle을 닫고 종료했다.

5초 외부 관측에서 제출 배열 크기 0 또는 4, alias 수 0–9를 보았다. 대부분의 순간에 제출 목록은 이미 비어 있지만 같은 관측 중 그래프 항목은 존재했다. 따라서 단일 RPM에서 빈 배열을 읽었다는 이유로 미러가 꺼졌다고 결론내릴 수 없다. 현재 namespace 관측은 0·1·2·5이며, 3·4의 영상이 다른 상태에서도 항상 비활성이라고 일반화하지 않는다.

렌더용 카메라 생성 시 `0x87FD70`은 미러 pose·FOV 관련 필드를 복사하고 quaternion을 변환한다. 이후 `0x538860`에서 far를 현재 `r_mirror_view_distance` 값으로 덮어쓰는 경로가 있다. 이어 160회 짧은 관측에서 21회 제출 목록을 잡았고, 미러 0·1·2·5의 실제 렌더 카메라 `+0x44` 값이 모두 **400**임을 읽었다. `+0x9C/+0xA0` FOV 필드는 큰 미러 약 55°/92.309°, 작은 close 90°/90°, front 약 121.555°/83.582°였다. 원본 mirror_camera의 far 500 및 CPU projection을 GPU의 최종 조건으로 사용하면 안 된다.

이 네 렌더 카메라의 `+0x08`이 같은 설정 객체 `0x1f74bb44170`를 가리키는 사례를 관측했다. 후속 조사에서 이 객체는 전역 렌더 상태 `S = *(EXE +0x36AE718)`의 **`*(S +0xBB0)`**와 같았다. `0x4215C0`은 조건에 따라 `+0xBB8` 또는 `+0xBB0`의 deferred context를 선택하고, `0x479930`은 그 경로를 사용할 수 없으면 `+0xBC0`의 별도 context로 내려간다. 앞선 문서에서 `+0xBC0`을 관측된 deferred context의 소유 경로로 적은 것은 잘못이었다. 이번 관측에서 `+0xBB8`은 NULL, `+0xBC0`은 다른 vtable을 가진 `0x1f74a9ef740`이었다.

deferred context vtable의 `+0x68` 설정 함수는 FOV 두 값을 context `+0x40/+0x44`에 기록한다. 따라서 공유 context를 나중에 읽어 각 카메라의 고유 행렬이라고 연결하면 안 된다. 원시 기록은 `render-camera-contexts.json`이다. 최종 GPU view/projection, viewport, 반사·flip·crop은 아직 픽셀과 대조하지 않았다.

### 제출 배열과 9슬롯 제한

**9는 현재 미러 생성·갱신·제출 경로에 들어 있는 고정 개수다. 엔진 전체의 동시 카메라 수 상한으로 확인한 값은 아니다.** 다음 연결을 정적으로 확인했다.

| 단계 | 확인한 동작 | 추가 센서에 미치는 제약 |
|---|---|---|
| 원본 카메라 구성 `0x533360` | `0x5350AB`에서 index를 9와 비교. `visual_interior +0x13A8`에 카메라를 보관 | 배열 크기만 늘려도 이 생성 순회가 늘어나지는 않음 |
| 선택된 카메라 갱신 `0x538860` | `0x538A48`에서 9와 비교. 선택 bit가 켜진 유효 카메라에 vtable `+0x68` 호출 | 새 index에는 갱신·선택 경로가 필요 |
| 제출 레코드 생성 | `0x538CF0`에서 9와 비교. 원본 카메라와 `ctrl +0xA8`의 drawable이 모두 있을 때 0x98-byte 레코드 추가 | mask 하나로 없는 카메라·출력을 생성하지 않음 |
| 렌더 카메라의 적용 mask | `0x538C4B`에서 RVA `0x1E15E80`의 9개 값 중 하나를 골라 렌더 카메라 `+0x18`에 기록 | 슬롯 번호는 다른 렌더 설정과도 연결됨 |
| 그래프 구성 `0x4D4450` | `ctrl +0x3AC0`의 실제 레코드 수를 순회. 레코드 index로 drawable과 원본 카메라를 다시 조회 | 임의 렌더 카메라 포인터만 넣은 독립 요청으로 취급할 수 없음 |

적용 mask 표의 값은 순서대로 `0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000, 0x0800000000000000, 0x1000000000000000`이다. 이는 앞 절의 slot 선택 mask `1 << index`와 다른 값이다. 그래프 구성은 전역 texture alias 기록의 mask가 이 값을 포함하는지 검사해 해당 뷰에 쓸 alias를 선택한다.

`0x4D4450`의 `0x4D458B`는 drawable 배열의 index 범위를 검사하고 NULL drawable이면 건너뛴다. `0x4D46E4`는 원본 카메라 배열의 범위를 검사한 뒤, 그 원본의 `+0x518`을 읽는다. 이 후자의 읽기 앞에는 별도 NULL 검사가 없다. 따라서 제출 레코드를 직접 추가하는 후속 구현은 **렌더 카메라, 원본 미러 카메라, drawable의 소유 관계와 같은 index**를 함께 다뤄야 한다. 현재 6–8 슬롯이 비어 있다는 사실만으로 세 센서를 안전하게 추가할 수 있다고 결론 내리지 않는다.

출력 연결은 drawable에서 얻은 graph image ID를 texture alias로 등록하고, 그 ID를 출력 참조 목록에 추가한 뒤 context에 전달하는 흐름이다. namespace 문자열 형식은 RVA `0x22515C8`의 `mirror%u`다. 기존 리소스 관측의 `mirrorN` 이름과 이 코드 경로가 연결된다. 그래프 구성의 순회 자체에는 9라는 종료 조건이 없지만, 위의 생성·갱신·lookup 의존성은 그대로 남는다.

근거는 `mirror-owner-camera-setup.txt`, `mirror-submission-entry.txt`, `mirror-submission-fragment.txt`, `mirror-render-graph.txt`와 `mirror-output-id.txt`다. 마지막 파일의 실제 함수는 `0x1565AB0`이며, 탐색 당시 붙인 파일명으로 `0x4D4210`의 역할을 단정하지 않는다.

### 미러 자세와 운전석 시점 의존성

**관측된 미러를 그대로 차체 고정 센서로 취급하면 안 된다.** 초기화의 `0x87DC60`은 저장된 placement를 복사하지만, 이후 갱신은 다른 계산을 한다. 선택된 미러의 vtable `+0x68 → 0x63BEE0 → vtable +0x100 → 0x87FD60 → 0x87E170`이 그 경로다.

`mirror_camera +0x518`은 이 갱신에서 0·1·2·3을 구별하는 내부 모드다. 현재 표본의 slot 0–5는 모두 모드 0, `+0x539`는 모두 0이었다. 활성 카메라 index는 2였다. 이 조합에서는 운전석 카메라를 얻은 뒤 `0x8AC960`으로 위치 벡터를 만들고, 이를 `0x87FA40`의 세 번째 인수로 전달한다. 그 helper는 미러 위치에서 입력 위치를 뺀 방향을 정규화한 뒤, 미러 방향에서 얻은 법선 `n`으로 `v - 2(n·v)n`을 계산한다. 결과는 회전 구성과 placement 합성을 거쳐 미러 `+0x40`의 현재 자세에 기록된다.

이는 **운전석 카메라에서 유래한 위치가 현재 미러 방향 계산에 사용된다는 정적 근거**다. 사용자의 고개를 움직이거나 각 입력 필드의 사용자 설정 이름을 모두 확인한 실험은 아니다. 순수한 고개 회전, 좌석 이동, 카메라 흔들림 각각의 변화량도 측정하지 않았다. 다만 초기 placement가 있다는 이유로 센서 외부 파라미터가 차체에 대해 불변이라고 가정할 근거는 없어졌다.

모드 1은 같은 반사 helper에 별도 기준 위치를 전달한다. 그 기준은 `vehicle +0x1070`의 float3와 `*(vehicle +0x1098) +0x4A4`의 float3를 더한 값이다. 후속 live 관측에서 이 하위 객체는 **동일한 운전석 카메라**였다. 따라서 모드 1에 운전석 객체 참조가 전혀 없다고 표현하면 틀리지만, 현재 시점에 적용되는 동적 위치 필드를 읽는 모드 0과는 분명히 다른 경로다. **모드 값을 1로 바꾸기만 하면 독립 센서가 된다고 검증한 것은 아니다.** 아래의 기준 위치·parent 변환과 설정 변경 수명을 함께 다뤄야 한다.

모드 2·3에는 추가 처리도 있다. 그래프 구성 `0x4D4450`은 원본 `+0x51C..0x528`에서 네 값을 만들어 context vtable `+0x70`으로 전달하고, 운전석 카메라의 `+0x4D4` placement 등을 이용한 float4를 vtable `+0x88`에 전달한다. deferred context의 실제 함수 `0x16102E0`은 전자를 `+0x48`에 저장하며, `0x16103B0`은 후자를 16-byte 배열 `+0xC8`의 항목에 저장하고 `+0x118`의 활성 bit를 켠다. 후자는 평면 형태의 계산이지만, 최종 shader·GPU의 clipping 효과 전체는 아직 연결하지 않았다. 현재 모드 0 표본은 이 분기를 사용하지 않는다.

후속 센서 구현에서는 기존 미러의 픽셀·동적 자세를 먼저 함께 수집하되, 차체 고정 센서는 별도의 자세 생성 규칙을 확보해야 한다. 기존 mirror pose를 외부에서 일회성으로 바꾸는 방식은 다음 게임 갱신에서 다시 계산될 수 있다. 이번 조사에서 그런 쓰기나 모드 변경을 수행하지 않았다.

정적 자료는 `camera-update-dispatch.txt`, `mirror-view-pose-update.txt`, `interior-mirror-viewpoint.txt`, `mirror-owner-camera-setup.txt`, `context-mirror-crop-clip.txt`다. 모드 관측은 `mirror-mode-inputs.json`에 저장했다. 19회, 1,124 bytes, reader 측 약 0.233 ms였으며 observer PID 4480의 handle을 닫고 프로세스 종료를 확인했다. 이 시간은 게임 성능 영향의 측정값이 아니다.

### 디지털미러 선택과 별도 기준 위치

**디지털미러 모드는 UI 화면 크기만으로 선택되지 않는다. 이 빌드는 해당 부품의 unit 식별자가 `cam`으로 시작하는지도 검사한다.** `0x544A50`은 부품 객체 `+0x0C`의 식별자를 `0x12A840`으로 문자열화하고, 부품 종류 token을 구별한 뒤 EXE RVA `0x2D57E70`이 가리키는 문자열 `cam`과 앞부분을 비교한다. 표시용 `name` 속성이나 `.sii` 파일명을 비교하는 코드가 아니다.

| 부품 종류 token | 켜지는 `visual_interior` 필드 | 모드 1을 선택하는 미러 slot |
|---|---|---|
| `mirror` | `+0x14C1` | 0–3 |
| `f_mirror` | `+0x14C2` | 5 |
| `s_mirror` | `+0x14C3` | 4 |
| `fl_mirror`, `fr_mirror` | `+0x14C4` | 6 |

부품 종류는 코드의 base-38 token을 해석해 확인했다. `0x533360`의 구성 분기에서 위 활성 필드가 켜지면 `mirror_camera +0x518 = 1`이 된다. slot 7·8도 이 모드를 선택하는 별도 조건이 있지만 현재 관측에서는 카메라가 없었다. 실제 설치 정의의 DAF 디지털미러 unit `cam_plast.daf.2021.mirror`는 이 접두어 조건에 맞는다. 새 이름의 모드를 만들 때 파일명만 `camera_...sii`로 정하는 것은 같은 조건이 아니다. 접두어만 바꾸면 필요한 모델·로케이터·출력까지 자동으로 갖춰진다는 뜻도 아니다.

자세 계산의 입력은 다음처럼 구별된다.

| 값 | 확인한 역할 |
|---|---|
| 운전석 `+0x498` | reflection 이름 `head_offset`인 float3. 카메라 구성 후 사용자 좌석 보정이 더해지고, 좌석 조절 UI도 이 값을 갱신 |
| 운전석 `+0x57C` | 현재 선택된 azimuth 범위의 시작·끝 위치 보정을 보간한 결과. 범위가 없으면 `head_offset` 사용 |
| 운전석 `+0x4A4` | 범위 보정 계산 및 미러 모드 1의 기준 float3. 카메라 구성 때 사용자 좌석 보정을 적용하기 전의 `head_offset`을 복사 |
| 운전석 `+0x4D4` | `0x8ACCF0 → 0x7975F0`이 차량에서 받아 저장하는 parent placement. 현재 고개 방향의 최종 placement `+0x40`과 구별 |

모드 0의 위치 getter `0x8AC960`은 차량 기준 위치에 운전석 `+0x48C`, `+0xE0`, `+0x4B4`의 위치 성분과 `+0x57C`를 합친다（cell X/Z는 512를 곱해 복원）. 모드 1의 반사 기준 위치는 이 동적 합산 대신 위의 `vehicle +0x1070 + camera +0x4A4`를 사용한다. 여기서 `vehicle`은 `game_physics_vehicle`이다. 따라서 **현재 시점 및 사용자 좌석 보정을 배제하는 기존 기준 위치 경로가 있다.** 실제 디지털미러 영상과 차체 상대 자세, 차량·부품 재구성 뒤의 동작은 추가 확인 대상이다.

기준값의 초기화와 좌석 조절을 다음 코드로 연결했다. 아래는 실행 파일의 데이터 흐름이며, 게임의 좌석을 직접 조작한 결과가 아니다.

1. 카메라 구성 `0x63FA10`은 원본 운전석 카메라를 복제·설정한 다음, `0x64002E..0x640047`에서 `camera +0x498`의 float3를 `+0x4A4`에 복사한다. 이어 `0x640077..0x6400C0`에서 `*(game_physics_vehicle +0x1F8)`의 `+0x1F0` float3를 **`+0x498`에만** 더한다. 대상 객체의 실제 class는 `vehicle`이고 이 속성의 reflection 이름은 `user_head_offset`이다. `vehicle`과 `game_physics_vehicle`은 별도 객체다.
2. 좌석 조절 UI 초기화 `0x114A550`은 `camera +0x4A4`를 UI `+0x39C`에 보관하고, 여기에 `user_head_offset`을 더한 값을 UI `+0x378`에 둔다. 적용 함수 `0x114B050`은 UI `+0x378`을 카메라 `+0x498`에 복사하고 가상함수 `+0x70 → 0x8AC330`으로 시점 상태를 갱신한다. 확인한 경로에서 미러 기준 `+0x4A4`를 다시 쓰지 않는다.
3. 조절 갱신 `0x114C9F0`은 위 적용 함수를 부른 뒤 `UI +0x378 − UI +0x39C`를 `vehicle.user_head_offset`에 저장한다. 같은 함수가 사용자 기본 시선 각도와 FOV 차이도 각각 `+0x208`, `+0x20C`, `+0x1FC`에 저장한다. 그러므로 좌석 조절값 자체를 미러 기준에 누적하는 흐름은 아니다. 이것은 확인한 UI·구성 경로의 결론이며, 다른 모드나 모든 객체 재생성 경로를 검증했다는 뜻은 아니다.

후속 read-only 표본에서도 `game_physics_vehicle +0x1F8 → vehicle`, `+0x1098 → vehicle_interior_camera`를 확인했다. `user_head_offset`, 운전석 `+0x498`, 기준 `+0x4A4`는 모두 `(0, 0, 0)`이었다. 이 표본은 비영점 좌석 조절 전후 비교가 아니다. `interior-seat-baseline-observation.json`에 저장했으며, 26회·532 bytes·reader 측 약 0.289 ms였다. observer PID 9520은 handle을 닫았고 종료도 확인했다. 좌석 변경이나 게임 입력은 수행하지 않았다.

이 연결의 원본은 `interior-reference-offset-reset.txt`, `vehicle-camera-setup-after-seat.txt`, `head-position-reflection.json`, `vehicle-user-settings-reflection.json`, `seat-adjustment-callbacks.txt`, `seat-adjustment-preview-update.txt`, `interior-camera-initialize-vfunc70.txt`에 있다.

공통 parent placement는 `0x7975F0 → 0x6480C0`의 차량 경로에서 온다. 후자는 차량의 가상함수와 조건부 보조 placement를 합성한다. 운전석 객체에 보관돼 있다는 이유로 이를 고개 회전행렬로 해석하지 않는다. live 표본에서 운전석 카메라와 mirror0의 `+0x3E8` 소유자는 같은 `game_physics_vehicle`였으며 `game_actor +0x18`과도 일치했다. 그 parent와 미러의 로컬 위치를 합성한 결과는 관측된 현재 미러 위치와 약 `2.0e-6` 게임 길이 단위 차이였다. 이는 한 순차 CPU 표본에서 변환 연결을 대조한 결과이며, GPU 영상 정합이나 움직이는 차량에서의 동기화 검증은 아니다.

관련 원본은 `interior-camera-feature-setup.txt`, `accessory-name-to-string.txt`, `interior-placement-update.txt`, `vehicle-camera-parent-placement.txt`, `vehicle-camera-parent-placement-source.txt`, `interior-reference-offset-update.txt`다. reflection 자료는 `interior-camera-reflection-attributes.json`, live 자료와 위치 합성은 `mirror-fixed-reference-inputs.json`, `mirror-fixed-reference-decoded.json`에 있다. reader는 33회, 952 bytes, 약 0.577 ms였으며 observer PID 24324의 handle 및 프로세스 종료를 확인했다. 현재 트럭의 모드를 바꾸거나 디지털미러를 장착하지 않았다.

### 공통 parent에는 캐빈 서스펜션 자세가 포함됨

**좌석 보정과 분리된 미러 모드 1도 캐빈의 물리 운동을 포함하는 경로를 사용한다.** 현재 빌드의 `0x6480C0`은 캐빈 물리 객체가 있는 경로에서 차량 본체 자세와 캐빈의 상대 자세를 합성한다. 따라서 센서의 외부 파라미터를 정할 때 샤시 기준과 캐빈 기준을 구별해야 한다. 캐빈에 실제 장착한 센서를 모사한다면 이 상대 운동이 필요한 정보가 된다.

캐빈이라는 판단은 이름 추측만으로 붙이지 않았다. 생성 함수 `0x641510`은 `game_physics_vehicle +0x1050`의 부품 데이터를 얻고, reflection 이름이 `accessory_cabin_data.suspension`인 `+0x448`을 검사한다. 이어 base-38 token `0x30BC767 = cabin` 로케이터를 조회하고, 물리 시스템에서 dynamic actor를 생성해 `game_physics_vehicle +0x1278`에 저장한다. 그 물리 객체에 `physics_data.cabin_mass`인 설정 `+0x80`도 전달한다. 설치 `def/vehicle/physics.sii`에는 캐빈 질량과 서스펜션 이동량·감쇠 설정이 존재한다. 이 파일의 기본값을 현재 차량에 적용된 값으로 실측한 것은 아니다.

| 차량 필드 / 함수 | 확인한 역할 |
|---|---|
| `game_physics_vehicle +0x30`, `+0x38` | 본체 물리 객체와 본체의 frame placement 상태 |
| `+0x1278`, `+0x1260` | 캐빈 물리 객체와 캐빈 상대 frame placement 상태 |
| `0x648260` | 두 물리 객체의 상대 자세 및 기준 위치 보정을 계산 |
| `0x646600 → 0x648260 → 0x4760B0` | 캐빈 상대 자세를 frame 자료에 게시 |
| `0x6481B0` | 캐빈 상대 frame placement를 읽음. `+0x13B8`이 켜졌거나 캐빈 물리 객체가 없으면 단위 변환 반환 |
| `0x6480C0` | 본체 가상함수 `+0xE8 → 0x866B10`의 자세와 위 상대 자세를 합성 |
| `0x7975F0 → 0x8ACCF0` | 성공한 합성 결과를 운전석 `+0x4D4`에 전달. 미러 모드 0·1이 이를 공통 parent로 사용 |

현재 표본은 캐빈 객체가 존재하고 `+0x13B8 = 0`이었다. 본체와 캐빈 상대 자세를 오프라인에서 합성했을 때 운전석 parent 위치와의 차이는 약 `1.9e-6` 게임 길이 단위, 쿼터니언 성분 차이의 크기는 약 `3.9e-8`이었다. 캐빈 상대 운동 자체는 이 표본에서 매우 작았다. 큰 조향·제동 중 진폭이나 GPU 영상과의 정합을 확인한 결과는 아니다.

원본은 `vehicle-auxiliary-physics-creation.txt`, `cabin-physics-reflection.json`, `cabin-suspension-enabled-reflection.json`, `vehicle-relative-placement-publish.txt`, `physics-body-placement-getters.txt`, `camera-parent-components.json`, `camera-parent-composition.json`이다. reader는 23회·993 bytes·약 0.888 ms였고 observer PID 1152의 handle 및 프로세스 종료를 확인했다.

### 렌더 시각의 본체·캐빈 자세를 담는 frame 버퍼

미러 parent의 본체·캐빈 상태가 mode 1일 때 `0x4762F0`을 거쳐 **보간 결과 버퍼**의 placement를 읽는다. 이 getter 자체가 매번 보간하는 것은 아니다. `0x426360`의 갱신 경로가 먼저 `0x742D20`을 호출해 두 이력 시각과 목표 시각을 비교하고 결과를 준비한다. `0x744AB0`은 placement 항목들을 처리하며, 유효한 같은 객체의 서로 다른 자세에는 `0x139A270`을 호출한다. 객체 세대가 다르거나 무효 상태인 경우에는 별도 복사·무효화 분기가 있으므로 임의의 두 항목을 그대로 보간하면 안 된다.

이 설치 빌드에서 필요한 주소 연결은 다음과 같다. 게임 함수는 호출하지 않고 메모리와 실행 파일을 읽어 확인했다.

| 경로 | 내용 |
|---|---|
| `C = *(EXE +0x36AE6D8)`, `G = *(C +0x3260)` | controller와 frame 자료 관리자 |
| `G +0x00` | 3개 버퍼의 배열. data `+0x08`, count `+0x10`, 버퍼 stride `0x78` |
| 각 버퍼 `+0x00` | placement 배열. data `+0x08`, count `+0x10`, 항목 stride `0x24` |
| placement 항목 `+0x00`, `+0x20` | 기존 32-byte placement와 상태·세대 bits |
| 버퍼 0 | 카메라 getter가 읽는 보간 결과 |
| `G +0x2D8`, `G +0x308` | 두 이력의 버퍼 index 배열과 대응 시각 배열 |
| `G +0x2C8` | 물리 자세를 게시하는 대상 버퍼 index. 관측 시점에는 `-1` |
| `S = *(EXE +0x36AE718)`, `S +0xB30` | `0x426360`이 `0x742D20`에 전달하는 목표 시각. 같은 표본의 `S +0xAA0`은 controller `C`와 일치 |

mode 1의 상태 객체 `+0x08`은 handle이다. 하위 52 bits가 배열 index이고, bits 52–55는 항목 `+0x20`의 하위 4 bits와 대조된다. getter는 항목의 `0x20` bit가 켜진 경우도 거부한다. 이는 읽을 항목의 실제 객체 수명 조건이며, 주소만 오래 보관해 쓰면 안 되는 이유다. mode 2·3에서는 상태 객체 `+0x10`의 placement를 직접 읽는 다른 분기가 있다.

후속 표본에서는 본체 handle index 454, 캐빈 상대 handle index 1031을 세 버퍼에서 읽었다. 이력 index는 `[2, 1]`, 이력 시각은 `[15491230326, 15491263658]`, 목표 시각은 `15491247036`이었다. 코드가 두 시간 차에 적용하는 상수는 `1e-6`이며, 이 표본의 간격은 33.332 ms에 해당한다. 이를 전체 게임의 고정 물리 주기로 일반화하지 않는다. 계산된 보간 계수는 약 `0.50132006`이고, 두 이력의 캐빈 위치를 보간한 결과와 실제 버퍼 0의 위치 차이는 약 `5.8e-13` 게임 길이 단위였다. 본체 위치는 두 이력에서 같았다.

이 결과는 **렌더용 자세를 찾고 CPU 값의 보간 관계를 대조한 것**이다. 순차 `ReadProcessMemory`는 여러 항목을 원자적으로 고정하지 않으며, 해당 자세와 GPU 픽셀이 같은 프레임이라는 보장은 아직 없다. 후속 수집 구현에서는 pass의 최종 카메라 상수와 픽셀을 함께 연결하는 작업이 필요하다.

정적 원본은 `controller-interpolated-placement.txt`, `placement-pool-getter.txt`, `controller-placement-publish.txt`, `frame-pool-begin-update.txt`, `frame-pool-finish-update.txt`, `frame-pool-interpolation.txt`, `placement-frame-interpolation.txt`, `placement-interpolate-helper.txt`이다. 실측과 계산은 `placement-history-observation.json`, `placement-history-interpolation.json`에 있다. 이 reader는 25회·1,344 bytes·약 0.538 ms였고 observer PID 27296의 handle 및 프로세스 종료를 확인했다. reader 측 시간은 게임의 FPS 영향을 측정한 값이 아니다.

### SDK 차량 자세와 렌더 이력은 같은 기준점을 사용한다

**SDK `truck.world.placement`와 엔진의 렌더용 본체 이력은 같은 차량 기준점에서 출발한다. 시간 보간 여부가 다르며, ETS2LA가 별도로 더하는 경계 상자 중심 보정과 구별해야 한다.** 원시 물리 actor의 위치와 SDK 위치를 그대로 같은 점으로 취급하는 것도 정확하지 않다.

SDK provider는 V slot `+0xF8 → 0x866C10`을 사용한다. 앞서 확인한 자세 이력 발행 단계에서도 controller `0x4CF940`이 V slot `+0xA0 → 0x646600`을 호출하고, 이어 `0x865F60`이 **같은 slot `+0xF8`**의 결과를 얻는다. `0x865F96 → 0x4760B0`이 그 결과를 V `+0x38`의 frame state에 연결된 이력 항목으로 게시한다. 렌더 getter `+0xE8 → 0x866B10`은 이 이력을 보간한 결과를 읽는다. 따라서 getter가 다르다는 사실만으로 SDK·렌더 본체의 원점도 다르다고 판단하지 않는다.

공통 물리 getter `0x866C10`은 body slot `+0x130`의 placement에 V `+0x464` float3의 부호를 반전한 로컬 이동을 적용한다. helper `0x1399C40`은 이 벡터를 placement 회전으로 변환하고 cell 표현도 처리한다. `d = V[+0x464]`라 할 때 관계는 다음과 같다.

```text
T_world_vehicle = T_world_physics_actor * Translation(-d)
p_world_vehicle = p_world_physics_actor + R_world_physics_actor * (-d)
```

즉 `d`를 월드 X/Y/Z에 그대로 빼는 것이 아니다. 이 조사에서는 공통 getter가 반환하는 점을 **차량 기준점**이라고 부른다. 이를 뒷차축 중심·차량 경계 상자 중심·PhysX 질량 중심이라고 임의로 명명하지 않았다.

보정량의 생성도 연결했다. 관측한 V `+0x200`은 `accessory_chassis_data`이며, reflection에서 `+0x62C`는 `cog_height`, 배열 `+0x548`은 `kerb_weight`였다. 현재 V slot `+0x240 → 0x64E800`은 `d=(0, h-h*s, 0)`을 저장한다. 여기서 `h`는 `cog_height`, `s`는 `g_truck_stability`다. 설정 getter `0x1DB4C0`의 연결 포인터와 float cache를 읽었으며 게임 함수를 호출하지 않았다.

차량 구성 경로 `0x85F710`은 위 초기화 뒤 V slot `+0x130 → 0x647540`의 결과를 `d.z`에 더한다（`0x85F9DC..0x85F9E5`）. 후자는 정의 객체의 좌표 배열 `+0x668`에서 각 쌍의 첫 좌표 Z를 선택해 `kerb_weight`로 가중 평균한다. 이 좌표 배열 자체의 reflection 이름은 확인하지 않았다.

| 입력·결과 | 현재 차량의 읽기 값 |
|---|---|
| `cog_height`, `g_truck_stability` | 약 `0.9`, `0.5` |
| `kerb_weight` | `5080`, `2200` |
| 가중 계산에 사용하는 두 Z | 약 `-1.79246879`, `2.03159165` |
| 계산한 `d.z` | `(5080*z0 + 2200*z1) / (5080+2200)` |
| 읽은 전체 `d` | **`(0, 0.449999988, -0.636846125)`** |

관측 입력으로 float 계산 순서를 적용한 Y·Z는 읽은 보정량과 일치했다. 이는 현재 차량 구성의 CPU 값 대조이며 설정 변경 실험이 아니다. 설정을 바꿔 보정량이 재계산되는 전체 lifecycle도 실행하지 않았다. 후속 조사에서는 PhysX의 질량중심 getter를 별도로 연결하여 **현재 차량의 actor 원점이 질량중심과 일치함**을 확인했다. SDK 위치 기준점은 여기서 `-d`만큼 떨어져 있지만 선형 속도에는 측정점 이동 보정이 없다. [질량중심·속도 연결](11_idle_memory_and_telemetry.md#sdk-선형-속도는-질량중심의-속도이며-위치-원점-보정이-없다)을 참고한다.

한 정차 표본에서는 SDK native cache의 위치와 렌더 버퍼 0의 위치가 모두 약 `(10286.024338, 44.305923, -9161.933868)`이었다. 두 물리 이력의 Y는 각각 약 `44.305920`, `44.305923`이었다. 외부 읽기가 비원자적이며 움직임도 매우 작으므로, 위치가 같았다는 사실을 동기화의 증명으로 사용하지 않는다. 같은 원점이라는 판단은 **동일한 getter 결과가 두 경로에 쓰이는 코드**를 근거로 한다.

ETS2LA의 추가 보정은 별도다. 확보한 plugin revision의 `src/core.cpp:105`는 `get_interpolated_placement`를 읽은 뒤 `get_center_coords`에 넘긴다. `src/processing/traffic.cpp:27`은 차량 로컬 경계 상자 중심을 회전시켜 그 위치에 더한다. 따라서 camera record의 `truck_pos_*`는 위 차량 기준점이 아니라 **회전된 경계 상자 중심**이며, `truck_rot_*`은 원래 회전을 유지한다.

현재 차량의 경계 상자에서 계산한 로컬 중심은 약 `(-0.016274, 1.868865, -0.153222)`다. 높이 방향 차이가 약 1.87 m이므로 무시할 만한 원점 차이가 아니다. 이는 보존한 ETS2LA 소스를 현재 AABB에 적용한 계산이며, 해당 plugin을 배치하거나 실제 camera shared-memory 값을 관측한 결과는 아니다.

```text
ETS2LA camera record의 truck_position
  = render_vehicle_position + render_vehicle_rotation * aabb_local_center

공통 미러 parent
  = render_vehicle_transform * interpolated_cabin_relative_transform
```

센서 변환에는 차량 기준점, 물리 actor 기준점, 경계 상자 중심, 캐빈 기준을 구별해 기록해야 한다. 엔진의 렌더 차량 자세에 SDK의 `d`를 다시 빼면 이미 적용한 보정을 중복 적용한다. 반대로 ETS2LA의 `truck_pos_*`를 같은 원점으로 사용하려면 AABB 중심 이동을 먼저 되돌려야 한다. 회전·시각·차량 구성도 함께 연결하며, 이 원점 정리가 같은 GPU 프레임의 픽셀 확보를 대신하지는 않는다.

근거는 `vehicle-body-render-placement-publish.txt`, `vehicle-relative-placement-publish.txt`, `physics-vehicle-placement-vfunc-e8.txt`, `telemetry-vehicle-raw-placement.txt`, `placement-translate-local.txt`, `vehicle-origin-translation-update.txt`, `vehicle-longitudinal-origin-update.txt`, `vehicle-longitudinal-origin-getter.txt`, `vehicle-origin-cvar-float-getter.txt`, `vehicle-origin-cog-height-reflection.json`, `vehicle-origin-chassis-properties.json`이다. 실측은 `vehicle-origin-and-sdk-render-observation.json`（26회·1,100 bytes·약 0.38 ms）과 `vehicle-origin-weight-inputs.json`（13회·212 bytes·약 0.21 ms）에 있다. observer PID 30812·23832는 handle을 닫고 종료했다. offset 검색에 걸린 `0x63CCB0`은 현재 차량 클래스의 이 보정 경로로 확인되지 않아 결론에서 제외했다.

## 미러에서 제외되는 맵 물체의 설정

**미러 경로를 센서에 재사용하면 맵 제작자가 지정한 물체 제외 설정도 확인해야 한다.** 공식 SCS 문서는 Hookup（주차 차량 등을 배치하는 지점）의 `No mirror reflection`이 생성된 모델을 트럭 미러에서 생략하는 최적화라고 설명한다. 같은 문서의 Model detail은 처음부터 낮은 LOD（덜 세밀한 모델）를 사용할 수 있게 하는 별도 설정이다. 따라서 미러에 물체가 없거나 덜 세밀하게 보이는 원인을 같은 것으로 취급하면 안 된다. [SCS Hookup 설명](https://modding.scssoft.com/wiki/Documentation/Tools/Map_Editor/New_Editor_Features_info_-_old_%2B_1.47#Hookup_Item)

공개 [TruckLib의 고정된 로컬 소스](../research/sources/sk-zk_TruckLib-c42dbe4f2d6b/TruckLib/ScsMap/)는 다섯 종류에 `MirrorReflection`을 정의한다. 설치 EXE의 편집기 함수 RVA `0xC5A540`도 `item +0x0A`의 종류를 분기하고 `item +0x34`의 다음 bits를 검사한다. 설정이 켜지면 결과 목록에 범주 11을 추가한다. 이 범주는 label 배열 `0x2DBE080`의 인덱스 11인 `No mirror reflection`이며 UI의 `0xECC323..0xECC39E`에서도 같은 배열을 순회한다.

| 맵 물체 종류 | type ID | `+0x34`의 제외 bit / mask | 설치 EXE 검사 RVA |
|---|---:|---:|---|
| Buildings（건물 배치） | `0x02` | bit 5 / `0x20` | `0xC5A7DB` |
| Model（모델） | `0x05` | bit 15 / `0x8000` | `0xC5A7E1` |
| Compound（복합 물체） | `0x28` | bit 6 / `0x40` | `0xC5A7EA` |
| Curve（곡선을 따라 배치한 모델） | `0x2C` | bit 7 / `0x80` | `0xC5A7F0` |
| Hookup（동적 모델 배치점） | `0x2F` | bit 3 / `0x08` | `0xC5A7F6` |

bit 번호는 0부터 시작하며, **1이 제외 설정**이다. 이 표는 종류별 설정의 의미와 저장 위치를 대조한 것이다. 모든 물체에 같은 mask를 적용할 수 없고, 편집기의 범주 11을 렌더 카메라 mask나 미러 slot bit로 사용해서도 안 된다. `no_mirror_geometry` 문자열 RVA `0x2327F68`의 참조 `0x2DB0908`은 편집기 표시 이름 배열의 원소이며, 일반 unit reflection 속성의 필드 오프셋으로 해석하지 않았다.

첫 외부 관측에서 `C +0x650`의 객체 목록은 250개였고, 위 다섯 종류 중에서는 Compound 12개만 들어 있었다. 12개 모두 해당 제외 bit가 0이었다. 후속 관측에서 이들의 `+0x58` 배열（data `+0x60`, count `+0x68`）을 읽으니 하위 물체는 25개이며 모두 Sign（type `0x24`）이었다. 이는 전체 렌더 대상 목록으로 확인한 배열이 아니어서 이후 다른 소유 목록으로 조사를 확장했다.

### 제외 설정이 있는 실행 중 객체와 렌더 객체 연결

**더 넓은 맵 목록뿐 아니라, 장면 객체가 연결된 물체에서도 미러 제외 설정을 확인했다.** 여기서 장면 객체는 맵 물체의 로딩·형상 자료를 소유하는 segment를 뜻한다. segment의 존재만으로 현재 화면이나 미러에서 그려졌다고 판단하지 않는다.

`C +0x10`의 맵 배열은 310,038개였다. 양 끝을 포함해 일정한 인덱스 간격으로 1,024개만 읽었으며, 제외 설정이 있는 Model 2개·Compound 3개·Curve 3개를 찾았다. 이 8개의 `item +0x48`은 모두 NULL이었다. 이 표본은 전체 맵의 제외 비율을 추정하기 위한 무작위 표본도, 화면 안의 물체 목록도 아니다. ETS2LA의 오래된 주석에 있는 `C +0x30`을 별도 배열로 해석한 시도는 현재 layout과 맞지 않아 포인터를 따라가지 않았다.

장면 segment는 별도 연결 목록으로 접근한다. 설치 EXE의 생성 함수 `0x48BEF0`은 0x4B8-byte segment를 만들고 `segment +0x08`의 연결 노드를 목록에 넣는다. 순회 함수 `0x489DB0`과 해제 함수 `0x48C0B0`에서 시작·종료 지점과 역방향 연결을 확인했다.

```text
C +0x2F20 → 첫 연결 노드
연결 노드 +0x00 → 다음 노드, 종료 표식은 C +0x2F30
segment = 연결 노드 − 0x08
segment +0x28 → 맵 물체
맵 물체 +0x48 → segment
C +0x2F40 → 목록의 항목 수
```

관측 당시 보고된 수는 2,347개였고, 읽기 상한에 따라 처음 2,048개를 읽었다. 이 2,048개 모두 맵 물체의 역방향 포인터가 원래 segment를 가리켰다. 그중 위 제외 설정을 해석할 수 있는 종류는 다음과 같다.

| 종류 | 읽은 segment 수 | 제외 설정이 1인 수 |
|---|---:|---:|
| Buildings | 85 | 0 |
| Model | 545 | 24 |
| Compound | 382 | 17 |
| Curve | 292 | 5 |
| Hookup | 0 | 0 |

나머지는 도로·지형·Prefab·Sign·Patch다. **제외 설정이 있는 46개는 실제 segment를 가진 물체지만, GPU에서 빠진 물체 46개를 측정한 결과는 아니다.** 목록에는 여러 로딩 상태가 섞이고 화면 가시성도 별도로 결정된다. Hookup은 이 표본에 없었다.

이어서 제외 설정이 있는 Model 6개와 없는 Model 6개의 연결을 읽었다. `segment +0xC8`의 배열（data `+0xD0`, count `+0xD8`）은 0x1F8-byte `segment_data_t` 기록을 보관한다. 타입 이름은 EXE의 배열 접근 오류 문자열 `0x22A3430`에서 확인했다. `0x735900`은 각 기록의 `+0x10`에서 렌더 객체 `Q`를 얻고 `Q +0x142`의 추가 component 묶음을 갱신한다. 이 필드는 뒤의 draw 생성·실행 분석에서 확인한 묶음과 연결된다.

```text
Model → item +0x48 → segment
      → segment +0xC8 배열 → segment_data_t +0x10 → Q
      → Q +0x142 → 추가 component 묶음
```

12개 모델에서 기록 64개와 NULL이 아닌 `Q` 59개를 읽었다. 제외된 모델에서도 렌더 객체가 만들어져 있었으므로, **미러 제외를 물체의 로딩 여부나 `Q`의 존재 여부로 판정하면 안 된다.** `Q +0x120`도 곧바로 미러 제외 mask로 사용할 수 없다. 양쪽 표본에서 `0x840` 등이 나타났고, 이 필드는 draw 생성 함수 `0x2DEC80`이 재질 경로에서 계산하는 pass 선택 값이다. `0xFFFFFFFF`이면 `0x2DED7A..0x2DEE23`에서 값을 구하므로, 해당 원시 값을 모든 pass 허용으로 해석하지 않았다. 이번에는 `Q`가 각 미러의 제출 목록에 들어갔는지까지 대조하지 않았다.

이 단계에서는 제외 설정을 실제 선택에 적용하는 경로를 찾지 못했지만, 아래 후속 조사에서 종류별 설정→공통 플래그→미러 후보 제외까지 연결했다. 복합 물체의 설정이 자식에게 적용되는 전체 방식과 AI 차량·보행자의 별도 선택 조건은 아직 확인 대상이다. 현재 확인한 미러 far 400은 최대 투영 범위이며, 범위 안의 모든 물체가 렌더된다는 보장은 아니다. 후속 GPU 수집에서는 주 화면과 미러를 같은 장면 상태로 비교해야 한다.

정적 근거는 `editor-item-display-categories.txt`, `editor-mirror-exclusion-switch.json`, `editor-item-display-category-labels.txt`와 대응 JSON이다. `model-mirror-exclusion-render-a.txt`·`-b.txt`는 탐색 당시의 후보 이름이며, 실제로는 type `0x29` 등이 관여하는 교통 경로 코드여서 이 표의 근거로 사용하지 않았다. 같은 `+0x34/0x8000`만으로 역할을 정하지 않는다.

실측은 `nearby-map-mirror-flags.json`（253회·20,040 bytes·reader 측 약 2.09 ms）과 `compound-child-mirror-flags.json`（302회·22,424 bytes·약 1.65 ms）이다. 읽기 오류는 없었으며 observer PID 3428·21852의 handle과 프로세스 종료를 확인했다. 입력·설정·메모리 쓰기와 게임 함수 호출은 하지 않았다. 순차 읽기이며 GPU 영상과 동기화한 표본은 아니다.

확장 관측은 `controller-map-array-probe.json`, `map-item-mirror-flag-sample.json`（2,050회·98,344 bytes·약 39.09 ms）, `loaded-map-segments.json`（4,098회·344,120 bytes·약 31.90 ms）, `map-model-render-object-links.json`（107회·68,016 bytes·약 1.13 ms）에 있다. 마지막 세 관측의 읽기 오류는 0건이며, observer PID 29996·28200·26868·26760의 handle과 프로세스 종료를 확인했다. 시간은 reader 측 경과 시간이며 게임의 프레임 지연 측정값이 아니다. 정적 연결 근거는 `controller-item-segment-acquire.txt`, `controller-segment-list-walk.txt`, `controller-segment-list-release.txt`, `map-segment-constructor.txt`, `map-segment-load-dispatch.txt`, `map-segment-finalize-model.txt`다. 이 자료들은 `research/live/2026-10-08-render-path/`에 있으며 draw 생성 원본은 별도 `research/live/2026-10-08-render-constants/scene-draw-source-to-items.txt`에 있다.

### 맵 모델에서 미러의 CPU 수집 목록까지 연결

**후속 관측에서는 맵 Model 147개, 서로 다른 렌더 객체 `Q` 337개를 실제 주 화면·미러의 CPU 수집 목록과 연결했다.** 앞 절의 모델→segment→`Q`에서 한 단계 더 나아간 결과다. 이 목록은 draw 생성의 입력이며, 최종 GPU draw나 화면 픽셀을 확인한 결과는 아니다.

정적 분석에서는 다음 제출 경로를 확인했다.

- 개별 segment 제출 함수 `0x72B0B0`은 0x1F8-byte 기록을 순회한다. 기록 `E`의 `+0x50`이 0이 아니고, `E +0x10`의 `Q`가 NULL이 아니며, `E +0x08`의 bit 0이 켜져 있으면 두 번째 인수 `R +0x08`의 수집기에 `Q` 한 개를 전달한다. 호출 위치는 `0x72B125`, 수집기 vtable slot은 `+0x10`이다. 이 함수 안에는 앞 절의 종류별 미러 제외 bit 검사가 없다. **이 함수를 일반 게임플레이 미러의 진입점으로 확정한 것은 아니다.** 아래에서 확인한 실제 미러 장면 경로는 미리 계산한 가시성 결과를 일괄 제출한다.
- 앞서 관측한 deferred context의 vtable `0x23F5C88`에서 같은 slot은 `0x147A960`이다. 이 함수는 context `+0x168`의 큐 배열（data `+0x170`, count `+0x178`, stride 0x250）중 `+0x190`부터 `+0x194`까지의 구간에 `Q`를 전달한다. 현재 관측한 context에는 큐 12개가 있었고 선택 구간 값은 2–5였다. 공유 context의 순차 관측값이므로 모든 미러·pass의 고정 설정으로 일반화하지 않는다.
- 수신 함수 `0x160B8F0`은 큐 `H`의 첫 uint32와 재질에서 얻은 `Q +0x120`의 AND가 0이 아닐 때 `H +0x128` 배열에 `Q`를 추가한다. `0x160BA9A`가 검사이고, `0x160BB20`이 선택된 포인터들의 복사다. 따라서 **재질의 pass 지원 여부가 목록 수집 단계에서도 검사된다.** 이 검사는 맵 물체의 미러 제외 설정과 구별해야 한다.

타입은 EXE의 배열 접근 오류 문자열에서도 확인했다. `0x22A6560`은 `array_t<r_proto_t*>`, `0x23F5AA0`은 `array_t<render_queue_set_t>`를 나타낸다. 문서의 `Q`는 이 `r_proto_t`, `H`는 `render_queue_set_t`와 연결된다. 위 정적 경로의 각 함수가 이번 표본에서 실제로 호출됐다는 실행 추적은 하지 않았다.

실측에서는 그래프의 `scene defattr`·`interior defattr` 작업을 찾고, 연결 이미지의 namespace로 화면을 구별한 뒤 `W +0x30 → H +0x128`을 읽었다. 이어 segment 목록의 처음 2,048개에서 Model 545개를 골라 현재 `Q` 포인터와 대조했다. 각 모델의 geometry 기록은 최대 64개로 제한했으며 **모델 5개는 이 상한으로 일부만 읽었다.** 관측한 서로 다른 `Q`는 총 2,275개다. 모델의 segment 역방향 포인터는 모두 일치했고 읽기 오류는 없었다.

| 화면 / 작업 | 해당 CPU 목록의 전체 `Q` 수 | 표본 Model과 연결된 모델 수 | 표본 Model과 연결된 서로 다른 `Q` 수 |
|---|---:|---:|---:|
| mirror0 / scene defattr | 1,119 | 55 | 147 |
| mirror1 / scene defattr | 1,510 | 109 | 242 |
| mirror2 / scene defattr | 1,058 | 42 | 116 |
| mirror5 / scene defattr | 419 | 7 | 25 |
| main0 / interior defattr | 95 | 0 | 0 |
| main0 / scene defattr | 1,113 | 18 | 41 |

같은 모델과 `Q`가 여러 화면에 들어가므로 행별 수를 합산하면 중복된다. 예를 들어 Model `0x1f5739d3c48`은 segment `0x1f79698a5f8`을 통해 미러 0·1·2의 목록에 연결됐다. 주소는 이번 실행의 관측값이며 영구 식별자로 사용하지 않는다.

**미러 제외 설정이 있는 Model 24개의 `Q` 103개는 여섯 목록 어디에서도 발견되지 않았다.** 주 화면 목록에도 없었기 때문에 이 표본만으로 제외 설정의 효과를 입증할 수는 없다. 시야·거리·로딩/LOD 선택 등 다른 조건과 분리하지 않았으며, 목록은 같은 GPU 프레임으로 고정하지 않았다. 아래에서 앞단의 선택 조건을 정적으로 연결했지만, 제외 대상이 주 화면에서 보이는 같은 장면의 GPU 비교는 남아 있다.

실측 원본은 [`map-model-pass-membership.json`](../research/live/2026-10-08-render-path/map-model-pass-membership.json)이다. 5,619회·2,089,704 bytes를 읽었고 reader 측 경과 시간은 약 38.25 ms였다. FPS 영향 측정값은 아니다. observer PID 30180은 handle을 닫고 종료됐음을 확인했다. 정적 근거는 `render-path/segment-submit-protos.txt`, `render-constants/deferred-context-add-protos.txt`, `render-constants/render-queue-add-protos.txt`다. 게임 함수 호출·입력·메모리 변경은 하지 않았다.

### 종류별 제외 설정이 실제 미러 후보 선택에 적용되는 경로

**맵의 미러 제외 설정은 공통 렌더링 플래그 `0x20`으로 변환되며, 미러용 가시성 질의가 이 플래그를 가진 항목을 후보에서 제외한다.** 편집기 표시만 확인한 단계에서 실제 게임플레이의 선택 코드까지 연결했다. GPU 픽셀을 비교한 결과는 아니다.

가시성 자료는 컨트롤러 `C +0x2BC8`의 `core_item_info_t`다. EXE 오류 문자열 `0x22A5950`에는 `cull_items`와 이 타입이, `0x22A5A40`에는 `visibility_task.cpp`가 남아 있다. 이 구조를 `S`라고 하면 `S +0x08`은 항목 수, `+0x10`은 맵 항목 포인터 표, `+0x18`은 40-byte 경계 자료 표, `+0x20`은 항목 속성 표, `+0x28`은 uint32 공통 플래그 표다. 이들은 같은 인덱스를 사용한다.

등록 함수 `0x73D8A0`은 `0x73D909`에서 원래 맵 항목을 저장하고, `0x73D95B`에서 변환 함수 `0x73D370`을 호출한 다음 `0x73D96E`에서 결과를 `S +0x28` 표에 쓴다. 변환 함수의 종류별 분기는 다음 원본 mask를 선택한다.

| 맵 종류 | 분기 RVA | 선택한 원본 제외 mask |
|---|---|---:|
| Buildings | `0x73D444` | `0x20` |
| Model | `0x73D4C9` | `0x8000` |
| Compound | `0x73D579` | `0x40` |
| Curve | `0x73D734` | `0x80` |
| Hookup | `0x73D799` | `0x08` |

분기 표는 `0x73D870`과 `0x73D83C`다. 공통 부분 `0x73D5E7..0x73D5EF`는 선택한 mask와 `item +0x34`의 AND가 0이 아닐 때 출력에 `0x20`을 넣는다. 따라서 원본 종류별 bits와 공통 표의 bit 5를 혼동하면 안 된다.

질의 생성 함수 `0x7494D0`은 렌더 카메라 `R +0x18`의 view mask를 질의 `T +0x38`에 복사한다. 첫 후보 선택 함수 `0x748510`의 `0x748610..0x748630`은 다음 조건으로 거부 mask에 `0x20`을 추가한다.

```text
if view_mask & 0x180003C00001FFF0:
    rejection_mask |= 0x20

if core_flags[item_index] & rejection_mask:
    skip item
```

실제 거부 분기는 `0x7486E0..0x7486EB`다. 기존 미러 0–6의 mask `0x400..0x10000`과 7·8의 `0x0800000000000000`, `0x1000000000000000`은 모두 이 조건에 해당한다. 주 화면 mask `1`은 이 조건에 해당하지 않는다. 상수에는 다른 view 종류도 포함되므로 이 상수 전체를 미러 전용 enum이라고 부르지는 않는다.

같은 함수는 이후 경계 겹침과 관측 위치·거리 조건도 검사한다. 통과한 segment 후보는 `0x749020 → 0x74A790 → 0x74B750`에서 거리별 LOD를 선택하고, `0x748F10`의 시야 검사 뒤 렌더 객체 `Q`를 결과 목록에 넣는다. `0x74BA00`의 거리 계산은 X/Z cell 차이×512와 local 좌표 차이로 구한 **수평 거리의 제곱**이다. 이 단계는 미러 제외 설정과 별도이며, far 값만으로 실제 선택을 예측할 수 없다.

미러 장면 함수 `0x4D4210`은 제출 기록 `B +0x68`의 `visibility_query_result_t*` 배열에서 첫 결과를 가져와 `0x4D42B5`에서 `0x47D620`에 전달한다. 후자는 결과 `+0x10`이 가리키는 `Q` 배열을 `0x47D657`에서 수집기 vtable `+0x10`에 일괄 제출한다. 이후 경로는 앞 절의 `0x147A960 → 0x160B8F0 → H +0x128`이다. 따라서 종류별 제외 검사는 재질별 큐 수집보다 앞에 있다. 이 연결은 disassembly의 데이터 흐름이며 함수를 외부에서 호출하지 않았다.

외부 관측에서는 공통 표 **2,481개 전체**를 읽었다. 위 다섯 종류에 속한 1,527개에서 원본 제외 bit와 공통 `0x20`은 모두 일치했다.

| 종류 | 관측 항목 수 | 원본 제외 설정과 공통 `0x20`이 모두 켜진 수 |
|---|---:|---:|
| Buildings | 91 | 0 |
| Model | 607 | 28 |
| Compound | 444 | 25 |
| Curve | 353 | 10 |
| Hookup | 32 | 1 |

제외된 항목은 64개이며 나머지 관련 항목에서는 두 값 모두 꺼져 있었다. 이는 현재 표본에서의 대응 확인이며 모든 맵·모드의 영상 누락 수를 추정한 통계가 아니다. 같은 순차 관측에서 미러 제출 기록은 0·1·2·5였고 view mask는 각각 `0x400/0x800/0x1000/0x8000`이었다. 미러 0의 질의 결과에서 `Q` 655개를 읽었다. 다른 세 기록의 결과 배열은 읽은 시점에 비어 있었으므로 그 미러가 렌더되지 않는다고 결론내리지 않는다. 비동기 작업의 결과 수명과 같은 GPU 프레임 연결은 별도 확인 대상이다.

실측은 [`core-item-mirror-cull-snapshot.json`](../research/live/2026-10-08-render-path/core-item-mirror-cull-snapshot.json)에 있다. observer PID 28964가 2,501회·463,432 bytes를 읽었고 reader 측 약 22.15 ms, 읽기 오류 0건이었다. handle을 닫고 프로세스 종료를 확인했다. 정적 근거는 `core-item-mirror-flags.txt`, `core-item-container-management.txt`, `scene-spatial-selection.txt`, `scene-selection-entry.txt`, `scene-visible-proto-selection.txt`, `scene-distance-lod-selection.txt`, `mirror-scene-query-submission.txt`, `controller-render-candidate-47d620.txt`다. 전부 `research/live/2026-10-08-render-path/` 아래에 있다.

센서 설계에서는 이 제외 규칙을 그대로 상속할지 구별해야 한다. 현재 조사에서는 게임 설정·플래그를 바꾸지 않았다. Hookup이 생성한 주차 차량과 AI의 차량·연결 모델은 아래에서 추가로 연결했다. 복합 물체의 하위 geometry 전체, 보행자 경로와 실제 픽셀 누락은 아직 확인하지 않았다.

### 주차 차량: 배치점의 제외 설정과 실제 모델 제출

**주차 차량의 미러 제외는 배치점의 후보 선택과 차량 모델의 제출 양쪽에서 적용된다.** 배치점에서 활성화된 모델은 별도의 공통 목록으로 넘어가므로, 첫 후보 검사만으로 실제 미러 표시를 설명할 수 없다. 설치 EXE의 두 번째 검사와 생성 시 플래그 전달까지 연결했다.

관측된 traffic 목록 23개 중 주차 차량은 21개였다. 모두 `parked_vehicle` unit `P`와 차량 객체 `A`, 렌더 모델 `M`을 다음 관계로 연결했다.

```text
독립 Hookup I +0xF8 → P
P +0x160 → 소유 맵 항목 I
P +0x168 → A
A +0x80 → P
*(A +0x90) → M
```

21개 모두에서 `P ↔ A` 관계가 일치했다. 19개는 독립 Hookup이고, 2개는 같은 Prefab（종류 4）에 붙은 unit이다. 독립 Hookup 19개에서는 `P +0x160`도 원래 Hookup을 가리켰다. 나머지 2개는 Prefab `0x1F57308B138`을 가리켰다. 이 주소는 해당 프로세스의 관측값이며 다음 실행에서 재사용할 상수가 아니다.

독립 Hookup의 segment getter（vtable `+0xB0 → 0xF5350`）는 NULL을 반환한다. 이 종류의 `I +0x48`을 일반 Model의 segment로 해석하면 안 된다. 공통 플래그 `0x400`인 비segment 항목은 `0x748510`에서 가시성 결과 `O +0x60` 목록으로 들어간다. `0x47D828..0x47D849`는 이 목록의 각 item에 vtable `+0x198`을 호출한다. Hookup에서는 `0x812440`이며 `I +0xF8`의 unit에 vtable `+0x98`을 호출한다.

Prefab 등에 붙은 unit은 다른 목록을 사용한다. `core_item_info_t +0x58`의 인덱스별 0x50-byte 기록에는 신호 객체 배열 `+0x00`과 부착 unit 배열 `+0x28`이 있다. 가시성 결과 `O +0x70`을 소비하는 `0x47D930..0x47D975`가 부착 unit의 vtable `+0x90`을 호출하고, `0x14AE840`은 이를 `+0x98`로 넘긴다. 이번 관측에서 비어 있지 않은 기록은 171개, 배열 헤더 합계는 신호 객체 29개·부착 unit 417개였다. 기록당 최대 32개라는 읽기 한도로 **부착 unit 411개를 실제 해석**했으며, 여기에 위 Prefab의 주차 unit 2개가 포함됐다. 417개 전부의 종류를 확인한 것은 아니다.

두 경로가 만나는 `parked_vehicle` callback은 `0xAEE760`이다. `P +0x158`의 bit 1이 켜져 있으면 위 포인터를 따라 `M`을 얻어 `0x98FCC0(M, 1, 1)`으로 활성화한다. **이 callback 자체는 draw 제출 함수가 아니다.** 활성 모델은 `M +0x08`을 node로 사용하는 연결 목록에 등록된다. 머리는 EXE RVA `0x2D8C388`, sentinel은 `0x2D8C398`이다. 미러 장면 함수의 `0x4D431E → 0x62DA80`은 이 목록을 돌면서 모델 vtable `+0x38`에 해당 미러 카메라를 전달한다. 관측된 주차 모델 21개의 vtable은 모두 `0x22DF760`이며 이 slot은 `0x977CA0`이다.

생성 시 제외 설정은 다음과 같이 전달된다.

| 단계 | 설치 EXE의 동작 |
|---|---|
| 원본 확인 | `0x95B78B`에서 `P +0x160`의 소유 항목을 읽음 |
| Hookup 제외 설정 | `0x95B7AB..0x95B7B7`: 종류 `0x2F`, `I +0x34 & 0x08`이면 생성 인수에 `0x08` 추가 |
| 렌더 모델 초기화 | `0x95B81A → 0x9747A0`; `0x974858..0x97487B`가 생성 인수 `0x08`을 `M +0x1B8`의 `0x200`으로 변환 |
| 미러 제출 시 검사 | `0x977DB2..0x977DCB`: `R +0x18 & 0x180000000001FC00`이며 `M +0x1B8 & 0x200`이면 모델 본체 제출 없이 반환 |

마지막 view mask에는 기존 미러 9개의 mask가 모두 들어간다. 이 함수는 앞서 LOD가 4 이상인지도 검사하며, 미러에서는 `M +0x1AC`, 주 화면 mask `1`에서는 `M +0x1B0`을 사용한다. 따라서 미러 제외 bit가 꺼져 있다는 것만으로 실제 draw나 픽셀의 존재를 보장하지 않는다. 생성 시 전달은 확인했지만 설정 변경 시 기존 모델이 어떻게 갱신되는지는 여기서 확인하지 않았다.

실측한 독립 Hookup 19개 모두에서 원본 `0x08`과 모델 `0x200`이 일치했다. 제외 설정이 켜진 1개의 모델은 `0x1F7EA307610`이고, 두 LOD 값이 모두 4이며 활성 목록에도 없었다. 다른 20개는 제외 bit가 꺼져 있고 LOD가 모두 3이었다. 읽은 활성 목록 72개 중 이 20개가 포함됐다. **제외된 1개는 미러 외 조건에서도 이미 빠져 있으므로, 이 표본을 미러 설정 때문에 화면에서 사라진 실험 결과로 해석하지 않는다.**

`0x977CA0`은 본체 검사 전에 `M +0x20`의 연결 모델을 조건부 호출할 수도 있다. 이번 21개에서는 모두 NULL이었다. 이 결과를 트레일러·연결 모델·모든 복합 물체의 제외 설정 상속으로 확대하지 않는다. 또한 외부 순차 읽기여서 활성 목록·소유 관계·가시성 결과가 같은 GPU 프레임이라는 보장은 없다.

원본은 [`hookup-render-owners.json`](../research/live/2026-10-08-render-path/hookup-render-owners.json)과 [`parked-render-models.json`](../research/live/2026-10-08-render-path/parked-render-models.json)이다. 각각 4,216회·611,208 bytes·약 25.55 ms, 255회·32,276 bytes·약 2.04 ms를 읽었고 오류는 0건이었다. 시간은 reader 기준이며 게임 FPS 영향 측정값이 아니다. 두 observer（PID 21884·18272）는 handle을 닫고 종료했다. 게임 함수 호출·입력·메모리 변경은 하지 않았다.

정적 근거는 같은 자료 폴더의 `hookup-item-render-entry.txt`, `parked-model-render-entry.txt`, `parked-model-visibility-toggle.txt`, `active-model-scene-render.txt`, `parked-actor-model-initialize.txt`, `model-flags-conversion.txt`, `parked-render-model-submit.txt`와 앞 절의 가시성·미러 제출 자료다.

### AI 차량과 연결 모델의 제출

**관측된 AI 차량 모델도 위 활성 목록과 `0x977CA0`을 사용한다. 연결된 trailer 모델은 목록에 직접 등록되지 않고 부모 모델에서 연쇄 호출된다.** 따라서 활성 목록에 없는 연결 물체를 렌더 누락으로 세면 안 된다.

공개 ETS2LA 소스의 `traffic_ai_vehicle_t`는 Windows에서 모델 포인터를 `A +0x210`, 첫 trailer를 `A +0x418`에 둔다. 설치 EXE에서도 `0x9212ED`가 위 공통 모델 초기화 함수 `0x9747A0`을 호출하고 `0x92130D`가 결과를 `A +0x210`에 저장한다. trailer 초기화 `0x9592E0`은 다음 trailer를 `T +0x2F8`에 보관하고 재귀 생성하며, `0x95954B`에서 모델 포인터를 `T +0xF8`에 저장한다. 공개 소스의 선언만 현재 빌드에 그대로 적용한 결론이 아니다.

```text
AI actor A +0x210 → 차량 모델 M
A +0x418 → 첫 trailer T
T +0xF8 → trailer 모델 Mt
T +0x2F8 → 다음 trailer
M +0x20 → 첫 Mt
Mt +0x20 → 다음 Mt
```

관측한 spawned 배열은 첫 목록 50개·두 번째 목록 0개이며 읽기 한도에 의한 잘림은 없었다. 50개 모델의 vtable은 모두 `0x22DF760`, 제출 slot `+0x38`은 `0x977CA0`이었다. 후속 표본에서는 50개 모두 활성 모델 목록 72개에 포함됐다. 이 중 8개에 첫 trailer가 있었고, 8개 모두 `M +0x20 == T +0xF8`이었다. 연결 사슬에는 총 35개 trailer 객체가 있었으며, 다음 객체가 있는 27개 모두에서 다음 모델 포인터도 일치했다. 가장 긴 사슬은 10개였고 관측 한도 16개에 걸린 사슬은 없었다. **이는 내부 trailer 객체 수이며 도로의 화물 트레일러 35대를 영상으로 식별한 결과는 아니다.** 모델 종류별 외형 분류는 수행하지 않았다.

35개 trailer 모델의 vtable은 모두 `0x23209C8`, 제출 slot `+0x38`은 `0xB8B0C0`이다. 이들은 활성 목록에는 직접 들어 있지 않았다. 대신 차량 함수 `0x977CAE..0x977CCC`가 `M +0x20`의 모델을 검사해 호출한다. 이 분기에 필요한 연결 모델의 `+0x1B8 & 0xE0000 == 0xE0000`은 관측된 35개 모두에서 성립했다. trailer 함수 `0xB8B0CF..0xB8B0DB`는 자신의 `+0x20`에 다음 모델이 있으면 같은 slot을 호출한다.

목록 등록 자격도 확인했다. 활성화 함수 `0x98FCC0`은 상태가 바뀔 때 slot `+0x48`이 참인 모델만 전역 목록에 넣거나 뺀다. 두 모델 종류의 이 slot은 `0x956AE0`이며 `M +0x1B8 & 1`을 반환한다. 부모의 활성화 callback `0x976A90`은 연결 모델에도 활성 상태를 전달한다. 따라서 연결 모델은 활성 상태를 가지면서도 전역 목록의 독립 node가 아닐 수 있다. 다음 절의 105개 표본에서 차량 70개는 등록 자격 bit가 켜져 있고 trailer 35개는 꺼져 있었다. 근거는 `traffic-model-list-eligibility.txt`, `vehicle-model-activation-callback.txt`, `parked-model-visibility-toggle.txt`다.

**연결 모델 호출은 각 부모의 LOD·미러 제외 검사보다 먼저 실행된다.** trailer 자신의 본체 검사에서는 `0xB8B0DE..0xB8B0FA`가 view 종류에 따른 LOD를 선택하고 4 이상이면 반환한다. `0xB8B1DD..0xB8B1EC`는 차량과 같은 view mask `0x180000000001FC00`에 대해 자신의 `+0x1B8 & 0x200`을 검사한다. 따라서 부모의 제외 bit를 읽어 모든 자식이 함께 제외된다고 단정할 수 없다. 자식마다 별도 조건이 있으며, 초기화·설정 변경에서 그 bit를 상속하는 모든 경우까지 확인한 것은 아니다.

후속 표본의 차량 50개·연결 모델 35개에서는 미러 제외 `0x200`이 모두 꺼져 있었다. 그래도 미러 쪽 LOD가 4인 모델은 각각 3개·21개였다. 활성 목록 포함, 모델 본체 제출 조건 통과, 실제 GPU 픽셀의 존재는 서로 다른 사실이다. 이번 조사는 앞의 두 단계에 쓰이는 코드와 CPU 상태를 연결했고 픽셀을 읽지 않았다. 기본 LOD 계산은 다음 절에서 추가로 확인했다. AI 활성화 조건 전체와 주 화면·미러의 같은 프레임 비교는 남아 있다.

원본은 [`ai-render-models.json`](../research/live/2026-10-08-render-path/ai-render-models.json)과 [`ai-trailer-render-links.json`](../research/live/2026-10-08-render-path/ai-trailer-render-links.json)이다. 첫 관측은 154회·64,072 bytes·약 1.54 ms, 후속 관측은 332회·138,384 bytes·약 2.84 ms였으며 읽기 오류는 없었다. 두 자료는 서로 다른 시점의 순차 읽기다. observer PID 23476·22552는 handle을 닫고 종료했으며 게임 상태를 고정하거나 함수를 호출하지 않았다.

정적 근거는 `ai-render-model-initialize.txt`, `ai-trailer-initialize.txt`, `ai-trailer-model-submit.txt`와 위 차량 제출·활성 목록 자료다. 공개 구조 선언은 고정 revision `ETS2LA_plugin-3b01d90b5be2/src/prism/traffic/objects/traffic_ai_vehicle.hpp`, `traffic_ai_trailer.hpp`를 참고했다.

### 차량 LOD는 어느 카메라와 거리를 기준으로 결정되는가

**차량·trailer의 기본 LOD는 개별 미러 카메라가 아니라 camera manager가 현재 선택한 게임 카메라를 기준으로 계산한다.** 두 LOD 값 중 `M +0x1AC`는 시야각 보정이 없는 거리 값, `M +0x1B0`은 선택된 게임 카메라의 FOV 보정을 더한 값이다. 기존 미러는 전자를, 주 화면 mask `1`은 후자를 소비한다. 독립 센서를 만들 때 미러의 위치·FOV만 변경하면 이 차량 상세도 계산까지 새 센서 기준으로 바뀐다고 볼 수 없다.

차량 update slot `+0x30 → 0x977740`과 trailer의 `0xB8ACC0`은 다음 공통 함수에 두 LOD 출력 주소를 전달한다.

| 호출 지점 | 거리 경계 배열을 얻는 위치 |
|---|---|
| 차량 `0x977A8F → 0x956920` | `*(M +0x310) +0x1A0`의 float 배열 |
| trailer `0xB8AF49 → 0x956920` | `*(M +0x228) +0x1D0`의 float 배열 |

`0x956920`은 EXE RVA `0x36AE740`의 camera manager에서 먼저 `+0x70`의 override stack 개수를 본다. 0이 아니면 `+0x68` 배열의 마지막 카메라를, 0이면 `+0x10`의 선택 index와 `+0x30`의 카메라 배열을 사용한다. 카메라가 없으면 두 LOD 모두 4로 만든다. 이 함수에는 미러의 렌더 카메라 `R`이 인수로 전달되지 않는다.

거리 함수 `0x12FBD0`은 모델 update가 계산한 기준점과 선택 카메라의 `+0x40` 위치를 뺀다. X/Z의 cell 차이×512와 local 좌표 차이를 합산한 뒤 두 성분의 제곱합을 구한다. Y 차이는 사용하지 않는다. 모델 기준점은 pose에 `M +0x1A0`의 보정 벡터를 반영한 값이므로, traffic actor의 원점과 항상 같다고 가정하지 않는다.

설치 코드의 기본 계산을 수식으로 쓰면 다음과 같다. `T`는 리소스의 **거리 제곱 경계 배열**, `s`는 모델의 거리 배율, `φ`는 선택 카메라 `+0x20`의 FOV다.

```text
D² = ΔX² + ΔZ²
F = max(0.637070298 / tan(φ × π / 360), 1)
LOD_without_FOV = count(T[i] < D² / s²)
LOD_with_FOV    = count(T[i] < D² / (s² × F²))
```

`0.637070298`은 float 상수이며 약 `tan(65° / 2)`다. 함수 `0x95699F..0x9569EB`가 이 FOV 배율을 만들고, `0x9569F4..0x956A1D`가 두 거리 값과 경계를 각각 비교한다. 좁은 FOV일수록 주 화면용 계산에서 더 가까운 것으로 취급할 수 있고, FOV가 65° 이상이면 이 보정은 기본적으로 1이다. 이것은 해당 코드의 관계이며 좌석·줌 설정을 바꿔 수행한 실험은 아니다.

이후 `M +0x1B8`의 `0x2000/0x1000/0x0800`은 우선순위대로 최소 LOD를 3/2/1로 제한하고, `0x100`이 켜져 있으면 최대 LOD를 3으로 제한한다. 여기서 LOD 숫자가 클수록 더 단순한 모델을 선택하며, 제출 함수는 4 이상에서 본체를 건너뛴다. 별도로 update 진입 시 `M +0x18 & 0x08`이 꺼져 있으면 거리 계산 없이 두 값 모두 4가 된다. 따라서 LOD 4를 읽었다는 사실만으로 거리 초과를 확정하면 안 된다.

아래 105개 표본에서는 이 상태 bit가 꺼진 차량 2개·trailer 21개가 모두 LOD 4였고, 나머지 82개는 bit가 켜져 있으며 LOD 0–3이었다. 후속 조사에서 이 표시 조건을 traffic actor와 소속 도로의 상태까지 연결했다（다음 절）. 실제 함수 실행 순간을 추적한 결과는 아니다. `+0x18`의 상태 bits와 `+0x1B8`의 모델 설정 bits는 다른 필드다.

거리 배율 `s`는 `M +0x1E8` 객체의 slot `+0x10`에서 얻으며 객체가 없으면 1이다. 확인한 두 구현과 실제 설정은 다음과 같다.

| 모델 경로 | getter / 설정 객체 RVA | 배율 계산 | 읽은 설정값 |
|---|---|---|---:|
| AI 차량과 연결 trailer | `0x98FDC0` / `0x2D51F00` | `g_lod_factor_traffic` | 1 |
| 주차 차량 | `0x95B170` / `0x2D52040` | `0.8 × g_lod_factor_parked` | 1（따라서 배율 약 0.8）|

설정 이름은 객체 `+0x08`의 인라인 문자열에서, 값은 연결 객체 `+0x120`을 확인한 뒤 유효한 `+0x116/+0x118` 캐시에서 읽었다. float getter `0x1DB4C0`의 실제 소비 경로와 대조했으며 게임 함수를 호출하지 않았다.

후속 관측은 활성 목록의 차량 모델 70개와 연결 trailer 35개, 총 105개다. 85개는 AI 배율 getter, 20개는 주차 배율 getter를 사용했다. 105개 리소스의 경계 배열은 모두 `[2500, 6400, 14400, 490000]`이었다. 배율 1일 때 기본 거리 경계는 **50·80·120·700m**, 배율 0.8일 때는 **40·64·96·560m**에 해당한다. 관측 카메라는 index 2, override stack은 비어 있었고 FOV는 약 65°였다. **이 수치는 LOD 계산 경계이며 실제 미러 가시거리나 모든 게임 차량의 고정 사양이 아니다.** 모델 상태·최소/최대 LOD·실제 미러 시야 검사·제외 bit·리소스 구성에 따라 결과가 달라진다.

제출 단계의 별도 설정도 구별했다. 앞서 이름을 확정하지 않았던 RVA `0x2D4FE80`은 `r_deferred_mirrors`였고 관측값은 2였다. 차량 `0x977DD1..0x977E3F`와 trailer `0xB8B1F2..0xB8B208`은 이 값이 1 이하일 때 미러의 모델 선택 단계에 1을 더한다. 이후 보유 모델 수와 일부 플래그에 맞춰 index를 제한한다. 따라서 메모리에 저장된 기본 LOD가 같더라도 미러의 최종 모델 선택은 달라질 수 있다. 해당 설정을 변경해 보지는 않았다.

원본은 [`traffic-model-lod-inputs-complete.json`](../research/live/2026-10-08-render-path/traffic-model-lod-inputs-complete.json)과 [`traffic-lod-scale-settings.json`](../research/live/2026-10-08-render-path/traffic-lod-scale-settings.json)이다. 각각 990회·84,160 bytes·약 6.40 ms, 2회·592 bytes·약 0.10 ms를 읽었고 오류는 없었다. observer PID 21604·29028은 handle을 닫고 종료했다. 첫 파일 `traffic-model-lod-inputs.json`은 설정의 인라인 이름을 포인터로 잘못 해석해 중단한 부분 관측이며 모델 자료가 없다. 그 observer PID 11752도 종료했다. 순차 CPU 관측이므로 계산 시점의 pose와 최종 GPU 프레임을 동기화한 검증은 아니다.

정적 근거는 `vehicle-model-update-slot30.txt`, `trailer-model-update-slot30.txt`, `traffic-model-lod-selection.txt`, `model-lod-distance-helper.txt`, `lod-active-camera-getter.txt`, `parked-lod-scale-getter.txt`, `lod-cvar-float-getter.txt`와 기존 차량·trailer 제출 자료다. `traffic-model-component-selection.txt`는 LOD에 따라 부가 항목을 등록·해제하는 후속 경로의 단서이며, 모든 부가 항목의 의미를 확인한 결과로 취급하지 않는다.

### 거리 계산 전의 차량 표시 조건과 소속 도로

**거리 계산 전에 모델을 생략하는 조건에는 차량 자체의 상태뿐 아니라 소속 도로·Prefab·trajectory의 상태가 들어간다.** 후속 표본의 AI 차량 50개와 연결 trailer 35개에서, 아래 코드의 입력으로 계산한 결과가 읽은 `M +0x18` bit 3과 85개 모두 일치했다. 이 조건은 미러만의 필터가 아니며 두 LOD 값에 공통으로 영향을 준다.

AI actor를 `A`, 렌더 모델을 `M`, traffic item을 `I`라고 할 때 `I = *(A +0x88)`이다. 차량 모델은 `*(A +0x210)`, trailer 모델은 `*(A +0xF8)`에 있다. 경로상의 위치 선택에 사용하는 float는 `A +0x90`이다. 이 값의 용도는 아래 road 구간 선택 코드로 확인했으며 차량의 월드 좌표와 구별한다.

주요 쓰기 경로는 다음과 같다. 표의 slot은 각 subobject의 vtable 기준이다.

| 경로 | 관련 함수 | 모델 표시 상태에 쓰는 조건 |
|---|---|---|
| 차량 일반 update | `0x922B30`의 `0x922FD1..0x92300C` | traffic item slot `+0x50`의 경로 판정 |
| 차량 초기화·활성 경로 | `0x920CC0`의 `0x9210A4..0x9210E3` | 활성화 호출 후 같은 경로 판정 |
| 차량 `A +0x248`, slot `+0x08` | `0x92DE50` | actor 활성 ∧ 추가 상태 조건 ∧ item 존재·활성 ∧ 경로 판정 |
| trailer `A +0x128`, slot `+0x08` | `0x95AAA0` | actor 활성 ∧ 추가 상태 조건 ∧ item 존재·활성 ∧ 경로 판정 |

마지막 두 경로의 결합 조건을 85개 표본과 대조했다. 차량 actor 활성 getter `0x920760`은 `A +0x08` bit 0과 `A +0x4B8 & 0x01000000`이 모두 꺼져 있어야 참이다. trailer의 `0x93EBA0`은 앞의 bit 0만 검사한다. 추가 상태 조건은 별도 참조 판정이 참인 경우 차량 `A +0x388`, trailer `A +0x268`의 하위 3 bits가 모두 켜져 있는지를 요구한다.

그 참조 판정은 차량 `0x9216A0`, trailer `0x9592B0`이다. 각각 `H = *(A +0x250)` / `*(A +0x130)`이며, H가 없으면 거짓이다. H가 있을 때 `O = *(H +0x08)`가 없거나 `uint16(H +0x16) != uint16(O +0x48)`이면 참이다. 이 비교의 데이터 흐름은 확인했지만 필드의 공식 명칭이나 멀티플레이어 의미를 확정하지 않았다.

item 활성 getter `0x94CDA0`은 `I +0x08` bit 0을 확인한 뒤 root의 getter를 호출한다. 관측한 root getter는 모두 `0x943110`으로, root `+0x104` bit 0이 켜져 있고 `+0x08` bit 0은 꺼져 있어야 참이다. root는 road lane·Prefab part의 `I +0x78`, trajectory의 `I +0x130`에서 얻는다.

| traffic item 종류 | 경로 판정 함수 | 검사할 맵 객체 선택 |
|---|---|---|
| `0x500000`, road lane | `0x960FD0..0x96103A` | root `+0x258`의 16-byte 구간 배열. 구간 시작값과 `A +0x90`을 비교하며 `I +0x74 & 0x800`이면 `I +0x70 - A +0x90`으로 방향을 반영 |
| `0x600000`, Prefab part | `0x96AB20..0x96ABA6` | root `+0x258`의 24-byte 배열을 `I +0x77`의 index로 선택 |
| `0x900000`, trajectory | `0x91EBD0..0x91EC92` | `I +0x138`의 맵 객체. 필요하면 root `+0x2A8`의 node와 연결 맵 객체까지 검사 |

road·Prefab의 선택 맵 객체를 `Q`라 하면 `Q +0x38`에서 **bit 6（`0x40`）이 꺼져 있고 bit 23（`0x800000`）과 bit 7（`0x80`）이 켜져 있어야** 경로 판정이 참이다. trajectory도 우선 bit 6과 23을 검사하고 bit 7이 켜져 있으면 통과한다. bit 7이 꺼져 있으면 node별 예외와 연결 객체 상태를 검사한다. 예외 node는 `(flags & 0x03F00000) == 0x02900000` 또는 `(flags & 0xFC000000) == 0xA4000000`이며, 나머지 node의 `+0x28/+0x20` 연결 객체 중 bit 6이 꺼지고 bit 7이 켜진 것이 있으면 통과한다. 검사 대상 node가 전혀 없는 경우도 통과한다. 이 encoded node 값에 임의의 타입 이름을 붙이지 않았다.

표시 상태가 꺼진 23개는 다음과 같이 나뉘었다.

| 관측 원인 | AI 차량 | 연결 trailer |
|---|---:|---:|
| 선택 road의 `Q +0x38 = 0x008000C0`으로 bit 6 검사 실패 | 2 | 14 |
| 소속 traffic item 포인터가 NULL | 0 | 7 |
| 표시 상태가 켜진 나머지 | 48 | 14 |

NULL 포인터를 모델 생성 실패나 영구 삭제로 해석하지 않는다. 이 표는 읽은 순간의 상태이며, 여러 쓰기 경로 중 마지막으로 실행된 함수까지 포착한 것은 아니다. 35개 연결 actor 전체를 일반 도로용 세미트레일러로 분류한 것도 아니다.

자료는 `traffic-model-visibility-inputs.json`（214회·122,064 bytes·약 2.03 ms）과 `traffic-path-visibility-inputs.json`（311회·143,844 bytes·약 2.62 ms）이다. 후자는 85 actor, 14 traffic item, 10 root와 관련 맵 객체의 입력을 담는다. 읽기 오류와 배열 생략은 없었고 observer PID 26876·13520은 handle을 닫고 종료했다. 정적 근거는 `ai-model-state-path-*.txt`, `trailer-model-state-path-95aaa0.txt`, `*-actor-state-predicate.txt`, `*-actor-secondary-predicate.txt`, `traffic-item-enabled.txt`, `traffic-root-enabled.txt`, `road-item-visibility.txt`, `prefab-part-visibility-predicate.txt`, `trajectory-visibility-predicate.txt`다. 일부 파일은 관련 분기만 저장한 함수 조각이다.

### 도로 제외 bit와 cut plane

**도로 제외 bit는 엔진의 cut plane 판정으로도 설정된다.** cut plane은 여기서 맵의 일부를 렌더 후보에서 제외하는 공간 판정 객체를 뜻한다. 카메라의 near/far clip plane이나 모델별 미러 제외 설정과는 별도다. 설치 코드 `0x473C60` 안의 `0x475510..0x4755DA`가 core item 표를 순회하며 맵 객체 `+0x38` bit 6을 갱신한다.

갱신은 다음 조건을 포함한다.

- core flags 하위 4 bits가 controller `+0x2FF0`의 현재 값과 같으면 생략한다. 이 값은 관련 목록·상태가 바뀌면 1→2→3→1로 순환하는 갱신 구분값이다（`0x475418..0x47542F`）.
- core flags `0x1000`이 켜져 있으면 제외한다. 이 bit는 등록 함수 `0x73D650..0x73D665`에서 맵 상태 `+0x38 & 0x04000000`으로부터 만들어진다.
- 그렇지 않으면 맵 객체 slot `+0x60`을 호출한다. Road vtable `0x22A9A18`의 해당 함수는 `0x7651B0`이다. Road `+0x70` bit 2가 꺼진 경우 `+0x60/+0x68`의 두 node를 `0x6BFB40`에 전달하고, controller `+0x2FC8`의 cut plane 목록과 비교한다.
- 이 판정이 참이고 `g_cut_plane_ignore`가 0이면 제외한다. 후속 UID 목록 검색은 제외를 켜거나 다시 끌 수 있다. 따라서 공간 판정 하나가 모든 객체의 최종 제외 bit를 독점적으로 결정하는 것은 아니다.

`0x6BFB40`은 node의 int32 XYZ에 `1/256`을 곱해 점 목록을 만들고 `0x7DC620`으로 각 cut plane을 검사한다. 내부 `0x7DC490`은 X/Z와 cut plane의 공간 분할 트리·경계 계수를 이용한다. cut plane `+0xD0`의 상태와 `+0x34`의 모드에 따라 점들의 영역 분류를 비교한다. 게임 함수를 호출하지 않고 이 연산을 외부에서 읽은 자료로 계산했다.

cut plane의 `+0xD0` 상태를 쓰는 함수도 확인했다. vtable slot `+0x1A0 → 0x7DBD70`은 맵 상태 `+0x38 & 0x00400000`이면 0을 쓰고, 그렇지 않으면 **camera manager가 선택한 게임 카메라의 위치**를 `0x7DC490`으로 분류해 기록한다. 카메라 선택은 LOD와 같은 override stack 우선 방식이며, X/Z cell×512와 local 좌표로 월드 위치를 만든다. 개별 미러의 카메라 위치는 이 함수의 입력이 아니다.

별도 후속 표본에서 선택 카메라 위치는 약 `(10284.056, 46.998, -9161.454)`, override stack 개수는 0이었다. 읽은 위치와 64개 cut plane의 계수·트리로 계산한 상태는 실제 `+0xD0`과 **64개 모두 일치했다**（0: 45개, 1: 11개, `0xFFFFFFFF`: 8개）. 이 값은 영역 분류 결과이며 단순 boolean으로 해석하지 않는다. 순차 읽기이므로 카메라 이동·렌더와 동기화된 실험은 아니다. 원본 `cut-plane-camera-inputs.json`은 199회·24,208 bytes·약 1.22 ms, 오류·배열 생략 없이 수집됐고 observer PID 23292는 handle을 닫고 종료했다. 정적 근거는 `cut-plane-camera-state-update.txt`다.

관측된 cut plane 목록은 64개였고 모두 맵 타입 8, vtable RVA `0x22B52B0`이었다. 앞선 경로 표본에 연결된 Road 31개를 대조했을 때 **core 목록에 있는 26개에서 cut plane 계산과 bit 6이 모두 일치했다（제외 8, 비제외 18）.** 나머지 5개는 공간 판정으로는 제외에 해당하지만 bit 6은 꺼져 있었고, 후속 core 목록 읽기에서 5개 모두 목록 밖임을 확인했다. 목록에 들어 있지 않은 객체까지 동일한 시점에 갱신된다고 가정하면 안 된다.

제외된 8개 Road는 모두 cut plane `0x1F573A84320`의 판정에 걸렸고 그중 3개는 `0x1F573A84AB8`에도 걸렸다. 앞 절의 표시 상태가 꺼진 차량·연결 모델이 선택한 road들은 이 제외된 8개에 포함된다. 이는 코드와 여러 순차 CPU 표본을 연결한 결과로, 함수 실행 이력이나 같은 GPU 프레임의 인과 실험은 아니다.

설정 객체 RVA `0x2D4CD40`의 이름은 `g_cut_plane_ignore`였으며 연결 객체 `+0x120`은 NULL, 캐시 `+0x116`은 유효, int `+0x118`은 0이었다. getter `0x1CBD70`의 실제 경로로 의미를 확인했다. 설정을 변경하지 않았다. 마지막 갱신 루프 `0x4756B4..0x4756D5`는 맵 bit 6을 core flags bit 8（`0x100`）로 옮긴다. 맵 원본 설정 `+0x34`, 런타임 상태 `+0x38`, core flags, 모델 `+0x18/+0x1B8`은 각각 구분해서 읽어야 한다.

controller `+0x968`의 별도 그룹에서 소유 맵 객체의 bit 6을 자식 맵 객체로 복사하는 코드도 있다（`0x4755E0..0x475677`）. 그러나 이번 표본은 NULL을 포함한 16 슬롯 중 4개가 비어 있지 않았고, 소유 맵 객체가 있는 두 항목의 타입은 46이었다. 이를 Compound 타입 40의 전체 상속 경로로 확정하지 않았다.

추가 자료는 `map-visibility-owner-inputs.json`（124회·22,432 bytes·약 3.57 ms）, `road-cut-plane-inputs.json`（265회·30,616 bytes·약 2.37 ms）, `road-cut-plane-core-membership.json`（36회·33,800 bytes·약 1.91 ms）이다. 모두 읽기 오류·배열 생략 없이 끝났으며 observer PID 30252·3240·27032의 handle과 프로세스를 정리했다. 정적 근거는 `map-state-bit6-writer-473c60.txt`, `road-map-item-slot60.txt`, `map-visibility-membership-predicate.txt`, `cut-plane-point-predicate.txt`, `cut-plane-point-side.txt`, `map-visibility-int-cvar-getter.txt`다. 시간은 외부 reader 측 소요이며 게임 FPS 영향의 측정값이 아니다.

독립 센서에서의 다음 확인 대상은 **센서 위치에 맞춘 cut plane 상태와 대상 목록**, 차량 표시·LOD의 갱신 순서, UID 예외 및 자식 상속이다. cut plane과 LOD 모두 선택된 게임 카메라를 참조하므로 기존 도로 상태를 공유한 채 카메라만 추가하면 센서 시점에서는 필요한 차량이 앞 단계에서 생략될 가능성이 있다. 해당 가능성의 실제 발생 빈도와 픽셀 영향은 아직 측정하지 않았다.

### Visibility area의 UID 예외와 갱신 순서

**UID 예외는 맵의 Visibility area가 지정한 객체 목록이다. 영역 활성 상태도 현재 선택된 게임 카메라로 계산된다.** 따라서 cut plane만 센서 기준으로 바꾸면 기존 예외 목록까지 함께 맞춰졌다고 볼 수 없다.

controller `+0x2FA0` 배열의 표본 48개는 모두 맵 타입 48, vtable RVA `0x22BA960`이었다. 각 영역 `V`에서 `+0x58`은 대상 UID 배열, `+0x80`은 현재 활성 byte다. `V +0x34` bit 0이 켜진 영역은 제외 목록에, 꺼진 영역은 포함 목록에 UID를 추가한다. 이 작업은 `0x4754A4..0x4754CA`와 배열 병합 함수 `0x148700`에서 확인했다. 포함 목록은 이후 제외 상태를 다시 지울 수 있어 두 목록에 동시에 있는 UID에서는 포함 쪽이 우선한다.

일반 core 항목의 bit 6 갱신 단계를 단순화하면 아래와 같다. 이미 같은 갱신 구분값을 가진 항목은 이 단계를 건너뛰며, 뒤의 소유 그룹→자식 복사는 별도다.

```text
base_excluded = core_flags[12] OR (item_cut_plane_test AND NOT g_cut_plane_ignore)
excluded = (base_excluded OR uid_in_exclude_areas) AND NOT uid_in_include_areas
```

영역의 활성 갱신 함수는 slot `+0x1A0 → 0x813710`이다. 영역 `+0x38 & 0x00400000`이면 비활성화한다. 그 외에는 `+0x48` node의 위치·회전과 영역 `+0x50/+0x54`의 두 extent를 사용해 선택 카메라의 상대 위치를 변환하고 범위 안인지 검사한다. camera manager의 override stack 우선 선택은 cut plane과 같다. 이 표본의 48개 영역에서 입력으로 계산한 활성 여부와 실제 byte가 모두 일치했다（활성 1개, 비활성 47개）.

활성 영역은 `0x1F5514653B8`, 원본 flags는 `0x2`였다. 포함 목록에 해당하며 서로 다른 UID 26개를 지정했다. 해당 UID를 core 항목과 연결해 새로 읽었을 때 Road 11개, Compound 8개, Curve 3개, 타입 1 항목 2개, Prefab 2개였고 모두 맵 bit 6과 core `0x100`이 꺼져 있었다. **예외 목록과 읽은 상태의 일치이며, 예외 적용 직전에는 반드시 제외돼 있었다는 뜻은 아니다.** 영역을 켜고 끄는 실험은 하지 않았다.

갱신 순서는 controller 함수 `0x473C60` 내부에서 다음까지 연결했다.

1. `0x475291`의 공간 질의가 vtable `0x224C7C0`, callback `0x4AACE0`을 사용한다. callback은 후보의 범위 교차를 검사하고 `0x4AADD9`에서 각 항목의 slot `+0x1A0`을 호출한다. 타입 8은 cut plane 목록 `controller +0x2FC8`에, 다른 수집 대상은 영역 목록 `+0x2FA0`에 추가한다.
2. `0x475311..0x47542F`에서 이전·현재 목록과 cut plane `+0xD0`, 영역 `+0x80`의 변화를 비교해 갱신 구분값을 바꾼다.
3. `0x4754A4..0x4754ED`에서 활성 영역의 UID 포함·제외 목록을 만든다.
4. `0x475510..0x4755DA`에서 일반 core 항목의 맵 bit 6을 갱신하고, 뒤의 그룹 복사를 거쳐 `0x475690..0x4756DF`에서 core `0x100`과 갱신 구분값을 기록한다.

이것은 같은 함수 안의 정적 실행 순서다. 전체 프레임에서 traffic actor update·LOD update·GPU 제출이 언제 실행되는지까지 관측한 것은 아니다. 이 함수는 목록 교체·core 항목 추가와 제거 등 공유 상태도 바꾸므로, 센서별 카메라만 임시 선택한 뒤 전체 함수를 반복 호출하는 구현을 안전한 해결책으로 확정하지 않았다. 필요한 분리는 센서 기준 영역 판정·대상 선택과 기존 게임 상태의 소유 범위를 함께 다뤄야 한다.

원본은 `visibility-area-uid-inputs.json`（153회·17,640 bytes·약 2.32 ms）과 `visibility-area-camera-targets.json`（132회·43,356 bytes·약 2.31 ms）이다. 오류와 배열 생략은 없었으며 observer PID 6848·16240은 handle을 닫고 종료했다. 26개 target은 UID로 연결했고 새로 읽은 UID도 동일했다. 정적 근거는 `visibility-area-camera-state-update.txt`, `visibility-area-uid-append.txt`, `visibility-query-collector.txt`, 기존 `map-state-bit6-writer-473c60.txt`다. `visibility-item-state-update.txt`와 `visibility-items-group-update.txt`에는 별도 항목 경로의 slot `+0x1A0` 호출도 있지만 해당 파일만으로 전체 프레임 순서를 주장하지 않는다.

### Compound의 부모 segment와 자식 목록

**관측한 Compound는 부모 단위로 core 후보에 등록되고 부모 segment의 geometry를 사용했다. 자식 맵 객체 목록을 독립 렌더 후보 목록으로 취급하면 안 된다.** 기존 core 표의 Compound 444개 가운데 미러 제외 설정이 켜진 25개와, 위 활성 Visibility area의 포함 대상 Compound 8개를 골랐다. 둘에 동시에 속하는 1개가 있어 서로 다른 부모는 32개다. 이는 의도적으로 고른 표본이며 전체 Compound의 대표 비율을 추정하는 자료가 아니다.

32개 부모의 vtable은 `0x22B7AF8`이다. segment getter slot `+0xB0 → 0x6C4950`은 부모 `+0x48`을 반환한다. 모두 non-NULL segment를 가졌고, segment `+0x28`의 소유 item은 해당 부모와 일치했다. core flags `0x400`은 모두 꺼져 있어, 앞 절의 비segment 항목 호출보다 segment 후보 선택 경로에 해당한다.

부모 `+0x58`의 자식 배열에는 총 211개가 있었고, 중복을 제거해도 211개였다. Curve 100개, Hookup 79개, 타입 1 항목 20개, Model 12개다. 이 자식들 가운데 같은 시점의 core 표에 직접 등록된 것은 없었다. Curve·Model·타입 1 자식의 `+0x48`은 모두 NULL이었다. Hookup의 해당 필드는 일반 segment 포인터가 아니므로 같은 방식으로 해석하지 않았다.

미러 제외 부모 아래의 Curve 91개 중 82개는 자신의 미러 제외 bit도 켜져 있었지만 9개는 꺼져 있었다. Model 10개와 Hookup 79개는 자신의 제외 bit도 켜져 있었다. 따라서 부모 설정과 자식별 원본 설정이 항상 같은 값으로 복사돼 있다고 가정하지 않는다. 표본만으로 그 설정의 생성·편집 이력까지 확정한 것은 아니다.

부모 segment의 `+0xC8` geometry 배열을 읽었을 때 1,164개 기록 중 `+0x10`이 non-NULL인 렌더 객체 Q는 215개였고 모두 서로 다른 주소였다. 부모 1개는 geometry 배열이 비어 있었다. segment `+0x30`의 부가 unit 배열은 32개 모두 비어 있었다. 부모의 별도 `+0x3B8` 구조도 모두 NULL, 맵 상태 bit 16도 모두 0이었다. 따라서 `compound-item-render-entry.txt`의 `0x7F92B0`（bit 16 검사 후 `+0x3B8`을 소비）을 이 표본의 일반 geometry 진입점으로 잘못 연결하지 않았다. 그 함수는 존재하지만 이번 상태에서는 해당 분기를 통과하지 않는다.

앞서 확인한 공통 경로에서는 부모의 core 미러 제외 `0x20`을 먼저 검사하고, 통과한 segment에서 LOD·시야 조건에 맞는 Q를 선택한다. 이 연결은 부모 segment가 보유한 geometry 묶음에 적용된다. **211개 원본 자식 각각의 모든 geometry가 어느 Q에 합쳐졌는지까지 복원한 결과는 아니다.** 자식의 부가 unit·동적 객체, 다른 로딩 상태의 별도 경로는 추가 확인 대상이다.

Visibility area 포함 예외와 미러 제외가 동시에 적용된 예는 Compound `0x1F750853FD0`, UID `5731ab897b8f20c9`다. 원본 flags `0x50`으로 미러 제외 bit 6이 켜져 있고 core flags는 `0x8B033`이었다. 여기에는 기본 제외 원인이 되는 `0x1000`과 미러 제외 `0x20`이 모두 있지만, 맵 `+0x38 = 0x04000080`의 최종 bit 6과 core `0x100`은 꺼져 있었다. 활성 영역의 포함 예외와 일치한다. 그러나 **영역 예외는 core `0x20`을 지우지 않으므로 미러 제외까지 해제하지 않는다.** 이 부모의 geometry 21개 중 non-NULL Q는 3개였다.

미러 후보와의 연결은 부분적으로만 포착했다. 첫 표본은 현재 제출 미러 수가 0이었다. 0.2초 간격으로 최대 8번 읽은 후속 관측에서는 첫 표본에 slot 0·1·2·5가 있었으나 slot 5의 결과 목록이 비어 있었고, 나머지 일곱 표본에서는 제출 목록 자체가 비어 있었다. 비어 있는 목록을 해당 물체가 항상 보이지 않는다는 근거로 사용하지 않았다.

첫 표본의 slot 0·1·2에서 읽은 Q 후보는 각각 655·1,025·602개였다. slot 2에는 선택한 Compound 두 개（`0x1F750864DD0`, `0x1F798D945B0`）가 보유한 Q 11개가 포함돼 있었다. 두 부모 모두 미러 제외 bit가 꺼져 있었다. 미러 제외 부모의 Q가 이 목록들에서 발견되지는 않았지만, 거리·시야 등 다른 조건도 있으므로 부재를 미러 flag 하나의 인과 검증으로 확대하지 않는다. segment와 후보 목록은 순차 관측이며 GPU 프레임을 고정한 결과가 아니다.

자료는 `compound-children-and-runtime.json`（311회·90,268 bytes·약 3.96 ms）, `compound-segment-mirror-candidates.json`（161회·600,008 bytes·약 2.64 ms）, `compound-mirror-candidate-followup.json`（8표본·23회·19,376 bytes·약 1.40초, 대기 포함）이다. 읽기 오류와 배열 생략은 없었다. observer PID 15792·22316·22216은 handle을 닫고 종료했다. 이 관측에서 소유 관계와 조건은 좁혔지만 게임 메모리·입력·설정은 변경하지 않았다.

### Mover로 배치된 인물의 모델 선택과 제출

**주변 Hookup에서 찾은 세 mover는 모두 사람 모델이었다. 이 경로는 게임 카메라 기준으로 모델 하나를 선택한 뒤, 렌더 뷰마다 별도의 시야 검사를 수행한다.** 차량처럼 주 화면·미러용 LOD 인덱스 두 개를 보관하는 경로와는 다르다. 따라서 독립 센서가 다른 위치에서 인물을 관측하려면 시야 검사뿐 아니라 선행 모델 선택·거리 숨김도 고려해야 한다.

앞선 Hookup/부착 unit 표본에서 `mover_hookup` 세 개를 찾았다. `pedestrian_hookup`은 그 표본에서 발견되지 않았다. 이는 다른 종류의 보행자 객체가 게임 전체에 없다는 뜻이 아니다. 또한 `mover`는 움직이는 배경 객체의 일반 분류이므로 클래스 이름만으로 사람이라고 판정하지 않았다.

관측한 연결은 다음과 같다. 함수·vtable 값은 설치 EXE의 RVA다.

```text
U: mover_hookup (vtable 0x229BC00)
  slot +0x98 → 0x6F0E20
  *(U +0x160) → M: mover instance
    *(M +0xF8) → D: mover_desc
    M +0x78    → LOD별 model_object 포인터 배열
    *(M +0xA8) → 현재 선택한 model_object O
    0xA1B740(M, render_view) → O의 slot +0x90
```

`D +0xB0`의 동적 문자열과 `D +0x140`의 LOD 문자열 배열을 읽어 설치 파일 `def/world/mover_desc.sii`와 대조했다. 기본 모델 경로·LOD 경로·거리 경계가 각각 다음 정의와 일치했고, 세 정의 모두 `group_tags[]: human`을 갖는다. 아래 정의 이름은 이 파일 대응에서 얻은 것이며 런타임 unit 이름 문자열을 직접 읽은 결과는 아니다.

| 대응 정의 | 기본 모델 파일 | 게임 카메라와의 3D 거리 | 선택 모델 | 보유 Q 수 |
|---|---|---:|---|---:|
| `mover_desc.n_servman` | `/model/mover/characters/new_generation/new/models/man/serviceman/ngn_serviceman_lod0.pmd` | 약 129.387 | 배열 index 3 / LOD 3 | 1 |
| `mover_desc.woman.asia.casual.4` | `/model/mover/characters/models/generic/w_asia_04.pmd` | 약 136.629 | 배열 index 3 / LOD 3 | 2 |
| `mover_desc.man.cauca.casual.4` | `/model/mover/characters/models/generic/m_white_01.pmd` | 약 128.237 | 배열 index 3 / LOD 3 | 2 |

이 결과는 인물 모델의 식별이며 실제로 걷고 있었는지, 화면에 보였는지, GPU에 그려졌는지의 관측은 아니다. Q 수는 선택 모델이 보유한 원본 렌더 객체 수이고 실행 draw 수가 아니다.

LOD 갱신 함수 `0xA1A730`의 `0xA1A9FA..0xA1AC61`은 선택된 게임 카메라를 얻어 mover pose와의 **Y를 포함한 3차원 거리 제곱**을 계산한다. camera manager의 override stack이 있으면 마지막 원소, 없으면 현재 index를 사용한다. 이 부분은 차량 경로의 수평 거리와 다르다. 카메라 FOV 보정과 배율은 다음과 같다.

```text
F = max(0.637070298 / tan(FOV_degrees * pi / 360), 1)
s = g_lod_factor_pedestrian
각 거리 경계의 비교: distance_squared >= (s * F)^2 * threshold_squared
```

제곱 경계 배열은 `D +0x168`이다. `M +0xA0`의 시작 경계 인덱스부터 비교하며, 모델 배열의 index 0에서 시작해 통과한 경계마다 다음 슬롯을 선택한다. 다음 슬롯이 NULL이면 기존 모델 포인터를 유지하면서 `M +0x158` byte의 bit 2를 켜 숨긴다. 이후 non-NULL 슬롯을 선택하면 이 bit를 다시 지운다. 따라서 `M +0xA8`에 모델이 남아 있다는 사실만으로 표시 상태라고 판단하면 안 된다.

표본의 시작 인덱스는 모두 0이었다. 첫 인물은 거리 경계 `18, 49, 128, 160`, 나머지 둘은 `18, 42, 69, 153, 248`을 사용했으며 마지막 LOD 경로는 빈 문자열이었다. 실제 배율 `s=1`, FOV 약 65°로 `F=1`이었다. 외부에서 계산한 선택 포인터와 숨김 bit가 세 객체 모두 읽은 값과 일치했다. 이는 한 상태의 순차 표본 대조이며 경계를 넘어 이동시키는 실험은 아니다.

pose getter `0x4575E0(M +0x08)`는 캐시가 유효하면 이를 반환하고, 아니면 상대 pose와 parent를 합성해 캐시를 갱신한다. 세 표본의 캐시 상태 `M +0x60`은 모두 1이었다. 따라서 표의 거리는 유효한 캐시의 local XYZ·X/Z cell을 이용해 계산했다. 다른 객체의 캐시가 무효인 경우에도 같은 메모리 필드를 그대로 읽으면 항상 정확하다고 일반화하지 않는다.

렌더 제출 함수 `0xA1B740`은 선택 O가 non-NULL이고 `O +0x08`의 bit 31이 켜져 있어야 하며, `(M +0x158 & 0x0C) == 8`도 요구한다. 초기화 bit 3은 켜지고 거리 숨김 bit 2는 꺼져야 한다. 세 표본은 이 선행 조건을 통과했다. 이어 뷰의 vtable slot `+0x40`에 pose와 `M +0x68`을 넘겨 시야를 검사한다. 앞서 관측한 미러 렌더 뷰 vtable `0x222AC50`의 해당 함수는 `0x1518290`이다. 이 함수는 로컬 중심을 pose로 변환하고 `M +0x74`의 반경을 `0x13D2120`에 전달한다. 따라서 구 경계 기반의 뷰 판정이며 픽셀 단위 가림 검사가 아니다. 세 인물에 대한 각 미러의 실제 판정 결과까지 이번 표본에서 읽은 것은 아니다.

통과하면 O의 slot `+0x90 → 0x15E0E50`을 호출한다. `0x15E12A0`은 모델 준비 상태와 part 활성 flag를 검사하고, `0x15E0F20`은 모델 metadata의 part별 Q 구간을 `O +0x2F0` 배열에서 골라 뷰의 collector에 넘긴다. part flag 데이터는 `O +0x190` 포인터, 각 원소의 bit 0이다. `0x15E1380`에는 부가 child unit을 제출하는 별도 경로가 있다. mover 자체의 `M +0xB8` 배열에도 동작 상태에 따라 제출하는 부가 model/hookup 경로가 있으므로 선택 O만으로 모든 소품을 대표한다고 가정하지 않는다.

현재 함수 안에는 차량 경로와 같은 직접적인 미러 제외 검사나 미러 전용 LOD 계산이 없었다. 그러나 Hookup 소유 항목의 상위 후보 선택과 각 뷰의 시야 검사 등은 계속 적용되므로, 이를 “사람은 모든 미러에 항상 나타난다”는 결론으로 확대하지 않는다. 다른 `pedestrian_hookup`의 정적 경로는 다음 절에서 구별했다. 해당 클래스의 실제 객체, 전체 프레임의 애니메이션·LOD·제출 순서, 실제 GPU 인물 픽셀은 미확인이다.

원본은 `mover-hookup-model-inputs.json`（34회·5,080 bytes·약 0.34 ms）, `mover-lod-and-geometry-inputs.json`（40회·7,464 bytes·약 0.52 ms）, `mover-definition-paths.json`（31회·3,101 bytes·약 0.54 ms）이다. 읽기 오류와 배열 생략은 없었다. observer PID 18788·22092·9232는 handle을 닫고 종료했음을 확인했다. 정적 근거는 `mover-instance-update.txt`, `mover-instance-render-entry.txt`, `mover-render-view-test.txt`, `mover-pose-buffer-getter.txt`, `model-object-prepare-render.txt`, `model-object-part-submit.txt`다. `mover-instance-initialize.txt`는 이름과 달리 준비 상태를 조회하는 helper이며, `mover-hookup-activation-entry.txt`의 역할도 활성화로 확정하지 않았다.

### 별도 pedestrian_hookup 클래스의 정적 경로

`pedestrian_hookup`은 위 mover instance를 호출하는 wrapper와 다른 클래스다. 타입 descriptor `0x1E18290`은 이름 포인터 `0x2D73450`을 거쳐 문자열 `pedestrian_hookup`에 연결된다. 생성 함수 `0x6F2EB0`은 vtable `0x229C830`을 설치하며, descriptor getter는 `0x6F33F0`이다. 이 식별은 설치 EXE에서 확인했고 실제 게임 객체로는 아직 관측하지 않았다.

이 클래스의 slot `+0x88 → 0x6F4610`은 선택 게임 카메라와 자신의 pose로 모델을 고른다. 거리 helper `0x12FAF0..0x12FB58`은 Y를 포함한 3차원 거리 제곱을 반환한다. 앞 절과 같은 `g_lod_factor_pedestrian`·FOV 배율로 나눈 값을 `U +0x120` 경계 배열과 비교하고, `U +0xF0`의 모델 포인터 배열에서 선택한 값을 `U +0x118`에 기록한다. 이 경로도 렌더 뷰별로 모델을 따로 고르지 않는다. 선택 슬롯이 NULL이면 `+0x118`도 NULL이 될 수 있다. mover instance에서 기존 모델을 유지하며 숨김 bit를 켜는 동작과 구별해야 한다.

제출 함수 slot `+0x98 → 0x6F4560`은 다음 순서로 검사한다.

1. 뷰 `R +0x18`과 mask `0x003FFFC0002003F0`의 교집합이 있으면 반환한다.
2. `U +0x118`의 선택 모델이 존재하고 그 모델 `+0x13C`의 bit 8이 켜져 있어야 한다.
3. 모델 metadata의 구 경계와 모델 pose를 뷰 slot `+0x40`에 넘겨 시야를 검사한다.
4. 통과한 모델의 slot `+0x90`을 호출한다.

기존 `render-camera-contexts.json`의 미러 0·1·2·5 mask（각각 `0x400`, `0x800`, `0x1000`, `0x8000`）는 첫 제외 mask와 겹치지 않는다. 따라서 이 함수가 해당 미러들을 종류만으로 일괄 제외하는 것은 아니다. 이는 저장된 뷰 입력과 정적 조건의 대조이며, 실제 pedestrian 객체가 이 조건을 통과해 렌더링됐다는 관측은 아니다. 생성·활성화·애니메이션의 전체 조건은 아직 연결하지 않았다.

근거는 `pedestrian-hookup-constructor.txt`, `pedestrian-hookup-frame-update.txt`, `pedestrian-hookup-submit.txt`, `pedestrian-camera-distance.txt`다. `pedestrian-hookup-registration.txt`의 문자열 해시 초기화는 클래스 존재의 보조 근거이며, 그 파일의 첫·마지막 경계 밖 명령은 해석에 사용하지 않았다. `pedestrian-hookup-update.txt`는 slot `+0xB0`에서 모델 배열을 순회하는 helper로, 실제 모델 선택은 `frame-update` 파일에 있다.

### 프레임 안의 가시성·모델 선택·미러 제출 순서

**서로 따로 추적했던 도로 가시성, 인물·차량 LOD, 미러 목록 생성과 장면 제출을 하나의 상위 호출 경로로 연결했다.** 아래는 설치 EXE의 조건부 호출 순서다. 실행 중 명령을 추적하거나 GPU 프레임을 고정한 기록은 아니다.

전역 `S = *(EXE +0x36AE718)`의 vtable은 관측 시 `0x2241278`이었고, slot `+0x08`은 프레임 함수 `0x426360`이다. `S +0xAA0`의 controller가 `*(EXE +0x36AE6D8)`와 같았으며, 실제 클래스는 `game_ctrl`, vtable은 `0x2250BE0`이었다. 따라서 이 프레임 함수의 controller 간접 호출을 현재 게임 클래스의 구현에 연결할 수 있다.

| 프레임 내부 호출 위치 | controller slot / 구현 | 확인한 역할 |
|---|---|---|
| `0x426AB4`, `0x426AD1` | camera manager `0x501810`, 현재 index 카메라 slot `+0x68` | 도로 가시성 판정보다 앞선 카메라 갱신 |
| `0x426ADF` | `+0x80 → 0x473C60` | cut plane·Visibility area·core/맵 제외 상태 갱신 |
| `0x426AF0` | `+0xE8 → 0x4CFC20` | 중간 controller 갱신 단계. 이 단계 전체를 물리 갱신이나 pose 보간으로 단정하지 않음 |
| `0x426B0B` | `+0xF0 → 0x4CFDB0` | Hookup 갱신과 차량 LOD 갱신을 포함한 준비 |
| `0x426B26` | `+0xB0 → 0x4DB6D0` | `0x538860`으로 미러 카메라·제출 기록을 `C +0x3AC0`에 생성 |
| `0x426BBB` | `+0x108 → 0x4D0D80` | 조건에 맞으면 `0x4D4450`으로 미러 렌더 작업 구성 |
| `0x4D4A06` | 위 작업 안의 `0x4D4210` | 해당 미러의 맵 결과와 활성 차량 모델 제출 |

표는 관련 단계만 추렸다. 중간에 다른 렌더 준비 함수도 실행된다. 특히 `0x426B8D`의 controller slot `+0x110`도 미러 작업보다 먼저 호출되므로 이 표를 전체 함수 목록으로 사용하지 않는다.

일반 순서를 읽을 때 두 제어값을 구별해야 한다. 프레임 함수가 읽은 `S +0xACC`가 0인 분기에서 현재 index 카메라의 slot `+0x68`과 controller `+0x80`을 호출하며, controller `+0xF0`도 다시 읽은 `S +0xACC == 0`일 때 호출한다. `S +0xAC4 == 0`일 때 미러 목록 생성 `+0xB0`을 호출한다. 미러 작업 진입 `+0x108` 자체는 다른 분기에서도 호출될 수 있으나, 그 내부 `0x4D0D80`이 `S +0xAC4 > 0`이면 실제 미러 구성 전에 반환한다. 이 숫자들을 근거 없이 “일시정지” 등의 제품 설정명으로 바꾸지 않았다. 관측한 `S +0xAC0/+0xAC4/+0xAC8/+0xACC`는 모두 0이었다. 표본이 전체 실행 시간 동안 같은 분기를 사용했다는 뜻은 아니다.

`0x4CFDB0` 내부에서는 먼저 `0x47AF10`을 호출한다. 후자는 근처 맵 item의 slot `+0x190`을 순회한 다음, **controller `C +0x800`을 sentinel로 하는 unit 연결 목록**을 순회하며 각 unit의 slot `+0x88`을 호출한다. 노드 `+0x10`이 unit 포인터다. 이 호출 위치는 `0x47AFF7`이다.

관측한 `mover_hookup`의 slot `+0x88 → 0x6F0E40`은 `U +0x160`의 mover instance를 `0xA1A730`으로 갱신한다. 별도 `pedestrian_hookup`의 같은 slot은 `0x6F4610`이다. 따라서 앞 절의 인물 LOD 선택은 이 unit 갱신 단계에 연결된다. 뒤이어 같은 `0x4CFDB0`의 `0x4D034E`가 `0x62D970`을 호출해 활성 차량 모델의 slot `+0x30`을 갱신한다. 이 두 호출 사이에는 다른 controller 작업도 있으므로, 인물·차량이 동시에 갱신된다고 해석하지 않는다.

현재 연결 목록은 sentinel까지 끝났고 unit 449개를 읽었다. 종류별로 parked vehicle 34, light source 353, flare lamp 4, garage door 2, sign editable area 20, speed camera 2, flare blink 1, LOD model hookup 1, license plate 28, mover hookup 3, sound hookup 1이었다. 앞 절의 세 mover 주소 모두 이 목록에 있었다. 이 목록에는 `pedestrian_hookup`이 없었다. 이는 해당 순간 이 controller의 갱신 목록에 대한 결과이며, 로딩되지 않은 지역이나 다른 소유 목록까지 부재를 증명하지 않는다.

미러 단계의 연결도 구별된다. `0x4DB6D0 → 0x538860`은 **렌더할 미러 목록을 만드는 단계**다. 그 뒤의 `0x4D0D80 → 0x4D4450 → 0x4D4210`이 목록을 사용해 장면을 제출한다. `0x4D4210` 안에서 `0x47D620`이 맵 후보 Q를 수집한 뒤 `0x62DA80`이 활성 차량 모델의 slot `+0x38`에 해당 미러 뷰를 전달한다. CPU의 작업 구성·객체 제출 순서를 GPU 실행 완료 시각과 동일시하지 않는다.

센서 구현에는 두 가지 의미가 있다. 먼저, 센서마다 game controller의 가시성·LOD 갱신 전체를 다시 실행하는 방식은 공유 목록과 모델 선택을 바꾸므로 단순한 카메라 교체로 끝나지 않는다. 둘째, 이미 선택된 물체·모델을 제출하는 지점과 각 미러 pass 완료 후의 픽셀 복사 지점을 연결할 단서가 확보됐다. 후속 조사에서는 아래처럼 **AI 갱신·물리 step·telemetry 전달·자세 이력 발행·보간을 이 렌더 준비 단계 앞에 연결했다.** GPU 실행 완료와 개별 telemetry 채널의 원본 상태까지 같은 시각으로 연결한 결과는 아직 없다.

실측 자료는 `frame-order-controller-vtable.json`（9회·1,560 bytes·약 0.15 ms）과 `frame-order-update-list.json`（961회·17,168 bytes·약 5.59 ms）이다. 후자는 배열 대신 연결 목록을 최대 1,024개까지만 읽도록 제한했고 실제로 449개에서 sentinel에 도달했다. 읽기 오류는 없었다. observer PID 27716·22132는 handle을 닫고 종료했다. 이 시간은 외부 reader 소요 시간이며 게임 frame time이나 FPS 영향의 측정값이 아니다.

정적 근거는 `render-frame-controller-dispatch.txt`, `controller-render-frame-prepare.txt`, `controller-prepare-prelude.txt`, `controller-frame-slot-b0.txt`, `mirror-render-dispatch-entry.txt`, `mirror-graph-frame-submit.txt`다. `frame-stage-callers.json`은 호출 후보 탐색 자료이며 실제 연결은 해당 disassembly와 읽은 vtable을 기준으로 했다. 중간 helper `0x4DC340`, slot `+0xE8`의 prelude `0x47AA50`, slot `+0xB0`의 tail `0x488D50`도 별도 파일에 보존했으나 이들을 각각 모델 갱신·pose 보간·가시성 query 생성이라고 판정하지 않았다.

### 물리 step과 SDK frame 이벤트는 렌더 준비보다 먼저다

**SDK의 `frame_end`를 화면 렌더 완료나 GPU 복사 완료로 사용할 수 없다.** 현재 빌드의 상위 프레임 함수 `0x426360`은 `0x42697F`에서 simulation 함수 `0x4260C0`을 호출하고, 반환 뒤 `0x4269A0`에서 `0x742D20`으로 렌더용 자세를 보간한다. 앞 절의 카메라·가시성·LOD·미러 준비는 이보다 뒤에 있다. 다음 순서는 disassembly와 현재 객체의 vtable 연결로 확인했으며, callback을 설치해 실행 시각을 계측한 기록은 아니다.

1. 프레임 경과 시간을 step 누산기에 더하고, 필요한 simulation step을 반복한다.
2. 물리 시뮬레이션이 정지되지 않은 step에서는 controller slot `+0x90 → 0x4CF6B0`으로 사전 갱신한다. 여기에 AI 배치 갱신 `0x62CAA0`이 포함된다.
3. controller `+0x2D90`, 그리고 서로 다르면 `+0x2D98`의 world를 slot `+0x08 → 0x1651BA0`으로 각각 갱신한다.
4. controller slot `+0x98 → 0x4CF8A0`으로 사후 갱신한다. traffic의 `0x62C770`과 player actor 갱신이 이 단계에 있다.
5. 같은 step 반복 안의 `0x4261D5 → 0x165E910`에서 SDK 이벤트와 채널 전달을 처리한다. 시뮬레이션이 정지된 분기에서는 2–4를 건너뛰어도 이 전달 경로에 도달한다.
6. step 반복을 마친 뒤 `0x7427A0`과 controller slot `+0xA0 → 0x4CF940`으로 자세 이력을 기록한다. 초기화 조건에서는 별도 이력 초기화도 선행한다.
7. simulation 함수가 렌더 시각을 증가시키고 반환한다. 상위 함수가 자세를 보간한 뒤 카메라·도로 가시성·LOD·미러 작업을 준비한다.

관측한 step 전역 `EXE +0x3676488`은 **16,666 μs**였고, 초 단위 변환 배율은 `0.000001`이었다. 약 60 Hz의 simulation step이라는 뜻이며 화면 FPS나 모든 AI의 판단 빈도를 뜻하지 않는다. 이 함수는 누산값이 양수인 동안 step을 실행하므로 렌더 반복 한 번에 SDK 전달 경로가 여러 번 실행되거나 실행되지 않을 수 있다. 실제 callback 수나 FPS를 계측한 것은 아니다.

| state `S` 필드 | SDK와 연결한 의미 | 이 경로의 갱신 |
|---|---|---|
| `+0xB20` | signed step 누산 잔여값 | 프레임 delta를 더하고 매 step 길이를 뺌 |
| `+0xB28` | `simulation_time` | 정지 여부와 무관하게 매 step 증가 |
| `+0xB30` | `render_time` | step 반복과 SDK 전달 뒤에 프레임 delta를 더함 |
| `+0xB38` | `paused_simulation_time` | 물리 시뮬레이션이 정지되지 않은 step에서만 증가 |
| `+0xB48` | 현재 프레임 delta | 상위 함수가 계산해 저장한 값. SDK payload의 별도 시각 필드는 아님 |

예시 표본은 `B20=-7,669`, `B28=24,301,244,578`, `B30=24,301,236,909`, `B38=23,487,660,456`, `B48=33,333`이었다. `B30-B28`이 누산 잔여값과 일치했지만 여러 메모리 읽기의 결과이므로 원자적 스냅샷이나 GPU 동기화의 증명으로 사용하지 않는다. `+0xB40`을 64-bit 시각으로 해석하지 않았다.

SDK payload에서 시각을 혼동할 여지가 있다. `0x165E910`은 현재 `B30/B28/B38`을 복사한 32-byte payload로 event 1을 전달한다. 그 뒤 gameplay event 처리와 임시 목록 정리 `0x1661310`, 정지되지 않은 경우의 채널별 값 전달 `0x1664710`, event 2 전달이 이어진다. 공식 `scssdk_telemetry_event.h`의 선언에 따라 event 1은 `frame_start`, event 2는 `frame_end`다. 이벤트 수신 목록은 `EXE +0x3676E00 + event_id*0x70`에서 순회한다. gate `EXE +0x369240F`는 표본에서 1이었다. 후속 관측에서는 frame_start·frame_end·paused·started에 각각 callback 하나가 있었고 모두 기존 `Real_G27_ffb_x64.dll`의 주소였다. 우리가 callback을 설치하거나 호출한 것은 아니다.

따라서 한 렌더 반복에서 여러 simulation step이 처리되면, 이 경로의 SDK payload들은 동일한 `render_time`과 서로 다른 `simulation_time`을 가질 수 있다. 렌더 보간은 그 뒤 증가한 `B30`을 사용한다. 이것을 일정한 한 프레임 지연으로 단순화하지 않는다. 영상과 차량 데이터를 묶으려면 SDK의 세 시각·restart 상태를 보존하고, 실제 센서 pass의 보간된 자세와 픽셀 복사 시점을 별도로 연결해야 한다. ETS2LA plugin의 `src/core.cpp`는 `frame_end`에 `telemetry_tick`을 등록하므로, 그 callback에서 읽는 내부 상태를 이미 완성된 같은 렌더 장면이라고 가정할 수 없다.

채널의 원본도 일부 연결했다. 현재 `truck.world.placement`는 `game_physics_vehicle`의 slot `+0xF8 → 0x866C10`에서 본체 물리 객체의 자세를 가져오며, 렌더 parent의 slot `+0xE8 → 0x866B10`과 다른 경로다. local linear/angular velocity 역시 물리 body를 읽는다. scalar speed는 차량 좌표계 속도의 `-Z`로, linear acceleration은 local velocity 차분의 10-step 이동평균으로 물리 사후 갱신에서 저장된다. 현재 step에서는 약 166.66 ms의 변화 구간을 반영하므로 동일한 callback에서 나온다는 이유로 모든 채널을 같은 시각의 순간값이라고 취급하지 않는다. 원본 getter·실제 캐시 값·등록 flag와 계산 조건은 [실행 중인 SDK 채널 분석](11_idle_memory_and_telemetry.md#실행-중인-sdk-채널에서-실제-원본까지-연결)에 정리했다.

#### 실제 world에서 PhysX 실행과 결과 대기를 연결

두 world의 member 배열에는 각각 객체 하나가 있었다. 각 member의 `+0xDC` skip counter는 0, `+0xD8` step counter는 모두 1,417,793이었다. world 갱신 함수는 skip counter가 양수이면 건너뛰고, 그렇지 않으면 member slot `+0xD8`에 step 초 값을 전달한다. 두 객체의 역할 전체나 서로의 물리 대상 분담을 확인한 것은 아니다.

| world member 구현 | 읽은 backend와 호출 | 판정 |
|---|---|---|
| `0x1633630` | member `+0x178`의 backend, vtable RVA `0x2500CD8`; slot `+0x1C0 → 0x1AE3710`, `+0x1E8 → 0x1AE3FE0` | `PxScene::simulate`, `PxScene::fetchResults`의 내장 오류 문자열과 구현을 연결. 호출자는 simulate 다음 `fetchResults(true, &errorState)`를 호출 |
| `0x161BAC0` | member `+0x138`의 backend, vtable RVA `0x250DA80`; slot `+0x60 → 0x1D0F230` | `stepSimulation` 문자열과 `(dt, 0, 1/60)` 인수는 확인. 라이브러리·클래스의 확정 식별은 보류 |

PhysX 경로에는 `D:\manual\third\physx\version_patched\PhysX_3.4\Source\PhysX\src\NpScene.cpp`가 들어 있었다. 이는 patched PhysX 3.4 계열이라는 근거이며 upstream 그대로의 빌드라는 뜻은 아니다. 공식 [PhysX 3.4 `PxScene.h`](https://github.com/NVIDIAGameWorks/PhysX-3.4/blob/master/PhysX_3.4/Include/PxScene.h)는 `fetchResults`의 `block=true`가 결과를 기다린다고 명시한다. 실제 함수도 true 인수를 무한 대기 값으로 변환하는 경로를 가진다. 따라서 정상적인 이 호출 경로에서는 **PhysX 결과 수신 뒤에 controller 사후 갱신과 telemetry 전달로 진행한다.** 성공·실패 반환을 실시간 추적하거나 우리가 해당 함수를 호출한 것은 아니다.

#### AI 판단의 배치 처리와 가시성 상태

사전 갱신의 `0x62CAA0`은 phase·countdown·처리 index로 actor 배열을 나눠 처리한다. 두 주요 phase의 `0x62D190`, `0x62D258`에서 actor slot `+0x18`을 호출하며 넘기는 명목 delta는 약 **0.1초**다. 현재 배열의 AI 50개는 모두 type 1, vtable RVA `0x22D9208`, 해당 slot 구현 `0x922B30`이었다. 표본 scheduler는 phase 2, countdown 2, index 36이었다. 이 조건만으로 각 차량이 정확히 매 100 ms마다 호출됐다고 판정하지 않는다. 거리·상태·처리 예산에 따른 분기와 별도 매-step 갱신도 있다.

앞서 차량 표시 상태를 기록한다고 해석한 `0x922FD1..0x92300C`는 이 AI 갱신 함수 안에 있다. 따라서 상위 호출 경로상 그 상태 기록은 **뒤의 렌더 단계에서 도로 가시성을 갱신하기 전**에 일어날 수 있다. 센서용 도로 가시성과 LOD만 다시 계산해도 AI가 저장한 표시 상태까지 같은 시각으로 갱신된다고 가정하면 안 된다. 실제 지연 시간이나 GPU 누락의 크기는 계측하지 않았다.

실측 자료는 `simulation-clock-and-worlds.json`（10회·392 bytes·약 0.19 ms）, `simulation-world-members-and-ai.json`（69회·1,863 bytes·약 0.55 ms）, `simulation-backend-vtables.json`（16회·184 bytes·약 0.19 ms）이다. observer PID 21668·22036·22632는 handle을 닫고 종료했다. 정적 근거는 `frame-time-step-dispatch.txt`, `simulation-time-event-dispatch.txt`, `telemetry-event-broadcast.txt`, `controller-sim-slot-90.txt`, `controller-sim-slot-98.txt`, `controller-sim-slot-a0.txt`, `traffic-simulation-pre-step.txt`, `traffic-simulation-post-step.txt`, `physics-world-step-entry.txt`, `simulation-world-member-1633630.txt`, `simulation-world-member-161bac0.txt`, `physics-backend-simulate-entry.txt`, `physics-backend-fetch-entry.txt`, `auxiliary-backend-step-entry.txt`다. 이 파일들은 모두 `research/live/2026-10-08-render-path/` 아래에 있다.

## 깊이 복원 상수와 투영 행렬의 후속 추적

셰이더 이름과 실행 파일의 uniform 등록 함수를 연결했다. 아래 주소는 이 설치 빌드의 RVA이며, 함수 이름은 역할을 설명하기 위한 것이다. 게임 함수를 실행하거나 호출한 결과가 아니라 disassembly의 데이터 흐름이다.

| uniform | 등록 위치 / callback | 읽는 값 |
|---|---|---|
| `deferred_param_viewport_coord_to_es_ray` | `0x14839C0` / `0x1481560` | callback 인수 `U`의 `+0x18` 포인터가 가리키는 슬롯 테이블에서 `+0x50` 포인터 `D`를 얻고, `D +0x28`의 float4를 출력 |
| `deferred_param0_data` | `0x1483939` / `0x1481330` | 같은 `D`의 `+0x18`, `+0x38`, `+0x268/+0x26C/+0x270`을 조합 |
| `transform0_data` | `0x3ED77C` / `0x3EB9F0` | `0x3EB290`이 조립한 13개 float4를 uniform 출력 버퍼로 복사 |

첫 두 callback은 `U +0x20`의 `0x400` 비트로 해당 슬롯의 사용 여부를 판별한다. 이 표의 `U`는 callback 실행 시의 인수다. 외부 reader가 직접 그 인수를 확보한 것은 아니다.

미러 준비 함수 `0x147B0E0`은 context `+0x158`이 가리키는 scene 객체 `O`를 `0x1465550`에 전달한다. 이 함수는 `Ds = *(O +0x3B9D8)`에 상수를 기록하고, `0x1465B96`에서 `Ds +0x28`에 ray 생성식의 float4를 쓴다. 객체 할당·해제 경로에서 크기는 `0x278`이다. 후속 조사에서 이 객체의 실제 타입 이름 `pp_deferred_state`, 제출용 포인터로의 이동, component 묶음과 callback 슬롯으로의 전달을 확인했다. 경로는 아래 절에 정리했다.

셰이더의 복원식은 다음과 같다. `uv`는 셰이더가 viewport 변환을 적용한 좌표이며, 원본 이미지의 픽셀 index를 그대로 대입하는 값이 아니다.

```text
A = deferred_param_viewport_coord_to_es_ray
z = attributes_0.w
eye_position = (uv.x * A.z + A.x, uv.y * A.w + A.y, 1) * z
range = length(eye_position)
```

생성 함수는 수평·수직 FOV에 `π/360`을 곱한 뒤 tangent를 계산하고, shift와 crop 조건을 반영한다. 해당 보정 입력이 0이고 crop이 전체 화면인 기본 분기는 `H = tan(hfov/2)`, `V = tan(vfov/2)`에 대해 `A = (H, -V, -2H, 2V)`가 된다. 이것은 코드에서 유도한 기본 분기 식이며, 이번 미러의 실제 uniform 값을 측정했다는 뜻은 아니다. 실제 수집에서는 같은 pass의 `A`를 저장해야 한다. `attributes_3`의 무효·interleaved 조건도 앞 절처럼 함께 처리한다.

scene 객체 `O +0x3BEE0`에는 4×4 투영 행렬이 저장된다. 생성 경로는 `0x1465550 → 0x13BDAF0` 또는 crop 분기의 `0x13BDBB0 → 0x13BD9F0`이다. 마지막 함수가 만드는 행렬은 `w_clip = -z_eye`, near/far의 NDC Z가 `-1/+1`인 CPU 표현이다. 후속 추적에서 이 행렬이 DX11 셰이더 상수에 도달하기 전에 **near를 1, far를 0으로 보내는 reversed Z 변환**을 거치는 경로를 확인했다. 따라서 CPU 행렬을 그대로 DX11 depth 역투영에 사용하면 안 된다. 아래에서 변환 경로를 설명한다. 같은 draw의 행렬과 GPU 픽셀을 함께 확보한 것은 아니다. `0x3EDDF0`은 4×4 복사 함수일 뿐 depth 변환 함수가 아니다.

후속 외부 관측의 결과와 한계는 다음과 같다.

- 잘못된 `+0xBC0` 경로로 시도한 6초/120표본은 읽기에 실패했다. 결과는 `camera-uniform-samples.json`이며 유효한 uniform 자료가 아니다.
- 바로잡은 `+0xBB0` 경로의 6초/120표본에서는 context와 scene 객체를 읽었지만 `*(O +0x3B9D8)`이 모두 NULL이었다. `deferred-camera-uniform-samples.json`의 오류는 이 NULL 소비 지점이다.
- 이어 4초/40표본에서 scene의 투영 행렬은 읽혔다. 그러나 context FOV는 `-1/-1`, 렌더 크기는 `0/0`, 미러 제출 목록은 비어 있었고 deferred 포인터도 NULL이었다. 남아 있는 행렬을 현재 미러의 유효한 프레임으로 인정할 수 없다. `projection-observation.json`에 원시 결과를 보존했다.

위 세 관측은 scene의 준비용 포인터에서 상수를 잡지 못한 결과다. 이후 제출 묶음에서 실제 값을 읽는 데 성공했으므로 전체 상수 수집이 실패했다는 뜻으로 읽으면 안 된다. 빈 포인터의 원인을 포커스·일시정지 중 하나로 단정하지 않았고 게임 상태를 바꾸지 않았다. 관련 disassembly와 자료는 [`research/live/2026-10-08-render-constants/`](../research/live/2026-10-08-render-constants/)에 있다.

### 준비 상태에서 셰이더 상수까지

`pp_deferred_state`의 vtable RVA는 `0x23F64A0`이다. 타입 descriptor에서 이름을 확인했고, vtable `+0x60`의 getter `0x8D6860`은 정수 **10**을 반환한다. 이 함수는 게임에서 호출하지 않고 실행 파일의 `mov eax,10; ret`를 읽었다. 아래 component slot 10과 flag `1 << 10 = 0x400`은 이 타입에서 직접 유래한다.

확인한 제출 경로는 다음과 같다. 이는 정적 데이터 흐름이며 개별 GPU draw를 추적한 실행 기록은 아니다.

1. `0x147B0E0 → 0x1466610`은 `O +0x3B9D8`과 `O +0x3B9E0`의 포인터를 교환한다. 따라서 상수를 완성한 객체는 제출용 `+0x3B9E0`으로 옮겨진다. 준비용 `+0x3B9D8`만 계속 읽으면 놓칠 수 있다.
2. `0x1463010`은 `+0x3B9E0` 객체의 참조를 component 배열에 넣고 `0x1DFC20`으로 묶음을 만든다. `0x1DFC20`은 각 component의 type getter를 읽어 mask를 만들고 참조를 보관한다. 이 경로의 `pp_deferred_state`는 float 배열을 다시 계산해 복사하는 대신 객체 참조로 전달된다.
3. 한 확인된 소비 경로인 `0x1467F50`은 반환된 묶음 ID를 pass descriptor `+0x8A`에 기록하고 `0x166CC70`으로 제출한다. 그 준비 callback `0x166D220`은 ID를 작업 자료 `+0x24`에 보존하고, 실행 callback `0x166D2F0`은 이를 렌더 작업 context `T +0xD0`에 넣은 뒤 `0x2E5FF0`을 부른다.
4. `0x2E5FF0 → 0x2E5040 → 0x1509140`에서 uniform callback 인수 `U`를 만든다. draw packet `P`의 `+0x10` 포인터를 `Q`라 하면, `U +0x18 = Q +0x18`이다. 이 주소는 Q 내부 component 슬롯 배열의 시작이다.
5. 슬롯은 `T +0xD0`의 묶음과 `Q +0x142`의 묶음에서 채워진다. 첫 묶음을 넣은 뒤 두 번째 묶음을 넣으므로 같은 타입은 두 번째 값이 덮어쓴다. 각 component `C`는 `Q +0x18 + 8 * type(C)`에 들어간다. 따라서 타입 10인 `D`의 슬롯은 **`Q +0x68`**이다. 묶음 ID 조합이 `Q +0x11C`와 같으면 기존 슬롯을 재사용한다.
6. `0x2E5336`에서 uniform 명령 레코드 `+0x10`의 등록 callback을 호출한다. ray callback `0x1481560`은 위 `D +0x28`의 float4를 출력 버퍼에 쓴다. 등록부 이름 일치뿐 아니라 실제 소비 지점까지 연결한 결과다.

component 묶음은 타입 문자열이 `pp_batch_data_t`인 풀에 있다. 전역 array RVA `0x2737A78`, data `+8`, count `+0x10`, record stride `0x128`이며 accessor는 `0x1E0260`이다. 각 record의 첫 array가 component 포인터 목록이고 `+0x120`이 타입 mask다. 실제 풀 크기는 65,535였지만 이것이 활성 draw 수는 아니다. 할당 비트나 남아 있는 mask 역시 현재 프레임의 사용을 보증하지 않는다.

### 제출 묶음에서 읽은 실제 상수

풀의 앞 4,096개만 읽은 첫 시도에는 타입 10이 없었다. 이어 한 번의 전체 풀 읽기에서 `0x400`을 포함한 mask 193개를 찾았고, 그중 앞 64개를 조사해 `pp_deferred_state` 포인터와 아래 첫 값을 확보했다. 다음 한 번의 전체 읽기는 component 목록이 비어 있지 않은 193개 묶음을 조사해 아래 나머지 두 값을 확보했다. 게임 전체 메모리를 검색한 것이 아니라 위에서 해석한 약 19.4 MB의 특정 풀을 읽은 것이다.

| 관측 객체 | `D +0x28`의 ray float4 | `D +0x38`의 앞 두 값 | 해석 |
|---|---|---|---|
| `0x1f7ebcb65f8` | `(0.520567, -1.041134, -1.041134, 2.082268)` | `(512, 1024)` | 기본 분기 식으로 환산하면 약 55°/92.309°. 미러 0·2 설정에 대응하지만 둘 중 하나를 식별하지는 못함 |
| `0x1f863917fb8` | `(1, -1, -2, 2)` | `(512, 512)` | 90°/90°의 관측 미러 1 설정에 대응 |
| `0x1f55f257f18` | `(0.8493444, -0.4776222, -1.6988539, 0.9556054)` | `(3840, 2160)` | 앞서 읽은 main 중간 렌더 크기에 대응. 기본 대칭식과 작은 차이가 있지만 그 원인을 확정하지 않음 |

세 객체에서 `D +0x18 = (1,1,1,1)`, `D +0x38`의 뒤 두 값도 `(1,1)`이었다. 이 표는 **실제 메모리 값**과 설정을 비교한 결과다. namespace나 frame ID를 함께 읽어 연결한 자료가 아니므로 이 값으로 픽셀을 보정하는 수집기가 완성됐다고 볼 수 없다. 미러 5의 상수와 두 큰 미러의 구별도 이 관측에서는 확보하지 못했다.

원시 자료는 `component-pool-full-observation.json`, `component-active-observation.json`이다. 후자는 533회 읽기, 19,403,888 bytes, 읽기 오류 0건, reader 측 약 56.1 ms였다. 이것은 게임 FPS나 GPU 지연 측정값이 아니다. 관측 후 모든 reader handle을 닫았다. scene 포인터를 별도로 읽은 `deferred-published-state.json`에서는 `+0x3B9D8/+0x3B9E0`가 둘 다 NULL이었지만 제출 풀에서는 위 객체를 읽었다. 서로 다른 시점의 관측이므로 객체 수명 전체를 증명하는 자료로 사용하지 않는다.

### DX11 투영 보정과 reversed Z

이 빌드의 CPU 투영 행렬에서 기본 diffuse vertex shader의 `SV_Position`까지 다음 경로를 연결했다. 정적 실행 경로와 실행 중 renderer의 vtable 포인터를 대조한 결과이며 GPU 명령 캡처는 아니다.

1. 준비 callback `0x1475E70`은 `O +0x3BEE0`의 행렬을 작업 자료 `W +0x0C`로, viewport 자료를 `W +0x4C`로 복사한다. 실행 callback은 `0x1475F60 → 0x1465260`이다.
2. `0x1465260`은 원본 행렬을 렌더 작업 context `T +0x00`에 보존하고, 보정 행렬 `T +0x40`을 왼쪽에서 곱한 결과를 `T +0x80`에 저장한다. 이 경로에서는 추가 투영 보정값 `T +0xD4..0xE0`을 0으로 초기화한다.
3. context 생성 함수 `0x226960`은 renderer의 vtable `+0x220`에서 얻은 flag로 `0x27AAF0`을 호출하여 보정 행렬을 만든다. 실행 중 DX11 renderer의 vtable은 RVA `0x22127A8`, 해당 getter는 `0xF5350`이었다. getter의 명령은 `xor eax,eax; ret`로 **항상 0을 반환**한다. 게임 함수를 호출해서 얻은 값이 아니다.
4. flag 0에서 `0x27AAF0`이 만드는 행렬은 아래 `R`이다. `0x320F60`은 이 보정을 적용한 투영 행렬을 반환하고, `0x3EB290`은 이를 model/view와 결합한다. `0x3EB9F0`은 13개 float4를 셰이더 상수로 복사한다. 탐색 파일 `model-view-assemble.txt`의 `0x320F60`은 실제로는 보정된 **투영 행렬**을 조립하는 함수다.
5. 기본 diffuse defattr vertex shader `de0c30da0da67badc345a992c952dd83.sm5x.vso`는 `cb0[4..7]`와 정점의 내적으로 `SV_Position.xyzw`를 만든다. 이 네 행은 위 조립 결과의 `+0x40..0x70`이다. 해당 vertex shader에서 별도의 Z 재변환은 없다.

열벡터 표기로 표현하면 다음과 같다.

```text
R = [ 1  0    0    0   ]
    [ 0  1    0    0   ]
    [ 0  0  -0.5  0.5 ]
    [ 0  0    0    1   ]

P_DX11 = R * P_CPU
z_clip_DX11 = -0.5 * z_clip_CPU + 0.5 * w_clip_CPU
z_ndc_DX11 = (1 - z_ndc_CPU) / 2
```

보통의 유한 원근 투영에서 near `n > 0`, far `f > n`, 양의 광축 거리 `r = -z_eye`라 하면 다음 관계를 얻는다. viewport의 깊이 범위가 0–1이고 별도 depth bias·특수 투영·pixel shader의 depth 쓰기가 없는 경우에 해당한다.

```text
d = n * (f / r - 1) / (f - n)
r = f * n / (n + (f - n) * d)

d = 1 → r = n
d = 0 → r = f
```

이 `r`은 유클리드 거리가 아니다. 일반적인 복원에는 같은 pass의 실제 `P_DX11`과 viewport를 사용한다. 예시 `n=0.1, f=400`으로 행렬 투영과 위 역변환을 수치 대조했지만, 실제 GPU depth를 측정한 결과는 아니다. clear 값과 유효 표면도 별도로 구분해야 한다. **이 비선형 DSV 식을 이미 선형 카메라 Z인 `attributes_0.w`에 적용하지 않는다.** 첫 수집에서 attributes와 ray를 우선하는 방침은 유지한다.

근거는 `projection-pass-input.txt`, `scene-projection-dispatch.txt`, `context-projection-init.txt`, `render-context-create.txt`, `dx11-device-vtable.json`과 기존 transform·shader disassembly다. 이 결과로 backend의 기본 깊이 변환은 해석했지만, 모든 특수 재질·투영·viewport 조건 또는 특정 미러 프레임의 픽셀 정합을 검증한 것은 아니다.

### viewport 깊이 범위가 D3D11에 전달되는 경로

viewport의 최소·최대 깊이는 투영 보정과 별도 상태다. `O +0x3BA80`의 첫 두 float가 `W +0x4C/+0x50`으로 복사되고, `0x219C50`은 전체 viewport 자료를 `T +0x110`에 보존한다. `0x326990`은 상대 좌표의 화면 사각형을 렌더 크기에 맞춰 바꾸지만 앞의 두 깊이 값은 바꾸지 않는다.

`0x27B020`은 이를 일반 렌더 명령 9로 기록한다. DX11 명령 컴파일러 `0x2B1A30`의 해당 분기는 `0x2BD790`에 전달한다. 그 함수는 두 실행 방식에서 같은 깊이 범위를 사용한다.

- D3D command list를 직접 만드는 분기에서는 `0x2BD816`이 `RSSetViewports(1, &viewport)`를 호출한다. 전달 자료의 `+0x10/+0x14`가 그대로 `D3D11_VIEWPORT.MinDepth/MaxDepth`가 된다.
- 토큰 스트림 분기에서는 viewport를 DX11 명령 7로 저장한다. 실행기 `0x2B28C0`의 `0x2B2F67` 분기가 이를 읽고 `0x2B3033`에서 같은 API를 호출한다. 이때도 깊이 값의 추가 반전은 없다.

`ID3D11DeviceContext` vtable `+0x160`이 `RSSetViewports`라는 점과 자료의 여섯 float 배치는 로컬 Windows SDK 10.0.26100.0의 `um/d3d11.h`와 대조했다. 따라서 실제 depth가 viewport 변환을 거친 값이면 먼저 `d_ndc = (d_buffer - MinDepth) / (MaxDepth - MinDepth)`로 되돌린 뒤 앞 절의 행렬 또는 식을 적용한다. 범위의 폭이 0이면 이 역변환은 정의되지 않는다.

한 번의 외부 읽기에서 scene의 깊이 범위는 `(0,1)`, renderer `+0x3999A87`의 실행 방식 flag는 0이었다. 그러나 scene의 사각형 값은 초기화 상태의 `±FLT_MAX`였으므로 **이를 활성 미러 pass의 viewport 관측으로 인정하지 않는다.** 자료는 `scene-viewport-observation.json`이며 6회, 81 bytes만 읽고 handle을 닫았다. 정적 근거는 `viewport-command-submit.txt`, `viewport-normalize.txt`, `dx11-command-compile.txt`, `dx11-viewport-encode.txt`, `dx11-command-execute.txt`다.

### 미러별 준비 작업에서 읽은 투영행렬과 깊이 범위

후속 관측에서는 위의 비어 있는 공유 scene 상태 대신 **렌더 그래프의 개별 pass와 그 작업 자료**를 읽었다. 이 경로로 미러 0·2를 이름으로 구별하고 네 미러의 투영행렬을 각각 연결했다. 결과는 순차적인 외부 읽기이며 하나의 GPU frame을 멈춰 얻은 스냅샷은 아니다.

`rendergraph_builder_t::create_pass`（RVA `0x21DF20`）와 accessor `0x21E230`에서 확인한 pass 배열은 그래프 버퍼 시작 `+0xF0`에 있다. 버퍼는 기존과 같이 `EXE +0x304FC50 + buffer_index * 0x2A8`이며 배열 원소는 pass 포인터다. pass 객체 크기는 `0x1C80`이다. 문자열 객체 `+0x18/+0xB8`의 문자열 포인터 `+0x20/+0xC0`가 각각 이름과 namespace다. `+0x1F8`은 그래프 안의 index, `+0x1C68`은 실행 callback, `+0x1C70`은 준비된 작업 자료 `W`다. 이 index와 주소는 프레임을 넘어 유지되는 센서 ID가 아니다.

미러 namespace의 `draw_geometry` 작업을 먼저 잡았다. wrapper의 vtable RVA `0x2223828`은 `0x22EAA0`에서 wrapper `+0x110`의 내부 callback으로 전달한다. 내부 vtable `0x23F55B0`의 실행 함수는 `0x1475F60 → 0x1465260`으로, 앞서 해석한 `W +0x0C` 투영행렬과 `W +0x4C` viewport를 사용한다. 실제 연결 이미지는 diffuse/specular lighting과 depth였다. 따라서 이 관측만으로 깊이를 처음 기록하는 surface pass의 조건까지 일반화하지 않았다.

이어 `scene defattr`, `scene defattrcu`, `interior defattr`를 별도로 읽었다. **이 작업들의 namespace는 `mirrorN`이 아니라 공통 `deferred`였다.** 카메라 구별은 pass에 연결된 그래프 이미지의 namespace로 했다. 관측된 `+0x658/+0x6C0` 배열 원소는 8 bytes이며 첫 uint32가 이미지 그래프 ID다. 같은 버퍼의 이미지 배열에서 이를 따라가면 `mirrorN/attributes_0..3`, `mirrorN/depth_stencil`에 도달한다. 두 번째 uint32의 의미는 아직 확정하지 않았으므로 raw 자료에서는 `reference_word_4_unresolved`로 표시했다.

이 defattr wrapper의 vtable은 `0x21FD1B0`, 내부 vtable은 `0x23F5530`이었다. 같은 전달 함수 `0x22EAA0`을 거쳐 `0x1476140 → 0x1473E60`을 실행한다. `0x1473E60`은 `W +0x00`의 viewport를 `0x219C50`에 넘기고, `W +0x3C`의 투영행렬을 context에 넣는다. `W +0x8C`가 0이면 추가 투영 보정 없이 앞 절의 `R * P_CPU`를 만든다. `W +0x8E`의 component 묶음 ID는 `T +0xD0`으로 전달된다.

이 소비 함수의 layout으로 저장한 작업 자료를 해석한 결과는 다음과 같다. 해상도는 pass의 viewport 사각형에서 읽었으며 연결 이미지의 크기와도 대응했다.

| 연결 이미지 namespace / 작업 | viewport 크기 | MinDepth / MaxDepth | CPU 투영의 대각 성분 `P00 / P11` | 행렬에서 환산한 near / far（근사） |
|---|---|---|---|---|
| mirror0 / scene defattr·defattrcu | 512×1024 | 0.01 / 0.9 | 1.9209824 / 0.9604912 | 0.1 / 400 |
| mirror1 / scene defattr·defattrcu | 512×512 | 0.01 / 0.9 | 1 / 1 | 0.1 / 400 |
| mirror2 / scene defattr·defattrcu | 512×1024 | 0.01 / 0.9 | 1.9209824 / 0.9604912 | 0.1 / 400 |
| mirror5 / scene defattr·defattrcu | 512×256 | 0.01 / 0.9 | 0.5594000 / 1.1187994 | 0.1 / 400 |
| main0 / interior defattr | 3840×2160 | 0.9 / 1.0 | 1.1772643 / 2.0929141 | 0.1 / 15 |
| main0 / scene defattr·defattrcu | 3840×2160 | 0.01 / 0.9 | 1.1772643 / 2.0929141 | 0.2 / 1600 |

관측한 11개 작업 모두 viewport 좌표 mode는 1, 추가 투영 보정 flag와 보정 float4는 0이었다. 표의 near/far는 FP32 행렬을 역산한 값이며 실제 표면과의 거리 측정값이 아니다. 특히 far 역산에는 반올림 오차가 증폭된다. 미러에서 직접 역산하면 약 400.034이며, 앞서 렌더 카메라 필드에서 읽은 400과 구별한다. 원시 행렬을 보존하고 임의로 이상적인 near/far 값으로 대체하지 않는다.

**실용적인 차이는 NDC의 reversed Z와 텍스처에 기록되는 깊이 범위가 같지 않다는 점이다.** 관측된 미러 surface pass에서는 기본 경로의 near가 약 0.9, far가 약 0.01로 매핑된다. 일반적인 정규화는 다음과 같으며 실제 수집에서는 저장한 MinDepth/MaxDepth를 사용한다.

```text
d_ndc = (d_buffer - MinDepth) / (MaxDepth - MinDepth)
eye_h = inverse(R * P_CPU) * [x_ndc, y_ndc, d_ndc, 1]
eye_position = eye_h.xyz / eye_h.w
```

또한 `main0`의 실내와 실외는 같은 depth 이미지 이름을 참조하면서 서로 다른 깊이 범위와 투영을 사용한다. 따라서 main depth 전체를 하나의 near/far와 viewport로 복원하는 방법은 맞지 않는다. pass 구분 및 픽셀 대응은 수집 시 해결해야 한다. attributes의 카메라 Z 경로를 우선하는 이유도 더 분명해졌지만, 그 경로 역시 같은 pass의 ray/view와 무효 픽셀 처리가 필요하다.

근거는 `render-pass-create.txt`, `render-pass-accessor.txt`, `pass-callback-2223828.txt`, `pass-callback-21fd1b0.txt`, `defattr-pass-execute.txt`다. 첫 pass 목록은 `render-pass-observation.json`과 대응 binary에, 이름이 있는 조명 작업은 `named-pass-projection-observation.json`에, surface 작업은 `defattr-pass-observation.json`, `defattr-pass-records.bin`, 해석 결과 `defattr-pass-decoded.json`에 보존했다. surface 관측은 783회 읽기, 352,280 bytes, reader 측 약 4.34 ms였으며 GPU 지연 측정값은 아니다. 작업 자료의 raw prefix 중 문서화한 layout 밖의 byte에는 의미를 부여하지 않았다. 모든 reader handle은 종료 시 닫았다.

### 미러 작업에서 카메라와 ray 상수까지 연결

위 surface 작업의 `W +0x8E` 묶음 ID를 `pp_batch_data_t` 풀에 연결해 **네 미러 각각의 기본 카메라와 `pp_deferred_state`를 읽었다.** 처음의 풀 전체 탐색과 달리, 이번에는 연결 이미지의 `mirrorN` namespace에서 출발한다. 이에 따라 앞선 관측에서 남았던 미러 0·2의 구별과 미러 5의 ray 상수 확보가 해결됐다. 개별 draw의 추가 component 묶음과 GPU 픽셀까지 연결한 것은 아니다.

실제 경로는 다음과 같다.

```text
surface pass → 연결 이미지의 mirrorN namespace
             → W +0x8E: 기본 component 묶음 ID
             → *(EXE +0x2737A80) + ID * 0x128
             → component 포인터 배열
                 pp_camera         : type 0,  vtable RVA 0x2223120
                 pp_deferred_state : type 10, vtable RVA 0x23F64A0
```

카메라 타입 이름은 Prism의 타입 descriptor에서 확인했다. `pp_camera`의 vtable `+0x28` getter `0x31F1B0`이 descriptor RVA `0x1E0E490`을 가리키며, 이름은 `pp_camera`다. slot getter `+0x60`은 0을 반환한다. `pp_deferred_state`는 앞 절에서 확인한 slot 10이다. getter는 실행하지 않고 명령·자료를 읽었다.

관측한 미러마다 서로 다른 카메라 객체와 deferred 상태 객체가 연결됐다. 다음은 한 번의 순차 관측에서 읽은 값이다. 주소·묶음 ID는 raw 자료에 보존했으며 영구 식별자로 사용하지 않는다.

| 연결 이미지 | 카메라 원점의 월드 좌표（게임 길이 단위, 반올림） | ray float4 |
|---|---|---|
| mirror0 | `(10283.364, 46.940, -9160.731)` | `(0.520567, -1.041134, -1.041134, 2.082268)` |
| mirror1 | `(10283.364, 46.550, -9160.731)` | `(1, -1, -2, 2)` |
| mirror2 | `(10283.784, 46.940, -9163.708)` | `(0.520567, -1.041134, -1.041134, 2.082268)` |
| mirror5 | `(10282.966, 47.046, -9163.059)` | `(1.787630, -0.893815, -3.575259, 1.787630)` |

0·2의 ray가 같더라도 카메라 원점은 약 3.01 게임 길이 단위 떨어져 있었다. 따라서 동일한 투영·해상도·물리 텍스처 주소만으로 두 센서의 영상을 같은 것으로 취급하면 안 된다. 이 표는 CPU 카메라의 구별을 증명하며 두 GPU 영상의 차이를 측정한 결과는 아니다.

카메라 객체 `C`에서 일반 model/view 조립 함수 `0x320B40`이 소비하는 필드는 다음과 같다.

- `C +0x18`: 4×4 카메라 회전 행렬. 관측값의 이동 성분은 0이고 마지막 행은 `(0,0,0,1)`이었다. 일반 분기에서 이 회전이 model 변환과 카메라 상대 위치에 곱해진다.
- `C +0x98`: local XYZ의 float 3개. `C +0xA4/+0xA6`에는 X/Z cell의 signed 16-bit 정수가 들어간다. 이를 float4로 읽으면 마지막 값이 NaN처럼 보일 수 있지만 마지막 4 bytes는 부동소수점 값이 아니다.
- `C +0x58`: 관측한 여섯 카메라 표본에서 `+0x18` 행렬의 전치와 같았다. 여기서는 복원 수식의 근거를 소비 함수가 사용하는 `+0x18`에 둔다.

`0x320D5E..0x320DE4`는 model 위치에서 카메라 위치를 빼고 X/Z cell 차이에 **512**를 곱해 더한다. 상수는 EXE RVA `0x251D198`의 float `512.0`이다. 따라서 관측된 일반 분기의 카메라 원점과 좌표 복원은 열벡터 표기로 다음과 같다.

```text
camera_world = (local_x + 512 * cell_x,
                local_y,
                local_z + 512 * cell_z)

eye_position = (uv.x * A.z + A.x, uv.y * A.w + A.y, 1) * attributes_0.w
world_position = camera_world + inverse(R_camera) * eye_position
```

`R_camera`는 `C +0x18`의 좌상단 3×3이다. 위 식은 기본 camera/model 변환이 적용되는 분기이며, 별도 `pp_mvp_filter`가 카메라 회전·이동을 무시하도록 설정한 특수 draw에는 그대로 적용하지 않는다. 이 플래그를 앞서 model flag라고 적었으나, 실제 소비 경로는 model의 slot 1과 구별되는 **slot 2의 `pp_mvp_filter +0x18`**이다. **같은 pass 안에서도 두 번째 draw 묶음이 component를 덮어쓸 수 있다.** 따라서 이 관측은 pass의 기본 상태를 연결한 결과이고 모든 vertex shader의 최종 상태를 확정한 결과는 아니다. 픽셀의 UV 방향, 무효·interleaved mask, 좌표 단위 및 최종 영상 정합은 여전히 실제 수집에서 대조해야 한다.

같은 관측의 main 실내·실외 작업은 카메라 객체와 deferred 상태 객체를 공유했다. 따라서 앞 절의 서로 다른 깊이 범위·near/far가 이 표본에서 서로 다른 카메라 원점을 뜻하지는 않았다. 회전행렬의 수치 대조에서 determinant는 약 1, 전치와의 차이는 0이었으며, 이는 읽은 행렬의 내부 관계를 확인한 결과다. 실제 월드 점의 영상 투영을 검증한 것은 아니다.

자료는 `named-pass-component-snapshot.json`, 해석 결과는 `named-pass-component-decoded.json`이다. 645회, 75,224 bytes를 읽었고 reader 측 약 3.99 ms, 읽기 오류 0건이었다. 앞선 `named-pass-component-observation.json`은 한 component 포인터의 vtable을 해석하지 못한 부분 관측이며 완전한 연결 자료로 취급하지 않는다. 해당 오류의 발생 시점에 객체 수명을 고정하거나 게임을 정지시키지는 않았다. 최종 관측은 조회와 해석을 분리해 읽는 양을 줄였으며 모든 handle을 닫았다.

### draw별 덮어쓰기와 변환 필터의 적용 범위

**후속 표본의 네 미러 원본 렌더 객체에서는 카메라·ray 교체나 `pp_mvp_filter`가 발견되지 않았다.** 다만 준비된 GPU draw를 포착한 것이 아니라, draw 생성 함수가 소비하는 원본 객체 목록을 읽은 결과다. 다른 장면·재질 전체에 대한 보장은 아니다.

정적으로 확인한 연결은 `W +0x30 → H`이며, `0x160C990`이 `H +0x128`의 객체 포인터 목록에서 `H +0x218`의 draw 그룹을 만든다. 그룹은 0x58 bytes, 내부 항목은 24 bytes다. 생성 함수 `0x2DEC80`의 `0x2DF1F5`에서 원본 객체 `Q`를 항목 `+0x10`에 넣는다. 이후 `0x160D580 → 0x2E5FF0 → 0x2E5040 → 0x1509140`이 pass 기본 묶음 `T +0xD0`와 추가 묶음 `Q +0x142`를 병합하며, 추가 묶음의 같은 slot이 기본 값을 덮어쓴다.

처음 표본에서는 미러 0·1의 원본 목록이 각각 1,110·1,498개였지만 draw 그룹 수는 0이었다. 따라서 빈 그룹만 보고 override가 없다고 판단하지 않았다. 다음 표본에서는 다음 원본 목록을 읽었다.

| 연결 이미지 / 작업 | 원본 목록 항목 수 | 추가 묶음의 camera / deferred state / MVP filter |
|---|---:|---|
| mirror0 / scene defattr | 1,110 | 없음 / 없음 / 없음 |
| mirror1 / scene defattr | 1,497 | 없음 / 없음 / 없음 |
| mirror2 / scene defattr | 1,049 | 없음 / 없음 / 없음 |
| mirror5 / scene defattr | 418 | 없음 / 없음 / 없음 |
| main0 / interior defattr | 95 | 없음 / 없음 / 없음 |
| main0 / scene defattr | 1,088 | 없음 / 없음 / 없음 |

여섯 목록은 객체를 공유하며, 중복을 제거하면 `Q` 3,595개와 비어 있지 않은 추가 묶음 2,803개다. 추가 묶음의 mask에는 slot 0（camera）, 10（deferred state）, 2（MVP filter）가 모두 없었다. 각 pass의 기본 묶음에도 slot 2는 없었다. 일반 변환 함수 `0x3EB290`은 mask bit 2가 없으면 filter 포인터를 NULL로 만들고, `0x320B40`은 NULL일 때 filter flags를 0으로 사용한다. `pp_mvp_filter`의 타입은 vtable RVA `0x2222E98`, slot getter `0x14B090`의 반환값 2로 구별했다. 따라서 이 표본의 일반 변환 경로에는 앞 절의 카메라 회전·이동 무시 옵션이 적용되지 않는다. 물체 자체의 model 변환은 별도로 존재한다.

또한 `H +0x150`에는 viewport·투영·기본 component 묶음·추가 상태 byte를 구간별로 변경하는 10-byte 기록 목록이 있다. `0x160D010`은 이 목록과 각 그룹의 경계 인덱스를 사용해 구간을 실행한다. 네 상태 인덱스의 `0xFFFF`는 원래 pass 상태를 선택하며, 마지막 word가 signed 음수이면 해당 구간을 건너뛴다. 이번 여섯 `H`에서 이 상태 기록 수는 모두 0이었다. 이 결과는 엔진에 override 기능이 없다는 뜻이 아니라, **읽은 목록에는 그 변경 기록이 없었다는 뜻**이다.

관측은 `named-pass-draw-overrides.json`에 저장했다. 7,062회, 1,118,128 bytes, reader 측 약 44.72 ms, 읽기 오류 0건이었다. 게임 프레임 시간이나 FPS 영향의 측정값은 아니다. 모든 원본 객체의 관련 필드를 읽었지만 자료는 순차 관측이며, 객체 수명을 고정하거나 GPU 실행을 동기화하지 않았다. pass의 selector 11에 해당하는 mask bit가 꺼진 원본도 포함되므로 표의 수는 실행 draw 수가 아니다. GPU에 제출된 최종 상태·특수 vertex shader·실제 픽셀까지 확인한 것으로 확대하지 않는다.

정적 근거는 `scene-draw-list-prepare.txt`, `scene-draw-source-to-items.txt`, `scene-draw-groups-dispatch.txt`, `scene-draw-group-items.txt`, 기존 `uniform-argument-construct.txt`와 `transform-assemble.txt`다. 첫 부분 관측은 `named-pass-draw-headers.json`에 별도로 보존했다. 두 observer는 handle을 닫고 종료됐음을 확인했다.

## 픽셀 수집에서 아직 필요한 작업

현재 외부 읽기로 얻은 것은 CPU에 있는 리소스 설명·주소와 렌더 상태의 상수다. GPU 텍스처의 픽셀은 읽지 않았다. 게임 프로세스의 COM 포인터를 외부 Python에서 호출하지 않았다. 로컬 `d3d11.dll` 10.0.26100.9549의 `Texture2D::GetDesc` 구현（RVA `0x59CE0`）을 disassemble해 필드 복사를 읽기 연산으로 해석했다. 이 내부 layout은 Windows 업데이트에도 달라질 수 있고, 일반 D3D11 API 계약이 아니다.

전체 image pool의 non-NULL 리소스 2,491개 중 render/depth bind를 가진 텍스처는 111개였다. 해당 111개에서 CPU read 또는 shared/keyed-mutex/NT-handle flag가 켜진 리소스는 없었다. 그러므로 이번 프로세스의 미러를 기존 공유 handle로 바로 열어 얻는 경로는 확인되지 않았다. GPU→CPU 복사는 별도 staging 리소스를 사용하는 경로가 필요하다. [Microsoft D3D11_USAGE](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_usage), [공유 리소스 flag](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_resource_misc_flag)

다음 구현 지점은 구체적으로 다음과 같다.

1. `mirrorN` pass가 만든 `composition_raw`, `attributes_0`, `attributes_3`를 **각 pass 완료 후, 재사용 전** 복사한다. 필요하면 DSV도 추가 비교한다. 최종 reflection 출력만으로 시작할 경우 네 출력은 별도 리소스이지만 depth 정합을 별도로 해결해야 한다.
2. 해당 pass의 view/projection·viewport·scene/frame 식별자를 같은 callback에서 보관한다. 시간상 가까운 외부 RPM 표본을 같은 frame이라고 선언하지 않는다.
3. 색상은 실제 format·row pitch를 보존해 먼저 원시 데이터를 받는다. HDR 값의 tone mapping, 미러 반사, 해상도 변경은 픽셀을 확인한 뒤 결정한다.
4. Z 복원은 우선 위 `attributes_0.w` 경로를 대조한다. 비교용 DSV는 현재 format 20이며 SRV bind가 없다. DSV를 바로 shader input으로 읽을 수 있다고 가정하지 않는다. 호환 typeless 복사본과 depth SRV 또는 지원되는 readback 경로는 실제 device에서 확인해야 한다. `CopyResource`의 같은 크기·호환 format·sample 조건과 staging의 depth 제약을 따른다. [CopyResource 계약](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource)
5. 기존 미러 픽셀·좌표 정합이 확보된 뒤에 독립 전방 센서와 추가 뷰 생성으로 진행한다. 기존 미러 경로가 발견됐다는 이유로 고개 움직임과 무관한 4–6센서 구현이 끝난 것은 아니다.

위 GPU callback 연결·복사는 현재의 외부 읽기 전용 방식보다 게임에 관여한다. 이번 조사에서는 준비 코드 추적과 메타데이터 수집까지만 실행했다. 게임에 캡처 코드를 설치하거나 재시작시키지는 않았다.

## 재현 도구와 원시 자료

[`research/read_render_memory.py`](../research/read_render_memory.py)는 기존 Reader의 `PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION` 권한을 재사용한다. 읽기 중 대상 배열이 재구성되면 결과에 오류가 기록될 수 있다. 최대 30초·10Hz로 제한하며 파일은 새로 만들고 종료 시 handle을 닫는다. 실행 전에 현재 PID와 EXE/D3D11 모듈 base 및 빌드를 확인해야 한다.

```powershell
py -3.13 research/read_render_memory.py --pid 24940 --base 0x7ff707440000 --d3d-base 0x7ffa12000000 --seconds 8 --hz 5 --output research/live/next-render-observation.jsonl
```

실제 실행은 8초/5Hz, 40표본, 23,017회 읽기, 17,535,888 bytes, 읽기 오류 0건이었다. 표본 수집 시간은 reader 기준 중앙값 3.738ms, 최대 4.326ms이며 게임 FPS나 GPU overhead 수치가 아니다. 관측 observer PID 27336은 실행 종료 후 handle을 닫았다.

자료는 [`research/live/2026-10-08-render-path/`](../research/live/2026-10-08-render-path/)에 있다. `render-graph.jsonl`은 현재 namespace와 DX11 리소스를 함께 수집한 결과, `summary.json`은 선택 미러와 재사용 집계, `d3d11-render-target-inventory.json`은 전체 풀의 render/depth 항목이다. disassembly 후보 파일명 중에는 탐색 당시의 임시 이름이 있으므로 의미는 이 문서의 검증된 경로를 우선한다.

## 잎 billboard의 Z 보정과 DSV 차이

0.8.2의 같은 Present 구간 60에서 DSV를 역투영한 Z와 `attributes0.w`를 비교했다. 근거리의 작은 차이는 FP16 절삭과 일관됐지만, 원거리 최대 차이는 약 1.4 게임 길이 단위였다. 미러 0·1·2에서 가장 큰 차이의 픽셀은 모두 packed material 값 `2`, stencil `1`이었다. 이 상관관계만으로 픽셀의 의미론적 종류를 선언하지 않고, 설치된 `effect.scs`에서 대응 가능한 셰이더를 조사했다.

`/effect/eut2/leaves/eut2.leaves.rfx`와 `eut2.leaves.instanced.rfx`는 defattr fragment shader를 공유한다. GLSL은 `d98ecb96da47988d6532e49c773a71dc.glsl.fso`, SM5는 `e89c181b8d2a80a4738c467d9f3cd691.sm5x.fso`다. GLSL에서 다음 계산을 직접 확인했다.

```text
eye_position_adjust = input_normal_eye * (mask_texture.b * 2)
attributes0.w = interpolated_eye_position.z + eye_position_adjust.z
```

SM5 DXBC도 같은 의미의 `dp2 r0.x, r3.xxxx, v4.zzzz`와 `add o0.w, r0.x, v5.z`를 실행한다. 여기서 `r3.x`는 texture mask의 B 채널이다. 출력 signature에는 `SV_Target0..3`만 있으며 `SV_Depth`는 없다. 따라서 이 재질은 deferred shading용 Z를 이동시키면서 하드웨어 깊이에는 그 이동을 쓰지 않는다. DSV가 나타내는 것은 실제 rasterized geometry이며 billboard 자체를 실제 나뭇잎의 입체 형상으로 바꾸지는 않는다.

같은 GLSL은 billboard 조건에서 mask bit `2`를 설정하고, 전환 구간에서 bit `8`을 설정한다. Z 보정 자체는 billboard 조건문 밖에 있으므로 bit `2`가 없는 잎에도 적용될 수 있다. 또한 저장된 최종 normal은 별도 혼합·정규화된 값이므로, `attributes0.xyz`를 위 식의 입력 법선으로 대입해 보정량을 되돌릴 수 있다고 가정하면 안 된다. 이 플래그 의미는 확인한 셰이더 범위의 해석이다.

이전 RenderDoc 프레임 3160의 네 미러 G-buffer draw가 쓴 rasterizer state는 ID `739`와 `755`였고 모두 `DepthBias`, `SlopeScaledDepthBias`, `DepthBiasClamp`가 0이었다. 그림자 pass의 별도 bias 상태 ID `6619`와 혼동하지 않았다. 해당 기록에서는 G-buffer 종료 이후 composition 종료까지 attributes 텍스처가 다시 렌더 타깃으로 바인딩되지 않았다. 현재 DLL 캡처의 개별 최대 차이 픽셀을 만든 draw·입력 법선·마스크 샘플까지 직접 추적한 결과는 아니므로, 모든 잔차의 원인을 입증한 것은 아니다.

실용적으로 깊이 출처를 나눈다. `otpy reconstruct`의 기본 `geometry`는 DSV와 viewport·pass 투영을 사용하고, `attributes`는 보정된 Z와 deferred ray를 사용한다. 기존 원시 자료를 덮어쓰거나 둘을 같은 센서값으로 혼합하지 않는다. 네 뷰에서 두 경로의 점군을 만들었으며 각 경로의 유효 점은 1,413,986개다. 기본 카메라 월드 변환과 실제 거리·AI 박스의 정합은 후속 검증 대상이다.

근거 자료는 `research/extracted/effect-analysis/leaves_defattr.asm`, 원본 GLSL 추출본, `research/live/2026-10-08-render-probe/0.8.2-depth-comparison/material-groups.json`, `0.8.2-world-reconstruction/`이다. 게임 셰이더 원본과 raw 배열은 공개 저장소에 포함하지 않는다.
