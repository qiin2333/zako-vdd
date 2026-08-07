<#
.SYNOPSIS
    Headless reproduction harness for "ZakoVDD creates a monitor but Windows never enumerates it".

.DESCRIPTION
    Installs one ZakoVDD build, drives it through the exact IOCTL sequence Sunshine uses
    (SETMODES then CREATEMONITOR), and then polls QueryDisplayConfig for the "Zako HDR"
    target the way Sunshine's find_device_by_friendlyname does.

    The point is to separate three states that look identical from Sunshine's logs:
      * the control interface never appeared        -> driver did not start
      * CREATEMONITOR failed at the IOCTL layer     -> driver rejected the request
      * CREATEMONITOR succeeded, no target appeared -> IddCxMonitorArrival did not stick

    Only the third state is the bug we are chasing. Everything is written to a JSON
    result file so a matrix of driver versions can be compared mechanically.

    Exit code reflects harness health, not the outcome under test: a build that fails to
    enumerate still exits 0 with monitorEnumerated=false, so the whole matrix runs.
#>
[CmdletBinding()]
param(
    # Directory holding an unpacked zakovdd.zip (ZakoVDD.inf/.dll/zakovdd.cat/ZakoVDD.cer/vdd_settings.xml).
    [Parameter(Mandatory = $true)][string]$DriverDirectory,

    # nefconw.exe. Sunshine uses it to create the Root\ZakoVDD devnode; pnputil alone cannot.
    [Parameter(Mandatory = $true)][string]$NefconPath,

    [Parameter(Mandatory = $true)][string]$OutputJson,

    # Wire format is the driver's: WxHxHz, comma separated. Mirrors what Sunshine sends.
    [string]$Modes = '2560x1440x60,1920x1080x60,1280x720x60,2560x1440x120,1920x1080x120,1280x720x120',

    # Reported in the result so a matrix run records which mode was under test.
    [string]$RequestedMode = '2560x1440x60',

    # Auto | Legacy | Modern. Written to the driver's registry key before install.
    # Empty leaves the build's own default alone.
    [string]$EdidProfile = '',

    [string]$Label = 'unknown',

    # The IOCTLs need LocalSystem, but a session-0 process sees an empty display
    # config, so the enumeration check has to run in an interactive session.
    # Run the script twice: once as SYSTEM, then again with -CheckOnly as the
    # ordinary runner user, which reloads the JSON and fills in the verdict.
    [switch]$CheckOnly,

    [int]$InterfaceTimeoutSeconds = 120,
    [int]$MonitorTimeoutSeconds = 20
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$hardwareId       = 'Root\ZakoVDD'
$serviceName      = 'ZAKO_HDR_FOR_SUNSHINE'
$displayClassGuid = '4d36e968-e325-11ce-bfc1-08002be10318'
$monitorName      = 'Zako HDR'
$confPath         = 'C:\VirtualDisplayDriver'
$registryKey      = 'HKLM:\SOFTWARE\ZakoTech\ZakoDisplayAdapter'

$result = [ordered]@{
    label                 = $Label
    identity              = ''
    modes                 = $Modes
    requestedMode         = $RequestedMode
    edidProfile           = $EdidProfile
    driverVersion         = ''
    stage                 = 'start'
    harnessError          = ''
    infStaged             = $false
    deviceNodeCreated     = $false
    driverBound           = $false
    controlInterface      = ''
    pingWin32             = -1
    setModesWin32         = -1
    createMonitorWin32    = -1
    monitorEnumerated     = $false
    monitorActive         = $false
    secondsToEnumeration  = $null
    targets               = @()
    pnpMonitors           = @()
    devnodeStatus         = ''
    verdict               = 'unknown'
}

function Save-Result {
    $result.targets     = @($result.targets)
    $result.pnpMonitors = @($result.pnpMonitors)
    $dir = Split-Path -Parent $OutputJson
    if ($dir -and -not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    ($result | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath $OutputJson -Encoding UTF8
    Write-Host "--- result written to $OutputJson"
    $result.GetEnumerator() | ForEach-Object { Write-Host ("  {0,-22} {1}" -f $_.Key, $_.Value) }
}

# ---------------------------------------------------------------------------
# Native helpers: cfgmgr32 for the control interface, DeviceIoControl for the
# command channel, QueryDisplayConfig for the enumeration check. The last one
# is deliberately the same API Sunshine uses, so a negative result here means
# a negative result there.
# ---------------------------------------------------------------------------
Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class VddRepro
{
    const uint FILE_DEVICE_UNKNOWN = 0x22;
    static uint Ctl(uint fn, uint access) { return (FILE_DEVICE_UNKNOWN << 16) | (access << 14) | (fn << 2); }

    // Must match Common/Include/vdd_control_ioctl.h.
    public static uint IoctlCommand { get { return Ctl(0x800, 2); } }  // FILE_WRITE_DATA
    public static uint IoctlPing { get { return Ctl(0x801, 1); } }     // FILE_READ_ACCESS

    static readonly Guid ControlGuid = new Guid("DA9F8C2B-7E4F-49A1-9D4E-6F2B0E1A0C4D");

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    static extern int CM_Get_Device_Interface_List_SizeW(out uint len, ref Guid cls, string devId, uint flags);
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    static extern int CM_Get_Device_Interface_ListW(ref Guid cls, string devId, char[] buf, uint len, uint flags);

    public static string[] FindControlInterfaces()
    {
        Guid g = ControlGuid;
        uint len;
        if (CM_Get_Device_Interface_List_SizeW(out len, ref g, null, 0) != 0 || len <= 1)
        {
            return new string[0];
        }
        char[] buf = new char[len];
        if (CM_Get_Device_Interface_ListW(ref g, null, buf, len, 0) != 0)
        {
            return new string[0];
        }
        List<string> list = new List<string>();
        int start = 0;
        for (int i = 0; i < buf.Length; i++)
        {
            if (buf[i] != '\0') { continue; }
            if (i > start) { list.Add(new string(buf, start, i - start)); }
            start = i + 1;
        }
        return list.ToArray();
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, byte[] inBuf, uint inLen, byte[] outBuf, uint outLen, out uint ret, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);

    // Returns 0 on success. A CreateFileW failure is returned negated so the
    // caller can tell "could not open the device" from "the driver rejected
    // the IOCTL" -- they otherwise collapse onto the same Win32 codes.
    public static int Send(string path, uint code, string payload)
    {
        // Access mask and SQOS flags match Sunshine's vdd_ioctl::device_handle
        // exactly, so a failure here is a failure there.
        IntPtr h = CreateFileW(path, 0xC0000000, 3, IntPtr.Zero, 3, 0x00120000, IntPtr.Zero);
        if (h == new IntPtr(-1))
        {
            int err = Marshal.GetLastWin32Error();
            return (err == 0) ? -1 : -err;
        }
        try
        {
            byte[] inBuf = (payload == null)
                ? new byte[0]
                : Encoding.Unicode.GetBytes(payload + "\0");
            uint returned;
            if (!DeviceIoControl(h, code, inBuf, (uint)inBuf.Length, null, 0, out returned, IntPtr.Zero))
            {
                return Marshal.GetLastWin32Error();
            }
            return 0;
        }
        finally { CloseHandle(h); }
    }

    [StructLayout(LayoutKind.Sequential)]
    struct LUID { public uint LowPart; public int HighPart; }

    [StructLayout(LayoutKind.Sequential)]
    struct RATIONAL { public uint Numerator; public uint Denominator; }

    [StructLayout(LayoutKind.Sequential)]
    struct PATH_SOURCE_INFO
    {
        public LUID adapterId; public uint id; public uint modeInfoIdx; public uint statusFlags;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct PATH_TARGET_INFO
    {
        public LUID adapterId; public uint id; public uint modeInfoIdx;
        public uint outputTechnology; public uint rotation; public uint scaling;
        public RATIONAL refreshRate; public uint scanLineOrdering;
        [MarshalAs(UnmanagedType.Bool)] public bool targetAvailable;
        public uint statusFlags;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct PATH_INFO
    {
        public PATH_SOURCE_INFO sourceInfo; public PATH_TARGET_INFO targetInfo; public uint flags;
    }

    // Opaque: we never read the modes, but QueryDisplayConfig still needs a
    // correctly sized array. DISPLAYCONFIG_MODE_INFO is 64 bytes on x64.
    [StructLayout(LayoutKind.Sequential, Size = 64)]
    struct MODE_INFO { }

    [StructLayout(LayoutKind.Sequential)]
    struct DEVICE_INFO_HEADER
    {
        public uint type; public uint size; public LUID adapterId; public uint id;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct TARGET_DEVICE_NAME
    {
        public DEVICE_INFO_HEADER header;
        public uint flags;
        public uint outputTechnology;
        public ushort edidManufactureId;
        public ushort edidProductCodeId;
        public uint connectorInstance;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string monitorFriendlyDeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string monitorDevicePath;
    }

    [DllImport("user32.dll")]
    static extern int GetDisplayConfigBufferSizes(uint flags, out uint numPath, out uint numMode);
    [DllImport("user32.dll")]
    static extern int QueryDisplayConfig(uint flags, ref uint numPath, [Out] PATH_INFO[] paths,
        ref uint numMode, [Out] MODE_INFO[] modes, IntPtr topo);
    [DllImport("user32.dll")]
    static extern int DisplayConfigGetDeviceInfo(ref TARGET_DEVICE_NAME req);

    const uint QDC_ALL_PATHS = 1;
    const uint DISPLAYCONFIG_PATH_ACTIVE = 1;
    const uint GET_TARGET_NAME = 2;

    // "friendly name|device path|active|inactive" for every target Windows knows
    // about, active or not -- the same QDC_ALL_PATHS view Sunshine queries.
    public static string[] QueryTargets()
    {
        uint numPath, numMode;
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, out numPath, out numMode) != 0)
        {
            return new string[0];
        }
        PATH_INFO[] paths = new PATH_INFO[numPath];
        MODE_INFO[] modes = new MODE_INFO[numMode];
        if (QueryDisplayConfig(QDC_ALL_PATHS, ref numPath, paths, ref numMode, modes, IntPtr.Zero) != 0)
        {
            return new string[0];
        }
        List<string> found = new List<string>();
        for (int i = 0; i < numPath; i++)
        {
            TARGET_DEVICE_NAME req = new TARGET_DEVICE_NAME();
            req.header.type = GET_TARGET_NAME;
            req.header.size = (uint)Marshal.SizeOf(typeof(TARGET_DEVICE_NAME));
            req.header.adapterId = paths[i].targetInfo.adapterId;
            req.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(ref req) != 0) { continue; }
            bool active = (paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
            found.Add((req.monitorFriendlyDeviceName == null ? "" : req.monitorFriendlyDeviceName)
                + "|" + (req.monitorDevicePath == null ? "" : req.monitorDevicePath)
                + "|" + (active ? "active" : "inactive"));
        }
        return found.ToArray();
    }
}
'@

function Wait-For([scriptblock]$Condition, [int]$Seconds, [int]$PollMs = 250) {    $deadline = (Get-Date).AddSeconds($Seconds)
    do {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds $PollMs
    } while ((Get-Date) -lt $deadline)
    return $false
}

# nefconw.exe is a GUI-subsystem binary, so PowerShell cannot pipe it; Start-Process
# with -Wait is the only way to get an exit code back.
function Invoke-Nefcon([string[]]$Arguments) {
    $proc = Start-Process -FilePath $NefconPath -ArgumentList $Arguments -Wait -PassThru -NoNewWindow
    if ($null -eq $proc.ExitCode) { throw 'nefcon did not report an exit code.' }
    return [int]$proc.ExitCode
}

$prior = $null
if ($CheckOnly -and (Test-Path -LiteralPath $OutputJson)) {
    # Carry forward everything the SYSTEM pass established; this pass only
    # contributes the enumeration verdict.
    $prior = Get-Content -LiteralPath $OutputJson -Raw | ConvertFrom-Json
    foreach ($p in $prior.PSObject.Properties) {
        if ($result.Contains($p.Name)) { $result[$p.Name] = $p.Value }
    }
}

try {
    # Sunshine drives these IOCTLs from a LocalSystem service. The device
    # interface SDDL can be stricter than "any administrator", so record who we
    # actually are -- an ACCESS_DENIED open means little without it.
    $result.identity = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    if ($CheckOnly -and $prior) { $result.identity = "$($prior.identity) + $($result.identity)" }

  if (-not $CheckOnly) {
    $inf = Join-Path $DriverDirectory 'ZakoVDD.inf'
    $cer = Join-Path $DriverDirectory 'ZakoVDD.cer'
    $xml = Join-Path $DriverDirectory 'vdd_settings.xml'
    foreach ($required in @($inf, $cer, $NefconPath)) {
        if (-not (Test-Path -LiteralPath $required)) {
            throw "Required file is missing: $required"
        }
    }

    $driverVerLine = (Select-String -LiteralPath $inf -Pattern '^DriverVer' | Select-Object -First 1)
    if ($driverVerLine) { $result.driverVersion = $driverVerLine.Line.Trim() }

    # --- Turn on driver logging before the driver ever starts -----------------
    # The registry values win over the XML when set to 1, but write both so the
    # log lands regardless of which path this build reads.
    $result.stage = 'configure'
    New-Item -ItemType Directory -Path $confPath -Force | Out-Null
    if (Test-Path -LiteralPath $xml) {
        $xmlText = Get-Content -LiteralPath $xml -Raw
        $xmlText = $xmlText -replace '<logging>[^<]*</logging>', '<logging>true</logging>'
        $xmlText = $xmlText -replace '<debuglogging>[^<]*</debuglogging>', '<debuglogging>true</debuglogging>'
        $xmlText | Set-Content -LiteralPath (Join-Path $confPath 'vdd_settings.xml') -Encoding UTF8
    }
    New-Item -Path $registryKey -Force | Out-Null
    New-ItemProperty -Path $registryKey -Name 'LOGS' -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $registryKey -Name 'DEBUGLOGS' -Value 1 -PropertyType DWord -Force | Out-Null
    if ($EdidProfile) {
        New-ItemProperty -Path $registryKey -Name 'EDIDPROFILE' -Value $EdidProfile -PropertyType String -Force | Out-Null
    }

    # --- Trust the build's self-signed cert so PnP will accept the catalog ----
    $result.stage = 'trust-certificate'
    foreach ($store in @('Root', 'TrustedPublisher')) {
        Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
    }

    # --- Stage the package, create the devnode, bind the driver --------------
    $result.stage = 'pnputil-add-driver'
    & "$env:SystemRoot\System32\pnputil.exe" /add-driver $inf
    if ($LASTEXITCODE -notin @(0, 3010)) {
        throw "pnputil /add-driver failed with exit code $LASTEXITCODE."
    }
    $result.infStaged = $true

    $result.stage = 'create-device-node'
    $exit = Invoke-Nefcon @('--create-device-node', '--hardware-id', $hardwareId,
        '--service-name', $serviceName, '--class-name', 'Display', '--class-guid', $displayClassGuid)
    if ($exit -ne 0) { throw "nefcon --create-device-node failed with exit code $exit." }
    $result.deviceNodeCreated = $true

    $result.stage = 'install-driver'
    $exit = Invoke-Nefcon @('--install-driver', '--inf-path', $inf)
    if ($exit -notin @(0, 3010)) { throw "nefcon --install-driver failed with exit code $exit." }
    $result.driverBound = $true

    try {
        $result.devnodeStatus = (Get-PnpDevice -InstanceId "ROOT\DISPLAY\*" -ErrorAction Stop |
            ForEach-Object { "$($_.InstanceId)=$($_.Status)/$($_.Problem)" }) -join '; '
    } catch { $result.devnodeStatus = "query failed: $($_.Exception.Message)" }

    # --- Wait for the control interface (i.e. the driver actually started) ----
    $result.stage = 'wait-control-interface'
    $iface = $null
    $ok = Wait-For {
        $list = [VddRepro]::FindControlInterfaces()
        if ($list.Length -gt 0) { $script:iface = $list[0]; return $true }
        return $false
    } $InterfaceTimeoutSeconds
    if (-not $ok) {
        $result.verdict = 'driver-did-not-start'
        throw "The VDD control interface never appeared within $InterfaceTimeoutSeconds s."
    }
    $result.controlInterface = $iface
    Write-Host "Control interface: $iface"

    # --- Drive the exact Sunshine sequence -----------------------------------
    $result.stage = 'ioctl'
    $result.pingWin32 = [VddRepro]::Send($iface, [VddRepro]::IoctlPing, $null)
    Write-Host "PING -> $($result.pingWin32)"

    $result.setModesWin32 = [VddRepro]::Send($iface, [VddRepro]::IoctlCommand, "SETMODES $Modes")
    Write-Host "SETMODES -> $($result.setModesWin32)"

    $result.createMonitorWin32 = [VddRepro]::Send($iface, [VddRepro]::IoctlCommand, 'CREATEMONITOR')
    Write-Host "CREATEMONITOR -> $($result.createMonitorWin32)"
  }

    # --- The actual question: does Windows enumerate the monitor? ------------
    $result.stage = 'wait-monitor'
    $started = Get-Date
    $enumerated = Wait-For {
        $targets = [VddRepro]::QueryTargets()
        foreach ($t in $targets) {
            if ($t.Split('|')[0] -eq $monitorName) { return $true }
        }
        return $false
    } $MonitorTimeoutSeconds

    $result.targets = [VddRepro]::QueryTargets()
    foreach ($t in $result.targets) {
        $parts = $t.Split('|')
        if ($parts[0] -eq $monitorName) {
            $result.monitorEnumerated = $true
            if ($parts[2] -eq 'active') { $result.monitorActive = $true }
        }
    }
    if ($enumerated) {
        $result.secondsToEnumeration = [math]::Round(((Get-Date) - $started).TotalSeconds, 2)
    }

    try {
        $result.pnpMonitors = @(Get-PnpDevice -Class Monitor -ErrorAction Stop |
            ForEach-Object { "$($_.FriendlyName)=$($_.Status)/$($_.InstanceId)" })
    } catch { $result.pnpMonitors = @("query failed: $($_.Exception.Message)") }

    $result.stage = 'done'
    $result.verdict = if ($result.createMonitorWin32 -lt 0) { 'device-open-denied' }
        elseif ($result.createMonitorWin32 -ne 0) { 'createmonitor-ioctl-failed' }
        elseif ($result.monitorEnumerated) { 'ok' }
        else { 'created-but-not-enumerated' }
}
catch {
    $result.harnessError = $_.Exception.Message
    if ($result.verdict -eq 'unknown') { $result.verdict = "harness-error@$($result.stage)" }
    Write-Host "::warning::$Label harness error at stage '$($result.stage)': $($_.Exception.Message)"
}
finally {
    Save-Result
}

# Always 0: a build that fails to enumerate is data, not a harness failure. The
# workflow's summary job decides what the matrix as a whole means.
exit 0
