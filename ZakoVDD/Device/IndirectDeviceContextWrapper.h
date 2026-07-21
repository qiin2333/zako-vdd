#pragma once

#include "IndirectDeviceContext.h"

struct IndirectDeviceContextWrapper
{
	Microsoft::IndirectDisp::IndirectDeviceContext* pContext;

	void Cleanup()
	{
		delete pContext;
		pContext = nullptr;
	}
};

// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object.
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);
