#pragma once

#include <Windows.h>

#include <string>

namespace logger
{
	// Opens (truncates) the log file. Until this is called every log call is a no-op.
	void Open(const std::wstring& path);

	void Write(const char* fmt, ...);

	std::string ToUtf8(const wchar_t* text);
}

#define LOG(fmt, ...) ::logger::Write(fmt, ##__VA_ARGS__)
