@echo off
rem Build the standalone Vulkan HDR diagnostic probe using x64 MSVC.
rem No Vulkan SDK is required; Vulkan is loaded dynamically at runtime.
setlocal
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /W4 /O2 /DUNICODE /D_UNICODE /Fo:build\ /Fe:build\vulkan_hdr_probe.exe ^
   vulkan_hdr_probe.cpp ^
   user32.lib advapi32.lib
exit /b %ERRORLEVEL%
