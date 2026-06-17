/*++

Module Name:

    EtwTrace.h

Abstract:

    Modern (TraceLogging) ETW provider for runtime trace capture of the
    ZakoTech Virtual Display Driver.

    This is independent of the legacy WPP framework defined in Trace.h.
    TraceLogging is header-only (no manifest, no build-step preprocessor)
    and emits no overhead when no listening session is enabled.

    Provider name : ZakoTech.VDD
    Provider GUID : {B254994F-46E6-4719-80A0-0A3AA50D6CE5}

    Capture example (PowerShell, admin):
        wpr -start docs\ZakoVDD.wprp
        ... reproduce issue ...
        wpr -stop trace.etl

    Decode:
        tracerpt trace.etl -of CSV -o trace.csv

Environment:

    Windows User-Mode Driver Framework 2

--*/

#pragma once

#include <windows.h>
#include <winmeta.h>            // WINEVENT_LEVEL_*
#include <TraceLoggingProvider.h>

TRACELOGGING_DECLARE_PROVIDER(g_VddEtwProvider);

// Lifecycle helpers - call from DllMain.
void VddEtwRegister();
void VddEtwUnregister();

// Bridge from the existing vddlog(type, message) call sites into ETW.
// type is the same single-char selector vddlog uses ('e','i','d','w','p','c','t').
// Safe to call before VddEtwRegister or after VddEtwUnregister - becomes a no-op.
void VddEtwLog(const char *type, const char *message);
