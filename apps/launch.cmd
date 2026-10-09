@echo off
setlocal
if "%~1"=="--host-only" goto host
if exist "%~dp0..\ot\.venv\Scripts\pythonw.exe" (
  start "" "%~dp0..\ot\.venv\Scripts\pythonw.exe" "%~dp0launcher\launcher.py" %*
) else (
  start "" pyw -3.13 "%~dp0launcher\launcher.py" %*
)
exit /b

:host
if "%~2"=="" (
  "%~dp0..\bridge\dist\ets2_relay.exe" "%~dp0..\bridge\config\bridge.local.json"
) else (
  if "%~3"=="" (
    "%~dp0..\bridge\dist\ets2_relay.exe" "%~2"
  ) else (
    "%~dp0..\bridge\dist\ets2_relay.exe" "%~2" "%~3"
  )
)
exit /b %errorlevel%
