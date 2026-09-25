#include "crash.hh"

#include <Windows.h>
#include <DbgHelp.h>

#include <atomic>
#include <cstdio>

#include "log.hh"

namespace crash
{
	namespace
	{
		constexpr int kMaxLogged = 4;
		constexpr int kMaxDumps = 2;
		constexpr int kMaxFrames = 20;

		wchar_t gDir[MAX_PATH] = {};
		std::atomic<int> gCount{ 0 };

		using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
			PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

		// "SDRadio.asi+0x1234", or the bare address outside any module.
		void Describe(DWORD64 address, char* out, size_t size)
		{
			HMODULE module = nullptr;
			wchar_t path[MAX_PATH];
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(address), &module) &&
				GetModuleFileNameW(module, path, MAX_PATH)) {
				const wchar_t* name = wcsrchr(path, L'\\');
				name = name ? name + 1 : path;
				char narrow[MAX_PATH];
				WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
				std::snprintf(out, size, "%s+0x%llX", narrow, static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
			}
			else {
				std::snprintf(out, size, "0x%llX (no module)", static_cast<unsigned long long>(address));
			}
		}

		// x64 unwind from the faulting context. Guarded: the stack of a crashed thread may be garbage.
		void LogStack(const CONTEXT& start)
		{
			CONTEXT ctx = start;
			char where[MAX_PATH + 32];
			__try {
				for (int i = 0; i < kMaxFrames && ctx.Rip; ++i) {
					Describe(ctx.Rip, where, sizeof(where));
					LOG("crash:   #%-2d %s", i, where);
					DWORD64 imageBase = 0;
					PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
					if (function) {
						void* handlerData = nullptr;
						DWORD64 establisher = 0;
						RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, function, &ctx, &handlerData, &establisher, nullptr);
					}
					else {
						// A leaf, or a jump into nowhere: the return address is on top of the stack.
						ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
						ctx.Rsp += 8;
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				LOG("crash:   (stack unreadable from here)");
			}
		}

		void WriteDump(EXCEPTION_POINTERS* info, int n)
		{
			HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
			auto write = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
			if (!write) {
				LOG("crash: no dbghelp, no dump");
				return;
			}
			wchar_t path[MAX_PATH];
			swprintf_s(path, L"%s\\SDRadio-crash-%d.dmp", gDir, n + 1);
			HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return;
			}
			MINIDUMP_EXCEPTION_INFORMATION exception{ GetCurrentThreadId(), info, FALSE };
			const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
			const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, type, &exception, nullptr, nullptr);
			CloseHandle(file);
			LOG("crash: dump %s: SDRadio-crash-%d.dmp", ok ? "written" : "FAILED", n + 1);
		}

		LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
		{
			const EXCEPTION_RECORD& record = *info->ExceptionRecord;
			if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION && record.ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
				record.ExceptionCode != EXCEPTION_PRIV_INSTRUCTION) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			const int n = gCount.fetch_add(1);
			if (n >= kMaxLogged) {
				return EXCEPTION_CONTINUE_SEARCH;
			}

			char where[MAX_PATH + 32];
			Describe(info->ContextRecord->Rip, where, sizeof(where));
			if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
				const ULONG_PTR kind = record.ExceptionInformation[0];
				LOG("crash: access violation (%s 0x%llX) at %s, thread %lu", kind == 0 ? "read" : kind == 1 ? "write" : "execute",
					static_cast<unsigned long long>(record.ExceptionInformation[1]), where, GetCurrentThreadId());
			}
			else {
				LOG("crash: exception 0x%08lX at %s, thread %lu", record.ExceptionCode, where, GetCurrentThreadId());
			}
			LogStack(*info->ContextRecord);
			if (n < kMaxDumps) {
				WriteDump(info, n);
			}
			return EXCEPTION_CONTINUE_SEARCH;
		}
	}

	void Install(const std::wstring& dir)
	{
		wcsncpy_s(gDir, dir.c_str(), _TRUNCATE);
		AddVectoredExceptionHandler(1, Handler);
	}
}
