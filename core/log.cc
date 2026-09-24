#include "log.hh"

#include <cstdarg>
#include <cstdio>

namespace logger
{
	static FILE* gFile = nullptr;
	static SRWLOCK gLock = SRWLOCK_INIT;

	void Open(const std::wstring& path)
	{
		AcquireSRWLockExclusive(&gLock);
		if (!gFile) {
			gFile = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
		}
		ReleaseSRWLockExclusive(&gLock);
	}

	void Write(const char* fmt, ...)
	{
		if (!gFile) {
			return;
		}

		char buffer[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buffer, sizeof(buffer), fmt, args);
		va_end(args);

		SYSTEMTIME t;
		GetLocalTime(&t);

		AcquireSRWLockExclusive(&gLock);
		fprintf(gFile, "%02u:%02u:%02u.%03u [%5lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), buffer);
		fflush(gFile);
		ReleaseSRWLockExclusive(&gLock);
	}

	std::string ToUtf8(const wchar_t* text)
	{
		if (!text || !*text) {
			return {};
		}

		const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
		if (size <= 1) {
			return {};
		}

		std::string result(static_cast<size_t>(size - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
		return result;
	}
}
