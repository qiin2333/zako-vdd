#pragma once

#include <string>

bool EnabledQuery(const std::wstring& settingKey);
int GetIntegerSetting(const std::wstring& settingKey);
std::wstring GetStringSetting(const std::wstring& settingKey);

bool UpdateXmlToggleSetting(bool toggle, const wchar_t* variable);
bool UpdateXmlGpuSetting(const wchar_t* gpuName);
bool UpdateXmlDisplayCountSetting(int displayCount);

bool initpath();
void LoadDriverSettings();
void loadSettings();
