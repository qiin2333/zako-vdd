param([string]$Ref)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$outputDir = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

function Read-Source([string]$path) {
    if ($Ref) {
        $lines = & git -C $repoRoot show ($Ref + ':' + $path)
        if ($LASTEXITCODE -ne 0) { throw "git show failed: $Ref $path" }
        return ($lines -join "`n") + "`n"
    }
    return [IO.File]::ReadAllText((Join-Path $repoRoot $path)).Replace("`r`n", "`n")
}

$lifecycle = Read-Source 'ZakoVDD/Device/MonitorLifecycle.cpp'
$commands = Read-Source 'ZakoVDD/Control/CommandHandlers.cpp'
$header = Read-Source 'ZakoVDD/Device/IndirectDeviceContext.h'
$refresh = Read-Source 'ZakoVDD/Util/RefreshRate.cpp'
# Compile the real method bodies verbatim; only Windows/IddCx boundaries are doubled.
$start = $lifecycle.IndexOf('void IndirectDeviceContext::CreateMonitor(')
if ($start -lt 0) { throw 'Missing lifecycle marker' }
$lifecycle.Substring($start) | Set-Content -Encoding utf8 (Join-Path $outputDir 'lifecycle.inc')
$start = $commands.IndexOf('void HandleRefreshModesCommand(')
$end = $commands.IndexOf('void HandleUnknownCommand(', $start)
if ($start -lt 0 -or $end -lt 0) { throw 'Missing command markers' }
$commands.Substring($start, $end - $start) | Set-Content -Encoding utf8 (Join-Path $outputDir 'commands.inc')
$start = $header.IndexOf('struct MonitorCreationParams')
$end = $header.IndexOf("`n`t`t`t};", $start) + "`n`t`t`t};".Length
if ($start -lt 0 -or $end -le $start) { throw 'Missing creation parameters marker' }
$header.Substring($start, $end - $start) | Set-Content -Encoding utf8 (Join-Path $outputDir 'creation_params.inc')
$refresh.Replace('#include "RefreshRate.h"', '') | Set-Content -Encoding utf8 (Join-Path $outputDir 'refresh.inc')
[ordered]@{
    source = $(if ($Ref) { $Ref } else { 'working-tree' })
    extracted_files = @(Get-ChildItem $outputDir -Filter '*.inc' | ForEach-Object {
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            $hash = [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($_.FullName))).Replace('-', '')
            @{ name = $_.Name; sha256 = $hash }
        } finally { $sha.Dispose() }
    })
} | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $outputDir 'source-manifest.json')
