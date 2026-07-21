#Requires -Version 5.1

$minimumVersion = [version]'1.1.1'
$displayConfig = Get-Module -ListAvailable -Name DisplayConfig |
    Where-Object { $_.Version -ge $minimumVersion } |
    Sort-Object Version -Descending |
    Select-Object -First 1

if (-not $displayConfig) {
    throw "DisplayConfig $minimumVersion or newer is required. Install it with: Install-Module DisplayConfig -Scope CurrentUser"
}

Import-Module -Name DisplayConfig -MinimumVersion $minimumVersion -ErrorAction Stop

function Get-ZakoDisplayInfo {
    [CmdletBinding()]
    param(
        [uint32]$DisplayId = 0
    )

    $displays = @(
        Get-DisplayInfo -ErrorAction Stop | Where-Object {
            $_.Active -and $_.DevicePath -match '(?i)DISPLAY#ZAK2333#'
        }
    )

    if ($DisplayId -ne 0) {
        $display = $displays | Where-Object { $_.DisplayId -eq $DisplayId } | Select-Object -First 1
        if (-not $display) {
            throw "Active Zako display ID '$DisplayId' was not found. Run .\list-VDD.ps1 to list the active Zako displays."
        }

        return $display
    }

    if ($displays.Count -eq 0) {
        throw 'No active Zako display (DISPLAY\ZAK2333) was found.'
    }

    $display = $displays | Sort-Object DisplayId | Select-Object -First 1
    if ($displays.Count -gt 1) {
        Write-Warning "Multiple active Zako displays were found. Using display ID $($display.DisplayId). Pass -DisplayId to select another display."
    }

    return $display
}
