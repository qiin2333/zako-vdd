#pragma once

#include <windows.h>

void ReloadDriver(HANDLE hPipe);
void DispatchVddCommandBuffer(HANDLE hPipeForResponse, wchar_t* buffer);
