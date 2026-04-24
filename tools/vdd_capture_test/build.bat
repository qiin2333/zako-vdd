@echo off
rem Build vdd_capture_test using x64 MSVC.
rem Run from "x64 Native Tools Command Prompt for VS 2022".
setlocal
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /W3 /O2 /Fo:build\ /Fe:build\vdd_capture_test.exe ^
   vdd_capture_test.cpp ^
   d3d11.lib dxgi.lib dxguid.lib
endlocal
