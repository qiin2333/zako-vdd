@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /W4 /O2 /Fo:build\ /Fe:build\vulkan_hdr_policy_test.exe vulkan_hdr_policy_test.cpp
if errorlevel 1 exit /b %ERRORLEVEL%
build\vulkan_hdr_policy_test.exe
exit /b %ERRORLEVEL%
