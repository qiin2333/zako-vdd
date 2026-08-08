$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$driverPath = Join-Path $repoRoot 'Virtual Display Driver (HDR)\ZakoVDD\Driver.cpp'
$source = Get-Content -LiteralPath $driverPath -Raw

function Get-SourceSection {
    param(
        [Parameter(Mandatory)] [string] $StartMarker,
        [Parameter(Mandatory)] [string] $EndMarker
    )

    $start = $source.IndexOf($StartMarker, [StringComparison]::Ordinal)
    if ($start -lt 0) {
        throw "Win10 compatibility guard could not find start marker: $StartMarker"
    }

    $end = $source.IndexOf($EndMarker, $start + $StartMarker.Length, [StringComparison]::Ordinal)
    if ($end -lt 0) {
        throw "Win10 compatibility guard could not find end marker: $EndMarker"
    }

    return $source.Substring($start, $end - $start)
}

$ioctlCallback = Get-SourceSection `
    -StartMarker 'VOID VirtualDisplayDriverIoDeviceControl(' `
    -EndMarker 'bool initpath()'

if ($ioctlCallback -notmatch 'WdfWorkItemEnqueue\s*\(\s*g_CommandWorkItem\s*\)') {
    throw 'The IOCTL_VDD_COMMAND path must leave the IddCx callback stack through the persistent WDF work item.'
}

if ($ioctlCallback -match 'DispatchVddCommandBuffer\s*\(') {
    throw 'Do not dispatch VDD commands inline from VirtualDisplayDriverIoDeviceControl.'
}

$workItem = Get-SourceSection `
    -StartMarker 'VOID VddCommandWorkItem(WDFWORKITEM WorkItem)' `
    -EndMarker '// IddCx redirects every IRP_MJ_DEVICE_CONTROL'

if ($workItem -notmatch 'DispatchVddCommandBuffer\s*\(\s*INVALID_HANDLE_VALUE\s*,\s*writable\.data\(\)\s*\)') {
    throw 'The WDF work item must dispatch the copied VDD command outside the IddCx callback stack.'
}

$completeIndex = $ioctlCallback.IndexOf('WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0)', [StringComparison]::Ordinal)
$enqueueIndex = $ioctlCallback.IndexOf('WdfWorkItemEnqueue(g_CommandWorkItem)', [StringComparison]::Ordinal)
if ($completeIndex -lt 0 -or $enqueueIndex -lt 0 -or $completeIndex -gt $enqueueIndex) {
    throw 'The IddCx-owned IOCTL request must be completed before the monitor-command worker is enqueued.'
}

if ($workItem -match 'WdfRequestComplete') {
    throw 'The command worker must not retain or complete the IddCx-owned IOCTL request.'
}

if ($source -notmatch 'IsAdapterReady\(\)' -or
    $source -notmatch 'Adapter is ready for monitor commands' -or
    $workItem -notmatch 'WaitForReadyAdapter') {
    throw 'Monitor commands must wait for successful EvtIddCxAdapterInitFinished completion.'
}

# Sunshine uses the IOCTL control surface on both Win10 and Win11. Earlier
# diagnostics incorrectly attributed Win10 monitor-enumeration failure to the
# callback; the isolated cause was the legacy 99.x/100.x DriverVer range.
if ($source -notmatch 'g_IsWin10OrOlder\.store\s*\(\s*DetectWin10OrOlderHost\(\)\s*\)') {
    throw 'DriverEntry must resolve the host version before device creation.'
}

if ($source -notmatch 'IddConfig\.EvtIddCxDeviceIoControl\s*=\s*VirtualDisplayDriverIoDeviceControl') {
    throw 'EvtIddCxDeviceIoControl must be registered on every supported Windows version.'
}

$deviceAdd = Get-SourceSection `
    -StartMarker 'VirtualDisplayDriverDeviceAdd(WDFDRIVER Driver' `
    -EndMarker 'VirtualDisplayDriverDeviceD0Entry(WDFDEVICE Device'

if ($deviceAdd -notmatch 'WdfDeviceCreateDeviceInterface\s*\(\s*Device\s*,\s*&GUID_DEVINTERFACE_ZAKO_VDD_CONTROL') {
    throw 'The custom IOCTL control device interface must be created on every supported Windows version.'
}

if ($deviceAdd -notmatch 'WdfWorkItemCreate\s*\(') {
    throw 'The persistent IOCTL command work item must be created on every supported Windows version.'
}

$pipeServer = Get-SourceSection `
    -StartMarker 'static void HandlePipeClient(HANDLE pipe)' `
    -EndMarker 'EVT_WDF_WORKITEM VddCommandWorkItem'

if ($pipeServer -notmatch 'CreateNamedPipeW\s*\(' -or
    $pipeServer -notmatch 'WaitForReadyAdapter\s*\(\s*command\s*\)[\s\S]*?DispatchVddCommandBuffer\s*\(\s*pipe\s*,\s*buffer\s*\)') {
    throw 'Win10 compatibility commands must use the shared parser through ZakoVDDPipe.'
}

if ($source -match 'if\s*\(\s*g_IsWin10OrOlder\.load\(\)\s*\)\s*\{\s*StartWin10NamedPipeServer\(\)') {
    throw 'Sunshine-supported Win10 builds must not select the named-pipe transport instead of IOCTL.'
}

if ($source -notmatch 'StopWin10NamedPipeServer\(\)') {
    throw 'Driver unload must stop the Win10 named-pipe thread.'
}

$adapterInit = Get-SourceSection `
    -StartMarker 'void IndirectDeviceContext::InitAdapter()' `
    -EndMarker 'void IndirectDeviceContext::FinishInit()'

if ($adapterInit -notmatch 'if\s*\(\s*g_IsWin10OrOlder\.load\(\)\s*\)[\s\S]*?AdapterCaps\.Size\s*=\s*sizeof\(AdapterCaps\)') {
    throw 'Win10 must advertise the down-level IDDCX_ADAPTER_CAPS size used by the known-good 0.14.3 driver.'
}

if ($source -notmatch 'Adapter already registered; skipping duplicate IddCxAdapterInitAsync') {
    throw 'Win10 D0 wake must not register an existing IddCx adapter a second time.'
}

$d0Exit = Get-SourceSection `
    -StartMarker 'VirtualDisplayDriverDeviceD0Exit(WDFDEVICE Device' `
    -EndMarker 'vector<BYTE> loadEdid(const string &filePath)'

if ($d0Exit -match 'MarkAdapterNotReady') {
    throw 'D3 transitions preserve the registered IddCx adapter and must not erase its initialization-complete state.'
}

if ($source -match 'commandWorkItemAttributes\.ExecutionLevel') {
    throw 'Do not set ExecutionLevel on the UMDF work item; Win10 rejects it with STATUS_WDF_EXECUTION_LEVEL_INVALID.'
}

$edidIdentity = Get-SourceSection `
    -StartMarker 'void modifyEdid(vector<BYTE> &edid)' `
    -EndMarker '// Modify EDID serial number'

$requiredAssignments = @(
    'edid\[8\]\s*=\s*0x68',
    'edid\[9\]\s*=\s*0x2b',
    'edid\[10\]\s*=\s*0x33',
    'edid\[11\]\s*=\s*0x23'
)

foreach ($assignment in $requiredAssignments) {
    if ($edidIdentity -notmatch $assignment) {
        throw "EDID identity must remain DISPLAY\ZAK2333; missing assignment: $assignment"
    }
}

if ($source -match 'edid\[8\]\s*=\s*0x36[\s\S]*?edid\[9\]\s*=\s*0x94[\s\S]*?edid\[10\]\s*=\s*0x37[\s\S]*?edid\[11\]\s*=\s*0x13') {
    throw 'Legacy DISPLAY\MTT1337 manufacturer spoof must not return.'
}

if ($env:GITHUB_REF -match '^refs/tags/v0\.15\.') {
    # An explicit branch fetch may update FETCH_HEAD only. Update the remote
    # tracking ref deterministically before checking release ancestry.
    & git -C $repoRoot fetch origin '+refs/heads/win10:refs/remotes/origin/win10' --no-tags
    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to fetch origin/win10 for release-lineage validation.'
    }

    & git -C $repoRoot merge-base --is-ancestor $env:GITHUB_SHA origin/win10
    if ($LASTEXITCODE -ne 0) {
        throw "Win10 release tag $env:GITHUB_REF_NAME must point to a commit contained in origin/win10."
    }
}

Write-Host 'Compatibility invariants passed: Win10/Win11 deferred IOCTL path, down-level IddCx adapter surface, single adapter registration across D3, and DISPLAY\ZAK2333.'
