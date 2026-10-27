@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0prepare.ps1" %*
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /DPR45=1 /I"%~dp0build" "%~dp0mode_refresh_test.cpp" /Fo"%~dp0build\mode_refresh_test.obj" /Fe"%~dp0build\mode_refresh_test.exe" ole32.lib
if errorlevel 1 exit /b 1
"%~dp0build\mode_refresh_test.exe"
exit /b %ERRORLEVEL%
