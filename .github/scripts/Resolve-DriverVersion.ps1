[CmdletBinding()]
param(
    [string] $Ref = $env:GITHUB_REF,
    [string] $BaseRef = $env:GITHUB_BASE_REF,
    [string] $RefName = $env:GITHUB_REF_NAME,
    [long] $RunNumber = $(if ($env:GITHUB_RUN_NUMBER) { [long] $env:GITHUB_RUN_NUMBER } else { 1 })
)

$ErrorActionPreference = 'Stop'
$maximumComponent = 65534
$isWin10Line = $BaseRef -eq 'win10' -or
    $RefName -eq 'win10' -or
    $Ref -match '^refs/tags/v0\.15\.'

if ($Ref -match '^refs/tags/(v.+)$') {
    $tag = $Matches[1]
    if ($isWin10Line) {
        if ($tag -notmatch '^v0\.15\.(\d+)$') {
            throw "Win10 maintenance releases require a numeric v0.15.<patch> tag; got '$tag'."
        }
        $patch = [int] $Matches[1]
        if ($patch -gt $maximumComponent) {
            throw "Win10 release patch $patch exceeds the DriverVer component limit $maximumComponent."
        }

        # Win10 22H2 fails to enumerate the IddCx monitor when the INF uses the
        # former 99.x/100.x CI scheme. Keep this release line in a low, stable
        # namespace. Explicit installers must migrate the already-published
        # 100.0.15.x packages instead of relying on Windows driver ranking.
        $version = "15.0.15.$patch"
    }
    else {
        $versionText = $tag.Substring(1)
        if ($versionText -notmatch '^\d+(\.\d+){1,2}$') {
            throw "Release tag '$tag' cannot be encoded as a numeric DriverVer."
        }
        $parts = @($versionText.Split('.') | ForEach-Object { [int] $_ })
        foreach ($part in $parts) {
            if ($part -gt $maximumComponent) {
                throw "Release tag '$tag' contains a component above $maximumComponent."
            }
        }
        $parts = @(100) + $parts
        while ($parts.Count -lt 4) { $parts += 0 }
        $version = ($parts[0..3] -join '.')
    }
}
elseif ($isWin10Line) {
    # PR and branch artifacts deliberately rank below a tagged maintenance
    # release. Their exact binary is identified by artifact/run ID and SHA-256.
    $build = $RunNumber % ($maximumComponent + 1)
    if ($build -eq 0) { $build = 1 }
    $version = "15.0.0.$build"
}
else {
    $version = '99.' + (Get-Date -Format 'MM.dd.HHmm')
}

$components = @($version.Split('.') | ForEach-Object { [int] $_ })
if ($components.Count -ne 4 -or @($components | Where-Object { $_ -lt 0 -or $_ -gt $maximumComponent }).Count -ne 0) {
    throw "Resolved invalid DriverVer '$version'."
}
if ($isWin10Line -and $components[0] -ge 99) {
    throw "Win10 DriverVer '$version' reintroduces the proven 99.x/100.x enumeration regression."
}

[pscustomobject]@{
    Version = $version
    Date = Get-Date -Format 'MM/dd/yyyy'
    IsWin10Line = $isWin10Line
}
