@echo off
setlocal
if exist "%~dp0..\ot\.venv\Scripts\pythonw.exe" (
  start "" "%~dp0..\ot\.venv\Scripts\pythonw.exe" "%~dp0launcher.py" %*
) else (
  start "" pyw -3.13 "%~dp0launcher.py" %*
)
