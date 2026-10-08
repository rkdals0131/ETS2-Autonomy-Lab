@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "VSLANG=1033"
set "BRIDGE_ROOT=%~dp0"
set "BRIDGE_CMAKE=%BRIDGE_ROOT%..\ot\.tools\cmake\data\bin\cmake.exe"
"%BRIDGE_CMAKE%" -S "%BRIDGE_ROOT%." -B "%BRIDGE_ROOT%build" -G Ninja -DCMAKE_MAKE_PROGRAM="%BRIDGE_ROOT%..\ot\.tools\bin\ninja.exe" -DCMAKE_BUILD_TYPE=RelWithDebInfo %*
if errorlevel 1 exit /b 1
"%BRIDGE_CMAKE%" --build "%BRIDGE_ROOT%build" --parallel 2
exit /b %ERRORLEVEL%
