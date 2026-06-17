#include "Logger.h"

#include "EtwTrace.h"
#include "..\Util\StringConversion.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <mutex>
#include <shlobj.h>
#include <sstream>
#include <string>
#include <windows.h>

using namespace std;

extern std::wstring confpath;
extern std::atomic<bool> logsEnabled;
extern std::atomic<bool> debugLogs;
extern std::atomic<bool> sendLogsThroughPipe;
extern HANDLE g_pipeHandle;

static FILE* s_cachedLogFile = nullptr;
static wstring s_cachedLogDate;
static mutex s_logFileMutex;

static wstring GetFallbackLogDir()
{
	wchar_t programDataPath[MAX_PATH];
	if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, SHGFP_TYPE_CURRENT, programDataPath)))
	{
		return wstring(programDataPath) + L"\\VirtualDisplayDriver\\Logs";
	}

	wchar_t tempPath[MAX_PATH];
	DWORD len = GetTempPathW(MAX_PATH, tempPath);
	if (len > 0 && len < MAX_PATH)
	{
		return wstring(tempPath) + L"VirtualDisplayDriver\\Logs";
	}

	return wstring(L"C:\\Windows\\Temp\\VirtualDisplayDriver\\Logs");
}

void SendToPipe(const std::string& logMessage)
{
	if (g_pipeHandle != INVALID_HANDLE_VALUE)
	{
		DWORD bytesWritten;
		DWORD logMessageSize = static_cast<DWORD>(logMessage.size());
		WriteFile(g_pipeHandle, logMessage.c_str(), logMessageSize, &bytesWritten, NULL);
	}
}

void vddlog(const char* type, const char* message)
{
	VddEtwLog(type, message);

	if (!logsEnabled)
	{
		lock_guard<mutex> lock(s_logFileMutex);
		if (s_cachedLogFile != nullptr)
		{
			fclose(s_cachedLogFile);
			s_cachedLogFile = nullptr;
			s_cachedLogDate.clear();
		}
		return;
	}

	auto now = chrono::system_clock::now();
	auto in_time_t = chrono::system_clock::to_time_t(now);
	tm tm_buf;
	localtime_s(&tm_buf, &in_time_t);
	wchar_t date_str[11];
	wcsftime(date_str, sizeof(date_str) / sizeof(wchar_t), L"%Y-%m-%d", &tm_buf);
	wstring currentDate = date_str;

	string logType;
	switch (type[0])
	{
	case 'e':
		logType = "ERROR";
		break;
	case 'i':
		logType = "INFO";
		break;
	case 'p':
		logType = "PIPE";
		break;
	case 'd':
		logType = "DEBUG";
		break;
	case 'w':
		logType = "WARNING";
		break;
	case 't':
		logType = "TESTING";
		break;
	case 'c':
		logType = "COMPANION";
		break;
	default:
		logType = "UNKNOWN";
		break;
	}

	if (logType == "DEBUG" && !debugLogs)
	{
		return;
	}

	lock_guard<mutex> lock(s_logFileMutex);

	if (s_cachedLogFile == nullptr || s_cachedLogDate != currentDate)
	{
		if (s_cachedLogFile != nullptr)
		{
			fclose(s_cachedLogFile);
			s_cachedLogFile = nullptr;
		}

		wstring logsDir = confpath + L"\\Logs";
		bool useFallback = false;

		if (!CreateDirectoryW(confpath.c_str(), NULL))
		{
			DWORD err = GetLastError();
			useFallback = (err != ERROR_ALREADY_EXISTS);
		}

		if (!useFallback && !CreateDirectoryW(logsDir.c_str(), NULL))
		{
			DWORD err = GetLastError();
			useFallback = (err != ERROR_ALREADY_EXISTS);
		}

		if (useFallback)
		{
			logsDir = GetFallbackLogDir();
			size_t lastSlash = logsDir.find_last_of(L"\\");
			if (lastSlash != wstring::npos)
			{
				CreateDirectoryW(logsDir.substr(0, lastSlash).c_str(), NULL);
			}
			CreateDirectoryW(logsDir.c_str(), NULL);
		}

		wstring logPath = logsDir + L"\\log_" + currentDate + L".txt";
		string narrow_logPath = WStringToString(logPath);
		FILE* logFile = nullptr;
		errno_t fileErr = fopen_s(&logFile, narrow_logPath.c_str(), "a");

		if ((fileErr != 0 || logFile == nullptr) && !useFallback)
		{
			logsDir = GetFallbackLogDir();
			size_t lastSlash = logsDir.find_last_of(L"\\");
			if (lastSlash != wstring::npos)
			{
				CreateDirectoryW(logsDir.substr(0, lastSlash).c_str(), NULL);
			}
			CreateDirectoryW(logsDir.c_str(), NULL);

			logPath = logsDir + L"\\log_" + currentDate + L".txt";
			narrow_logPath = WStringToString(logPath);
			fileErr = fopen_s(&logFile, narrow_logPath.c_str(), "a");
		}

		if (fileErr != 0 || logFile == nullptr)
		{
			return;
		}

		s_cachedLogFile = logFile;
		s_cachedLogDate = currentDate;
	}

	FILE* logFile = s_cachedLogFile;

	stringstream ss;
	ss << put_time(&tm_buf, "%Y-%m-%d %X");

	fprintf(logFile, "[%s] [%s] %s\n", ss.str().c_str(), logType.c_str(), message);
	fflush(logFile);

	if (sendLogsThroughPipe && g_pipeHandle != INVALID_HANDLE_VALUE)
	{
		string logMessage = ss.str() + " [" + logType + "] " + message + "\n";
		DWORD bytesWritten;
		DWORD logMessageSize = static_cast<DWORD>(logMessage.size());
		WriteFile(g_pipeHandle, logMessage.c_str(), logMessageSize, &bytesWritten, NULL);
	}
}
