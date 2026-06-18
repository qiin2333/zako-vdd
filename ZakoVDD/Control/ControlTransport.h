#pragma once

#include "..\Driver.h"

EVT_IDD_CX_DEVICE_IO_CONTROL VirtualDisplayDriverIoDeviceControl;

void StartNamedPipeServer();
void StopNamedPipeServer();
void SendLegacyPipeMessage(const char *message);
