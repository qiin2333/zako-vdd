#Requires -Version 5.1

param(
    [uint32]$DisplayId = 0
)

. "$PSScriptRoot\common.ps1"

$display = Get-ZakoDisplayInfo -DisplayId $DisplayId
Set-DisplayPrimary -DisplayId $display.DisplayId -ErrorAction Stop
Write-Host "Zako display $($display.DisplayId) is now the primary display."
