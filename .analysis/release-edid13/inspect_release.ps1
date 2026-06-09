$dll = Get-Item '.analysis/release-edid13/extracted/ZakoVDD.dll'
$inf = Get-Content '.analysis/release-edid13/extracted/ZakoVDD.inf'
$sig = Get-AuthenticodeSignature '.analysis/release-edid13/extracted/zakovdd.cat'
$cert = $null
if (Test-Path '.analysis/release-edid13/extracted/ZakoVDD.cer') {
  $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2('.analysis/release-edid13/extracted/ZakoVDD.cer')
}
[PSCustomObject]@{
  DllLength = $dll.Length
  FileVersion = $dll.VersionInfo.FileVersion
  ProductVersion = $dll.VersionInfo.ProductVersion
  CompanyName = $dll.VersionInfo.CompanyName
  OriginalFilename = $dll.VersionInfo.OriginalFilename
  CatSigner = if ($sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { '' }
  CatStatus = [string]$sig.Status
  CatThumbprint = if ($sig.SignerCertificate) { $sig.SignerCertificate.Thumbprint } else { '' }
  CerSubject = if ($cert) { $cert.Subject } else { '' }
  CerThumbprint = if ($cert) { $cert.Thumbprint } else { '' }
} | Format-List
'---INF_FIELDS---'
$inf | Select-String 'DriverVer|CatalogFile|Provider|ClassGUID|PnpLockdown'
'---VDD_SETTINGS---'
Get-Content '.analysis/release-edid13/extracted/vdd_settings.xml'
