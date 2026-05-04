// Shared IOCTL contract between the ZakoVDD UMDF driver and external callers
// (currently Sunshine). This header MUST stay byte-compatible across both
// projects; if you change anything here, mirror the change to the consumer
// (e.g. Sunshine src/display_device/vdd_control_ioctl.h).
//
// Why IOCTL instead of the legacy named pipe (\\.\pipe\ZakoVDDPipe):
//   The pipe server lives inside the WUDFHost.exe process that hosts the
//   indirect display driver. When the last IDDCX monitor is destroyed, the
//   reflector eventually recycles WUDFHost.exe and the pipe vanishes,
//   leaving subsequent connect attempts to time out for ~6s before a costly
//   DevManView disable_enable kicks the device back to life. Using a control
//   device interface lets CreateFile() PnP-wake the driver transparently and
//   removes the race entirely.

#pragma once

#if !defined(_WIN32) && !defined(_WIN64)
#error "vdd_control_ioctl.h targets Windows-only consumers (UMDF driver / Sunshine display_device)."
#endif

#include <initguid.h>
#include <Windows.h>

// {DA9F8C2B-7E4F-49A1-9D4E-6F2B0E1A0C4D}
DEFINE_GUID(GUID_DEVINTERFACE_ZAKO_VDD_CONTROL,
    0xDA9F8C2B, 0x7E4F, 0x49A1, 0x9D, 0x4E, 0x6F, 0x2B, 0x0E, 0x1A, 0x0C, 0x4D);

// Single dispatch IOCTL. The input buffer is a null-terminated UTF-16 string
// using the same command grammar as the legacy named pipe protocol so the
// in-driver dispatcher can be shared verbatim. The output buffer (optional)
// receives a response payload (UTF-8 or UTF-16 depending on the command;
// today only PING/GETSETTINGS write a response and the SDR/HDR commands
// do not, matching the pipe behaviour).
//
// METHOD_BUFFERED: the kernel copies the input/output buffers, so the
// driver works on stable, non-volatile memory and we don't have to worry
// about the caller's buffer disappearing mid-handler.
//
// FILE_WRITE_DATA matches the pipe's GENERIC_READ|GENERIC_WRITE access mask
// expectations and matches the SDDL-allows-everyone semantics of the legacy
// pipe (D:(A;;GA;;;WD)). If you tighten this later remember to update both
// the driver registration and the Sunshine SetupDi code path.
#define ZAKO_VDD_DEVICE_TYPE   FILE_DEVICE_UNKNOWN

#define IOCTL_VDD_COMMAND \
    CTL_CODE(ZAKO_VDD_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA)

// Optional probe IOCTL. Returns STATUS_SUCCESS with no payload if the
// driver is alive. Sunshine uses this as a cheap "is the IOCTL channel
// available?" check so it can short-circuit to disable_enable when the
// driver is missing instead of hanging on slow command IOCTLs.
#define IOCTL_VDD_PING \
    CTL_CODE(ZAKO_VDD_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)
