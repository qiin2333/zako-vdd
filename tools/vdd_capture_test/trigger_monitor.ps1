$ErrorActionPreference = 'Stop'

$commandTool = Resolve-Path (Join-Path $PSScriptRoot '..\vdd_command\build\vdd_command.exe')
Write-Host '--- sending CREATEMONITOR over IOCTL ---'
& $commandTool CREATEMONITOR
if ($LASTEXITCODE -ne 0) {
    throw "vdd_command failed with exit code $LASTEXITCODE"
}
