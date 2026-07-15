#Requires -Version 5.1

param(
    [Parameter(Mandatory, Position = 0)]
    [Alias('X', 'XRes', 'HorizontalResolution')]
    [ValidateRange(1, 65535)]
    [uint32]$Width,

    [Parameter(Mandatory, Position = 1)]
    [Alias('Y', 'YRes', 'VerticalResolution')]
    [ValidateRange(1, 65535)]
    [uint32]$Height,

    [uint32]$DisplayId = 0
)

. "$PSScriptRoot\common.ps1"

$display = Get-ZakoDisplayInfo -DisplayId $DisplayId
Set-DisplayResolution -DisplayId $display.DisplayId -Width $Width -Height $Height -ErrorAction Stop
Write-Host "Zako display $($display.DisplayId) resolution set to ${Width}x${Height}."
