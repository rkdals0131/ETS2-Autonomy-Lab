@echo off
rem pixi activation (pixi.toml). The overlay exists only after `pixi run build`.
if exist "%~dp0install\setup.bat" call "%~dp0install\setup.bat"
set "FASTRTPS_DEFAULT_PROFILES_FILE=%~dp0src\ets2_bridge\config\fastdds.xml"
