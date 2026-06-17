#include "EtwTrace.h"

TRACELOGGING_DEFINE_PROVIDER(
	g_VddEtwProvider,
	"ZakoTech.VDD",
	(0xb254994f, 0x46e6, 0x4719, 0x80, 0xa0, 0x0a, 0x3a, 0xa5, 0x0d, 0x6c, 0xe5));

void VddEtwRegister()
{
	TraceLoggingRegister(g_VddEtwProvider);
}

void VddEtwUnregister()
{
	TraceLoggingUnregister(g_VddEtwProvider);
}

static UCHAR VddTypeToEtwLevel(const char* type)
{
	if (type == nullptr || type[0] == '\0') return WINEVENT_LEVEL_INFO;
	switch (type[0])
	{
	case 'e': return WINEVENT_LEVEL_ERROR;
	case 'w': return WINEVENT_LEVEL_WARNING;
	case 'i':
	case 'c': return WINEVENT_LEVEL_INFO;
	case 'd':
	case 'p':
	case 't': return WINEVENT_LEVEL_VERBOSE;
	default: return WINEVENT_LEVEL_INFO;
	}
}

static const char* VddTypeToCategory(const char* type)
{
	if (type == nullptr || type[0] == '\0') return "log";
	switch (type[0])
	{
	case 'e': return "error";
	case 'w': return "warning";
	case 'i': return "info";
	case 'c': return "companion";
	case 'd': return "debug";
	case 'p': return "pipe";
	case 't': return "test";
	default:  return "log";
	}
}

void VddEtwLog(const char* type, const char* message)
{
	if (message == nullptr) return;

	if (!TraceLoggingProviderEnabled(g_VddEtwProvider, 0, 0))
	{
		return;
	}

	const UCHAR level = VddTypeToEtwLevel(type);
	const char* category = VddTypeToCategory(type);

	switch (level)
	{
	case WINEVENT_LEVEL_ERROR:
		TraceLoggingWrite(g_VddEtwProvider, "VddLog",
			TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
			TraceLoggingString(category, "Category"),
			TraceLoggingString(message, "Message"));
		break;
	case WINEVENT_LEVEL_WARNING:
		TraceLoggingWrite(g_VddEtwProvider, "VddLog",
			TraceLoggingLevel(WINEVENT_LEVEL_WARNING),
			TraceLoggingString(category, "Category"),
			TraceLoggingString(message, "Message"));
		break;
	case WINEVENT_LEVEL_VERBOSE:
		TraceLoggingWrite(g_VddEtwProvider, "VddLog",
			TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
			TraceLoggingString(category, "Category"),
			TraceLoggingString(message, "Message"));
		break;
	default:
		TraceLoggingWrite(g_VddEtwProvider, "VddLog",
			TraceLoggingLevel(WINEVENT_LEVEL_INFO),
			TraceLoggingString(category, "Category"),
			TraceLoggingString(message, "Message"));
		break;
	}
}

extern "C" BOOL WINAPI DllMain(
	_In_ HINSTANCE hInstance,
	_In_ UINT dwReason,
	_In_opt_ LPVOID lpReserved)
{
	UNREFERENCED_PARAMETER(hInstance);
	UNREFERENCED_PARAMETER(lpReserved);

	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		VddEtwRegister();
		break;
	case DLL_PROCESS_DETACH:
		VddEtwUnregister();
		break;
	}

	return TRUE;
}
