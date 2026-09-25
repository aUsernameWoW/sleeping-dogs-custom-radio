#pragma once

// Puts the cover art into the HUD. The station borrows the HKPD scanner's logo (Logo_HKPDScanner in
// Radio_HKPDScanner_TexturePack, seen only in cop-scanner mode). When the widget loads that pack, the game
// creates the logo as an immutable 128×64 BC3 texture (Illusion::TexturePlat::CreateResources →
// ID3D11Device::CreateTexture2D with the pixels as initial data); we recognize it by size, format and the
// hash of those pixels and create ours instead (cover_image.hh: 512×256, mip-mapped, updatable). Scaleform
// wraps whatever texture comes back, so later pictures are simply written into it with UpdateSubresource,
// on the render thread (from the first PSSetShaderResources of the immediate context after a change).
//
// Everything goes through D3D11 itself (the game's D3D11CreateDevice import, the device's and context's
// methods), no game addresses, so it's the same under ReShade and DXVK (Proton/CrossOver).

namespace d3d
{
	// Patches the game's D3D11CreateDevice import; the method hooks follow once the device exists. Call
	// after MH_Initialize, before the game creates its device.
	bool Install();
}
