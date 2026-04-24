@echo off
rem ==========================================================================
rem ZakoVDD driver deploy + test pipeline (admin required)
rem
rem 1. Stop any running VDD instance so the new DLL/INF can be picked up
rem 2. Install/Update certificate to TrustedPublisher + Root
rem 3. Add (or update) the driver via pnputil
rem 4. Open Device Manager / instructions for creating a virtual monitor
rem 5. Run vdd_capture_test against monitor 0
rem ==========================================================================
setlocal
net session >nul 2>&1
if errorlevel 1 (
  echo [ERROR] Run as Administrator.
  exit /b 1
)

set "VDD_BIN=c:\Users\mohaha\Program\github\Virtual-Display-Driver\Virtual Display Driver (HDR)\x64\Release"
set "TEST_DIR=%~dp0"
set "OUT_DIR=%TEMP%\vdd_capture_test_out"

if not exist "%VDD_BIN%\ZakoVDD.inf" (
  echo [ERROR] Driver build not found: %VDD_BIN%\ZakoVDD.inf
  exit /b 1
)
if not exist "%TEST_DIR%build\vdd_capture_test.exe" (
  echo [ERROR] Consumer not built: %TEST_DIR%build\vdd_capture_test.exe
  exit /b 1
)

echo === Trusting test cert ===
certutil -addstore -f "TrustedPublisher" "%VDD_BIN%\ZakoVDD.cer" >nul
certutil -addstore -f "Root"             "%VDD_BIN%\ZakoVDD.cer" >nul

echo === Adding driver (pnputil) ===
pnputil /add-driver "%VDD_BIN%\ZakoVDD.inf" /install
if errorlevel 1 (
  echo [WARN] pnputil reported non-zero exit. Continue if you already had VDD installed.
)

echo.
echo ================================================================
echo  If no VDD monitor exists yet:
echo    1. Open "Device Manager"
echo    2. Action -> Add legacy hardware -> Manually select
echo       -> Display adapters -> Have Disk -> point to:
echo       %VDD_BIN%\ZakoVDD.inf
echo    or just rely on the auto-attached PnP devnode if previously added.
echo  After a virtual display appears in Display Settings, press a key.
echo ================================================================
pause

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
echo === Running consumer (monitor 0, 5 frames, timeout 5000ms) ===
"%TEST_DIR%build\vdd_capture_test.exe" --monitor 0 --frames 5 --timeout 5000 --out "%OUT_DIR%"
echo.
echo Output dir: %OUT_DIR%
explorer "%OUT_DIR%"
endlocal
