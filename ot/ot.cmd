@echo off
setlocal
set "PYTHONPATH=%~dp0py;%PYTHONPATH%"
if exist "%~dp0.venv\Scripts\python.exe" (
  "%~dp0.venv\Scripts\python.exe" -m otpy %*
) else (
  py -3.13 -m otpy %*
)
exit /b %ERRORLEVEL%
