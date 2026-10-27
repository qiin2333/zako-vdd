$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$outputDir = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

# Compile production method bodies directly; the C++ harness supplies API doubles.
function Extract([string]$path, [string]$start, [string]$end, [string]$output) {
    $text = [IO.File]::ReadAllText((Join-Path $repoRoot $path)).Replace("`r`n", "`n")
    $first = $text.IndexOf($start)
    if ($first -lt 0) { throw "Missing start marker in $path" }
    $last = if ($end) { $text.IndexOf($end, $first) } else { $text.Length }
    if ($last -lt $first) { throw "Missing end marker in $path" }
    $text.Substring($first, $last - $first) | Set-Content -Encoding utf8 (Join-Path $outputDir $output)
}

Extract 'ZakoVDD/Device/MonitorLifecycle.cpp' 'void IndirectDeviceContext::CreateMonitor(' '' 'lifecycle.inc'
Extract 'ZakoVDD/Control/CommandHandlers.cpp' 'void HandleRefreshModesCommand(' 'void HandleUnknownCommand(' 'commands.inc'
Extract 'ZakoVDD/Device/IndirectDeviceContext.h' 'struct MonitorCreationParams' "`n`t`t`tbool RecreateMonitor(" 'creation_params.inc'
Extract 'ZakoVDD/Util/RefreshRate.cpp' '#include <cmath>' '' 'refresh.inc'
