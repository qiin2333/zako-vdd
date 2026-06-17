#include "CommandDispatcher.h"

#include "CommandHandlers.h"

#include <cwchar>

struct Command
{
	const wchar_t *name;
	size_t length;
	VddCommandAction action;
};

void DispatchVddCommandBuffer(HANDLE hPipeForResponse, wchar_t *buffer)
{
	Command commands[] = {
		{L"RELOAD_DRIVER", 13, HandleReloadDriverCommand},
		{L"LOG_DEBUG", 9, HandleLogDebugCommand},
		{L"LOGGING", 7, HandleLoggingCommand},
		{L"HDRPLUS", 7, HandleHdrPlusCommand},
		{L"SDR10", 5, HandleSdr10Command},
		{L"CUSTOMEDID", 10, HandleCustomEdidCommand},
		{L"PREVENTSPOOF", 12, HandlePreventSpoofCommand},
		{L"CEAOVERRIDE", 11, HandleCeaOverrideCommand},
		{L"EDIDPROFILE", 11, HandleEdidProfileCommand},
		{L"VRR", 3, HandleVrrCommand},
		{L"HARDWARECURSOR", 14, HandleHardwareCursorCommand},
		{L"D3DDEVICEGPU", 12, HandleD3DDeviceGpuCommand},
		{L"IDDCXVERSION", 12, HandleIddCxVersionCommand},
		{L"GETASSIGNEDGPU", 14, HandleGetAssignedGpuCommand},
		{L"GETALLGPUS", 10, HandleGetAllGpusCommand},
		{L"SETGPU", 6, HandleSetGpuCommand},
		{L"SETDISPLAYCOUNT", 15, HandleSetDisplayCountCommand},
		{L"GETSETTINGS", 11, HandleGetSettingsCommand},
		{L"PING", 4, HandlePingCommand},
		{L"REFRESHMODES", 12, HandleRefreshModesCommand},
		{L"SETMODES", 8, HandleSetModesCommand},
		{L"CREATEMONITOR", 13, HandleCreateMonitorCommand},
		{L"DESTROYMONITOR", 14, HandleDestroyMonitorCommand},
	};

	for (const auto &cmd : commands)
	{
		if (wcsncmp(buffer, cmd.name, cmd.length) == 0)
		{
			wchar_t *param = buffer + cmd.length;
			if (*param == L' ')
			{
				param++;
			}
			cmd.action(hPipeForResponse, param);
			return;
		}
	}

	HandleUnknownCommand(hPipeForResponse, buffer);
}
