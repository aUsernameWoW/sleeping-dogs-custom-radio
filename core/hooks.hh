#pragma once

namespace hooks
{
	// Finds the game functions by signature and hooks them. Call from DllMain, before the game runs.
	void Install();
}
