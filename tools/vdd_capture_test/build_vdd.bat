@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars64 failed & exit /b 1)
cd /d "c:\Users\mohaha\Program\github\Virtual-Display-Driver\Virtual Display Driver (HDR)"
msbuild ZakoVDD.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
exit /b %ERRORLEVEL%
