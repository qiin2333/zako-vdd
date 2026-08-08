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
    throw 'IOCTL_VDD_COMMAND must leave the IddCx callback stack through the persistent WDF work item.'
}

if ($ioctlCallback -match 'DispatchVddCommandBuffer\s*\(') {
    throw 'Do not dispatch VDD commands inline from VirtualDisplayDriverIoDeviceControl; Win10 IddCx rejects re-entrant monitor creation.'
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
    throw 'Win10 requires the IddCx-owned IOCTL request to be completed before the monitor-command worker is enqueued.'
}

if ($workItem -match 'WdfRequestComplete') {
    throw 'The command worker must not retain or complete the IddCx-owned IOCTL request.'
}

if ($source -notmatch 'IsAdapterReady\(\)' -or
    $source -notmatch 'Adapter is ready for monitor commands' -or
    $workItem -notmatch 'WaitForReadyAdapter') {
    throw 'Monitor commands must wait for successful EvtIddCxAdapterInitFinished completion.'
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

Write-Host 'Win10 compatibility invariants passed: completed IOCTL before FIFO dispatch, single adapter registration across D3, and DISPLAY\ZAK2333.'
