#Requires -Version 5.1

. "$PSScriptRoot\common.ps1"

$displays = @(
    Get-DisplayInfo -ErrorAction Stop | Where-Object {
        $_.Active -and $_.DevicePath -match '(?i)DISPLAY#ZAK2333#'
    } | Sort-Object DisplayId
)

if ($displays.Count -eq 0) {
    throw 'No active Zako display (DISPLAY\ZAK2333) was found.'
}

$displays | Select-Object DisplayId, GdiDeviceName, DisplayName, Primary,
    @{ Name = 'Resolution'; Expression = { "$($_.Mode.Width)x$($_.Mode.Height)" } },
    @{ Name = 'RefreshRate'; Expression = { [math]::Round($_.Mode.RefreshRate, 2) } }
