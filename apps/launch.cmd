@echo off
setlocal
rem Opens the launcher in the pixi ROS 2 environment (pixi.toml).
rem --host-only [config] [stop-event] runs the relay node alone in this console.
where pixi >nul 2>nul || (echo pixi not found: winget install prefix-dev.pixi, then pixi run build & exit /b 1)
pushd "%~dp0.."
if "%~1"=="--host-only" goto host
pixi run launcher
set "CODE=%errorlevel%"
popd
exit /b %CODE%

:host
set "CONFIG=%~2"
if "%CONFIG%"=="" set "CONFIG=bridge\config\bridge.local.json"
if "%~3"=="" (pixi run relay "%CONFIG%") else (pixi run relay "%CONFIG%" "%~3")
set "CODE=%errorlevel%"
popd
exit /b %CODE%
