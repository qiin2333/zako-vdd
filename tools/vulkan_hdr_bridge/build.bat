@echo off
rem Experimental bridge build. Do not register system-wide before probe validation.
setlocal
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /W4 /O2 /LD /DUNICODE /D_UNICODE /Fo:build\ /Fe:build\zako_vulkan_hdr_bridge.dll ^
   vulkan_hdr_bridge.cpp ^
   user32.lib
if errorlevel 1 exit /b %ERRORLEVEL%
copy /y VkLayer_zako_virtual_hdr.json build\VkLayer_zako_virtual_hdr.json >nul
exit /b 0
