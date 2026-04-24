$ErrorActionPreference='Stop'
$pipes = [System.IO.Directory]::GetFiles('\\.\pipe\') | Where-Object { $_ -match 'Vdd|Zako' }
Write-Host "--- pipes matching Vdd|Zako ---"
$pipes | ForEach-Object { Write-Host $_ }

if (-not ($pipes | Where-Object { $_ -match 'ZakoVDDPipe' })) {
    Write-Host "ZakoVDDPipe not present, abort." -ForegroundColor Yellow
    exit 1
}

Write-Host "--- sending CREATEMONITOR ---"
$client = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'ZakoVDDPipe', [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::None)
$client.Connect(2000)
$client.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
$bytes = [System.Text.Encoding]::Unicode.GetBytes("CREATEMONITOR")
$client.Write($bytes, 0, $bytes.Length)
$client.Flush()
Write-Host ("Wrote " + $bytes.Length + " bytes")

# Try to read response (best-effort)
try {
    $buf = New-Object byte[] 512
    $n = $client.Read($buf, 0, $buf.Length)
    if ($n -gt 0) {
        Write-Host ("Response: " + [System.Text.Encoding]::Unicode.GetString($buf, 0, $n))
    }
} catch {
    Write-Host "(no response)"
}
$client.Close()
