/* The bundled libraries' implementations (public domain / MIT-0 / MIT, see THIRD-PARTY-NOTICES.md),
   compiled as C in their own translation unit with warnings off. stb_vorbis.c is a separate item in the
   project: it defines one-letter macros that must not leak into anything else. */

#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>

#define DR_FLAC_IMPLEMENTATION
#include <dr_flac.h>

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

/* Cover art: decoding (only the formats covers come in; files are read by us, so no stdio), resizing, and
   BC3 compression for the HUD logo texture. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include <stb_image.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>
