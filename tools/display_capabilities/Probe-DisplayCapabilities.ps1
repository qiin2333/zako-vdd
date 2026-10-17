#Requires -Version 5.1
<#
Read-only Windows HDR capability probe. Run with Windows PowerShell 5.1:
  powershell.exe -NoProfile -File .\Probe-DisplayCapabilities.ps1
  powershell.exe -NoProfile -File .\Probe-DisplayCapabilities.ps1 -OutputPath report.json

Reports EDID declarations separately from Windows' effective support queries.
Does not apply display modes, change the registry, or restart devices.
#>
[CmdletBinding()]
param([string]$OutputPath)

$ErrorActionPreference = 'Stop'
if ($PSEdition -ne 'Desktop') {
    throw 'Run this script with Windows PowerShell 5.1 (powershell.exe), which provides the WinRT projection.'
}

if (-not ('VddDisplayCapabilityProbe.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
namespace VddDisplayCapabilityProbe {
    [ComImport, Guid("7449121C-382B-4705-8DA7-A795BA482013"),
     InterfaceType(ComInterfaceType.InterfaceIsIInspectable)]
    public interface IDisplayInformationStaticsInterop {
        [return: MarshalAs(UnmanagedType.IInspectable)]
        object GetForWindow(IntPtr window, ref Guid iid);
        [return: MarshalAs(UnmanagedType.IInspectable)]
        object GetForMonitor(IntPtr monitor, ref Guid iid);
    }
    public static class Native {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct MonitorInfo {
            public int size;
            public int left, top, right, bottom;
            public int workLeft, workTop, workRight, workBottom;
            public uint flags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string device;
        }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct DisplayDevice {
            public int size;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string name;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string description;
            public uint flags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string id;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string key;
        }
        public delegate bool MonitorCallback(IntPtr monitor, IntPtr hdc, IntPtr rect, IntPtr data);
        [DllImport("user32.dll")]
        public static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorCallback callback, IntPtr data);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern bool EnumDisplayDevices(string device, uint index, ref DisplayDevice info, uint flags);
        public static IntPtr[] GetMonitors() {
            var monitors = new List<IntPtr>();
            MonitorCallback callback = delegate(IntPtr monitor, IntPtr hdc, IntPtr rect, IntPtr data) {
                monitors.Add(monitor); return true;
            };
            if (!EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, callback, IntPtr.Zero))
                throw new InvalidOperationException("EnumDisplayMonitors failed");
            return monitors.ToArray();
        }
        public static MonitorInfo GetInfo(IntPtr monitor) {
            var info = new MonitorInfo(); info.size = Marshal.SizeOf(info);
            if (!GetMonitorInfo(monitor, ref info))
                throw new InvalidOperationException("GetMonitorInfo failed");
            return info;
        }
        public static string GetMonitorPath(string displayName) {
            var device = new DisplayDevice(); device.size = Marshal.SizeOf(device);
            return EnumDisplayDevices(displayName, 0, ref device, 1) ? device.id : null;
        }
        public static object GetInformation(object factory, IntPtr monitor) {
            var iid = new Guid("bed112ae-adc3-4dc9-ae65-851f4d7d4799");
            return ((IDisplayInformationStaticsInterop)factory).GetForMonitor(monitor, ref iid);
        }
    }
}
'@
}

function Get-EdidSummary([string]$DeviceInterfacePath) {
    $parts = $DeviceInterfacePath.Split('#')
    if ($parts.Count -lt 3) { return $null }
    $instance = 'DISPLAY\' + $parts[1] + '\' + $parts[2]
    $key = 'HKLM:\SYSTEM\CurrentControlSet\Enum\' + $instance + '\Device Parameters'
    $value = Get-ItemProperty -LiteralPath $key -Name EDID -ErrorAction SilentlyContinue
    if (-not $value) { return $null }
    [byte[]]$bytes = $value.EDID
    $validLength = $bytes.Length -ge 128 -and $bytes.Length % 128 -eq 0
    $checksums = @()
    $declarations = @()
    if ($validLength) {
        for ($block = 0; $block -lt $bytes.Length / 128; $block++) {
            $start = 128 * $block
            $sum = 0
            for ($i = $start; $i -lt $start + 128; $i++) { $sum += $bytes[$i] }
            $checksums += (($sum -band 255) -eq 0)
            if ($block -eq 0 -or $bytes[$start] -ne 2) { continue }
            $end = $start + [int]$bytes[$start + 2]
            if ($end -eq $start) { continue }
            if ($end -lt $start + 4 -or $end -gt $start + 127) {
                throw ('Invalid CTA data block boundary at block ' + $block)
            }
            for ($pos = $start + 4; $pos -lt $end;) {
                $length = 1 + ($bytes[$pos] -band 31)
                if ($pos + $length -gt $end) { throw 'CTA data block exceeds its declared boundary' }
                if (($bytes[$pos] -shr 5) -eq 7 -and $length -ge 2) {
                    $kind = $null
                    if ($bytes[$pos + 1] -eq 6) { $kind = 'HdrStaticMetadata' }
                    if ($bytes[$pos + 1] -eq 7) { $kind = 'HdrDynamicMetadata' }
                    if ($bytes[$pos + 1] -eq 1 -and $length -ge 5) {
                        $oui = [int]$bytes[$pos + 2] -bor ([int]$bytes[$pos + 3] -shl 8) -bor ([int]$bytes[$pos + 4] -shl 16)
                        if ($oui -eq 0x00d046) { $kind = 'DolbyVision' }
                        if ($oui -eq 0x90848b) { $kind = 'Hdr10Plus' }
                    }
                    if ($kind) {
                        $declarations += [pscustomobject]@{
                            Kind = $kind; Offset = $pos
                            Hex = [BitConverter]::ToString($bytes, $pos, $length)
                        }
                    }
                }
                $pos += $length
            }
        }
    }
    [pscustomobject]@{
        ByteLength = $bytes.Length
        ValidLength = $validLength
        ExtensionCountMatchesLength = $validLength -and ($bytes[126] + 1) * 128 -eq $bytes.Length
        BlockChecksumsValid = $checksums
        Declarations = $declarations
    }
}

$displayType = [Windows.Graphics.Display.DisplayInformation,Windows.Graphics.Display,ContentType=WindowsRuntime]
$factory = [Runtime.InteropServices.WindowsRuntime.WindowsRuntimeMarshal]::GetActivationFactory($displayType)
$activeDisplays = @(foreach ($handle in [VddDisplayCapabilityProbe.Native]::GetMonitors()) {
    $info = [VddDisplayCapabilityProbe.Native]::GetInfo($handle)
    $display = [VddDisplayCapabilityProbe.Native]::GetInformation($factory, $handle)
    $color = $display.GetAdvancedColorInfo()
    [pscustomobject]@{
        DisplayName = $info.device
        MonitorDevicePath = [VddDisplayCapabilityProbe.Native]::GetMonitorPath($info.device)
        AdvancedColorKind = [string]$color.CurrentAdvancedColorKind
        Hdr10CurrentlySupported = $color.IsHdrMetadataFormatCurrentlySupported([Windows.Graphics.Display.HdrMetadataFormat]::Hdr10)
        Hdr10PlusCurrentlySupported = $color.IsHdrMetadataFormatCurrentlySupported([Windows.Graphics.Display.HdrMetadataFormat]::Hdr10Plus)
    }
})

$manager = [Windows.Devices.Display.Core.DisplayManager,Windows.Devices.Display.Core,ContentType=WindowsRuntime]::Create([Windows.Devices.Display.Core.DisplayManagerOptions]::None)
try {
    $stateResult = $manager.TryReadCurrentStateForAllTargets()
    if ([string]$stateResult.ErrorCode -ne 'Success') { throw ('Read display state: ' + $stateResult.ErrorCode) }
    $connectedMonitors = @(foreach ($target in @($manager.GetCurrentTargets() | Where-Object { $_.IsConnected })) {
        $monitor = $target.TryGetMonitor()
        if (-not $monitor) { continue }
        $path = $stateResult.State.GetPathForTarget($target)
        $wire = $path.WireFormat
        [pscustomobject]@{
            Name = $monitor.DisplayName
            DeviceInterfacePath = $target.DeviceInterfacePath
            IsDolbyVisionSupportedInHdrMode = $monitor.IsDolbyVisionSupportedInHdrMode
            SourcePixelFormat = [string]$path.SourcePixelFormat
            WireFormat = if ($wire) { [pscustomobject]@{
                BitsPerChannel = $wire.BitsPerChannel
                ColorSpace = [string]$wire.ColorSpace
                Eotf = [string]$wire.Eotf
                HdrMetadata = [string]$wire.HdrMetadata
                PixelEncoding = [string]$wire.PixelEncoding
            } } else { $null }
            Edid = Get-EdidSummary $target.DeviceInterfacePath
        }
    })
} finally { $manager.Dispose() }

$os = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$report = [pscustomobject]@{
    CollectedAtUtc = [DateTime]::UtcNow.ToString('o')
    WindowsBuild = $os.CurrentBuild + '.' + $os.UBR
    ActiveDisplays = $activeDisplays
    ConnectedMonitors = $connectedMonitors
    Interpretation = 'EDID declarations, current HDR metadata support, and Dolby Vision-in-HDR-mode support are separate facts. These queries do not prove game activation, HDMI Dolby Vision signalling, or HDR10+ Gaming support.'
}
$json = $report | ConvertTo-Json -Depth 10
if ($OutputPath) {
    $destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
    [IO.File]::WriteAllText($destination, $json, (New-Object Text.UTF8Encoding($false)))
}
$json
