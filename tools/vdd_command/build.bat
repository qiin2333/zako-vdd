@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /W4 /O2 /DUNICODE /D_UNICODE /Fo:build\ /Fe:build\vdd_command.exe ^
   vdd_command.cpp setupapi.lib user32.lib
exit /b %ERRORLEVEL%
