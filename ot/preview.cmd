@echo off
call "%~dp0ot.cmd" preview --config "%~dp0presets\phase1-highway.json" --format rgbd8 %*
exit /b %ERRORLEVEL%
