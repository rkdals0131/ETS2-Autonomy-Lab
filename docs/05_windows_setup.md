# 개발 환경

현재 환경은 Windows 11, Ryzen 7 3700X, RTX 3060 Ti 8 GiB, RAM 약 23.95 GiB입니다. 게임과 DLL·릴레이는 Windows, ROS 2 Jazzy는 WSL2 Ubuntu에서 실행합니다.

## Windows

- ETS2 1.61.1.1 / Steam build 25482642 / DX11 x64
- MSVC x64 Build Tools, CMake·Ninja
- 공식 SCS SDK와 Python 3.13, ot/.venv
- Windows Foxglove
- 렌더 분석 시 RenderDoc·SCS extractor·ConverterPIX

게임 루트는 Steam 라이브러리의 `steamapps/common/Euro Truck Simulator 2`입니다. CMake SCS_SDK·ETS2_EXE와 build.cmd의 Build Tools 경로를 설치 환경에 맞춥니다.

## WSL과 실행

Ubuntu 24.04 / ROS 2 Jazzy를 `~/ets2-ros`에 빌드합니다. `bridge/ros-env.sh`가 ROS domain 42·Fast DDS SHM·Bash/Zsh 환경을 설정합니다. [설치 명령과 런처](../bridge/README.md).

WSL systemd user manager가 실행돼 있어야 런처가 ROS·Foxglove 소유 그룹을 관리할 수 있습니다. Windows와 WSL은 `bridge/config/bridge.local.json`을 공유합니다. 연결 주소는 Ubuntu eth0에서 조회합니다.

## 저장소

대용량 ROS 결과는 외장 SSD의 `~/Storage/ROS2_Workspace_offload/ETS2-Autonomy-Lab/`에 실험·역할별로 저장합니다. 쓰기 전 findmnt·df로 마운트와 여유 공간을 확인합니다. 저장소 소스에는 코드·문서·작은 설명 이미지와 라이선스를 보관합니다.

[DLL 빌드·설치](../ot/README.md), [장치·게임 분석](10_installed_game_static_analysis.md).
