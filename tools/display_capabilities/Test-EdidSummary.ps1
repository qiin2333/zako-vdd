#Requires -Version 5.1
$ErrorActionPreference = 'Stop'

# Load only the parser, without invoking WinRT or accessing real monitor state.
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'Probe-DisplayCapabilities.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$function = $ast.Find({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-EdidSummary'
}, $true)
if (-not $function) { throw 'Get-EdidSummary was not found' }
. ([scriptblock]::Create($function.Extent.Text))

function Get-ItemProperty {
    [CmdletBinding()]
    param([string]$LiteralPath, [string]$Name)
    [pscustomobject]@{ EDID = $script:testEdid }
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function New-TestEdid([byte]$Boundary = 6, [switch]$Overflow) {
    [byte[]]$bytes = New-Object byte[] 384
    $bytes[126] = 2
    # First extension declares static HDR; the next extension declares Dolby Vision.
    [byte[]]$cta = 2, 3, $Boundary, 0, 0xe1, 6
    [Array]::Copy($cta, 0, $bytes, 128, $cta.Length)
    if ($Overflow) { $bytes[134] = 0xe7 }
    [byte[]]$cta = 2, 3, 9, 0, 0xe4, 1, 0x46, 0xd0, 0
    [Array]::Copy($cta, 0, $bytes, 256, $cta.Length)
    for ($start = 0; $start -lt $bytes.Length; $start += 128) {
        $sum = 0
        for ($pos = $start; $pos -lt $start + 127; $pos++) { $sum += $bytes[$pos] }
        $bytes[$start + 127] = (256 - ($sum -band 255)) -band 255
    }
    return ,$bytes
}

$devicePath = '\\?\DISPLAY#TEST#INSTANCE'
$script:testEdid = New-TestEdid
$healthy = Get-EdidSummary $devicePath
Assert-True ($healthy.ParseErrors.Count -eq 0) 'Healthy EDID has parse errors'
Assert-True (($healthy.Declarations.Kind -join ',') -eq 'HdrStaticMetadata,DolbyVision') 'Healthy declarations changed'
Assert-True ($healthy.ExtensionCountMatchesLength) 'Extension count was not retained'

$summaries = @()
foreach ($boundary in @(1, 2, 3, 128, 255)) {
    $script:testEdid = New-TestEdid -Boundary $boundary
    $summary = Get-EdidSummary $devicePath
    Assert-True ($summary.ParseErrors.Count -eq 1) "Missing boundary error for $boundary"
    Assert-True ($summary.ParseErrors[0].Block -eq 1 -and $summary.ParseErrors[0].Offset -eq 130) 'Incorrect boundary error location'
    Assert-True ($summary.ParseErrors[0].Message -eq 'Invalid CTA data block boundary') 'Incorrect boundary error message'
    Assert-True (($summary.Declarations.Kind -join ',') -eq 'DolbyVision') 'Did not resume at the next extension'
    Assert-True ($summary.BlockChecksumsValid.Count -eq 3 -and $summary.BlockChecksumsValid -notcontains $false) 'Checksums were not retained'
    $summaries += $summary
}

# Offsets 0 and 4 are legal empty collections; 127 is the maximum legal end.
foreach ($boundary in @(0, 4, 127)) {
    $script:testEdid = New-TestEdid -Boundary $boundary
    $summary = Get-EdidSummary $devicePath
    Assert-True ($summary.ParseErrors.Count -eq 0) "Legal boundary $boundary was rejected"
    Assert-True ($summary.Declarations.Kind -contains 'DolbyVision') 'Later extension was lost'
}

$script:testEdid = New-TestEdid -Boundary 7 -Overflow
$overflow = Get-EdidSummary $devicePath
Assert-True ($overflow.ParseErrors.Count -eq 1) 'Missing data block length error'
Assert-True ($overflow.ParseErrors[0].Offset -eq 134 -and $overflow.ParseErrors[0].Block -eq 1) 'Incorrect length error location'
Assert-True ($overflow.ParseErrors[0].Message -eq 'CTA data block exceeds its declared boundary') 'Incorrect length error message'
Assert-True (($overflow.Declarations.Kind -join ',') -eq 'HdrStaticMetadata,DolbyVision') 'Valid prefix or later extension was lost'
$summaries += $overflow

# Parse another monitor after all malformed fixtures and round-trip the report as JSON.
$script:testEdid = New-TestEdid
$summaries += Get-EdidSummary $devicePath
$report = [pscustomobject]@{
    ConnectedMonitors = @($summaries | ForEach-Object { [pscustomobject]@{ Edid = $_ } })
}
$json = $report | ConvertTo-Json -Depth 10
$decoded = $json | ConvertFrom-Json
Assert-True ($decoded.ConnectedMonitors.Count -eq 7) 'Malformed EDID interrupted later monitors or JSON output'
Assert-True ($decoded.ConnectedMonitors[0].Edid.ParseErrors.Count -eq 1) 'JSON lost the malformed monitor error'
Assert-True ($decoded.ConnectedMonitors[-1].Edid.ParseErrors.Count -eq 0) 'Errors leaked into a later monitor'
Assert-True ($decoded.ConnectedMonitors[-1].Edid.Declarations.Count -eq 2) 'Later healthy monitor declarations were lost'
'EDID summary regression tests passed.'
