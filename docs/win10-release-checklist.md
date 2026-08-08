# Win10 compatibility and release checklist

This document records the failures found while validating the `v0.15.4` compatibility line on Windows 10 22H2 and defines the release gate for later `v0.15.x` builds.

## What went wrong

### The status code was initially misidentified

`IddCxMonitorCreate` returned `0xC0000476`. The authoritative WDK `ntstatus.h` definition is `STATUS_OPERATION_IN_PROGRESS`, not a structure-size error. Always decode the exact NTSTATUS value from the WDK headers before changing IddCx structures.

### A successful build did not model the Win10 IddCx runtime

The driver compiled with a current WDK, but Windows 10 22H2 loaded IddCx 1.5.1. Windows 11 uses a newer runtime and accepted a call path that failed on Win10. WDK compilation proves API and ABI compatibility; it does not prove callback-ordering or re-entrancy behavior on an older runtime.

### Changing the control transport changed the callback context

`v0.14.3` handled commands on a named-pipe worker thread. The `v0.15.x` IOCTL transport initially dispatched monitor commands inline from `EvtIddCxDeviceIoControl`. Calling `IddCxMonitorCreate` on that IddCx callback stack returned `STATUS_OPERATION_IN_PROGRESS` on Win10.

The first attempted fix moved dispatch to a WDF work item but kept the IddCx-owned IOCTL request pending until the worker finished. That changed the thread, not the IddCx operation lifetime, and the exact `v0.15.5` CI package still returned `0xC0000476`. The corrected path copies the command into a persistent FIFO, completes the IOCTL request first, and only then enqueues the passive worker. Monitor-management commands additionally wait until `EvtIddCxAdapterInitFinished` has reported success.

The exact `v0.15.6` release exposed a second Win10-only cold-boot failure. The device entered idle D3 before a monitor command arrived; opening the control interface woke it to D0, where the driver called `IddCxAdapterInitAsync` a second time. Win10 returned `STATUS_ALREADY_REGISTERED` (`0xC0000718`), and the following monitor creation remained blocked with `STATUS_OPERATION_IN_PROGRESS`. An IddCx adapter registration survives the D3 transition, so initialization is now idempotent for the device lifetime and D0 exit no longer clears the adapter-initialized state.

Do not explicitly set `ExecutionLevel` on the UMDF work-item object. Its callback is already passive; Win10 rejects that object attribute with `STATUS_WDF_EXECUTION_LEVEL_INVALID` (`0xC0200211`) and leaves the adapter at Device Manager Code 31.

### Display name and hardware ID are separate EDID fields

The EDID text descriptor already contained `Zako HDR`, while the manufacturer and product bytes were still overwritten with the legacy `MTT1337` values. Win10 therefore enumerated `DISPLAY\MTT1337`. The compatibility line now uses the same identity as v0.17.2: manufacturer `ZAK`, product `0x2333`, exposed as `DISPLAY\ZAK2333`.

### Windows driver ranking hid local test builds

An installed release package can outrank a locally built package even after `pnputil /add-driver`. Before a compatibility test, remove the old OEM package and verify the active INF, DLL hash, driver version, and certificate. Do not infer that the new binary loaded from a successful install command alone.

### DriverVer was a Win10 runtime compatibility input

The public `v0.14.3` package (`14.24.29.188`) repeatedly enumerated its monitor on the same Win10 22H2 VM. Rebuilding the same source and matched toolchain with `99.x` failed; changing only the numeric version back to `14.24.29.188` passed, including with the current package date. Public `v0.14.4` and `v0.15.x` packages began using `100.0.x.x` and reproduced the failure. The date, DLL logic, IddCx import surface, and hardware ID were not the differentiator in that A/B.

Win10 maintenance artifacts therefore use `15.0.0.<run>` for CI and `15.0.15.<patch>` for `v0.15.<patch>` tags. Never restore the `99.x`/`100.x` namespace on this line. Because Windows ranks the already-published `100.0.15.x` packages above the corrected version, upgrades must explicitly replace the device package; plain `pnputil /add-driver` is insufficient.

### Stacked PRs can bypass the intended workflow trigger

The build workflow listens to pull requests whose base is `win10` (among other maintained branches). A PR stacked on another feature branch does not match that trigger. Retargeting or marking a PR ready also uses event types outside GitHub's default pull-request trigger set. The workflow therefore listens to `edited` and `ready_for_review` explicitly. Before merge or release, retarget the final PR to `win10` and require its checks to complete.

PR builds use the standard `pull_request` event and never receive the production signing secret. The former `pull_request_target` configuration checked out and built PR-controlled code in a privileged base-repository context, which was both unreliable for the Win10 branch and an unnecessary secret-exposure risk. Signing is limited to protected branch and tag builds.

## CI guard

`.github/scripts/Test-Win10Compatibility.ps1` runs for `win10`, PRs targeting `win10`, and `v0.15.*` tags. It rejects these known regressions:

- inline command dispatch from `VirtualDisplayDriverIoDeviceControl`;
- removal of the FIFO WDF work-item dispatch path;
- enqueuing monitor work before completing the IddCx-owned IOCTL request;
- retaining/completing the IOCTL request from the worker;
- removal of the adapter-ready gate;
- duplicate `IddCxAdapterInitAsync` registration during D0 wake;
- clearing the adapter-initialized state during an idle D3 transition;
- an explicit execution level on the UMDF work item;
- restoration of the `DISPLAY\MTT1337` EDID bytes;
- a Win10 artifact using the proven-bad `99.x` or `100.x` DriverVer namespace;
- a `v0.15.*` tag whose commit is not contained in `origin/win10`.

The guard is intentionally static. GitHub-hosted Windows runners do not reproduce the Win10 IddCx runtime, so this check cannot replace the VM smoke test.

## Required VM smoke test

Before creating a `v0.15.x` tag:

1. Use Windows 10 Pro 22H2 build 19045 with test signing enabled and Secure Boot disabled for test-signed packages.
2. Remove the previously installed ZakoVDD OEM package, then install the exact CI artifact being tested.
3. Record the active INF version, DLL SHA-256, certificate, Windows build, and IddCx version.
4. Send `CREATEMONITOR` and verify monitor creation plus arrival.
5. Send `SETMODES 1920x1080x60,2560x1440x60` and verify re-enumeration.
6. Assert that PnP reports `DISPLAY\ZAK2333`, the desktop mode is 1920x1080, and the shared texture reaches 1920x1080.
7. Archive the driver log, test transcript, and `setupapi.dev.log`.

The durable fully automated option is a self-hosted Win10 22H2 runner or lab machine. It should consume the unsigned/test-signed build artifact, run the steps above, upload the evidence, and be configured as a required environment/check before the release job. Until such a runner exists, release approval must include a link to the archived VM evidence.

## Tag and release verification

1. Confirm the release commit is contained in `origin/win10`.
2. Create the tag only after the PR checks pass: `v0.15.<patch>`.
3. Wait for both the build and release jobs.
4. Download `zakovdd.zip` and verify it contains the DLL, INF, catalog, settings, and certificate.
5. Verify the stamped INF version is `15.0.15.<patch>` and the catalog signature is valid.
6. Record the release asset SHA-256 and confirm the Sunshine `vdd-win10` notification ran.
