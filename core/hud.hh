#pragma once

// The Flash side of the cover-art logo. RadioStations.swf places the logo's holder clip with a color
// transform that paints it white (mult 0,0,0,1, add 255,255,255,0: the game's logos are black shapes shown
// white), which turns a cover into a white square. Whenever the game sets the logo
// (Movie::Invoke("mc_RadioStations.SetTexture", "img://...")), this sets the holder's color transform:
// identity for our logo, the original white tint for every other station's.
//
// Hooks Scaleform::GFx::Movie::Invoke (a 3-instruction wrapper, found by signature in both builds) and uses
// Scaleform's public Value API through its vtables: ASMovieRootBase::GetVariable (+0x188),
// Value::ObjectInterface::GetCxform/SetCxform (+0x110/+0x118), ObjectRelease (+0x10). Layouts from the
// legacy PDB; the wrapper's own `jmp [r10+1C0h]` (Invoke_2) matches in the installed build.

namespace hud
{
	// `ourTexture`: the img:// name the widget uses for our station. Call after MH_Initialize.
	bool Install(const char* ourTexture);
}
