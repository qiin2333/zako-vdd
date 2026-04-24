Get-PnpDevice | Where-Object { $_.FriendlyName -match 'VDD|Indirect|Virtual|Iddx|Zako' } |
    Select-Object Status, FriendlyName, InstanceId |
    Format-Table -AutoSize | Out-String -Width 200 | Write-Host

Write-Host "--- Display monitors ---"
Get-PnpDevice -Class Monitor -ErrorAction SilentlyContinue |
    Select-Object Status, FriendlyName, InstanceId |
    Format-Table -AutoSize | Out-String -Width 200 | Write-Host
