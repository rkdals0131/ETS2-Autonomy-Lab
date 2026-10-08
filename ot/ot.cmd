@echo off
setlocal
set "PYTHONPATH=%~dp0py;%PYTHONPATH%"
py -3.13 -m otpy %*
exit /b %ERRORLEVEL%
