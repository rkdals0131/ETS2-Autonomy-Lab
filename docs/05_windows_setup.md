# Windows 개발 환경 준비

초기에는 게임과 C++ 플러그인, Python 실험 프로세스를 Windows에서 함께 실행합니다. 아래는 준비 순서이며, **현재 설치 확인 결과는 [설치 분석](10_installed_game_static_analysis.md)이 우선**합니다. 게임·MSVC·Python과 조사용 SDK/Extractor는 확보했고, 자체 플러그인 빌드와 게임 실행은 아직 수행하지 않았습니다.

## 우선 준비할 것

| 도구 | 목적 | 시점 |
| --- | --- | --- |
| Steam과 ETS2 | 실제 게임 빌드 확보 | 첫 실행 전 |
| Git for Windows | 소스와 revision 관리 | 소스를 가져올 때 |
| Visual Studio 또는 Build Tools | MSVC x64 컴파일러 | 플러그인 빌드 전 |
| Desktop development with C++와 Windows SDK | 네이티브 DLL과 D3D 개발 | 위 설치에 포함 |
| CMake | 플러그인 빌드 구성 | 선택한 저장소 요구사항 확인 후 |
| Python x64와 venv | 데이터·학습·제어 실험 | 외부 프로세스 구현 전 |
| VS Code | 문서·코드 편집 | 선택 |
| Foxglove Windows 앱 | 센서와 상태 보기 | 첫 출력 연결 전 |

Visual Studio 제품 버전, Python minor 버전과 CUDA 패키지는 선택한 저장소·모델의 지원 조합에 맞춥니다. 모델을 정하지 않은 지금 모든 프레임워크를 한꺼번에 설치할 이유는 없습니다. PyTorch 설치 명령은 [공식 선택기](https://pytorch.org/get-started/locally/)에서 Windows·Python·지원 CUDA 조합을 확인한 뒤 사용합니다.

현재 ETS2LA 자체를 빌드하려면 README의 .NET 10·Bun 지침을 별도로 따릅니다. 이것은 자체 C++ 센서 플러그인의 필수 의존성이라는 뜻은 아닙니다. [ETS2LA 개발 안내](https://github.com/ETS2LA/ETS2LA)

## 역공학 단계에서 준비할 것

- SCS Telemetry & Input SDK: 공식 헤더와 예제.
- SCS Game Archive Extractor: 게임 데이터·카메라·재질 정의 추출.
- RenderDoc: 한 프레임의 렌더 타깃, depth, 상수 버퍼, 호출 스택 조사.
- x64dbg와 Ghidra: Windows 실행 파일의 함수와 객체 경로 분석.
- SCS Blender Tools와 Conversion Tools: 로케이터·차량 모델을 실제 수정해야 할 때만.
- NVIDIA 개발 도구: 복사·추론 병목이 확인된 뒤 필요한 도구만.

[SCS 도구 목록](https://modding.scssoft.com/wiki/Documentation/Tools)에서 설치 게임 버전에 맞는 Extractor를 선택합니다. 1.55+ 아카이브에는 그 형식을 지원하는 Extractor가 필요하다는 공식 안내가 있습니다. [Extractor 설명](https://modding.scssoft.com/wiki/Documentation/Tools/Game_Archive_Extractor)

RenderDoc·후킹 플러그인·다른 오버레이를 한 번에 모두 넣지 않습니다. 처음에는 게임 단독에서 캡처가 되는지 확인하고 필요한 구성만 추가합니다. 문제가 생기면 무엇을 추가했을 때 바뀌었는지 바로 알 수 있습니다.

## 경로

요청된 문서 경로는 다음과 같습니다.

~~~text
C:\Users\kikiw\Desktop\ETS2-Autonomy-Lab
~~~

현재 프로젝트는 위 경로에서 직접 작업합니다. 게임 경로는 `D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2`로 확인했습니다. 별도 C:\Dev 경로는 생성하지 않았습니다.

게임 사용자 데이터도 실제 Documents 위치와 실행 옵션을 확인합니다. OneDrive나 사용자 경로 변경 때문에 표시 경로가 다를 수 있습니다. 연구용 별도 게임 프로필을 만들어 기존 운송 세이브와 분리하는 것이 작업 기본안입니다.

게임 아카이브는 원본 폴더에 덮어쓰지 않고 별도 출력 경로에 추출합니다. 공식 명령 형식은 다음과 같습니다.

~~~text
scs_extractor archive.scs [output_path]
~~~

처음부터 전체 아카이브를 대량 추출하기 전에 대상 파일과 출력 크기를 확인합니다. 추출본·GPU 캡처·동영상·학습 데이터의 실제 용량은 문서보다 훨씬 큽니다.

## 대용량 저장소

Windows의 지속적인 기록 경로는 아직 정하지 않았습니다. 실제 SSD의 드라이브 문자, 여유 공간, 지속 쓰기 속도를 확인한 뒤 실험별로 나눕니다. 예를 들어 별도 SSD에 프로젝트·run·역할별 디렉터리를 둡니다. 존재하지 않는 D: 드라이브를 가정해 명령을 실행하지 않습니다.

나중에 ROS 작업공간을 사용해 대용량 산출물을 만들면 기존 사용자 지침의 ~/Storage/ROS2_Workspace_offload를 따라야 합니다. Windows에서 그 외장 SSD가 어느 경로인지 먼저 확인합니다. 해당 저장소가 없으면 다른 디스크나 /tmp로 자동 대체하지 않습니다.

## Windows에서 확인할 시스템 정보

다음 PowerShell 명령은 준비 정보만 읽습니다. OS·CPU·RAM·GPU·Python·VS 설치 정보는 실제 조회했고 결과는 설치 분석에 기록했습니다. PATH의 `python`이 2.7이므로 명시적으로 `py -3.13`을 사용합니다.

~~~powershell
Get-CimInstance Win32_OperatingSystem |
    Select-Object Caption, Version, BuildNumber
Get-CimInstance Win32_Processor |
    Select-Object Name
Get-CimInstance Win32_ComputerSystem |
    Select-Object TotalPhysicalMemory
Get-Volume |
    Select-Object DriveLetter, FileSystemLabel, SizeRemaining, Size
nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv
git --version
cmake --version
py -0p
~~~

nvidia-smi가 PATH에 없다면 NVIDIA 설치 위치 또는 드라이버 도구에서 확인합니다. 명령이 없다는 이유로 GPU가 없다고 판단하지 않습니다.

Python 가상환경을 만들 때는 먼저 설치된 버전을 선택합니다. 이후 일반적인 형식은 다음과 같습니다.

~~~powershell
py -m venv .venv
.\.venv\Scripts\python.exe -m pip --version
~~~

위 명령은 프로젝트 경로를 현재 디렉터리로 잡은 상태를 전제로 합니다. 본 문서 작성 중에는 실행하지 않았습니다.

## 게임 빌드와 의존성

처음 기록할 값은 게임의 전체 빌드 문자열, 렌더 API, 드라이버, SDK 버전, 선택한 플러그인 commit입니다. 최신 빌드라는 이유만으로 오래된 패턴 스캔 코드가 동작한다고 가정하지 않습니다.

DX11을 초기 조사 후보로 삼되 설치 빌드의 실행 옵션과 실제 로그로 선택 결과를 확인합니다. 기존 저장소가 지원하지 않으면 버전 검사를 지워 강행하지 말고, 지원 빌드를 선택하거나 해당 빌드에 맞는 구조를 조사합니다.

가져온 외부 코드·패키지는 필요한 revision이나 배포 식별을 취득 시 한 번 확인합니다. 같은 파일을 매번 해시하는 검증 루프는 만들지 않습니다. 코드를 실제 편입하거나 배포할 때에는 선택한 revision의 라이선스 조건을 읽습니다.

## 첫 실행 전 완료할 준비

- 연구용 프로필과 원래 세이브의 분리 방식을 정합니다.
- 입력 장치와 수동 해제 방법을 정합니다.
- 디지털미러가 있는 차량의 접근 가능성을 확인합니다.
- GPU 캡처와 기록을 저장할 SSD 경로를 정합니다.
- 사용할 게임 빌드·렌더 API와 첫 플러그인 조합을 좁힙니다.
- 실제 조향을 주입하기 전에 영상과 상태가 나오는지 확인합니다.

DLC나 추가 모니터의 구매는 현재 센서 경로 조사에 필수라고 확인되지 않았습니다. 먼저 사용할 기본 차량과 실제 설치 빌드에서 가능한 경로를 봅니다.
