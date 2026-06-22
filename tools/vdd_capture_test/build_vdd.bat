@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars64 failed & exit /b 1)
pushd "%~dp0..\.."
msbuild ZakoVDD.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
set "BUILD_EXIT=%ERRORLEVEL%"
popd
exit /b %BUILD_EXIT%
