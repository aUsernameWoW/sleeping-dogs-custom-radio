/* The bundled decoders' implementations (public domain / MIT-0, see THIRD-PARTY-NOTICES.md), compiled as C
   in their own translation unit with warnings off. stb_vorbis.c is a separate item in the project: it
   defines one-letter macros that must not leak into anything else. */

#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>

#define DR_FLAC_IMPLEMENTATION
#include <dr_flac.h>

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>
