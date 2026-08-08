$ErrorActionPreference = 'Stop'
$resolver = Join-Path $PSScriptRoot 'Resolve-DriverVersion.ps1'

function Assert-Version {
    param(
        [Parameter(Mandatory)] [hashtable] $Arguments,
        [Parameter(Mandatory)] [string] $ExpectedVersion,
        [Parameter(Mandatory)] [bool] $ExpectedWin10
    )

    $result = & $resolver @Arguments
    if ($result.Version -ne $ExpectedVersion -or $result.IsWin10Line -ne $ExpectedWin10) {
        throw "DriverVer policy mismatch: expected $ExpectedVersion/Win10=$ExpectedWin10, got $($result.Version)/Win10=$($result.IsWin10Line)."
    }
}

Assert-Version `
    -Arguments @{ Ref = 'refs/tags/v0.15.7'; BaseRef = ''; RefName = 'v0.15.7'; RunNumber = 321 } `
    -ExpectedVersion '15.0.15.7' `
    -ExpectedWin10 $true
Assert-Version `
    -Arguments @{ Ref = 'refs/pull/34/merge'; BaseRef = 'win10'; RefName = '34/merge'; RunNumber = 321 } `
    -ExpectedVersion '15.0.0.321' `
    -ExpectedWin10 $true
Assert-Version `
    -Arguments @{ Ref = 'refs/heads/win10'; RefName = 'win10'; RunNumber = 65435 } `
    -ExpectedVersion '15.0.0.65435' `
    -ExpectedWin10 $true
Assert-Version `
    -Arguments @{ Ref = 'refs/tags/v0.17.2'; BaseRef = ''; RefName = 'v0.17.2'; RunNumber = 321 } `
    -ExpectedVersion '100.0.17.2' `
    -ExpectedWin10 $false

try {
    & $resolver -Ref 'refs/tags/v0.15.7-preview' -BaseRef '' -RefName 'v0.15.7-preview' -RunNumber 321 | Out-Null
    throw 'A non-numeric Win10 maintenance tag unexpectedly passed DriverVer validation.'
}
catch {
    if ($_.Exception.Message -notmatch 'numeric v0\.15') {
        throw
    }
}

Write-Host 'DriverVer policy passed: Win10 artifacts stay below the proven 99.x/100.x regression namespace.'
