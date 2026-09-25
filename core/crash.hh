#pragma once

// Crash diagnostics, so one test round is enough: a vectored exception handler that logs the first few
// access violations (what was accessed, where, the stack as module+offset) and writes the first two as
// SDRadio-crash-<n>.dmp next to the .asi (tools\dump.ps1 reads them). It only observes: exceptions go on
// to whoever handles them (the game's handlers, SEH guards). Only installed with logging on.

#include <string>

namespace crash
{
	void Install(const std::wstring& dir);
}
