@echo off
setlocal
rem Builds bridge\build\lateral_capi\lateral_capi.dll from the production lateral controller headers.
set "ROOT=%~dp0..\.."
set "OT_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined OT_VS if exist "%OT_VSWHERE%" for /f "usebackq delims=" %%i in (`"%OT_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "OT_VS=%%i"
if not exist "%OT_VS%\VC\Auxiliary\Build\vcvars64.bat" (
  echo MSVC x64 Build Tools not found. Set OT_VS to the Visual Studio directory.
  exit /b 1
)
call "%OT_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "OUT=%ROOT%\bridge\build\lateral_capi"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /LD /O2 /std:c++20 /EHsc /W4 /utf-8 /I "%ROOT%\ros2\src\ets2_autonomy\src" /I "%ROOT%\ot\vendor" ^
  "%~dp0lateral_capi.cpp" /Fo"%OUT%\\" /Fe"%OUT%\lateral_capi.dll" /link /NOLOGO
exit /b %ERRORLEVEL%
