#Requires -Version 5.1

param(
    [ValidateSet('Toggle', 'On', 'Off')]
    [string]$State = 'Toggle',

    [uint32]$DisplayId = 0
)

. "$PSScriptRoot\common.ps1"

$display = Get-ZakoDisplayInfo -DisplayId $DisplayId
$hdrInfo = Get-DisplayHDR -DisplayId $display.DisplayId -ErrorAction Stop
$enableHdr = switch ($State) {
    'On' { $true }
    'Off' { $false }
    'Toggle' { -not $hdrInfo.HDREnabled }
}

Set-DisplayHDR -DisplayId $display.DisplayId -EnableHDR:$enableHdr -ErrorAction Stop
$newState = if ($enableHdr) { 'enabled' } else { 'disabled' }
Write-Host "HDR $newState on Zako display $($display.DisplayId)."
