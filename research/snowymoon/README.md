# snowymoon 프록시 dxgi.dll 정적 분석: 렌더 패스 병렬 기록과의 공존

2026-10-22. [28 렌더 패스 병렬화](../../docs/28_render_pass_parallel_prototype.md)의 M3b~M3d가 snowymoon 프록시와 함께 돌 수 있는지 판단하려고 프록시 DLL을 정적으로 읽은 결과입니다. §1–§9가 그 훅·공존 기록이고, §10은 같은 복사본을 라이브러리와 효과 등록까지 포함해 DLL 전체로 읽은 기록입니다. 지금 멈춘 곳은 §10.16입니다. 표시는 ✔ 코드로 확인, △ 추정입니다. DLL 주소는 `dxgi+RVA`(이미지 베이스 `0x180000000`), 게임 주소는 1.61.1.9 RVA(28 §11 M3b `foreign` 목록과 같음)입니다. DLL은 로드·실행하지 않았고 게임도 켜지 않았습니다. 게임 폴더 원본은 건드리지 않고 복사본으로만 분석했습니다.

## 결론 (3줄)

1. **AA(TAA)를 꺼도 `order=free`(패스 본체 동시 기록)와 공존 불가.** closure 안 세 훅의 핸들러(`0x1A890`·`0x1A440`·`0x1A8C0`)는 "고정 전역에 값 저장 → 원본 호출 → 전역 clear"로 중첩 호출에 상태를 넘기며, 락·원자 연산·TLS가 **하나도 없습니다**(✔). AA 검사는 D3D 그리기 디스패치에만 있고(`0x9FE10`), 패스 기록 중계 핸들러는 AA와 무관하게 같은 전역을 씁니다(✔).
2. **`accepted_text_patches` 화이트리스트로도 못 풉니다.** 가장 위험한 공유 상태(락 없는 리소스 `std::map` `DAT_c76810`)는 D3D11 장치·컨텍스트 **COM 인스턴스 vtable 포인터 교체** 훅(`0x17B70`·`0x17C90`)에 있어 `scan_text`의 `.text` 비교에 **보이지 않습니다**(✔).
3. 공존이 성립하는 유일한 모드는 **패스 본체 전체를 한 락 아래 두는 경우**(M3b-1 티켓, M3b-2 전체 락)뿐입니다 — 이는 스레드만 바뀌는 직렬 실행이라 성능 이득이 없습니다. M3c(락 축소)·M3d는 프록시를 뺀 세션에서만 켜야 합니다(§7).

위 3은 지금 core의 판정입니다. 스노위문 상태를 스레드별·순서 보존으로 바꿔 M3c·M3d와 함께 쓰는 설계(B안)는 [multicore.md](multicore.md)에 있습니다.

1.61.1.9 훅 RVA는 모두 닫혔습니다(§9). 오프셋 전역은 파일에 들어 있는 RVA 표가 아니라 `0x6BD80`이 채웁니다. `0x6BD80`은 정적 링크된 Capstone 5로 게임 `.text`를 한 번 훑어 고르므로, `xref_resolve.py`가 같은 선택을 Python capstone으로 다시 돌려 uniform 합성 함수 `0x3EB300`, `persistent_drawable` 호출 지점 `0x405618`·`0x40565B`·`0x405788`, 장면 범위 `0x1475C90` 길이 `0x3A9`·내부 호출 `+0x30D`를 냈습니다(§10.12).

## 1. 대상과 방법

| 항목 | 값 |
| --- | --- |
| 파일 | 게임 폴더 `bin/win_x64/dxgi.dll.off`(원래 `dxgi.dll`), 18,304,568 bytes, sha256 `d819c16805a9a0ca17f34ef46cc445c5847f32308d10e69bf08f3c1b4779f4dc`. 분석은 `work/snowymoon_dxgi.bin` 복사본 |
| PDB·내보내기 | 디버그 디렉터리 `C:\Pdb\TAA\TAA.pdb`. 내보내기 `CreateDXGIFactory{,1,2}`와 `NVSDK_NGX_D3D11_*`(DLSS 중계) |
| 게임 | eurotrucks2.exe 1.61.1.9 |
| 도구 | Ghidra 12.1.4 headless 전용 프로젝트(`ghidra/`, Git 제외), capstone·pefile |

재현:

```
py -3 -I research/snowymoon/ghidra.py import               # 복사·해시 확인·전용 프로젝트 import + 자동 분석
py -3 -I research/snowymoon/ghidra.py decompile 1A890 1A440 # ghidra/out/decomp/<entry>.c
py -3 -I research/snowymoon/ghidra.py refs C780E8           # ghidra/out/refs.csv
py -3 research/snowymoon/sm.py func 1B090                   # capstone 역어셈블(.pdata 함수 단위)
py -3 research/snowymoon/log_sites.py                       # snowymoon_logs.txt 64줄 → 호출 지점
py -3 research/snowymoon/xref_resolve.py                    # 0x6BD80의 참조 문자열 탐색을 게임 exe에 재현(첫 실행 약 45초, 이후 캐시로 약 2초)
py -3 research/snowymoon/xref_engine.py str "<문자열>"       # 범용 탐색 엔진: 문자열 RVA와 참조 명령·함수 범위 (branches <rva>, range <rva>도 있음)
```

`sm.py`·`log_sites.py`·`xref_resolve.py`는 capstone이 사용자 site-packages에 있어 `-I` 없이 돌립니다(스크립트 폴더에서 실행, DLL은 데이터로만 읽음). 기존 `research/live/2026-10-15-snowymoon/hook_owner.py`의 점프 체인 추적과 상호 검증했습니다.

## 2. 패킹·난독화 (분석을 막지 않음)

| 관찰 | 판정 |
| --- | --- |
| 섹션 `.vlizer` `0x4DAA00` 바이트, RWX(`0xE0000060`), 엔트로피 6.44 | **Oreans Code Virtualizer**의 가상화 코드(✔ 섹션 이름) |
| `.text` 4 MB 엔트로피 6.50, 진입점 `dxgi+0x141DF0`(MSVC CRT), import 정상(KERNEL32 182·USER32 204·gdiplus 등, wxWidgets·ImGui·zlib·libpng 정적 링크) | 전체 패킹 아님(✔) |
| `.text`→`.vlizer` 진입 | 매크로 단위 부분 가상화. 이 분석에 걸린 곳은 장면 패스 트램펄린 설치(`dxgi+0x76510` → `dxgi+0x11BA8BD`) 한 곳뿐(✔) |
| 문자열·서명 | 전부 평문. IDA식 바이트 서명 문자열·원본 프롤로그 바이트 배열 **0건**(✔ → 훅 대상은 서명 스캔이 아니라 버전 표로 찾음, §3) |

핸들러·D3D 훅·MinHook 본체는 모두 평문이라 끝까지 읽혔습니다. 분석을 막는 것은 장면-패스 트램펄린 설치 코드와 그 중계 스텁 내용뿐입니다.

## 3. 훅 라이브러리와 대상 찾기 (답 1)

- **훅 엔진은 MinHook입니다**(✔). HDE64 길이 디코더 표가 `dxgi+0xA3D3A0`·`0xA3D5C0`·`0xA3D950`에 있고, `MH_CreateHook` 상당 `0x21170`(트램펄린 생성 + `VirtualProtect`), 활성화 `0x213F0→0x21330→0x20FF0`, 등록 래퍼 `0x79760`(rel32 E9)·`0x79990`(14바이트 FF25)·`0x796F0`(rel32 call 치환) 구조가 전형적입니다.
- **트램펄린은 대상 ±2 GB 안에 둡니다**(✔). `0x76510`이 `VirtualQuery`로 대상 근처 빈 페이지를 찾아 `VirtualAlloc(0x20000, PAGE_EXECUTE_READWRITE)`하고, 이 영역에 XOR로 가린 스텁을 씁니다. 28이 본 "익명 메모리 중계 스텁"이 이것입니다.
- **대상 RVA는 `0x6BD80`이 게임 exe를 이미지 레이아웃으로 펼친 뒤 참조 문자열로 고릅니다**(✔, §9). 파일의 오프셋 전역(`DAT_c78010` 등)은 0입니다. `0x1B090`이 버전 dword `DAT_c7381c`로 `0x35..0x3D`(1.53~1.61)만 허용하고, 훅 주소는 `GetModuleHandleW 베이스(DAT_c73670) + 그 전역`입니다. 버전 분기(`cmp 0x36/0x37/0x3B/0x3C`)로 핸들러가 갈리고, 1.61(`0x3D`)에서는 `DAT_c78020`·`DAT_c78078` 경로가 빠집니다. 지원 밖 버전이면 `0x1AEC0`이 "unsupported game version" MessageBox를 띄웁니다. `E8` 검사(`[+0xC]==5 && [+8]==0x3E`)는 Capstone 명령의 길이 5와 `X86_INS_CALL`(62)이고, uniform 합성 함수·장면 참조 문자열에만 있습니다. uniform 합성 함수가 저장하는 값은 그 호출의 첫 즉시값(`cs_detail`의 `op[0].imm`, 객체 `+0xB0`), 곧 호출 목적지입니다(✔ §9).

## 4. 훅별 표 (답 1·2·3·4)

게임 RVA는 1.61.1.9(28 §11 `foreign`), 핸들러는 `dxgi+RVA`. "AA 끔"은 cfg `taa=0`일 때 핸들러 동작입니다.

### 4.1 기록 경로(closure) 안 — 결정 대상

| 게임 RVA | 역할 | 핸들러 | 하는 일 | AA 끔 동작 | 공유 쓰기 | 스레드 안전 |
| --- | --- | --- | --- | --- | --- | --- |
| `0x1475C90` | 장면 패스 콜백(FF25, 내부 vtable slot1) | `0x1A890` | `DAT_c780e8 = 패스; (*DAT_c73630)(); DAT_c780e8 = 0` | **동일**(무조건 전역 기록) | 고정 전역 `DAT_c780e8` | ✗ 락·원자·TLS 0(✔) |
| `0x3EB300` | uniform composer 잎(`0x2E5040` 아래) | `0x1A440` | `DAT_c780e8`(미룬 패스) 읽어 비 반사 투영 행렬 합성, 결과를 링에 push | 효과만 no-op, **전역 기록은 그대로** | 링 `DAT_c73848[20]` + 비원자 카운터 `DAT_c7673c`, `DAT_ccc788`(via `0x84460/0x84670`) | ✗ 락·원자·TLS 0(✔) |
| `0x2B5FE0` | 버퍼 업로드 | `0x1A8C0` | `DAT_c735bc = 업로드크기(R9D); (*DAT_c73628)(); DAT_c735bc = 0` | **동일** | 고정 전역 `DAT_c735bc`. 소비자는 컨텍스트 vtable 슬롯 15 Unmap `0x17890` | ✗ 락·원자·TLS 0(✔) |

세 핸들러 본문 전체를 `LOCK` 접두·`xchg`·`cmpxchg`·`xadd`·`gs:`로 스캔해 **0건**임을 확인했습니다(✔ `0x1A440`·`0x84460` 본문 240·177 명령 포함). 동작 패턴은 "값을 고정 전역에 넣고 원본을 부른 뒤 지움"이라, **임의 시점에 이 중첩에 스레드가 하나만 있다**는 것을 전제합니다. 중첩은 `0x1475C90`(장면) → 안쪽에서 `0x3EB300`(합성)·`0x2B5FE0`(업로드)으로 한 스레드에서 일어납니다.

### 4.2 기록 경로 밖 — 1.61.1.9 바인딩

참조 문자열과 전역 산출은 §9. 여기의 RVA는 그 문자열을 참조하는 함수의 pdata 시작입니다. 핸들러는 `0x1B090`이 1.61(`DAT_c7381c == 0x3D`)에서 거는 쪽입니다.

| 게임 RVA | 핸들러 | 역할 | 공유 쓰기 |
| --- | --- | --- | --- |
| `0x2BFEF0` | `0x1AEC0` | 프레임 브래킷. Present 실패 문자열의 명령 단위 xref는 `0x2BFF50` 하나(✔ `xref_resolve.py`). 라이브 게이트에 찍힌 주소와 같음 | 프레임 상태(메인 스레드) |
| `0x4208A0` | `0x1AD50` | cvar 저장을 건너뛰는 로그 함수. `0x1AC80`으로 `uset r_aa "0"` 등을 강제한 뒤 원본 | 초기화 1회 |
| `0x332F40` | `0x1ABE0` | effect semantics 토큰화 실패 로그. 객체 포인터를 세고 원본을 부른 뒤 지움. 패스 기록기가 아님 | `DAT_c735b0`·`DAT_c735b8`·`DAT_c73658`. 락 없음 |
| `0x338790` | `0x1AFF0` | `<undefined>` 참조 문자열. 원본 뒤 `DAT_c78088 + param_1`에 마커 `0x3D9DB22D` | 마커 베이스. 기록 경로 밖 |
| `0x2B0A30` | `0x1AE30` | `[dx11] Backbuffer acquired` 함수. 1.61은 버전 `!= 0x3C` 가지라 stride `0xA8`, 필드 `+0x58`. `0xA0`/`+0x50`은 버전 `== 0x3C` 전용 | `DAT_c735a0` |
| `0x405618`·`0x40565B`·`0x405788` | `0x1AB40`·`0x1AB20`·`0x1AB60` | `persistent_drawable`. 부모 `0x1567D20`을 부르는 `0x405070` 안의 `call` 지점에 14바이트 FF25(`0x79990`). ETS2는 호출 지점 목록의 0번·1번·3번(✔ `xref_resolve.py`) | `DAT_c7307b`·`DAT_c7307a`·`DAT_c73079` (set→호출→clear) |

1.61에서 설치되지 않는 것: `DAT_c78020`(`fake_shadow_%u` → `0x1A8F0`, 버전 `<= 0x36`), `DAT_c78078`(`g_additional_water_fov` → `0x1AC30`, 버전 `< 0x37`일 때만 검색). `0x1AB80`은 `0x1ABE0`의 구버전 짝이라 1.61에서는 안 겁니다. `DAT_c78028`(`unknown command` 함수 `0x1E0CC0`)은 훅 대상이 아니라, `DAT_c73645 != 0`일 때 `quit`을 부르고 초기화를 끊는 게이트입니다.

### 4.3 D3D11 장치·컨텍스트 COM vtable 훅 — `.text`에 안 보임 (답 2의 핵심)

`0x17B70`이 D3D11 **장치** vtable 슬롯 3·5·7·8·9·0xA·0xB·0xC·0xF·0x15에, `0x17C90`이 **컨텍스트** vtable 슬롯 여러 곳(8·9·0xB~0xF·0x11·0x14·0x15·0x21·0x23·0x24·0x2C·0x36·0x45·0x77·0x7B)에 핸들러를 심습니다. 심는 방식은 `FUN_180019430`의 `*(vtable + slot*8) = handler`(**힙/.rdata vtable 포인터 교체**)라, `.text` 바이트는 바뀌지 않습니다 → **`scan_text`가 탐지 불가**(✔).

슬롯 번호는 `0x17B70`·`0x17C90`의 즉시값이고, 이름은 Windows SDK 10.0.26100.0의 `ID3D11DeviceVtbl`·`ID3D11DeviceContext1Vtbl`과 대조했습니다. `0x17890`은 장치 Map이 아니라 컨텍스트 Unmap(슬롯 15)입니다. Map은 `0x177D0`(슬롯 14)입니다.

장치 (`0x17B70`):

| 슬롯 | 메서드 | 핸들러 | 원본 저장 | 공유 쓰기 |
| --- | --- | --- | --- | --- |
| 3 | CreateBuffer | `0x16100` | `DAT_c73390` | 카운터 `DAT_ccc8bc`, 프록시로 감쌈 |
| 5 | CreateTexture2D | `0x16710` | `DAT_c734d0` | desc 검사. 맵 삽입은 이 함수 첫 조각에서 안 보임 |
| 7 | CreateShaderResourceView | `0x16200` | `DAT_c734e8` | `DAT_c735c8`·`DAT_c735a8`, `0x18B00` 맵 삽입 |
| 8 | CreateUnorderedAccessView | `0x16470` | `DAT_c734f0` | 첫 조각 0 |
| 9 | CreateRenderTargetView | `0x164D0` | `DAT_c734d8` | `0x14F70`이 먼저 `0x17B60`→`0x19410`으로 슬롯 9를 `0x17B20`에 넣는다. 게임 원본은 `DAT_c73588`, `DAT_c734d8`에 남는 값은 `0x17B20`. `0x17B20`이 `DAT_c735a0`을 씀 |
| 10 | CreateDepthStencilView | `0x16800` | `DAT_c734e0` | 첫 조각 0 |
| 11 | CreateInputLayout | `0x173F0` | `DAT_c73550` | 첫 조각 0 |
| 12 | CreateVertexShader | `0x16B10` | `DAT_c73560` | 카운터 `DAT_ccc8c4` |
| 15 | CreatePixelShader | `0x16F50` | `DAT_c73568` | 카운터 `DAT_ccc8c0` |
| 21 | CreateDepthStencilState | `0x17350` | `DAT_c734a8` | 첫 조각 0 |

컨텍스트 (`0x17C90`). 슬롯 119·123은 `ID3D11DeviceContext1`의 `VSSetConstantBuffers1`·`PSSetConstantBuffers1`입니다.

| 슬롯 | 메서드 | 핸들러 | 공유 쓰기 |
| --- | --- | --- | --- |
| 8 | PSSetShaderResources | `0x17970` | 첫 조각 0 |
| 9 | PSSetShader | `0x176F0` | `mov [DAT_c76838], rdx` 후 원본으로 jmp. 락 없음 |
| 11 | VSSetShader | `0x176E0` | `mov [DAT_c76830], rdx` 후 원본으로 jmp. 락 없음 |
| 12 | DrawIndexed | `0x17510` | 첫 조각 0 |
| 13 | Draw | `0x17560` | `DAT_c77e00`이 있으면 그쪽으로 jmp. 자신은 전역을 안 씀 |
| 14 | Map | `0x177D0` | 첫 조각 0 |
| 15 | Unmap | `0x17890` | `DAT_c735bc`를 읽어 업로드 사본을 만듦. 락 없음 |
| 17 | IASetInputLayout | `0x17780` | `DAT_c76840` |
| 20 | DrawIndexedInstanced | `0x174D0` | 첫 조각 0 |
| 21 | DrawInstanced | `0x17530` | 첫 조각 0 |
| 33 | OMSetRenderTargets | `0x17580` | 원본 호출 뒤 `DAT_c77e98`(개수), `DAT_c77e58`(RTV 블록), `DAT_c77ea0`(DSV). 락 없음 |
| 35 | OMSetBlendState | `0x17A50` | `DAT_c76848` |
| 36 | OMSetDepthStencilState | `0x17760` | `DAT_c73578`·`DAT_c73570` |
| 44 | RSSetViewports | `0x17630` | `DAT_c77c60`·`DAT_c77c64` |
| 54 | GenerateMips | `0x17A60` | 전역 쓰기 없음. `DAT_c77e40` 또는 원본으로 jmp |
| 69 | CSSetShader | `0x17700` | `DAT_c77e30`을 읽음 |
| 119 | VSSetConstantBuffers1 | `0x17A80` | `StartSlot==0 && NumBuffers==1`일 때 `DAT_c76820`·`DAT_c76828`·`DAT_c7682c` |
| 123 | PSSetConstantBuffers1 | `0x17AD0` | 같은 조건에서 `DAT_c77de8`·`DAT_c77df0`·`DAT_c77df4` |

- `CreateBuffer`·`CreateShaderResourceView`는 리소스를 프록시 객체로 감싸고 포인터→프록시 **`std::map`(RB-tree `DAT_c76810`)에 삽입**합니다. 삽입(`0x18B00`)·조회(`0x17E80`·`0x17F50`)·리밸런스에 **락이 없습니다**(✔). 삽입 카운터 `DAT_ccc8bc`도 비원자입니다.
- 패스 기록 중 바인딩 패킷 생성(`0x2B6B20` 장치 `[0x278]`)이 SRV/샘플러를 **지연 생성**하므로, worker가 동시에 기록하면 이 맵에서 경합합니다. 그와 별도로 위 컨텍스트 썽크는 셰이더·RTV·뷰포트·상수 버퍼 포인터를 고정 `.data`에 덮어 씁니다. 풀 락은 이 주소들을 덮지 않습니다.

## 5. 왜 AA 끔이 조건이 못 되나 (답 2·4)

AA/TAA 검사는 그리기 디스패치 표(`DAT_c77E00..`, `0x17460`이 채움)의 `0x9FE10`류에 있습니다: 프레임당 한 번 `DAT_c7382c`(= cfg `taa` 값 `DAT_a3d840`의 스냅숏)를 갱신하고, `DAT_c7382c==0 || DAT_c737ac!=1`이면 원본만 호출(효과 생략). 그러나 **패스 기록 중계 핸들러(`0x1A890`·`0x1A8C0`)는 이 검사를 거치지 않고** 무조건 `DAT_c780e8`·`DAT_c735bc`를 씁니다(✔). 그래서 AA를 꺼도 기록 경로의 공유 전역 쓰기는 그대로 남습니다 — 28 §2.10의 "AA 끔은 조건이 못 됨" 실측과 정적으로 일치합니다.

## 6. 공존 판정 (답 5)

snowymoon이 기대하는 불변: **기록 경로 중첩(장면→합성·업로드)에 임의 시점 스레드 1개.**

| 28 모드 | snowymoon 전역에 미치는 영향 | 판정 |
| --- | --- | --- |
| M3a `serial`(합류점 직렬 실행) | 메인 1스레드 | ✔ 안전(실측 완료) |
| M3b-1 ticket(한 번에 한 패스) | 항상 1스레드 활성 | ✔ 안전(28 §11.2 실측) |
| **M3b-2 order=free + 전체 락** | 전체 락이 곧 snowymoon의 "1스레드" 불변을 강제 | ✔ **안전**(새 근거). 단 직렬이라 이득 없음 |
| M3c 락 축소 | 우리 풀 락(`0x2E5040` 등)은 snowymoon 전역을 **덮지 않음**. 장면/합성의 `DAT_c780e8`, lockless `std::map` `DAT_c76810`, 그리고 컨텍스트 썽크가 매 바인드마다 쓰는 `DAT_c76830`·`DAT_c76838`·`DAT_c77e58`·`DAT_c77c60`·`DAT_c735bc`가 풀 함수 밖에 있음 | ✗ |
| M3d 링 2단계 | 같음 + composer 잎(`0x3EB300`→`0x1A440`)이 worker에서 동시 실행 | ✗ |

즉 M3b-2까지는 프록시가 있어도 "본체 전체 한 락"이 snowymoon의 단일-스레드 불변을 지켜 안전하지만, **M3c부터 락을 줄이는 순간 깨집니다**. 풀 락을 snowymoon 전역까지 넓히는 것은 불가능합니다. `DAT_c780e8`는 장면 패스 진입(`0x1A890`)에서 쓰이고 우리가 거는 풀 함수보다 상위이며, `std::map` 삽입과 컨텍스트 바인드 전역(§4.3)은 장치·immediate context 경로라 우리 락 밖입니다.

## 7. 28 §5 게이트 권고

1. **M3b-1·M3b-2는 프록시가 있어도 허용**해도 됩니다. 근거를 "실측상 크래시 없음"만이 아니라 "전체 락이 snowymoon의 단일-활성-스레드 불변을 강제"로 문서화하세요(§6). `accept_foreign_hooks: true`의 정당화가 M3b-2로 확장됩니다.
2. **M3c·M3d는 프록시 제거 세션에서만.** 게이트 조건은 "기록 경로 closure 안 foreign 훅 0"이고, **AA 끔은 이 조건을 완화하지 못합니다**(§5, 증명됨). 게임 폴더 `dxgi.dll` 유무는 진단 메시지일 뿐.
3. **`scan_text`는 필요하지만 불충분합니다.** D3D11 장치·컨텍스트 COM vtable 훅은 포인터 교체라 `.text` 비교에 안 잡힙니다(§4.3). M3c를 켜기 전 게이트는 **장치·immediate context vtable 포인터가 eurotrucks2.exe 범위를 가리키는지**(dxgi.dll이 아니라)도 확인해야 합니다. 구체적으로 `DAT_143681860` 장치의 vtable과, 그 vtable이 참조하는 컨텍스트 객체 vtable의 각 슬롯이 exe `.rdata`/`.text` 안을 가리키는지 검사하세요.
4. (선택) snowymoon이 cvar `r_aa`를 강제로 0으로 돌려놓으므로(§4.2 `0x1AD50`→`0x1AC80`), 게임 설정에서 AA를 바꿔도 프록시가 있는 한 되돌려집니다. 프록시를 뺀 세션 판정 때 이 cvar 상태를 함께 기록하세요.

## 8. 진행도와 밝혀내지 못한 것

§1–§9는 훅과 공존 판정입니다. DLL 전체는 §10입니다.

### 8.1 결정 범위 — 결론 확정

| 밝혀낸 것 | 근거 |
| --- | --- |
| 기록 경로 closure 안 3개 훅의 핸들러·공유 상태·스레드 안전성 | ✔ §4.1 (디컴파일 전문 스캔) |
| AA 끔이 기록 중계의 전역 쓰기를 멈추지 못함 | ✔ §5 |
| COM vtable 훅이 `.text` 비교에 안 보임 + lockless `std::map` | ✔ §4.3 |
| 공존 판정(M3b-2 안전, M3c 불가)과 게이트 권고 | ✔ §6·§7 |
| 패킹 여부, 훅 엔진(MinHook), 참조 문자열로 RVA를 고르는 방식 | ✔ §2·§3·§9 |
| 1.61.1.9 훅 RVA ↔ 핸들러, 슬롯별 핸들러와 공유 전역 | ✔ §4.2·§4.3·§9 |

잔여 △는 없습니다. uniform 합성 함수 `DAT_c78010` = `0x3EB300`, `persistent_drawable` = `0x405618`·`0x40565B`·`0x405788`, 장면 `DAT_c78000` = `0x3A9`·`DAT_c78008` = `0x30D`, Present = `0x2BFEF0`. 이 값들은 DLL 파일에 없고, 게임 `.text`를 Capstone으로 훑은 힙 레코드에서 나옵니다. `xref_resolve.py`가 같은 선택을 재현했고, 검증용 참조 문자열 일곱 개의 범위 시작이 pdata 시작과 모두 같습니다(§10.12).

### 8.2 DLL 전체

구성은 §10입니다. 큰 스위치 세 개(`0xE4EE0` SPARC, `0xCDE40` PowerPC, `0xFB5A0` SystemZ)와 Wasm 이름 표는 정적 링크된 Capstone 5의 아키텍처 모듈이고, 실제로 여는 것은 x86-64 하나입니다(§10.7). `0xCD2B0`을 81 KB로 센 것은 맞닿은 pdata를 한 함수로 붙인 오류이고, 실제 구간은 `0xB5`바이트입니다. `.vlizer`와 라이선스 비교는 열지 않았습니다. 동적 검증은 없습니다.

### 8.3 결론

공존 판정은 §8.1에서 끝났습니다. DLL 전체는 §10이고, 효과 한 프레임의 경로는 §10.15, 셰이더 슬롯과 Capstone은 §10.16입니다. 라이브러리 구성은 §10.2에서 이름까지 닫혔습니다. `.vlizer`는 열지 않았습니다. 장면 스텁의 평문은 게임 함수 `0x1475C90`에 있고 VM은 그 복사본을 XOR한 뒤 받습니다.

## 9. 1.61.1.9 참조 문자열 → RVA

`0x6BBD0`(`0x1B150`에서 호출)이 exe를 디스크에서 읽어 MZ/PE를 확인하고, 섹션을 파일 오프셋에서 가상 주소로 복사합니다. 재배치는 적용하지 않습니다. `0x6ABE0`이 ImageBase를 `DAT_a3d848`에, 펼친 이미지 기준을 `DAT_c737e0`에, `.text`·`.rdata`(그리고 이어진 `.data`까지의 검색 길이)를 전역에 넣습니다. `0x6BD80`은 그 버퍼에서 참조 문자열을 찾습니다(`0x6AFB0`, `.rdata`부터 `.data` raw 끝까지에서 첫 히트 하나). `r9b==1`이면 널 포함 정확 일치, `r9b==0`이면 접두사입니다. 히트의 선호 VA로 `0x6B750`이 12바이트 xref 레코드(`DAT_ccc658`, `+4`의 qword가 문자열 주소)를 걷고, 대응 13바이트 심볼(`DAT_ccc6a0`)의 코드 주소를 모읍니다. `0x6B6E0`이 그 주소를 품는 0x34바이트 범위의 시작을 돌려주고, ImageBase를 뺀 값이 훅 RVA입니다.

아래 함수 범위는 1.61.1.9 `eurotrucks2.exe`의 연속 pdata입니다. DLL의 범위는 pdata가 아니라 Capstone 선형 스캔이 `int3`·`ret`·먼 `jmp`에서 끊은 구간이지만(§10.12), `xref_resolve.py`로 재현하면 이 표의 시작 주소와 모두 같습니다. 게임 문자열 RVA도 이 파일에서 확인했습니다.

| 참조 문자열 | 일치 | 전역 | 함수 (xref) | 1.61 핸들러 |
| --- | --- | --- | --- | --- |
| `transform_p_matrix` | 정확 | `DAT_c77ff0` | `0x16706E0` (`+0x42`). 문자열 `0x24506E0` | 훅 아님. `0x1B090`이 `DAT_c73668 = base + c77ff0`으로 두고 uniform 합성 함수가 읽음 ✔ |
| `transform_mvp_matrix` | 정확 | `DAT_c78010` | `0x3ECD90` (xref `0x3ED7B6`, 범위 시작에서 `>= 0x300` 통과)와 `0x16706E0` (`+0x27`, 탈락). 문자열 `0x223A138` | `0x3EB300` → `0x1A440` ✔. 아래 |
| `?element_at@?$array_t@Uqueue_data_t@render_queue_set_data_t@prism` | 접두 | `DAT_c77ff8` | `0x1475C90`–`0x1476039` (xref `0x1475FB5`). 문자열 `0x2445F30` | `0x1A890`, 설치는 `0x76510` ✔. 길이 `0x3A9`, 내부 호출 `+0x30D` ✔ |
| `[dx11] Mapping buffer with unsupported usage flags!` | 접두 | `DAT_c78018` | `0x2B5FE0`–`0x2B6319` (`+0x117`). 문자열 `0x2214740` | `0x1A8C0` ✔ |
| `fake_shadow_%u` | 정확 | `DAT_c78020` | `0x421710`, `0x146F9E0`, `0x147B0C0` | 1.61에서 미설치 (`<= 0x36`만) |
| `persistent_drawable` | 정확 | `DAT_c78068`·`DAT_c78060`·`DAT_c78070` | 부모 `0x1567D20`–`0x1567F2D` (`+0x1B`). 문자열 `0x24302E0` | 부모를 부르는 `0x405618`·`0x40565B`·`0x405788`에 `0x1AB40`·`0x1AB20`·`0x1AB60` ✔ |
| `[rfx] Cannot tokenize semantics name '%s' in effect` | 접두 | `DAT_c78090` | `0x332F40`–`0x3332E5` (`+0x78`). 문자열 `0x2226F00` | `0x1ABE0` (`>= 0x3B`). `0x1AB80`은 그 미만 ✔ |
| `g_additional_water_fov` | 정확 | `DAT_c78078` | xref `0x36C7F`는 pdata 밖. 문자열 `0x2240C18` | 1.61은 `cmp 0x37; jae`로 검색 자체를 건너뜀 |
| `[cmd] '%s' - unknown command` | 접두 | `DAT_c78028` | `0x1E0CC0`–`0x1E112A` (`+0x423`). 문자열 `0x21F2D18` | 훅 아님. quit 게이트 ✔ |
| `[cvar] Skipping store of cvars to %s - store is disabled.` | 접두 | `DAT_c78030` | `0x4208A0`–`0x4209B7` (`+0x17`). 문자열 `0x21EFDD0` | `0x1AD50` ✔ |
| `[dxgi] Present failed (0x%lx)` | 접두 | `DAT_c78038` | `0x2BFEF0` (`+0x60`). 문자열 `0x2215E58`. `0x1913940`의 `+0x88`은 `lea r9, [rsp+0x90]` 안의 바이트가 rel32로 우연히 맞은 오탐 | `0x1AEC0` ✔. 라이브와 같음 |
| `[dx11] Backbuffer acquired` | 접두 | `DAT_c78048` | `0x2B0A30`–`0x2B0CDB` (`+0x1C0`). 문자열 `0x2213DF8` | `0x1AE30` ✔. `0x2B0AAC`는 이 함수 중간의 pdata 조각 |
| `<undefined>` | 정확 | `DAT_c78040` | `0x338790`–`0x338933` (`+0x3C`). 문자열 `0x2227C90`. 진입부 `sub rsp`를 16바이트로 맞춘 주소 | `0x1AFF0` ✔ |
| `additional_ambient` | 정확 | `DAT_c78088` | 코드 xref `0x32ABC0`, `0x336360`. 문자열 `0x21FF258`. 다른 두 히트 `0x2253BE5`·`0x2253C55`는 코드 xref 없음 | 훅 RVA 아님. 태그 `0x1EA` 심볼을 `0x6BAA0`에 넣어 얻는 마커 베이스 |

uniform 합성 함수(✔, 두 홉): `DAT_c78010`은 문자열을 품은 함수 안의 호출이 아닙니다. `0x6BD80`은 xref 중 범위 시작에서 `0x300` 이상 떨어진 첫 xref(`0x3ED7B6`)를 고르고, 같은 범위에서 그 뒤의 첫 RIP 상대 명령의 목표(`0x6BAB0`, 여기서는 `0x3EBB70`)를 콜백 함수로 봅니다. 그 콜백 범위 `0x3EBB70`–`0x3EBBD3`에서 첫 `E8` 호출(`0x3EBB84`)을 찾고, `0x6BB60`이 그 명령을 다시 디코드해 `op[0].imm`을 돌려줍니다. 값은 `0x3EB300`이고 라이브 게이트가 써 온 주소와 같습니다. `0x3ECD90` 안의 직접 `call`에 `0x3EB300`이 없던 것은 이 홉 때문입니다.

백버퍼 패턴(✔, 둘 다 `0x2B0A30` 안에 하나):

- 마스크 `xxxxx`, 바이트 `B9 FF FF 00 00`이 `0x2B0A66`. 매치 `+8`의 dword `0x363FD40`이 `DAT_c78050`. 핸들러는 `word ptr [param_1 + DAT_c78050]`으로 읽습니다. 그 다음 명령이 `movzx eax, word ptr [rdi + 0x363FD40]`이라 같은 필드입니다.
- 마스크 `x??????x??x`, 바이트 `48 8D 8F 58 FE 63 03 48 3B 51 10`이 `0x2B0A80`(`lea rcx, [rdi + 0x363FE58]`). 매치 `+3`의 dword `0x363FE58`이 `DAT_c78058`. 핸들러는 `[param_1 + DAT_c78058 + 8]`의 qword를 테이블 베이스로 씁니다.
- `0x1AE30`의 stride는 패턴이 아니라 핸들러 상수입니다. `DAT_c7381c == 0x3C`일 때만 `u * 0xA0 + 0x50`, 그 외(1.61의 `0x3D` 포함)는 `u * 0xA8 + 0x58`. 설치 조건은 버전 `>= 0x3C`이고 `DAT_c78050`·`DAT_c78058`이 둘 다 0이 아닌 것입니다. 이 빌드는 그 조건에 들어가 `0x1AE30`을 겁니다.

`persistent_drawable` 세 지점(✔): `0x6B810`은 xref 레코드가 아니라 분기 레코드(`DAT_ccc670`, 니모닉이 `j`·`c`로 시작하고 `op[0]`이 즉시값인 명령)를 걷습니다. 목표가 부모 범위 시작 `0x1567D20`인 명령의 주소를 스캔 순서대로 모으면 `0x405618`, `0x40565B`, `0x40569F`, `0x405788`, `0x4227A0`, … 12개이고, 앞의 넷이 `0x405070` 안의 `call 0x1567D20`입니다. `[0]` → `DAT_c78068` → `0x1AB40`, `[1]` → `DAT_c78060` → `0x1AB20`. 세 번째는 `DAT_c73644`가 0이면 `[3]`, 0이 아니면 `[2]`이고 둘 다 `DAT_c78070` → `0x1AB60`입니다. `DAT_c73644`는 `0x71CC0`이 exe 경로에 `American` 또는 `amtrucks`가 있을 때만 1이 되므로 ETS2에서는 `[3]` = `0x405788`입니다. 설치는 `0x1B090`이 `0x79990`(14바이트 FF25)으로 그 호출 지점에 겁니다.

장면 복사 길이(✔): 참조 문자열의 마지막 xref `0x1475FB5`가 든 범위는 `0x1475C90`–`0x1476039`입니다. `DAT_c77ff8`은 그 시작에서 ImageBase를 뺀 값, `DAT_c78000`은 범위 길이 `0x3A9`입니다. `DAT_c78008`은 범위 안의 `E8` 호출 가운데, 호출 목적지 범위에 "두 번째 이후 즉시값이 `0xFFFF` 하나뿐인 명령"(`0x6B8D0`)이 있는 마지막 호출의 오프셋입니다. 이 빌드에서 마지막으로 걸린 호출은 `0x1475F9D`의 `call 0x160EE40`이라 `+0x30D`입니다. `0x76510`은 이 길이만큼 게임 함수에서 평문을 복사한 뒤에야 XOR하고 `.vlizer`의 `0x11BA8BD`로 넘깁니다. 공존 판단에는 핸들러 `0x1A890`의 전역 쓰기면 충분하고, VM을 되돌릴 필요는 없습니다.

## 10. DLL 전체

18 MB는 효과 셰이더 하나가 아닙니다. `.text` 약 4 MB 안에 프록시, ImGui, wxWidgets, NGX SDK, 효과 등록이 한 바이너리로 들어 있고, `.vlizer` 4.9 MB는 Oreans 가상화 구역입니다. DLL은 로드하지 않았고 게임도 켜지 않았습니다. 셰이더 원문은 적지 않습니다. 이름, 등록 순서, 부트 순서만 적습니다.

### 10.1 섹션

| 섹션 | VA | 가상 크기 | 비고 |
| --- | --- | --- | --- |
| `.text` | `0x1000` | `0x3E8765` | 엔트로피 6.50. 전체 패킹 아님 |
| `.rdata` | `0x3EA000` | `0x62E1AC` | 문자열, RTTI, vtable, Wasm 명령 이름 |
| `.data` | `0xA19000` | `0x2B7ED8` | 훅 오프셋 전역은 파일에서 0 (§9) |
| `.pdata` | `0xCD1000` | `0x29B44` | |
| `_RDATA` | `0xCFB000` | `0x15C` | |
| `.fptable` | `0xCFC000` | `0x100` | 가상화기 지문 표 |
| `.rsrc` | `0xCFD000` | `0x338` | |
| `.reloc` | `0xCFE000` | `0x3B7B0` | |
| `.vlizer` | `0xD3A000` | `0x4DAA00` | RWX, 엔트로피 6.44 |

이미지 크기 `0x1215000`. 진입점 `0x141DF0`. 6글자 이상 문자열 19804개. RTTI 이름 879개.

### 10.2 한 파일에 들어 있는 것

| 덩어리 | 근거 |
| --- | --- |
| 자기 그래픽 프록시 | RTTI `SVertexShader`, `SPixelShader`, `STexture2D`, `SShaderResourceView`, `SInputLayout`, `SD3D11Buffer`, `SRenderTargetView`, `SDepthStencilView`, `SDepthStencilState`, `SResource`. 훅은 §3–§4 |
| MinHook | §3. HDE64, `0x21170`, 래퍼 `0x79760`·`0x79990`·`0x796F0` |
| Dear ImGui | `imgui_impl_dx11`, `imgui_impl_win32`, `imgui.ini`, `imgui_log.txt`. 메뉴 문자열 `Colors / Tonemap`의 코드 참조는 `0x257DD`, `0x26337` |
| wxWidgets | 소스 경로 `src\common` 90개, `src\msw` 85개, `include\wx`와 `src\generic`. RTTI에 `wxApp`, `wxDialog`, `wxGDIPlusRenderer`. 자기 클래스 `MyApp`, `InputDialog` |
| NVIDIA NGX SDK | 경로 `nvsdk_ngx_d3d11_lib.cpp`, `nvsdk_ngx_lib_windows.cpp`, `util-log.cpp`, `common_lib.cpp`. 내보내기는 한 줄 점프가 아님 (§10.8) |
| Capstone 5 디스어셈블러 | `.rdata` `0x3ED000`의 `arch_configs` 18칸(0x18 간격, init·option·금지 모드 마스크). ARM·ARM64 칸은 비어 있고 MIPS부터 TriCore까지 16개가 링크됨. 마스크가 Capstone 5.0 소스와 일치(x86 `0xFFFFFFF1`, PPC `0x7FFFFF63`, M680X `0xFFFFF801` 등). `cs_open` `0xAD8B0`, `cs_option` `0xADA00`, `cs_malloc` `0xAD830`, `cs_disasm_iter` `0xAD640`, 할당 콜백 표 `0xA19000`. 호출은 `0x6B060`의 `cs_open(X86, MODE_64)` 하나 (§10.16) |
| Wasm 명령 이름 표 | `.rdata` `0x3F0530` 근처에 `unreachable`, `i32.add`, `f32.convert_s/i32`. 분류 문자열이 `numberic`(철자 그대로), `parametric`, `variable`, `memory`, `control`. Capstone WASM 모듈(`arch_configs` 13번 칸)의 표 |
| Oreans Code Virtualizer | `.vlizer`. `.text`에서 들어오는 간선은 `0x76510` → `0x11BA8BD` 하나 (§2) |

`DNameNode`, `charNode`, `pairNode` 같은 RTTI는 MSVC 이름 복원입니다. §10.7의 스위치와는 별개입니다.

### 10.3 부트

`DllMain` `0x141DF0`은 `edx==1`(PROCESS_ATTACH)에서 `0x142570`만 부릅니다. 그 함수는 보안 쿠키(`DAT_a37c68`) 초기화이고, 이어서 CRT `0x141CBC`로 갑니다. 효과 초기화가 아닙니다.

효과 부트는 `0xA6B20`입니다. 바이트 `DAT_ccdd81`이 0일 때 한 번 돌고 1로 둡니다. 호출부는 창 클래스 경로입니다. `0x14137`이 `GetClassNameW` 결과를 `prism3d`, `wxWindowNR`, `InvisibleWindowClassNvPresent`와 비교하고, `0x142C7`·`0x1432E`·`0x14390`·`0x143DE`에서 `0xA6B20`을 부릅니다. 이어진 함수 `0x14469`·`0x14631`·`0x146A0`·`0x1470D`에도 호출이 하나씩 있습니다.

`0xA6B20` 안의 순서입니다. 사이사이의 `0x83200`은 파일 id `0x28`, 줄 번호 `0x169`·`0x171`·`0x173`·`0x175`입니다.

1. `0x7A560` — 라이팅·톤맵 키 (§10.5).
2. 두 번째 인자의 vtable `+0x60`. 반환 포인터는 `DAT_c288c8`.
3. `0x1B090` — 게임 훅 설치 (§4·§9).
4. `0x14D60` — 두 인자를 `DAT_ccb690`와 `DAT_ccb680`에 저장합니다. 효과 등록은 이 전역의 주소를 레지스트리로 씁니다.
5. `0x14F70` — 장치·즉시 컨텍스트 COM vtable (§4.3).
6. `0xA4390` — 도로 셰이더 캐시. 여기서 `0x8CC00` → `0x86660` 효과 등록 (§10.4·§10.6).
7. `0x6F310` — NGX. `DAT_ccdd20`이 0일 때만. `0x137940(0xCC4F861, 빈 문자열, [DAT_ccb680], 0, 0x15)`의 상위 비트가 `0xBAD`이면 빠집니다. 아니면 `NVSDK_NGX_D3D11_GetCapabilityParameters(&DAT_c73810)`. `SuperSampling.Available`이 켜져 있으면 `0x6F190(0x87000000F00, 모드, …)`을 모드 5·2·1·0으로 부르고, 채워진 64비트의 상위 32비트를 정수로 보아 2160으로 나눈 값을 `DAT_c73800`·`DAT_c73804`·`DAT_c73808`·`DAT_c7380c`에 넣습니다. `DAT_c730b4 = 1`.

Present 브래킷 `0x1AEC0`이 부르는 `0xA6900`은 프레임 시작입니다. 바이트 `DAT_c7367f`가 아직 1이 아니면 `0x23330`을 부릅니다.

### 10.4 효과 등록 `0x86660`

호출은 `0x8CC00` 하나이고, 그 호출은 `0xA4390` 하나입니다. `0x8CC00`은 바이트 `DAT_ccdd88`이 0일 때 한 번 돕니다. `0x86660(&DAT_cca710)` 앞에 헬퍼 쌍 `0xA8930`·`0xA8A00`·`0xA8AD0`·`0xA8BA0`·`0xA8C70`으로 슬롯 10개(`0xCCA540`부터 0x18 간격, 로그 줄 번호 `0x335`·`0x336`·`0x339`·`0x33A`·`0x33D`·`0x33E`·`0x341`·`0x342`·`0x345`·`0x346`)를 만들고, `0x85C70(&DAT_cca630)`을 부릅니다. 그 다음 `DAT_ccdd8a`가 0이면 `0x11A20`으로 `&DAT_ccb680`에 항목 다섯(`0xCE`–`0xD2`)을 더하고 `[DAT_c734a0](DAT_ccb680, …)`을 부릅니다. 실패하면 `0xEC20`/`0x14E70`로 빠집니다. `0x86660` 바디는 26015바이트입니다. 이름 문자열을 만드는 `0x10380`은 SSO 문자열 생성(`0x106A0`)입니다. 버텍스 등록은 `0xFA80`, 픽셀 등록은 `0xFD40`, 레지스트리 주소는 `&DAT_ccb680`입니다.

등록되는 내부 이름입니다. 슬롯 이름(`VShader`, `PShader`, `main_lighting`, `PShader_TAA`)은 내부 이름과 따로입니다.

- VS `shader_postprocess_vs` / 슬롯 `VShader`. VS `shader_fog_vs` / 슬롯 `VShader_Fog`.
- PS 변종: `taamasked`, `taamask`, `motionblur`, `sharpen`, `sharpenmasked`(디파인 `USETAAMASK`), `dummynormals`.
- 라이팅. 패스 슬롯은 `main_lighting`. 디파인은 `LIGHT_VER`, `DISABLE_REFLECTIONS`. 캐빈은 `PASS_CABIN`, 미러는 `PASS_MIRROR`, 반사는 `DEF_LIGHTINGREFLECTIONS`와 `PASS_lightingreflection`. 내부 이름은 `lighting`, `lightingcabin`, `lightingmirror`, `lightingref`, `lightingrefcabin`이고, 같은 다섯이 2·3·4·5세대로 반복됩니다. 이어서 `lightingcombine`, `lightingcombine2`, `lightingadd`, `lightingadd2`, `lightingadd3`.
- `tonemap`부터 `tonemap5`.
- SSAO: `ssao_q1`, `ssaoblur1`, `ssaoblur2`, `ssaotaa`, cabin 세 개(`ssaoblur1cabin`, `ssaoblur2cabin`, `ssaotaacabin`), sharp 세 개(`ssaoblur1sharp`, `ssaoblur2sharp`, `ssaotaasharp`). 이 구간의 슬롯 이름에 `PShader_TAA`가 있습니다.
- `gameshaders_1_1`. 마이너 1.53–1.61용 `gameshaders_15N_*`. 1.53–1.55는 접미 `1033`, `300`, `301`, `302`, `100`, `101`, `102`, `104`, `105`. 1.56–1.61은 `1033`, `300`, `301`, `302`만. 1.61 이름은 `gameshaders_161_1033`, `_300`, `_301`, `_302`.

이름이 실제로 그려지는 입구는 §10.11입니다. `0x9FE10`은 그 네 가로채기 지점 중 DrawIndexedInstanced 쪽이고, 패스 본체를 직접 그리지 않습니다.

### 10.5 라이팅·톤맵 키 `0x7A560`

`0xA6B20`이 게임 훅보다 먼저 부릅니다. 바디 5088바이트. 진입부에서 dword `0x99F56C`와 byte `0x99F570`을 복사하고 `word [DAT_c77f80]`을 0으로 지웁니다. 키 문자열은 `0x10380`이 만들고, 세 헬퍼가 전역에 묶습니다. `0x7A3C0`과 `0x7A490`은 값 전역 하나를 받고, `0x7A2D0`은 전역과 정수 둘을 받습니다. 호출부에 적힌 그 정수는 `0,1` 또는 `0,3`입니다. 헬퍼 본체는 안 열었습니다.

키는 다섯 벌입니다. 1번 벌: `Tonemap_TonemapA`, `Tonemap_Dynamic_Contrast`, `Tonemap_Pre_Exposure`, `Tonemap_Pre_Contrast`, `Tonemap_Exposure`, `Tonemap_Saturation`, `Tonemap_Contrast`, `Tonemap_Vibrance`, `Tonemap_Highlights`, `Tonemap_NightExp`. 2번 벌부터 접미 `2`. 3번 벌부터 `Shadows`, `Blacks`, `Whites`가 붙고, 4번 벌부터 `Temp`가 붙으며 5번 벌까지 갑니다.

그 다음: `Lighting_Interior`, `UseInteriorLighting`, `LightingMethod`(정수 `0,3`), `UseDefaultMirrors`, `UseDefaultRain`, `SSAO_Preset`, `UseHalfResSSAO`, `UseSSS`, `SSAO_DetailQuality`, `SSAO_Intensity`, `Global_Quality`, `roads_defaultnormals`, `roads_normalintensity`, `vegetation_leavesthickess`(철자 그대로), `vegetation_grassthickness`, `surface_albedosaturation`.

### 10.6 도로 셰이더 캐시 `0xA4390`

호출은 `0xA6B20` 하나입니다. 바디 9544바이트. 버전 dword `DAT_c7381c`를 `0x35`부터 비교하며 xmm 상수를 고릅니다. `0x8CC00`(효과 등록), `0x17460`(드로우 테이블), `__std_fs_create_directory`를 부릅니다. 경로 `snowymoon/cache_road`의 코드 참조는 `0xAB133`(`0xAB090` 근처)입니다. 실패 문구는 `snowymoon_error.zip`, `snowymoon_dump.zip`입니다.

### 10.7 큰 스위치 세 개

pdata는 함수 끝이 다음 함수 시작과 맞닿는 일이 있습니다. 그 경계를 지우고 붙이면 옆 함수까지 한 덩어리가 됩니다. `0xCD2B0`의 81 KB는 그 오류입니다. 그 주소의 pdata는 `0xCD2B0`–`0xCD365`, `0xB5`바이트이고, `0xCC9A0`이 `0xCCDF7`에서 부릅니다.

한 함수로 확인된 큰 몸체는 세 개입니다. 앞의 둘은 같은 노드 접근자를 씁니다.

| 함수 | pdata 크기 | 스위치 | 호출 |
| --- | --- | --- | --- |
| `0xE4EE0` | `0x14694` | `dword [rcx+8]`을 `0x14A`와 비교한 뒤 함수 안의 점프 표 | `0xE4B70`의 `0xE4B8D` |
| `0xCDE40` | `0x12CDB` | 같은 태그를 `0x413`과 비교 | `0xCC9A0`의 `0xCCE0D` |
| `0xFB5A0` | `0x8718` | `edx`를 `0x189`와 비교. `0x103CC0`은 비트 폭이 `0x40`이면 시프트, 아니면 마스크 후 시프트 | `0xFAC0D`의 `0xFAF3A` |

노드 접근자는 pdata 밖의 짧은 리프입니다. `0xAEC00`은 `byte [rcx+4]`, `0xAEC10`은 `dword [rcx+8]`, `0xAEC30`은 `rcx+(index+1)*16`, `0xAED70`은 `qword [rcx+8]`입니다. `0xCC9A0`이 `0xAEC30`으로 자식을 걷다가 `0xCDE40`으로 들어갑니다.

태그가 고르는 것은 명령 템플릿입니다. 니모닉 뒤에 `$`와 피연산 바이트가 붙어 있어서 길이만 보는 문자열 스캔에는 안 잡힙니다.

`0xE4EE0`의 단독 키가 싣는 접두는 `ba`, `bn`, `fba,a,pn`, `fmovda %icc`, `mova %icc`입니다. 파일에서 `fmovda`는 `0x5792F0`, `ba,a,pn`은 `0x578B79`입니다. `0xE4B70`은 스위치가 돌려준 문자열을 `0x584FD0`의 이름 표(id, 포인터)와 비교합니다. 표의 앞 이름은 `addcc`, `addx`, `addxcc`, `addxc`, `alignaddr`, `andcc`로, SPARC 정수·VIS 니모닉입니다.

`0xCDE40`의 접두는 `bltctr`, `bltlrl`, `cmpd`, `crclr`, `mbar`, `sync`, `lwsync`, `ptesync`입니다. `bltctr`는 `0x53E9B0`, `lwsync`는 `0x53FB24`입니다. `0xCD2B0`은 `b%sctr`, `b%slrl` 형식 문자열을 고르고, 바로 이어지는 `0xCD365`가 `lt` `gt` `eq`와 `4*cr%d+`를 붙입니다. PowerPC 분기·조건 레지스터 형식입니다.

`0xFB5A0` 본문에는 니모닉이 없습니다. `edx`가 `0`부터 `0x189`까지 고유 383개 케이스로 갈라지고, `0x103CC0`이 비트를 뽑은 뒤 니블 id 표로 `0xAED20` 자식 노드를 만듭니다.

키 가운데 `0x4A`처럼 Wasm 단일바이트 opcode와 값이 같은 것이 있습니다. 그 케이스의 문자는 위의 SPARC 접두고, §10.2의 Wasm 이름 표를 rel32로 집는 곳은 없습니다. `DXBC`, SPIR-V `0x07230203`, `\0asm`은 세 본문의 즉시값에 없습니다.

정체(✔ §10.16): 셋 다 Capstone 5의 아키텍처 모듈입니다. `0xAE390`·`0xAE440`·`0xAE4D0`은 `PPC_global_init`·`Sparc_global_init`·`SystemZ_global_init`이고, `.rdata` `0x3ED060`·`0x3ED078`·`0x3ED090`(`arch_configs`의 4·5·6번 칸)에서만 가리킵니다. 0x58바이트는 `cs_mem_malloc`으로 잡는 `MCRegisterInfo`이고 핸들의 `+0x10`(printer_info)·`+0x20`(getinsn_info)에 들어갑니다. 함수 포인터는 그 객체가 아니라 핸들 `cs_struct`에 들어갑니다. `+8` printer(`0xCC9A0` PPC, `0xE4B70` SPARC), `+0x18` disasm(`0xC8660` PPC, `0xFABA0` SystemZ), `+0x28` reg_name, `+0x30` insn_name, `+0x38` group_name, `+0x40` insn_id, `+0x48` post_printer입니다. 부르는 쪽은 `cs_disasm_iter` `0xAD640`(`call [rbx+0x18]`·`[rbx+0x40]`·`[rbx+8]`)뿐인데, 유일한 `cs_open`이 x86이라 이 세 모듈은 실행되지 않습니다. `0xFB5A0`은 SystemZ 디코더 표입니다.

### 10.8 내보내기

`CreateDXGIFactory` `0x14820`은 `DAT_c730a8`에 들어 있는 함수 포인터를 부릅니다. 시스템 팩토리로 넘기는 자리입니다.

`NVSDK_NGX_D3D11_AllocateParameters` `0x137560`은 `DAT_c34cd0`이 0이면 `0xBAD00007`을 반환하고, 아니면 `DAT_c34d28`을 부릅니다. `DLSS.Hint.Render.Preset.Quality`의 코드 참조는 `0x1372C1`, `0x1372DE`로, NGX 덩어리 안입니다.

### 10.9 이름만 적고 본문은 열지 않은 것

- `Invalid license!`의 코드 참조는 `0x27B7B`입니다. 톤맵 메뉴와 같은 UI 구간이고, wx `InputDialog`·`MyApp`이 그 창입니다. 비교 루틴은 열지 않았습니다.
- `0x23330`이 `GetProcAddress`로 `XInputGetState`를 가져옵니다. 참조는 `0x23ECE`와 `0x552C6`입니다. 입력 경로의 동작 절차는 적지 않습니다.
- `.vlizer`는 되돌리지 않습니다. 장면 스텁이 복사하는 평문은 게임 함수 `0x1475C90`에 있습니다(§9).
- `0xADA00`은 `cs_option`입니다. `edx==4`(`CS_OPT_MEM`)로 부르는 곳은 없고, 호출은 `0x6B060`의 `CS_OPT_DETAIL`(2)·`CS_OPT_SKIPDATA`(5) 두 번입니다.

### 10.11 그리기 호출 가로채기

`0xA4390`이 `0x17460`으로 함수 포인터를 채웁니다. 컨텍스트 vtable 썽크(§4.3)가 그 포인터가 0이 아니면 원본 대신, 또는 원본 앞에서 부릅니다. 슬롯 번호는 `ID3D11DeviceContext`입니다.

| API | 슬롯 | 썽크 | 표 | 본체 | 원본 슬롯 |
| --- | --- | --- | --- | --- | --- |
| DrawIndexed | 12 | `0x17510` `jmp` | `DAT_c77e08` | `0x9FFB0` | `DAT_c73120` |
| Draw | 13 | `0x17560` `jmp` | `DAT_c77e00` | `0xA37A0` | `DAT_c73518` |
| DrawIndexedInstanced | 20 | `0x174D0` `call` | `DAT_c77e10` | `0x9FE10` | `DAT_c73258` |
| DrawInstanced | 21 | `0x17530` `call` | `DAT_c77e18` | `0x9FEE0` | `DAT_c730c0` |
| OMSetRenderTargets | 33 | `0x17580` | `DAT_c77e20` | `0xA3870` | `DAT_c733c8` |
| RSSetViewports | 44 | `0x17630` | `DAT_c77e28` = 0 | 호출 안 함 | |
| GenerateMips | 54 | `0x17A60` `jmp` | `DAT_c77e40` | `0xA3CE0` | `DAT_c732c8` |
| CSSetShader | 69 | `0x17700` | `DAT_c77e30` | `0x10200` (`ret`) | `DAT_c73548` |

네 그리기 본체는 같은 게이트를 복사해 두었습니다. `DAT_c73830 != DAT_c288d8`이면 `DAT_c7367d = DAT_c7307f`, `DAT_c7367e = DAT_c730b6`, `DAT_c7382c = DAT_a3d840`을 복사합니다. `DAT_c7382c==0`이거나 `DAT_c737ac!=1`이거나 `0x9F7B0`이 0이 아니면 원본을 부르고, `0x9F7B0`이 0이면 원본을 삼킵니다. `0x9F7B0`의 첫 인자는 그리기 종류입니다. 0이 Draw, 1이 DrawInstanced, 2가 DrawIndexed, 3이 DrawIndexedInstanced.

`0x9F7B0`은 `DAT_b10fe8` 상태 기계입니다. taa가 꺼져 있거나 설치가 아니면 즉시 1을 반환합니다. 상태는 `OMSetRenderTargets` 썽크가 채워 둔 RT 스냅샷(`DAT_c77e58` 최대 8포인터, 개수 `DAT_c77e98`, DSV `DAT_c77ea0`)을 봅니다.

- 개수 4이고 DSV와 RT 네 장이 있으면 상태를 0으로 두고 `0x951F0`을 부릅니다. 이 함수의 문자열은 `Base Pass`입니다. 그리기 반환값은 `0x97880`입니다.
- 개수 2이면 상태 1, `0x98970`. 문자열은 `SSAO Blur`, `SSAO Temporal`입니다. 반환은 `0x9C430`. 문자열은 `lighting (`, `mirror lighting (`입니다.
- 개수 1이면 RT0에 `ID3D11Texture2D`를 묻습니다. IID는 `0x985A00`의 `6f15aaf2-d208-4e89-9ab4-489535d34f9c`입니다. `GetDesc`의 Format 필드(`+0x10`)가 10이면 `DXGI_FORMAT_R16G16B16A16_FLOAT`이고 상태 2입니다. 반환은 `0x9EC00`이고, 이 함수가 집는 경로는 `/effect/deferred/def_compose_lighting/def.compose.lighting.crefl.prefl.rfx1`입니다.
- DrawIndexed이고 인덱스 수가 `0x12C0`이고 상태가 4이면 상태 3, `0x9F440`, 반환은 `0x9F6A0`입니다.
- 그 외 상태는 1이라 원본 그리기가 통과합니다.

Draw이고 정점 수가 3이고 `DAT_b10fe8==4`이면 `0xA37A0`이 `0xA34A0`을 한 번 더 부릅니다. VS·PS 슬롯 썽크가 저장한 객체(`DAT_c76830`, `DAT_c76838`)의 `+0x2D0`/`+0x2E0`이 `0x65`/`0x64`이면 로그 문자열이 `tonemap pass` 또는 `resize pass`이고, `0xA1960` 또는 `0xA0070`이 `&DAT_ccb680` 레지스트리를 쓴 뒤 원본 Draw를 삼킵니다. 등록된 톤맵이 화면 쿼드(정점 3개)에서 원본을 대체하는 지점입니다.

`OMSetRenderTargets` 본체 `0xA3870`은 스냅샷이 이전과 같으면 원본을 부르지 않습니다. 디스크립터 dword가 `0x1A`이고 배열 6, misc 5일 때 로그 문자열은 `Interior1 `입니다. `GenerateMips` 본체 `0xA3CE0`은 원본을 부른 뒤 같은 디스크립터면 해시 테이블을 보고, 없을 때 `DAT_ccb680`을 this로 `[DAT_c734d0]`(CreateTexture2D 슬롯, §4.3)을 부릅니다. `DAT_c73828==2`이면 로그 문자열은 `Interior2 `입니다.

패스 본체는 등록 이름 문자열을 다시 읽지 않습니다. `0x1521D`가 컨텍스트 vtable을 전역에 복사해 두고, 본체는 그 함수 포인터를 `DAT_ccb688`(컨텍스트)에 대해 부릅니다. 슬롯 번호는 §4.3과 같은 `ID3D11DeviceContext`입니다.

| 전역 | 슬롯 | 메서드 | 본체가 부르는 모습 |
| --- | --- | --- | --- |
| `DAT_c73538` | 11 | VSSetShader | `0x97880`, `0x9EC00` |
| `DAT_c73540` | 9 | PSSetShader | `0x97880`, `0x9EC00`, `0xA0070`. 포인터는 레지스트리 필드 |
| `DAT_c73428` | 8 | PSSetShaderResources | 직접 호출과 헬퍼 `0x13480`. 시작 슬롯 0·2·3·7·8·9·0xC·0xD·0xF |
| `DAT_c73400` | 10 | PSSetSamplers | 헬퍼 `0x13580`. 톤맵 `0xA0070`은 시작 슬롯 5 |
| `DAT_c73500` | 16 | PSSetConstantBuffers | `0x9C430` 시작 슬롯 0, `0x98970` 시작 슬롯 3 |
| `DAT_c733c8` | 33 | OMSetRenderTargets | 헬퍼 `0x13370` |
| `DAT_c73478` | 35 | OMSetBlendState | `0x9C430` |
| `DAT_c73408` | 36 | OMSetDepthStencilState | `0x97880`, `0x9EC00` |
| `DAT_c734c0` | 50 | ClearRenderTargetView | `0x9C430` |
| `DAT_c73518` | 13 | Draw | 정점 수 3. 합성·라이팅·톤맵의 풀스크린 쿼드 |

`0x98970`은 그 밖에 장치 슬롯 7 `DAT_c734e8`(CreateShaderResourceView)로 SSAO용 뷰를 만듭니다. 등록 이름(`ssao_q1`, `lighting`, `shader_postprocess_vs`)이 어느 필드에 들어가는지는 아직입니다. 본체가 집는 오프셋 예는 `0x9EC00`의 `[DAT_ccb660+0xE218]`, `0xA0070`의 객체 `+0x20`입니다.

### 10.12 세 △는 파일에 없고, 게임 exe에서 다시 계산됨

`DAT_c78000`, `DAT_c78008`, `DAT_c78010`, `DAT_c78060`, `DAT_c78068`, `DAT_c78070`과 심볼 벡터 머리 `0xCCC628`–`0xCCC6E0`, `DAT_c737b0`은 `.data` raw의 밖입니다. raw는 `0xC34E00` 앞에서 끝나고, 로더가 그 뒤를 0으로 채웁니다. 값은 실행 중에 게임 `.text`에서 계산됩니다.

`0x6B060`은 `cs_open(CS_ARCH_X86, CS_MODE_64, &DAT_c737b0)`, `cs_option(DETAIL, ON)`, `cs_option(SKIPDATA, ON)`, `cs_malloc`을 한 뒤, `cs_disasm_iter` `0xAD640`으로 `.text`를 raw 크기만큼 처음부터 끝까지 풉니다. 버퍼는 `0x6ABE0`이 펼친 이미지의 `.text` VA, 주소는 ImageBase + VA입니다. 명령마다 힙 벡터 넷을 채웁니다.

| 벡터 | 원소 | 채우는 조건 |
| --- | --- | --- |
| `DAT_ccc6a0` 심볼 | 13바이트 `{address, id, size}` (`cs_insn` `+8`, `+0`, `+0x10`) | 모든 명령 |
| `DAT_ccc658` RIP 참조 | 12바이트 `{심볼 번호, address + size + disp}` | 메모리 오퍼랜드 중 base가 `X86_REG_RIP`(41)인 첫 것 |
| `DAT_ccc670` 분기 | 12바이트 `{심볼 번호, op[0].imm}` | 니모닉이 `j` 또는 `c`로 시작하고 `op[0]`이 즉시값 |
| `DAT_ccc640` 범위 | 0x34바이트 `{start, len, end, 첫·끝 심볼, 즉시값·RIP 구간}` | 아래 |

이전 판에서 `+8`을 "디코드 바이트 누적값"이라 적은 것은 틀렸습니다. `+8`은 `cs_insn.id`이고 `0x3E`는 `X86_INS_CALL`입니다. 범위는 짧은 분기 목표가 늘린 상한을 넘은 뒤 `int3`, `ret`, 또는 `|목표 − 주소| ≥ 0x400`인 `jmp`에서 닫힙니다. `int3`이면 그 명령을 빼고, 나머지는 포함합니다.

`0xA19000`의 다섯 함수 포인터(`0x371180`, `0x3719B0`, `0x381B50`, `0x371100`, `0x6F590`)는 Capstone의 `cs_mem_malloc`·`calloc`·`realloc`·`free`·`vsnprintf`입니다. 심볼 행이 아닙니다.

`xref_resolve.py`는 이 루프와 `0x6BD80`의 선택(`0x6AFB0` 첫 히트, `0x6B750` RIP 참조, `0x6B810` 분기, `0x6B6E0` 범위, `0x6BAB0`·`0x6BB60`·`0x6B8D0` 재디코드)을 Python capstone 5.0.7로 옮긴 것입니다. 1.61.1.9에서 명령 7,948,474개, 범위 100,292개, 약 45초입니다. 검증은 두 가지입니다. 이미 ✔였던 일곱 참조 문자열(`Mapping buffer`, `cvar`, `Backbuffer`, `<undefined>`, `rfx`, `Present`, `cmd`)의 범위 시작이 §9 표의 pdata 시작과 모두 같고, uniform 합성 함수 결과가 라이브 게이트의 `0x3EB300`과 같습니다.

| 전역 | 값 (1.61.1.9) | 경로 |
| --- | --- | --- |
| `DAT_c78010` | `0x3EB300` | xref `0x3ED7B6` → RIP 목표 `0x3EBB70` → 첫 `E8` `0x3EBB84` |
| `DAT_c78068`·`DAT_c78060`·`DAT_c78070` | `0x405618`·`0x40565B`·`0x405788` | `0x1567D20`으로 가는 분기 12개 중 0·1·3번 |
| `DAT_c77ff8`·`DAT_c78000`·`DAT_c78008` | `0x1475C90`·`0x3A9`·`0x30D` | xref `0x1475FB5`의 범위, 내부 `call 0x160EE40` |

DLL이 링크한 Capstone의 정확한 5.0.x 판은 모릅니다. 마이너 판 차이로 디코드가 갈릴 수 있지만, 고르는 조건(길이 5, `E8`, `CALL`, RIP base)과 결과가 라이브·pdata와 맞으므로 이 빌드의 값으로 둡니다.

### 10.14 메뉴

프레임 시작 `0xA6900`이 `0x23330`(4210바이트, `0x23330`–`0x243A2`)을 부릅니다. 이 함수가 cfg, ImGui 초기화, 입력 프로시저 취득, 메뉴 창을 같이 둡니다.

- `DAT_c737ac==1`이면 `0x21810`과 `0x22460`을 부릅니다. 둘 다 파일 이름 `snowymoon.io_lighting_v1_0_0.cfg`를 집습니다. `0x7A560`의 끝이 `0x21810`으로 이어지고, 키는 세 벡터에 들어갑니다. int는 `DAT_c77f88`(`0x7A38D`, 원소 안의 값 dword), float는 `DAT_c77fa0`(`0x7A464`), bool은 `DAT_c77fb8`(`0x7A52E`, 값 byte)입니다.
- `0x21810`은 `0x2186A`에서 `r8d=0`으로 `0x66F10`을 부릅니다. openmode는 `0x21`이고, 모드 표가 고르는 와이드 문자열은 `0x880050`의 `rb`입니다. 읽은 줄은 `stoi`/`stof`로 라이브 포인터에 씁니다.
- `0x22460`은 `0x22C03`에서 `r8b=1`입니다. openmode는 `0x22`이고 와이드 문자열은 `0x880058`의 `wb`입니다. 스냅샷과 라이브 값이 다를 때 `=`(`0x9868A4`)와 `\n`(`0x985A50`)으로 `key=값` 한 줄을 만들어 `0x672C0`으로 덮어씁니다. 파싱은 하지 않습니다.
- 바이트 `DAT_ccdd00`이 0일 때 Win32 백엔드 `0x55150`, DX11 백엔드 `0x54F40`(문자열 `imgui_impl_dx11`), 폰트 아틀라스를 초기화합니다.
- 이미 초기화됐으면 `GetProcAddress`로 `XInputGetState`, `SetCursorPos`, `PeekMessageW`를 가져오고 직후 `0x79760`(§3의 설치 래퍼)을 부릅니다. 그 훅이 입력을 어떻게 바꾸는지, 푸는 절차는 적지 않습니다.
- 이어서 `0x276E0`이 창을 엽니다. 제목 포인터 `DAT_a3d850`은 파일에서 `'.'` 한 바이트뿐이라 보이는 제목은 △입니다.

페이지는 `DAT_c73760`입니다. 0이 아니면 `<  Go Back`을 그립니다.

| 값 | 함수 | 화면 |
| --- | --- | --- |
| 0 | `0x25440` | 홈. `LightingMethod` 콤보(Lighting A/B/C, Lighting D (Default)), `Global Quality`(High/Medium/Low), 아래 페이지 버튼, Show/Hide Key, Reset, Copy & Dump Frame Information, Website, Patreon / Blog |
| 1 | `0x26300` | Colors / Tonemap. 슬라이더 Temp, Dynamic, Pre-Exposure, Pre-Contrast, Exposure, Saturation, Contrast, Vibrance, Shadows, Highlights, Blacks, Whites, Night Exposure. 행마다 `R` |
| 2 | `0x25A40` | Anti Aliasing / Motion Blur. Disabled, Temporal Anti Aliasing, Temporal Anti Aliasing Clarity, DLAA, DLSS Quality/Balanced/Performance Rev.2, Preset C/D/F, FXAA, Sharpness, Sharpen Edges, Motion Blur, Intensity |
| 3 | `0x26D10` | Rendering / Lighting. Distance Screen Space Shadows, Low Resolution SSAO, SSAO Soft/SSAO/SSAO Sharp, Details High/Low, SSAO Intensity, Interior Intensity, Interior Lighting, Default Mirrors, Default Rain |
| 4 | `0x27250` | Objects. 하위 인덱스는 `DAT_c7375c`. Surface의 Albedo Saturation, Roads의 Default Normals와 Normals Intensity, Vegetation의 Leaves/Grass Thickness |

`0x7A560`이 등록한 키 문자열을 이 구간이 직접 집는 것은 `LightingMethod` 하나입니다. 화면 글자는 `## Exposure` 같은 별도 라벨이고, `0x26230`이 `##` 두 바이트를 건너뛴 뒤 `%.2f` 슬라이더를 만듭니다. 테이블 id는 `globalsettings1`, `tonemapsettings`, `ssaosettings`, `ssaosettings1`, `lightingsettings`, `surfacesettings`, `roadssettings`, `vegetationsettings`입니다.

`DAT_c736b8`이 0이 아니면 `Critical error %d! Everything disabled.`이고, 값이 `0xC3B4`이면 `Please disable MSAA.`가 붙습니다. `DAT_c737ac`이 1이 아닌 분기에는 연결 오류, 버전, 게임 버전, 처리 오류, 메모리, 컴퓨터 변경, 에셋 없음(`snowymoon_lighting` 폴더를 `bin\win_x64`에 두라는 문구), `Invalid license!`가 있습니다. 비교 루틴은 열지 않았습니다.

### 10.15 여기서 멈춤

2026-10-22. 효과 한 프레임의 경로는 닫혔습니다. 아래를 다시 디스어셈블할 필요는 없습니다. §10.10과 §10.13 번호는 비어 있고, 그 내용은 §10.11 이후와 §10.14에 들어 있습니다.

한 줄로 보면 이렇습니다. 창 클래스가 `prism3d`이면 `0xA6B20`이 한 번 돕니다. 순서는 키 등록 `0x7A560`, 게임 훅 `0x1B090`, 장치·컨텍스트 훅 `0x14F70`, 셰이더 등록 `0x8CC00`→`0x86660`, NGX `0x6F310`입니다. 첫 Present의 `0xA6900`이 cfg와 메뉴 `0x23330`을 엽니다. `0x17460`이 Draw 네 종류와 `OMSetRenderTargets`, `GenerateMips`를 가로챕니다. `0x9F7B0`이 RT 스냅샷으로 패스를 고르고, 조건이 맞으면 원본 드로우를 삼킵니다. 패스 본체는 이름 문자열을 다시 읽지 않고, `0x1521D`가 복사해 둔 컨텍스트 원본으로 셰이더·SRV·샘플러를 묶은 뒤 정점 3개 쿼드를 그립니다(§10.11).

이미 닫힌 것:

- 1.61 훅과 공존 판정은 §4·§6·§9. 세 △(uniform 합성 함수 홉, `persistent_drawable` 세 주소, 장면 복사 길이)는 DLL 파일에 없지만 `xref_resolve.py`로 다시 계산해 닫았습니다(§10.12).
- 메뉴 다섯 페이지와 cfg. `0x21810`이 `rb`로 읽고, `0x22460`이 `key=값`을 `wb`로 씁니다(§10.14).
- 스위치 세 개는 Capstone 5의 SPARC·PowerPC·SystemZ 모듈이고 실행되지 않습니다(§10.7, §10.16).

일부러 열지 않은 것: 라이선스 비교, 입력 훅이 값을 어떻게 바꾸는지, 셰이더 바이트, `.vlizer`.

§10.15를 쓸 때 남겨 둔 두 질문(등록 이름 → 레지스트리 필드, 0x58바이트 객체의 함수 포인터)은 §10.16에서 답했습니다.

### 10.16 셰이더 슬롯과 Capstone

2026-10-22. §10.15의 두 질문에 대한 답입니다.

**등록 이름 → 슬롯(✔).** `0x86660(&DAT_cca710)`의 각 등록은 `FUN(&DAT_ccb680 레지스트리, 슬롯, 이름 디스크립터, 진입점, 디파인)`입니다. 이름은 디스크립터를 고를 뿐 슬롯에 저장되지 않습니다. 디스크립터의 `+0x40`이 0이면 `+0x20`/`+0x28`의 바이트코드를 `0x39`로 XOR해 쓰고, 아니면 `+0x30` 문자열을 디파인과 함께 컴파일합니다. 슬롯은 0x38바이트입니다.

| 등록 함수 | 슬롯에 남는 것 |
| --- | --- |
| `0xFD40` PS | `ID3D11PixelShader*`를 슬롯 `+0x20` |
| `0xFA80` VS | VS를 `+0`, 바이트코드 벡터를 `+8`, 입력 레이아웃을 `+0x30` |
| `0xFF00` | `pbr_brdf`, `pbr_ir_ref1/2`, `pbr_sp_ref1/2` 다섯 개. 본문은 안 열었음 |

슬롯 주소는 `0xCCA710 + 오프셋`입니다. 그리기 본체는 `lea reg, [슬롯]` 뒤 `+0x20`을 읽거나, PS 포인터 주소를 바로 읽습니다.

| 그리기 본체 (§10.11) | 슬롯 (이름) |
| --- | --- |
| `0x98970` SSAO (상태 1) | `0xCCB190` `ssao`, `0xCCB1C8` `ssao_q1`, `0xCCB200`–`0xCCB3C0` `ssaoblur1/2`·`ssaotaa`와 cabin·sharp 변종, `0xCCB5B8` `half_depth` |
| `0x9A640` 라이팅 (`0x9C430`·`0x9BD20`이 부름) | `0xCCA898`–`0xCCADD8` `lighting`·`lightingcabin`·`lightingmirror`·`lightingref`·`lightingrefcabin` 1–5세대, `0xCCAF28`–`0xCCB008` `lightingadd`–`lightingadd5`, `0xCCB628` `shader_fog_vs` |
| `0x9C430` 라이팅 반환 | `0xCCA908` `lightingmirror`, `0xCCB580` `dummynormals`, `0xCCB5F0` `gbuffergen2` |
| `0x9EC00` 합성 (상태 2) | `0xCCAE30` `lightingcombine` PS, `0xCCAE68` `lightingcombine2` PS를 직접 읽음 |
| `0xA0070` 톤맵 | `0xCCB040`–`0xCCB120` `tonemap`–`tonemap5` |
| `0xA1960` resize·AA | `0xCCA710` `taa`, `0xCCA780` `taamasked`, `0xCCA748` `taamask`, `0xCCA7B8` `fxaa`, `0xCCA7F0` `sharpen`, `0xCCA828` `sharpenmasked`, `0xCCA860` `motionblur`, `0xCCB3F8` `sky_velocity` |
| `0x9E7C0` (`0x9F7B0`이 부름) | `0xCCA710` `taa` |

`shader_postprocess_vs`의 슬롯은 `DAT_c76750`이고, 그리기 쪽에서 이 주소를 직접 집는 곳은 `0xEE60` 두 곳과 pdata 밖 한 곳뿐입니다. 본체가 VS를 어디서 가져오는지는 △입니다.

**`[DAT_ccb660+0xE218]`(✔).** `DAT_ccb660` = `DAT_cca710 + 0xF50`은 `gameshaders_*`용 힙 배열의 포인터입니다. `gameshaders_15N_K`는 배열 `K * 0x38`에 등록되므로, 접미 `K`가 곧 인덱스입니다. 1.61은 `K` = 300·301·302(`PASS_foliage2/3/4`)와 1033(`PASS_raindrop`)이고, 1.53–1.55만 100–105(`PASS_road1/2`, `PASS_roadflat1/2/3`)가 더 있습니다. `gameshaders_1_1`은 `K` = 1(`PASS_unkowntransparent`)입니다. `+0xE218`은 `1033 * 0x38 + 0x20`, 곧 빗방울 PS입니다. Base Pass 반환 `0x97880`은 바인드된 PS(`DAT_c76838`)를 `0x189D0`으로 프록시 객체로 바꾼 뒤 `+0x248`의 id로 `[DAT_ccb660 + id * 0x38 + 0x20]`을 `PSSetShader`로 끼웁니다. id 100–199(도로)는 `switch`를 거치는 별도 가지이고, 그 밖의 id > 1(1.61의 300–302, 1033)은 슬롯이 비어 있지 않을 때 바로 바꿉니다. 프록시가 id를 매기는 방법은 안 열었습니다.

**`DAT_c76740 * 0x68 + 0x408`(✔, 셰이더 아님).** `DAT_c76740`은 `0x8EC20`이 프레임마다 `DAT_c288d8 & 1`로 두는 짝홀 인덱스이고, 기준 `DAT_c76748`은 Base Pass `0x951F0`이 씁니다. 프레임 짝홀로 갈리는 0x68바이트 묶음이라 히스토리 RT·SRV 쪽이고, 등록 이름과는 무관합니다.

**0x58바이트 객체와 함수 포인터(✔).** §10.7 끝 문단입니다. Capstone 5의 `*_global_init`이고, 함수 포인터는 핸들 `cs_struct`에 들어가며, 부르는 것은 `cs_disasm_iter`뿐입니다. `cs_open`은 `0x6B060`의 x86-64 한 번이므로 SPARC·PowerPC·SystemZ 모듈은 링크만 되어 있습니다. 이 사실 덕분에 §10.12의 참조 문자열 선택을 Python capstone으로 재현할 수 있었습니다.

이 파일에서 남은 일은 없습니다. 더 열 수 있는 것은 `0xFF00`의 슬롯 배치, 프록시가 게임 PS에 id를 매기는 방법, `shader_postprocess_vs`를 본체가 바인드하는 경로이고, 셋 다 공존 판정과 무관합니다.

도구를 다시 쓸 때: Ghidra는 프로젝트 락이 있으니 한 프로세스만. `py -3 research/snowymoon/sm.py`는 capstone이라 `-I` 없이. `sm.py xref`는 REX 때문에 한 바이트 어긋날 수 있어, RIP 상대는 capstone의 `X86_OP_MEM`으로 확인합니다. pdata는 끝이 다음 시작과 맞닿는다고 한 함수로 붙이지 않습니다. `0xCD2B0`의 실제 구간은 `0xB5`바이트입니다.
