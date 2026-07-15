#Requires -Version 5.1

param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateRange(1, 1000)]
    [double]$RefreshRate,

    [uint32]$DisplayId = 0
)

. "$PSScriptRoot\common.ps1"

$display = Get-ZakoDisplayInfo -DisplayId $DisplayId
Set-DisplayRefreshRate -DisplayId $display.DisplayId -RefreshRate $RefreshRate -ErrorAction Stop
Write-Host "Zako display $($display.DisplayId) refresh rate set to $RefreshRate Hz."
