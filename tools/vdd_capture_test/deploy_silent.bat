@echo off
rem Non-interactive deploy: trust cert + add driver + force PnP rescan.
rem Then attempt consumer test on monitor 0.
setlocal
net session >nul 2>&1
if errorlevel 1 (echo [ERROR] Run as Administrator. & exit /b 1)

set "VDD_BIN=c:\Users\mohaha\Program\github\Virtual-Display-Driver\Virtual Display Driver (HDR)\x64\Release\ZakoVDD"
set "TEST_DIR=c:\Users\mohaha\Program\github\Virtual-Display-Driver\tools\vdd_capture_test"
set "OUT_DIR=C:\Temp\vdd_test_out"
set "LOG=%TEST_DIR%\deploy.log"

if exist "%LOG%" del "%LOG%"

echo === [1/5] Trusting test cert === >>"%LOG%"
certutil -addstore -f "TrustedPublisher" "%VDD_BIN%\ZakoVDD.cer" >>"%LOG%" 2>&1
certutil -addstore -f "Root"             "%VDD_BIN%\ZakoVDD.cer" >>"%LOG%" 2>&1

echo === [2/5] enum-drivers (filtered) === >>"%LOG%"
pnputil /enum-drivers >"%TEMP%\_pnpall.txt" 2>&1
findstr /i /c:"oem" /c:"zakovdd" /c:"ZakoTech" "%TEMP%\_pnpall.txt" >>"%LOG%"

echo === [3/5] pnputil /add-driver /install === >>"%LOG%"
pnputil /add-driver "%VDD_BIN%\ZakoVDD.inf" /install >>"%LOG%" 2>&1
echo pnputil exit=%ERRORLEVEL% >>"%LOG%"

echo === [4/5] Restart Zako PnP devnodes === >>"%LOG%"
powershell -NoProfile -Command "Get-PnpDevice | Where-Object { $_.FriendlyName -match 'Zako' } | ForEach-Object { Write-Host ('Restarting ' + $_.FriendlyName + ' ' + $_.InstanceId); pnputil /restart-device $_.InstanceId }" >>"%LOG%" 2>&1

echo === [5/5] Wait 4s then run consumer === >>"%LOG%"
ping 127.0.0.1 -n 5 >nul

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
echo --- consumer monitor=0 --- >>"%LOG%"
"%TEST_DIR%\build\vdd_capture_test.exe" --monitor 0 --frames 3 --timeout 5000 --out "%OUT_DIR%" >>"%LOG%" 2>&1
echo consumer monitor=0 exit=%ERRORLEVEL% >>"%LOG%"

echo --- consumer monitor=1 --- >>"%LOG%"
"%TEST_DIR%\build\vdd_capture_test.exe" --monitor 1 --frames 3 --timeout 5000 --out "%OUT_DIR%" >>"%LOG%" 2>&1
echo consumer monitor=1 exit=%ERRORLEVEL% >>"%LOG%"

echo === [done] log=%LOG% out=%OUT_DIR% === >>"%LOG%"
type "%LOG%"
endlocal
