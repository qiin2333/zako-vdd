#include "StringConversion.h"

#include <windows.h>

std::string WStringToString(const std::wstring& wstr)
{
	if (wstr.empty())
	{
		return "";
	}

	int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
	if (sizeNeeded <= 0)
	{
		return "";
	}

	std::string str(sizeNeeded, 0);
	int converted = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &str[0], sizeNeeded, nullptr, nullptr);
	if (converted <= 0)
	{
		return "";
	}

	return str;
}
