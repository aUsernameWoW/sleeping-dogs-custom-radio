#pragma once

// Which picture the station logo shows: the playing track's embedded cover art, else an image file named
// cover/folder/front/logo (.jpg/.jpeg/.png/.bmp/.gif) in its folder or any parent up to the music folder (so a
// logo.png in the music folder is the station's own logo), else nothing. Prepared on a worker thread when a
// track starts; d3d.cc uploads it on the render thread.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cover_image.hh"

namespace cover
{
	// `tracks[k - 1]` is track k. Starts the worker, which first prepares the music folder's own picture.
	void Start(std::vector<std::wstring> tracks, std::wstring root);

	// Track k (1-based) starts playing.
	void Request(uint32_t track);

	struct Current
	{
		std::shared_ptr<const coverimage::Image> mImage; // null until Start's first picture is ready
		uint32_t mGeneration = 0;                         // changes with every new picture
	};
	Current Get();
	uint32_t Generation();
}
