#Requires -Version 5.1

[CmdletBinding(DefaultParameterSetName = 'UserDefined')]
param(
    [Parameter(Mandatory, Position = 0, ParameterSetName = 'UserDefined')]
    [ValidateSet(100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500)]
    [int]$Scale,

    [Parameter(Mandatory, ParameterSetName = 'Recommended')]
    [switch]$Reset,

    [uint32]$DisplayId = 0
)

. "$PSScriptRoot\common.ps1"

$display = Get-ZakoDisplayInfo -DisplayId $DisplayId
$maxScale = Get-DisplayScale -DisplayId $display.DisplayId -ErrorAction Stop | Select-Object -ExpandProperty MaxScale
if ($PSCmdlet.ParameterSetName -eq 'UserDefined') {
    if ($Scale -gt $maxScale) {
        throw "Scale percentage $Scale is higher than the supported maximum of $maxScale%."
    }

    Set-DisplayScale -DisplayId $display.DisplayId -Scale $Scale -ErrorAction Stop
    Write-Host "Zako display $($display.DisplayId) scale set to $Scale%."
}
else {
    Set-DisplayScale -DisplayId $display.DisplayId -Recommended -ErrorAction Stop
    Write-Host "Zako display $($display.DisplayId) scale reset to the recommended value."
}
