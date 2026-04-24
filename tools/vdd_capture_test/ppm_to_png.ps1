param([string]$Src = 'C:\Temp\vdd_test_out2\vdd_frame_004.ppm')
Add-Type -AssemblyName System.Drawing
$bytes = [System.IO.File]::ReadAllBytes($Src)
$headerEnd = 0
$nl = 0
for ($i = 0; $i -lt $bytes.Length; $i++) {
    if ($bytes[$i] -eq 10) { $nl++; if ($nl -eq 3) { $headerEnd = $i + 1; break } }
}
$w = 1280; $h = 720
$bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
$bd = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$stride = $bd.Stride
$ptr = $bd.Scan0
for ($y = 0; $y -lt $h; $y++) {
    $rowSrc = $headerEnd + $y * $w * 3
    $rowDst = [IntPtr]::Add($ptr, $y * $stride)
    # PPM stores R,G,B; GDI 24bpp expects B,G,R. Swap per pixel.
    $row = New-Object byte[] ($w * 3)
    for ($x = 0; $x -lt $w; $x++) {
        $row[$x*3]   = $bytes[$rowSrc + $x*3 + 2]  # B
        $row[$x*3+1] = $bytes[$rowSrc + $x*3 + 1]  # G
        $row[$x*3+2] = $bytes[$rowSrc + $x*3]      # R
    }
    [System.Runtime.InteropServices.Marshal]::Copy($row, 0, $rowDst, $w * 3)
}
$bmp.UnlockBits($bd)
$out = [System.IO.Path]::ChangeExtension($Src, '.png')
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "PNG -> $out"
