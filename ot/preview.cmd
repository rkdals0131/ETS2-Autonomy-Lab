@echo off
call "%~dp0ot.cmd" preview --config "%~dp0presets\surround-preview.json" %*
exit /b %ERRORLEVEL%
