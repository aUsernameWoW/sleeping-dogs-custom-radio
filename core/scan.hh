#pragma once

#include <cstdint>

namespace scan
{
	// Finds an IDA-style pattern ("48 89 5C 24 ? 57") in the game exe's .text section.
	// Returns the match only if it is unique; logs the outcome under `name`.
	uint8_t* FindUnique(const char* name, const char* pattern);

	// Resolves the target of a RIP-relative operand whose 32-bit displacement is at `disp`
	// and whose instruction ends at `disp + 4 + trailing`.
	void* RipTarget(const uint8_t* disp, int trailing = 0);
}
