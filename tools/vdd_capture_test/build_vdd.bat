@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars64 failed & exit /b 1)
pushd "%~dp0..\.."
set "VERSION_ARG="
if defined DRIVER_PACKAGE_VERSION set "VERSION_ARG=/p:DriverPackageVersion=%DRIVER_PACKAGE_VERSION%"
msbuild ZakoVDD.sln /p:Configuration=Release /p:Platform=x64 %VERSION_ARG% /m /v:minimal /nologo
set "BUILD_EXIT=%ERRORLEVEL%"
popd
exit /b %BUILD_EXIT%
