#pragma once

#include <string>

void SendToPipe(const std::string& logMessage);
void vddlog(const char* type, const char* message);
