@echo off
rem pixi activation (pixi.toml). The overlay exists only after `pixi run build`.
if exist "%~dp0install\setup.bat" call "%~dp0install\setup.bat"
set "FASTRTPS_DEFAULT_PROFILES_FILE=%~dp0src\ets2_bridge\config\fastdds.xml"
rem RoboStack's win-64 foxglove-bridge (3.4.3 and 3.6.0) installs foxglove.dll and
rem foxglove_cpp_shared.dll in Library\lib, off PATH (exit 0xC0000135). Appended last;
rem that directory holds only these two DLLs.
set "PATH=%PATH%;%CONDA_PREFIX%\Library\lib"
