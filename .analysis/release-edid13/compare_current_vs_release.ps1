$releaseDll = Get-Item '.analysis/release-edid13/extracted/ZakoVDD.dll'
$currentDll = Get-Item './Virtual Display Driver (HDR)/x64/Release/ZakoVDD/ZakoVDD.dll'
$releaseSig = Get-AuthenticodeSignature '.analysis/release-edid13/extracted/zakovdd.cat'
$currentSig = Get-AuthenticodeSignature './Virtual Display Driver (HDR)/x64/Release/ZakoVDD/zakovdd.cat'
$releaseInf = Get-Content '.analysis/release-edid13/extracted/ZakoVDD.inf' | Select-String 'DriverVer|CatalogFile|Provider|ClassGUID|PnpLockdown'
$currentInf = Get-Content './Virtual Display Driver (HDR)/x64/Release/ZakoVDD/ZakoVDD.inf' | Select-String 'DriverVer|CatalogFile|Provider|ClassGUID|PnpLockdown'
'=== DLL SIZE ==='
"release=$($releaseDll.Length)"
"current=$($currentDll.Length)"
'=== CAT SIGNER ==='
"release=$($releaseSig.SignerCertificate.Subject) [$($releaseSig.Status)]"
"current=$($currentSig.SignerCertificate.Subject) [$($currentSig.Status)]"
'=== INF ==='
'release:'
$releaseInf
'current:'
$currentInf
'=== XML tail ==='
'release:'
Get-Content '.analysis/release-edid13/extracted/vdd_settings.xml' | Select-Object -Last 20
'current:'
Get-Content './Virtual Display Driver (HDR)/x64/Release/ZakoVDD/vdd_settings.xml' | Select-Object -Last 30
