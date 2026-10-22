#pragma once

// Experimental external audio (the gpl/external-audio branch): another app's audio and its media session.
// For now diagnostics only: with [External] Enabled = 1 a thread logs the media sessions, captures the
// configured app (or a playback device) and logs the level it gets alongside the app's volume-mixer state,
// so one test shows whether a muted app can still be captured. Nothing reaches the game yet.

namespace external
{
	// Starts the diagnostics thread if enabled in the config.
	void Start();
}
