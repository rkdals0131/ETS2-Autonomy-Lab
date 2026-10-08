@echo off
setlocal
chcp 65001 >nul
set "OT_ROOT=%~dp0"
set "OT_VS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
if not exist "%OT_VS%\VC\Auxiliary\Build\vcvars64.bat" (
  echo MSVC x64 Build Tools not found at "%OT_VS%"
  exit /b 1
)
call "%OT_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "VSLANG=1033"
set "OT_CMAKE=%OT_ROOT%.tools\cmake\data\bin\cmake.exe"
set "OT_NINJA=%OT_ROOT%.tools\bin\ninja.exe"
if not exist "%OT_CMAKE%" (
  echo Run: py -3.13 -m pip install --target "%OT_ROOT%.tools" cmake==4.1.3 ninja==1.13.0
  exit /b 1
)
"%OT_CMAKE%" -S "%OT_ROOT%." -B "%OT_ROOT%build" -G Ninja -DCMAKE_MAKE_PROGRAM="%OT_NINJA%" -DCMAKE_BUILD_TYPE=RelWithDebInfo %*
if errorlevel 1 exit /b 1
"%OT_CMAKE%" --build "%OT_ROOT%build" --parallel 2
exit /b %ERRORLEVEL%
