@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /W4 /O2 /DUNICODE /D_UNICODE /Fo:build\ /Fe:build\desktop_hdr_capture.exe ^
   desktop_hdr_capture.cpp d3d11.lib dxgi.lib
exit /b %ERRORLEVEL%
