@echo off
setlocal
set "BRIDGE_ROOT=%~dp0"
rem OT_VS may be preset; otherwise use the newest Visual Studio install with x64 C++ tools.
set "OT_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined OT_VS if exist "%OT_VSWHERE%" for /f "usebackq delims=" %%i in (`"%OT_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "OT_VS=%%i"
if not exist "%OT_VS%\VC\Auxiliary\Build\vcvars64.bat" (
  echo MSVC x64 Build Tools not found. Install them or set OT_VS to the Visual Studio directory.
  exit /b 1
)
call "%OT_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "VSLANG=1033"
set "BRIDGE_CMAKE=%BRIDGE_ROOT%..\ot\.tools\cmake\data\bin\cmake.exe"
if not exist "%BRIDGE_CMAKE%" (
  echo Run ot\build.cmd once; it explains how to install the local CMake and Ninja.
  exit /b 1
)
"%BRIDGE_CMAKE%" -S "%BRIDGE_ROOT%." -B "%BRIDGE_ROOT%build" -G Ninja -DCMAKE_MAKE_PROGRAM="%BRIDGE_ROOT%..\ot\.tools\bin\ninja.exe" -DCMAKE_BUILD_TYPE=RelWithDebInfo %*
if errorlevel 1 exit /b 1
"%BRIDGE_CMAKE%" --build "%BRIDGE_ROOT%build" --parallel 2
exit /b %ERRORLEVEL%
