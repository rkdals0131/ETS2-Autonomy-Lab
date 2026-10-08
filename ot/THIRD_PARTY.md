# 빌드 입력과 라이선스

- **SCS Telemetry SDK**: 프로젝트의 `research/sdk/include` 사용. SDK 원본과 라이선스 안내는 `research/sdk`에 보존되어 있습니다. 플러그인 진입·채널·이벤트 계약의 근거입니다. [공식 SDK 페이지](https://modding.scssoft.com/wiki/Documentation/Engine/SDK).
- **nlohmann/json 3.12.0**: MIT. `vendor/nlohmann/json.hpp`, `vendor/nlohmann/LICENSE.MIT`. 공식 tag의 commit `55f93686c01528224f448c19128836e7df245f72`에서 받았습니다. [고정 소스](https://github.com/nlohmann/json/tree/55f93686c01528224f448c19128836e7df245f72).
- **CMake 4.1.3 / Ninja 1.13.0**: `ot/.tools`에 설치한 빌드 도구. DLL 런타임에 포함되지 않습니다. [CMake](https://cmake.org/), [Ninja](https://ninja-build.org/).
- **MSVC / Windows SDK**: 이 PC의 Visual Studio Build Tools와 Windows SDK를 사용합니다. CNG, named pipe, file mapping, interlocked, DbgHelp API는 Windows 제공 기능입니다.
- **SafetyHook 0.7.0 + bundled Zydis/Zycore**: `vendor/safetyhook`의 공식 amalgamated 배포본. SafetyHook은 BSL-1.0, Zydis/Zycore는 파일에 보존한 MIT 저작권·라이선스를 따릅니다. [고정 소스](https://github.com/cursey/safetyhook/tree/19223663fb8a573253ffb2e82da87cc354bf5c16), [릴리스](https://github.com/cursey/safetyhook/releases/tag/v0.7.0). ZIP 취득 시 GitHub asset digest `505d4c07ec1c5b94a17f3906ca86afbe1264e738d8becaa244866694c6200c2c`와 SHA256을 한 번 대조했습니다. C++23이 필요합니다. 로컬 변경은 `MidHook::stub()`·`trampoline()` 읽기 전용 접근자 두 개이며, 언로드 전 아직 실행 중인 코드 범위를 확인하기 위한 것입니다. Hook 구현과 assembly는 변경하지 않았습니다.

MinHook, Dear ImGui, RenderDoc은 DLL에 포함하지 않았습니다. 게임 EXE와 추출 게임 자산도 배포물에 포함하지 않습니다.

Python의 무손실 기록에는 선택 의존성 **python-zstandard 0.25.0 / BSD-3-Clause**를 사용합니다. [공식 패키지](https://pypi.org/project/zstandard/0.25.0/), [stream API](https://python-zstandard.readthedocs.io/en/latest/compressor.html). Python 3.13 Windows x64 wheel의 SHA256을 `requirements-recording.txt`에 고정하고 취득 시 pip `--require-hashes`로 확인했습니다. 설치는 로컬 `ot/.venv`에만 있으며 wheel·가상환경은 Git에 넣지 않습니다. 배포 라이선스는 설치된 `zstandard-0.25.0.dist-info/licenses/LICENSE`에 보존됩니다. DLL에는 이 라이브러리를 연결하지 않습니다.
