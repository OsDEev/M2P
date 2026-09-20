#ifndef _MELEE_GX_HAL_PRIV_H_
#define _MELEE_GX_HAL_PRIV_H_

// Private shared state between gx_hal.c (GL backend) and gx_hal_api.c
// (public GX API entry points). Not installed, not included by game code.

#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXStruct.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    GXTexCoordID coord;
    GXTexMapID map;
    GXChannelID color;
    GXTevColorArg ca, cb, cc, cd;
    GXTevAlphaArg aa, ab, ac, ad;
    GXTevOp cop, aop;
    GXTevBias cbias, abias;
    GXTevScale cscale, ascale;
    GXBool cclamp, aclamp;
    GXTevRegID cout, aout;
    GXTevKColorSel kcsel;
    GXTevKAlphaSel kasel;
} TEVStage;

typedef struct {
    GXBool enable;
    GXColorSrc amb_src, mat_src;
    unsigned light_mask;
    GXDiffuseFn diff_fn;
    GXAttnFn attn_fn;
    GXColor amb, mat;
} ChanState;

typedef struct {
    float x, y, z; // position, or direction when w == 0
    float w;
    float r, g, b;
    float a0, a1, a2; // distance attenuation
    float k0, k1, k2; // angle attenuation
    float cutoff;     // spot cutoff (deg)
} LightState;

// TEV / channel / light / matrix state (owned by gx_hal.c)
TEVStage* hal_tev_stage(int i);
void hal_set_num_tev(int n);
int hal_get_num_tev(void);
GXColor* hal_tev_reg(int i);
GXColor* hal_kcolor(int i);
ChanState* hal_chan(int i);
LightState* hal_light(int i);
float* hal_proj(void);
float* hal_posmtx(int i);
float* hal_nrmmtx(int i);
float* hal_texmtx(int i);
void hal_set_cur_mtx(int id);

// TLUT registry (owned by gx_hal_api.c): resolves the TLUT name stored in
// a GXTexObj into decoded data for palettized textures.
const void* hal_tlut_resolve(unsigned name, int* fmt_out);

// Extended texture metadata. GXTexObj is only 32 bytes in the original
// headers (game code allocates exactly that), so everything beyond the
// opaque blob lives here, keyed by object pointer. Owned by gx_hal.c.
typedef struct {
    const GXTexObj* obj;
    unsigned w, h;
    unsigned fmt, wrap_s, wrap_t, mipmap, min_f, mag_f;
    const void* data;
    const void* tlut;
    int tlut_fmt;
    unsigned tlut_name; // 0xFFFFFFFF = none
    unsigned lod_bias_bits;
    void* userdata;
    int used;
} TexObjExt;

TexObjExt* hal_texobj_ext(const GXTexObj* obj); // find-or-create
const TexObjExt* hal_texobj_get(const GXTexObj* obj); // find or NULL

#ifdef __cplusplus
}
#endif

#endif
