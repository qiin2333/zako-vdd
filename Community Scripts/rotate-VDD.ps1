#Requires -Version 5.1

param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateSet(0, 90, 180, 270)]
    [int]$Rotation,

    [uint32]$DisplayId = 0
)

. "$PSScriptRoot\common.ps1"

$display = Get-ZakoDisplayInfo -DisplayId $DisplayId
$displayRotation = switch ($Rotation) {
    0 { 'None' }
    90 { 'Rotate90' }
    180 { 'Rotate180' }
    270 { 'Rotate270' }
}

Set-DisplayRotation -DisplayId $display.DisplayId -Rotation $displayRotation -ErrorAction Stop
Write-Host "Zako display $($display.DisplayId) rotation set to $Rotation degrees."
