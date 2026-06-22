#pragma once

#include <windows.h>

using VddCommandAction = void (*)(HANDLE, wchar_t *);

void HandleReloadDriverCommand(HANDLE hPipe, wchar_t *param);
void HandleHdrPlusCommand(HANDLE hPipe, wchar_t *param);
void HandleSdr10Command(HANDLE hPipe, wchar_t *param);
void HandleCustomEdidCommand(HANDLE hPipe, wchar_t *param);
void HandlePreventSpoofCommand(HANDLE hPipe, wchar_t *param);
void HandleCeaOverrideCommand(HANDLE hPipe, wchar_t *param);
void HandleEdidProfileCommand(HANDLE hPipe, wchar_t *param);
void HandleVrrCommand(HANDLE hPipe, wchar_t *param);
void HandleHardwareCursorCommand(HANDLE hPipe, wchar_t *param);
void HandleD3DDeviceGpuCommand(HANDLE hPipe, wchar_t *param);
void HandleIddCxVersionCommand(HANDLE hPipe, wchar_t *param);
void HandleGetAssignedGpuCommand(HANDLE hPipe, wchar_t *param);
void HandleGetAllGpusCommand(HANDLE hPipe, wchar_t *param);
void HandleSetGpuCommand(HANDLE hPipe, wchar_t *param);
void HandleSetDisplayCountCommand(HANDLE hPipe, wchar_t *param);
void HandleGetSettingsCommand(HANDLE hPipe, wchar_t *param);
void HandlePingCommand(HANDLE hPipe, wchar_t *param);
void HandleCreateMonitorCommand(HANDLE hPipe, wchar_t *param);
void HandleDestroyMonitorCommand(HANDLE hPipe, wchar_t *param);
void HandleRefreshModesCommand(HANDLE hPipe, wchar_t *param);
void HandleSetModesCommand(HANDLE hPipe, wchar_t *param);
void HandleUnknownCommand(HANDLE hPipe, wchar_t *buffer);
