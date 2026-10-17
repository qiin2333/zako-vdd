@echo off
setlocal
if not exist "%~dp0build" mkdir "%~dp0build"
cl /nologo /std:c++17 /EHsc /W4 /WX "%~dp0edid_test.cpp" /Fo"%~dp0build\edid_test.obj" /Fe"%~dp0build\edid_test.exe"
if errorlevel 1 exit /b 1
"%~dp0build\edid_test.exe" "%~dp0build"
exit /b %ERRORLEVEL%
