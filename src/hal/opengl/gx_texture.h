#ifndef _MELEE_GX_TEXTURE_PC_H_
#define _MELEE_GX_TEXTURE_PC_H_

// Decoders for GameCube texture formats into RGBA8 + GL upload helpers.
// Covered: I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, C4, C8, C14X2, CMPR (S3TC
// DXT1 variant with swapped color interpolation). Mirrors the layout used by
// libogc/Dolphin so game .dat textures render correctly.

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decode a GC texture into a freshly malloc'd RGBA8 buffer.
// Caller frees with free(). Returns NULL on unsupported format.
// `tlut` + `tlut_fmt` are only used for palettized formats (C4/C8/C14X2):
// tlut_fmt 0 = IA8, 1 = RGB565, 2 = RGB5A3.
u8* gx_tex_decode(int gx_format, const u8* src, int width, int height,
                  const u8* tlut, int tlut_fmt);

// Size in bytes of the GC source image.
unsigned gx_tex_src_size(int gx_format, int width, int height);

// Upload an RGBA8 image to the currently bound GL texture (2D).
// Handles wrap/filter/anisotropy + optional mipmaps.
void gx_tex_upload_gl(unsigned gl_tex, const u8* rgba, int width, int height,
                      int wrap_s, int wrap_t, int min_filter, int mag_filter,
                      int use_mipmaps, float lod_bias, int max_aniso);

#ifdef __cplusplus
}
#endif

#endif
