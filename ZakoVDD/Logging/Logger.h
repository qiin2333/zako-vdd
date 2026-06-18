#pragma once

#include <sstream>
#include <string>

enum class VddLogLevel : unsigned char
{
	Critical,
	Error,
	Warning,
	Info,
	Debug,
	Verbose
};

bool VddLogIsEnabled(VddLogLevel level);
void VddLogWrite(VddLogLevel level, const char *sourceFile, const char *functionName, unsigned int line, const char *message);

#define VDD_LOG(level, message) \
	do \
	{ \
		const VddLogLevel vddLogLevel = (level); \
		if (VddLogIsEnabled(vddLogLevel)) \
		{ \
			VddLogWrite(vddLogLevel, __FILE__, __FUNCTION__, __LINE__, message); \
		} \
	} while (false)

#define VDD_LOG_LAZY(level, messageFactory) \
	do \
	{ \
		const VddLogLevel vddLogLevel = (level); \
		if (VddLogIsEnabled(vddLogLevel)) \
		{ \
			const std::string vddLogMessage = (messageFactory)(); \
			VddLogWrite(vddLogLevel, __FILE__, __FUNCTION__, __LINE__, vddLogMessage.c_str()); \
		} \
	} while (false)

#define VDD_LOG_DEBUG_LAZY(messageFactory) VDD_LOG_LAZY(VddLogLevel::Debug, messageFactory)

#define VDD_LOG_STREAM(level, messageExpression) \
	VDD_LOG_LAZY(level, [&]() -> std::string \
	{ \
		std::ostringstream vddLogStream; \
		vddLogStream << messageExpression; \
		return vddLogStream.str(); \
	})

#define VDD_LOG_CRITICAL_STREAM(messageExpression) VDD_LOG_STREAM(VddLogLevel::Critical, messageExpression)
#define VDD_LOG_ERROR_STREAM(messageExpression) VDD_LOG_STREAM(VddLogLevel::Error, messageExpression)
#define VDD_LOG_WARNING_STREAM(messageExpression) VDD_LOG_STREAM(VddLogLevel::Warning, messageExpression)
#define VDD_LOG_INFO_STREAM(messageExpression) VDD_LOG_STREAM(VddLogLevel::Info, messageExpression)
#define VDD_LOG_DEBUG_STREAM(messageExpression) VDD_LOG_STREAM(VddLogLevel::Debug, messageExpression)
#define VDD_LOG_VERBOSE_STREAM(messageExpression) VDD_LOG_STREAM(VddLogLevel::Verbose, messageExpression)

#define VDD_LOG_CRITICAL(message) VDD_LOG(VddLogLevel::Critical, message)
#define VDD_LOG_ERROR(message) VDD_LOG(VddLogLevel::Error, message)
#define VDD_LOG_WARNING(message) VDD_LOG(VddLogLevel::Warning, message)
#define VDD_LOG_INFO(message) VDD_LOG(VddLogLevel::Info, message)
#define VDD_LOG_DEBUG(message) VDD_LOG(VddLogLevel::Debug, message)
#define VDD_LOG_VERBOSE(message) VDD_LOG(VddLogLevel::Verbose, message)
