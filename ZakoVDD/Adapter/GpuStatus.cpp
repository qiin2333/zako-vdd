#include "GpuStatus.h"

#include "..\Core\DriverOptions.h"
#include "..\Logging\Logger.h"
#include "..\Util\StringConversion.h"

#include <AdapterOption.h>
#include <exception>
#include <string>

using namespace std;

extern DriverOptions Options;

LUID getSetAdapterLuid()
{
	AdapterOption &adapterOption = Options.Adapter;

	if (!adapterOption.hasTargetAdapter)
	{
		VDD_LOG_ERROR("No Gpu Found/Selected");
	}

	return adapterOption.adapterLuid;
}

void GetGpuInfo()
{
	AdapterOption &adapterOption = Options.Adapter;

	if (!adapterOption.hasTargetAdapter)
	{
		VDD_LOG_ERROR("No GPU found or set.");
		return;
	}

	try
	{
		string utf8_desc = WStringToString(adapterOption.target_name);
		LUID luid = getSetAdapterLuid();
		string logtext = "ASSIGNED GPU: " + utf8_desc +
						 " (LUID: " + std::to_string(luid.LowPart) + "-" + std::to_string(luid.HighPart) + ")";
		VDD_LOG_INFO(logtext.c_str());
	}
	catch (const exception &e)
	{
		VDD_LOG_ERROR(("Error: " + string(e.what())).c_str());
	}
}
