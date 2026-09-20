#include "gx_texture.h"
#include "gl_loader.h"

#include <stdlib.h>
#include <string.h>

// GX texture format ids (subset of GXTexFmt we support).
#define TF_I4 0x0
#define TF_I8 0x1
#define TF_IA4 0x2
#define TF_IA8 0x3
#define TF_RGB565 0x4
#define TF_RGB5A3 0x5
#define TF_RGBA8 0x6
#define TF_C4 0x8
#define TF_C8 0x9
#define TF_C14X2 0xA
#define TF_CMPR 0xE

static unsigned be16(const u8* p)
{
    return ((unsigned) p[0] << 8) | p[1];
}

static unsigned expand5(unsigned v)
{
    return (v << 3) | (v >> 2);
}
static unsigned expand6(unsigned v)
{
    return (v << 2) | (v >> 4);
}
static unsigned expand4(unsigned v)
{
    return (v << 4) | v;
}
static unsigned expand3(unsigned v)
{
    return (v << 5) | (v << 2) | (v >> 1);
}

static void put_px(u8* dst, int stride, int x, int y, unsigned r, unsigned g,
                   unsigned b, unsigned a)
{
    u8* p = dst + y * stride + x * 4;
    p[0] = (u8) r;
    p[1] = (u8) g;
    p[2] = (u8) b;
    p[3] = (u8) a;
}

// ---- palette entry decode ----
static void tlut_entry(const u8* tlut, int idx, int fmt, unsigned* r,
                       unsigned* g, unsigned* b, unsigned* a)
{
    if (fmt == 0) { // IA8
        *a = tlut[idx * 2];
        *r = *g = *b = tlut[idx * 2 + 1];
    } else if (fmt == 1) { // RGB565
        unsigned v = be16(tlut + idx * 2);
        *r = expand5((v >> 11) & 0x1F);
        *g = expand6((v >> 5) & 0x3F);
        *b = expand5(v & 0x1F);
        *a = 255;
    } else { // RGB5A3
        unsigned v = be16(tlut + idx * 2);
        if (v & 0x8000) {
            *a = 255;
            *r = expand5((v >> 10) & 0x1F);
            *g = expand5((v >> 5) & 0x1F);
            *b = expand5(v & 0x1F);
        } else {
            *a = expand3((v >> 12) & 0x7);
            *r = expand4((v >> 8) & 0xF);
            *g = expand4((v >> 4) & 0xF);
            *b = expand4(v & 0xF);
        }
    }
}

// ---- DXT1 block with GameCube's REVERSED color comparison ----
// PC DXT1: c0 > c1 means 4 opaque colors. GC CMPR: c1 > c0 means opaque,
// otherwise color #3 is transparent. Getting this backwards breaks every
// CMPR texture (most of Melee's stage/character textures).
static void decode_cmpr_block(u8* dst, int stride, int bx, int by,
                              const u8* src)
{
    unsigned c0 = be16(src);
    unsigned c1 = be16(src + 2);
    unsigned r0 = expand5((c0 >> 11) & 0x1F);
    unsigned g0 = expand6((c0 >> 5) & 0x3F);
    unsigned b0 = expand5(c0 & 0x1F);
    unsigned r1 = expand5((c1 >> 11) & 0x1F);
    unsigned g1 = expand6((c1 >> 5) & 0x3F);
    unsigned b1 = expand5(c1 & 0x1F);
    unsigned bits = ((unsigned) src[4] << 24) | ((unsigned) src[5] << 16) |
                    ((unsigned) src[6] << 8) | src[7];
    int opaque = (c1 > c0) ? 1 : 0;
    int y, x;
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            unsigned sel = (bits >> (2 * (15 - (y * 4 + x)))) & 3;
            unsigned r = 0, g = 0, b = 0, a = 255;
            if (sel == 0) {
                r = r0;
                g = g0;
                b = b0;
            } else if (sel == 1) {
                r = r1;
                g = g1;
                b = b1;
            } else if (opaque || sel == 2) {
                int div = (sel == 2 && !opaque) ? 2 : 3;
                if (opaque) {
                    if (sel == 2) {
                        r = (2 * r0 + r1) / 3;
                        g = (2 * g0 + g1) / 3;
                        b = (2 * b0 + b1) / 3;
                    } else {
                        r = (r0 + 2 * r1) / 3;
                        g = (g0 + 2 * g1) / 3;
                        b = (b0 + 2 * b1) / 3;
                    }
                } else { // sel == 2, transparent mode: 50% mix
                    r = (r0 + r1) / div;
                    g = (g0 + g1) / div;
                    b = (b0 + b1) / div;
                }
            } else { // sel == 3 in transparent mode
                a = 0;
            }
            put_px(dst, stride, bx + x, by + y, r, g, b, a);
        }
    }
}

u8* gx_tex_decode(int gx_format, const u8* src, int width, int height,
                  const u8* tlut, int tlut_fmt)
{
    int stride = width * 4;
    u8* dst = (u8*) malloc((size_t) stride * (unsigned) height);
    int x, y, ix, iy;
    if (!dst || !src || width <= 0 || height <= 0) {
        free(dst);
        return NULL;
    }
    memset(dst, 0, (size_t) stride * (unsigned) height);

    switch (gx_format) {
    case TF_I4:
        for (y = 0; y < height; y += 8)
            for (x = 0; x < width; x += 8)
                for (iy = 0; iy < 8; iy++) {
                    for (ix = 0; ix < 8; ix += 2) {
                        u8 v = *src++;
                        unsigned h0 = v >> 4, h1 = v & 0xF;
                        if (x + ix < width && y + iy < height) {
                            unsigned g0 = h0 * 17;
                            put_px(dst, stride, x + ix, y + iy, g0, g0, g0,
                                   255);
                        }
                        if (x + ix + 1 < width && y + iy < height) {
                            unsigned g1 = h1 * 17;
                            put_px(dst, stride, x + ix + 1, y + iy, g1, g1,
                                   g1, 255);
                        }
                    }
                }
        break;
    case TF_I8:
        for (y = 0; y < height; y += 4)
            for (x = 0; x < width; x += 8)
                for (iy = 0; iy < 4; iy++) {
                    for (ix = 0; ix < 8; ix++) {
                        u8 v = *src++;
                        if (x + ix < width && y + iy < height)
                            put_px(dst, stride, x + ix, y + iy, v, v, v,
                                   255);
                    }
                }
        break;
    case TF_IA4:
        for (y = 0; y < height; y += 4)
            for (x = 0; x < width; x += 8)
                for (iy = 0; iy < 4; iy++) {
                    for (ix = 0; ix < 8; ix++) {
                        u8 v = *src++;
                        if (x + ix < width && y + iy < height) {
                            unsigned a = expand4(v >> 4);
                            unsigned l = expand4(v & 0xF);
                            put_px(dst, stride, x + ix, y + iy, l, l, l,
                                   a);
                        }
                    }
                }
        break;
    case TF_IA8:
        for (y = 0; y < height; y += 4)
            for (x = 0; x < width; x += 4)
                for (iy = 0; iy < 4; iy++) {
                    for (ix = 0; ix < 4; ix++) {
                        u8 a = src[0], l = src[1];
                        src += 2;
                        if (x + ix < width && y + iy < height)
                            put_px(dst, stride, x + ix, y + iy, l, l, l,
                                   a);
                    }
                }
        break;
    case TF_RGB565:
        for (y = 0; y < height; y += 4)
            for (x = 0; x < width; x += 4)
                for (iy = 0; iy < 4; iy++) {
                    for (ix = 0; ix < 4; ix++) {
                        unsigned v = be16(src);
                        src += 2;
                        if (x + ix < width && y + iy < height) {
                            put_px(dst, stride, x + ix, y + iy,
                                   expand5((v >> 11) & 0x1F),
                                   expand6((v >> 5) & 0x3F),
                                   expand5(v & 0x1F), 255);
                        }
                    }
                }
        break;
    case TF_RGB5A3:
        for (y = 0; y < height; y += 4)
            for (x = 0; x < width; x += 4)
                for (iy = 0; iy < 4; iy++) {
                    for (ix = 0; ix < 4; ix++) {
                        unsigned v = be16(src);
                        src += 2;
                        if (x + ix < width && y + iy < height) {
                            unsigned r, g, b, a;
                            if (v & 0x8000) {
                                a = 255;
                                r = expand5((v >> 10) & 0x1F);
                                g = expand5((v >> 5) & 0x1F);
                                b = expand5(v & 0x1F);
                            } else {
                                a = expand3((v >> 12) & 0x7);
                                r = expand4((v >> 8) & 0xF);
                                g = expand4((v >> 4) & 0xF);
                                b = expand4(v & 0xF);
                            }
                            put_px(dst, stride, x + ix, y + iy, r, g, b,
                                   a);
                        }
                    }
                }
        break;
    case TF_RGBA8:
        for (y = 0; y < height; y += 4) {
            for (x = 0; x < width; x += 4) {
                const u8* ar = src;
                const u8* gb = src + 32;
                for (iy = 0; iy < 4; iy++) {
                    for (ix = 0; ix < 4; ix++) {
                        if (x + ix < width && y + iy < height) {
                            put_px(dst, stride, x + ix, y + iy,
                                   ar[ix * 2 + 1], gb[ix * 2],
                                   gb[ix * 2 + 1], ar[ix * 2]);
                        }
                    }
                    ar += 8;
                    gb += 8;
                }
                src += 64;
            }
        }
        break;
    case TF_C4:
        if (!tlut) {
            free(dst);
            return NULL;
        }
        for (y = 0; y < height; y += 8)
            for (x = 0; x < width; x += 8)
                for (iy = 0; iy < 8; iy++) {
                    for (ix = 0; ix < 8; ix += 2) {
                        u8 v = *src++;
                        unsigned r, g, b, a;
                        if (x + ix < width && y + iy < height) {
                            tlut_entry(tlut, v >> 4, tlut_fmt, &r, &g, &b,
                                       &a);
                            put_px(dst, stride, x + ix, y + iy, r, g, b,
                                   a);
                        }
                        if (x + ix + 1 < width && y + iy < height) {
                            tlut_entry(tlut, v & 0xF, tlut_fmt, &r, &g, &b,
                                       &a);
                            put_px(dst, stride, x + ix + 1, y + iy, r, g,
                                   b, a);
                        }
                    }
                }
        break;
    case TF_C8:
        if (!tlut) {
            free(dst);
            return NULL;
        }
        for (y = 0; y < height; y += 4)
            for (x = 0; x < width; x += 8)
                for (iy = 0; iy < 4; iy++) {
                    for (ix = 0; ix < 8; ix++) {
                        u8 idx = *src++;
                        if (x + ix < width && y + iy < height) {
                            unsigned r, g, b, a;
                            tlut_entry(tlut, idx, tlut_fmt, &r, &g, &b,
                                       &a);
                            put_px(dst, stride, x + ix, y + iy, r, g, b,
                                   a);
                        }
                    }
                }
        break;
    case TF_C14X2:
        if (!tlut) {
            free(dst);
            return NULL;
        }
        for (y = 0; y < height; y += 4)
            for (x = 0; x < width; x += 4)
                for (iy = 0; iy < 4; iy++) {
                    for (ix = 0; ix < 4; ix++) {
                        unsigned v = be16(src);
                        src += 2;
                        if (x + ix < width && y + iy < height) {
                            unsigned r, g, b, a;
                            tlut_entry(tlut, v & 0x3FFF, tlut_fmt, &r, &g,
                                       &b, &a);
                            put_px(dst, stride, x + ix, y + iy, r, g, b,
                                   a);
                        }
                    }
                }
        break;
    case TF_CMPR:
        for (y = 0; y < height; y += 8) {
            for (x = 0; x < width; x += 8) {
                // GC stores the 4 DXT1 sub-blocks in row-major order:
                // TL, TR, BL, BR.
                if (x + 4 <= width && y + 4 <= height)
                    decode_cmpr_block(dst, stride, x, y, src);
                src += 8;
                if (x + 8 <= width && y + 4 <= height)
                    decode_cmpr_block(dst, stride, x + 4, y, src);
                src += 8;
                if (x + 4 <= width && y + 8 <= height)
                    decode_cmpr_block(dst, stride, x, y + 4, src);
                src += 8;
                if (x + 8 <= width && y + 8 <= height)
                    decode_cmpr_block(dst, stride, x + 4, y + 4, src);
                src += 8;
            }
        }
        break;
    default:
        free(dst);
        return NULL;
    }
    return dst;
}

unsigned gx_tex_src_size(int gx_format, int width, int height)
{
    int tw = 4, th = 4, bpt = 32; // tile w/h, bytes per tile
    switch (gx_format) {
    case TF_I4:
        tw = 8;
        th = 8;
        break;
    case TF_I8:
    case TF_IA4:
    case TF_C8:
        tw = 8;
        th = 4;
        break;
    case TF_IA8:
    case TF_RGB565:
    case TF_RGB5A3:
    case TF_C14X2:
        tw = 4;
        th = 4;
        break;
    case TF_RGBA8:
        tw = 4;
        th = 4;
        bpt = 64;
        break;
    case TF_C4:
        tw = 8;
        th = 8;
        break;
    case TF_CMPR:
        tw = 8;
        th = 8;
        break;
    default:
        return 0;
    }
    {
        int ntx = (width + tw - 1) / tw;
        int nty = (height + th - 1) / th;
        return (unsigned) (ntx * nty * bpt);
    }
}

void gx_tex_upload_gl(unsigned gl_tex, const u8* rgba, int width, int height,
                      int wrap_s, int wrap_t, int min_filter, int mag_filter,
                      int use_mipmaps, float lod_bias, int max_aniso)
{
    GLint gl_wrap_s =
        (wrap_s == 1) ? GL_REPEAT : ((wrap_s == 2) ? GL_MIRRORED_REPEAT
                                                  : GL_CLAMP_TO_EDGE);
    GLint gl_wrap_t =
        (wrap_t == 1) ? GL_REPEAT : ((wrap_t == 2) ? GL_MIRRORED_REPEAT
                                                  : GL_CLAMP_TO_EDGE);
    GLint gl_min, gl_mag;
    glBindTexture(GL_TEXTURE_2D, (GLuint) gl_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (use_mipmaps) {
        // min_filter: 0 near, 1 linear, 2..5 mipped variants
        if (min_filter <= 1)
            gl_min = (min_filter == 0) ? GL_NEAREST : GL_LINEAR;
        else if (min_filter == 2)
            gl_min = GL_NEAREST_MIPMAP_NEAREST;
        else if (min_filter == 3)
            gl_min = GL_LINEAR_MIPMAP_NEAREST;
        else if (min_filter == 4)
            gl_min = GL_NEAREST_MIPMAP_LINEAR;
        else
            gl_min = GL_LINEAR_MIPMAP_LINEAR;
    } else {
        gl_min = (min_filter == 0) ? GL_NEAREST : GL_LINEAR;
    }
    gl_mag = (mag_filter == 0) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap_t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_min);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_mag);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei) width,
                 (GLsizei) height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (use_mipmaps)
        _glGenerateMipmap(GL_TEXTURE_2D);
    (void) lod_bias;
    (void) max_aniso; // TODO: picks up GL_EXT_texture_filter_anisotropic
}
