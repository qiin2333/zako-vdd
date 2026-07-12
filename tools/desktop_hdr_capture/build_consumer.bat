@echo off
setlocal
set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL set "VSINSTALL=C:\Program Files\Microsoft Visual Studio\2022\Community"
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
call "%~dp0build.bat"
exit /b %ERRORLEVEL%
