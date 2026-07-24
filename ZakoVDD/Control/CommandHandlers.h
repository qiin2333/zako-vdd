#pragma once

#include <wchar.h>

using VddCommandAction = void (*)(wchar_t *);

void HandleReloadDriverCommand(wchar_t *param);
void HandleHdrPlusCommand(wchar_t *param);
void HandleSdr10Command(wchar_t *param);
void HandleCustomEdidCommand(wchar_t *param);
void HandlePreventSpoofCommand(wchar_t *param);
void HandleCeaOverrideCommand(wchar_t *param);
void HandleEdidProfileCommand(wchar_t *param);
void HandleVrrCommand(wchar_t *param);
void HandleHardwareCursorCommand(wchar_t *param);
void HandleD3DDeviceGpuCommand(wchar_t *param);
void HandleIddCxVersionCommand(wchar_t *param);
void HandleGetAssignedGpuCommand(wchar_t *param);
void HandleGetAllGpusCommand(wchar_t *param);
void HandleSetGpuCommand(wchar_t *param);
void HandleSetDisplayCountCommand(wchar_t *param);
void HandleCreateMonitorCommand(wchar_t *param);
void HandleDestroyMonitorCommand(wchar_t *param);
void HandleRefreshModesCommand(wchar_t *param);
void HandleSetModesCommand(wchar_t *param);
void HandleUnknownCommand(wchar_t *buffer);
