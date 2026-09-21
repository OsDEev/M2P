// GX -> OpenGL HAL, core: context, shaders (TEV/lighting approximation),
// vertex assembler, texture objects, EFB emulation.
//
// Approximation notes (honest limits of this layer):
// - TEV compare ops (COMP_*) degrade to ADD-path with a note; exotic indirect
//   stages are treated as passthrough.
// - Channel lighting implements ambient + up-to-8 directional diffuse lights
//   (GX_DF_NONE/SIGN/CLAMP). Specular/spot/distance attenuation are ignored.
// - GXTexGen MTX modes apply the texture matrix to POS/NRM/TEXn sources;
//   SRTG and bump modes fall back to passthrough UVs.

#include "gx_hal.h"
#include "gx_hal_priv.h"
#include "gl_loader.h"
#include "gx_texture.h"

#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXStruct.h>
#include <dolphin/vi/vitypes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- render modes
#define DEFMODE(sym, tv, fbw, efbh, xfbh, viw, vih, xfbm, aa_)                 \
    GXRenderModeObj sym = { tv, fbw, efbh, xfbh, 0, 0, viw, vih, xfbm, 0,      \
                            aa_, { { 0 } }, { 0 } }

DEFMODE(GXNtsc240Ds, VI_TVMODE_NTSC_DS, 640, 240, 240, 640, 240, VI_XFBMODE_SF, 0);
DEFMODE(GXNtsc240DsAa, VI_TVMODE_NTSC_DS, 640, 240, 240, 640, 240, VI_XFBMODE_SF, 1);
DEFMODE(GXNtsc240Int, VI_TVMODE_NTSC_INT, 640, 240, 480, 640, 480, VI_XFBMODE_SF, 0);
DEFMODE(GXNtsc240IntAa, VI_TVMODE_NTSC_INT, 640, 240, 480, 640, 480, VI_XFBMODE_SF, 1);
DEFMODE(GXNtsc480IntDf, VI_TVMODE_NTSC_INT, 640, 480, 480, 640, 480, VI_XFBMODE_DF, 0);
DEFMODE(GXNtsc480Int, VI_TVMODE_NTSC_INT, 640, 480, 480, 640, 480, VI_XFBMODE_SF, 0);
DEFMODE(GXNtsc480IntAa, VI_TVMODE_NTSC_INT, 640, 480, 480, 640, 480, VI_XFBMODE_SF, 1);
DEFMODE(GXNtsc480Prog, VI_TVMODE_NTSC_PROG, 640, 480, 480, 640, 480, VI_XFBMODE_SF, 0);
DEFMODE(GXNtsc480ProgAa, VI_TVMODE_NTSC_PROG, 640, 480, 480, 640, 480, VI_XFBMODE_SF, 1);
DEFMODE(GXMpal240Ds, VI_TVMODE_MPAL_DS, 640, 240, 240, 640, 240, VI_XFBMODE_SF, 0);
DEFMODE(GXMpal240DsAa, VI_TVMODE_MPAL_DS, 640, 240, 240, 640, 240, VI_XFBMODE_SF, 1);
DEFMODE(GXMpal240Int, VI_TVMODE_MPAL_INT, 640, 240, 480, 640, 480, VI_XFBMODE_SF, 0);
DEFMODE(GXMpal240IntAa, VI_TVMODE_MPAL_INT, 640, 240, 480, 640, 480, VI_XFBMODE_SF, 1);
DEFMODE(GXMpal480IntDf, VI_TVMODE_MPAL_INT, 640, 480, 480, 640, 480, VI_XFBMODE_DF, 0);
DEFMODE(GXMpal480Int, VI_TVMODE_MPAL_INT, 640, 480, 480, 640, 480, VI_XFBMODE_SF, 0);
DEFMODE(GXMpal480IntAa, VI_TVMODE_MPAL_INT, 640, 480, 480, 640, 480, VI_XFBMODE_SF, 1);
DEFMODE(GXPal264Ds, VI_TVMODE_PAL_DS, 640, 264, 264, 640, 264, VI_XFBMODE_SF, 0);
DEFMODE(GXPal264DsAa, VI_TVMODE_PAL_DS, 640, 264, 264, 640, 264, VI_XFBMODE_SF, 1);
DEFMODE(GXPal264Int, VI_TVMODE_PAL_INT, 640, 264, 528, 640, 528, VI_XFBMODE_SF, 0);
DEFMODE(GXPal264IntAa, VI_TVMODE_PAL_INT, 640, 264, 528, 640, 528, VI_XFBMODE_SF, 1);
DEFMODE(GXPal528IntDf, VI_TVMODE_PAL_INT, 640, 528, 528, 640, 528, VI_XFBMODE_DF, 0);
DEFMODE(GXPal528Int, VI_TVMODE_PAL_INT, 640, 528, 528, 640, 528, VI_XFBMODE_SF, 0);
DEFMODE(GXPal528IntAa, VI_TVMODE_PAL_INT, 640, 528, 528, 640, 528, VI_XFBMODE_SF, 1);

// ------------------------------------------------------------------ state

static GXHALContext s_hal;

typedef struct {
    float pos[4];
    float nrm[3];
    float c0[4];
    float c1[4];
    float uv[8][2];
    float mtx;
} HAL_Vertex;

#define MAX_VTX 65536

typedef struct {
    GXAttrType desc[32];
    struct {
        GXCompCnt cnt;
        GXCompType type;
        u8 frac;
    } fmt[8][32];
    const u8* array[32];
    u8 stride[32];
} HAL_VtxState;

// TEV stage mirror, channel and light state live in gx_hal_priv.h so that
// gx_hal_api.c (public GX entry points) shares the exact same layout.

static HAL_VtxState s_vtx;
static HAL_Vertex s_verts[MAX_VTX];
static int s_vert_count;
static HAL_Vertex s_cur;
static unsigned s_fed_mask;
static unsigned s_enabled_mask;
static GXPrimitive s_prim;
static GXVtxFmt s_vtxfmt;

static TEVStage s_tev[16];
static int s_num_tev = 1;
static GXColor s_tevreg[3]; // C0..C2 (+A in .a)
static GXColor s_kcolor[4];
static GXColor s_konst_def; // fallback KONST = 1,1,1,1

static ChanState s_chan[2];
static int s_num_chan = 1;
static LightState s_lights[8];

static float s_posmtx[10][16];
static float s_nrmmtx[10][16];
static float s_texmtx[10][16];
static float s_proj[16];
static int s_cur_mtx = 0;

static GXTexGenType s_tg_type[8];
static GXTexGenSrc s_tg_src[8];
static int s_tg_mtx[8];
static int s_num_tg = 0;

// texture object cache
typedef struct {
    const GXTexObj* obj;
    const void* data;
    u16 w, h;
    u32 fmt;
    unsigned gltex;
    int params_key;
} TexCacheEnt;
#define TEXCACHE_N 512
static TexCacheEnt s_texcache[TEXCACHE_N];
static int s_texcache_next;

// program cache
typedef struct {
    unsigned vhash, fhash;
    unsigned prog;
    int loc_proj, loc_posmtx, loc_nrmmtx, loc_texmtx;
    int loc_amb, loc_mat, loc_chanen, loc_ambsrc, loc_matsrc, loc_lightmask,
        loc_difffn;
    int loc_lightdir, loc_lightcol;
    int loc_texgensrc, loc_texgenmtx, loc_texgencount;
    int loc_tex[8];
    int loc_fogcolor, loc_fogstart, loc_fogend;
    int used;
} ProgCacheEnt;
#define PROGCACHE_N 16
static ProgCacheEnt s_progcache[PROGCACHE_N];
static int s_progcache_next;
// Last used program (fast path in hal_get_program; declared up here so
// gx_hal_shutdown() can reset it).
static unsigned s_last_state_hash;
static ProgCacheEnt* s_last_prog;

static unsigned s_vao, s_vbo;
static unsigned s_efb_fbo, s_efb_color, s_efb_depth;
static int s_efb_w, s_efb_h;
static int s_win_w, s_win_h;
static u8* s_readback; // scratch RGBA buffer for XFB readback
static size_t s_readback_size;

static unsigned fnv1a(const char* s)
{
    unsigned h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char) *s++) * 16777619u;
    return h;
}

// ------------------------------------------------------------ GL helpers
static unsigned compile_shader(unsigned type, const char* src)
{
    unsigned sh = _glCreateShader((GLenum) type);
    GLint len = (GLint) strlen(src);
    _glShaderSource(sh, 1, &src, &len);
    _glCompileShader(sh);
    {
        GLint ok = 0;
        _glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048];
            GLsizei n = 0;
            _glGetShaderInfoLog(sh, (GLsizei) sizeof(log), &n, log);
            fprintf(stderr, "[hal] shader compile failed: %s\n", log);
        }
    }
    return sh;
}

static unsigned link_program(const char* vs, const char* fs)
{
    unsigned a = compile_shader(GL_VERTEX_SHADER, vs);
    unsigned b = compile_shader(GL_FRAGMENT_SHADER, fs);
    unsigned p = _glCreateProgram();
    _glAttachShader(p, a);
    _glAttachShader(p, b);
    _glLinkProgram(p);
    {
        GLint ok = 0;
        _glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048];
            GLsizei n = 0;
            _glGetProgramInfoLog(p, (GLsizei) sizeof(log), &n, log);
            fprintf(stderr, "[hal] program link failed: %s\n", log);
        }
    }
    _glDeleteShader(a);
    _glDeleteShader(b);
    return p;
}

// ------------------------------------------------------------ GLSL gen
static const char* tev_c_arg(GXTevColorArg a, int stage, char* buf,
                             size_t n)
{
    const TEVStage* st = &s_tev[stage];
    switch (a) {
    case GX_CC_CPREV:
        snprintf(buf, n, "prev.rgb");
        break;
    case GX_CC_APREV:
        snprintf(buf, n, "vec3(prev.a)");
        break;
    case GX_CC_C0:
        snprintf(buf, n, "vec3(%g,%g,%g)", s_tevreg[0].r / 255.f,
                 s_tevreg[0].g / 255.f, s_tevreg[0].b / 255.f);
        break;
    case GX_CC_A0:
        snprintf(buf, n, "vec3(%g)", s_tevreg[0].a / 255.f);
        break;
    case GX_CC_C1:
        snprintf(buf, n, "vec3(%g,%g,%g)", s_tevreg[1].r / 255.f,
                 s_tevreg[1].g / 255.f, s_tevreg[1].b / 255.f);
        break;
    case GX_CC_A1:
        snprintf(buf, n, "vec3(%g)", s_tevreg[1].a / 255.f);
        break;
    case GX_CC_C2:
        snprintf(buf, n, "vec3(%g,%g,%g)", s_tevreg[2].r / 255.f,
                 s_tevreg[2].g / 255.f, s_tevreg[2].b / 255.f);
        break;
    case GX_CC_A2:
        snprintf(buf, n, "vec3(%g)", s_tevreg[2].a / 255.f);
        break;
    case GX_CC_TEXC: {
        int m = (st->map >= GX_TEXMAP0 && st->map <= GX_TEXMAP7)
                    ? (int) st->map
                    : 0;
        int c = (st->coord >= GX_TEXCOORD0 && st->coord <= GX_TEXCOORD7)
                    ? (int) st->coord
                    : 0;
        snprintf(buf, n, "texture(uTex[%d], vUV[%d]).rgb", m, c);
        break;
    }
    case GX_CC_TEXA: {
        int m = (st->map >= GX_TEXMAP0 && st->map <= GX_TEXMAP7)
                    ? (int) st->map
                    : 0;
        int c = (st->coord >= GX_TEXCOORD0 && st->coord <= GX_TEXCOORD7)
                    ? (int) st->coord
                    : 0;
        snprintf(buf, n, "vec3(texture(uTex[%d], vUV[%d]).a)", m, c);
        break;
    }
    case GX_CC_RASC:
        snprintf(buf, n, "%s",
                 (st->color == GX_COLOR1 || st->color == GX_COLOR1A1)
                     ? "vC1.rgb"
                     : "vC0.rgb");
        break;
    case GX_CC_RASA:
        snprintf(buf, n, "vec3(%s)",
                 (st->color == GX_COLOR1 || st->color == GX_COLOR1A1)
                     ? "vC1.a"
                     : "vC0.a");
        break;
    case GX_CC_ONE:
        snprintf(buf, n, "vec3(1.0)");
        break;
    case GX_CC_HALF:
        snprintf(buf, n, "vec3(0.5)");
        break;
    case GX_CC_KONST: {
        // GX_CC_KONST doubles as QUARTER selector; treat as konst color
        snprintf(buf, n, "konst%d", stage);
        break;
    }
    case GX_CC_ZERO:
        snprintf(buf, n, "vec3(0.0)");
        break;
    case GX_CC_TEXRRR: {
        int m = (st->map >= GX_TEXMAP0 && st->map <= GX_TEXMAP7)
                    ? (int) st->map
                    : 0;
        int c = (st->coord >= GX_TEXCOORD0 && st->coord <= GX_TEXCOORD7)
                    ? (int) st->coord
                    : 0;
        snprintf(buf, n, "vec3(texture(uTex[%d], vUV[%d]).r)", m, c);
        break;
    }
    case GX_CC_TEXGGG: {
        int m = (st->map >= GX_TEXMAP0 && st->map <= GX_TEXMAP7)
                    ? (int) st->map
                    : 0;
        int c = (st->coord >= GX_TEXCOORD0 && st->coord <= GX_TEXCOORD7)
                    ? (int) st->coord
                    : 0;
        snprintf(buf, n, "vec3(texture(uTex[%d], vUV[%d]).g)", m, c);
        break;
    }
    case GX_CC_TEXBBB: {
        int m = (st->map >= GX_TEXMAP0 && st->map <= GX_TEXMAP7)
                    ? (int) st->map
                    : 0;
        int c = (st->coord >= GX_TEXCOORD0 && st->coord <= GX_TEXCOORD7)
                    ? (int) st->coord
                    : 0;
        snprintf(buf, n, "vec3(texture(uTex[%d], vUV[%d]).b)", m, c);
        break;
    }
    default:
        snprintf(buf, n, "vec3(0.0)");
        break;
    }
    return buf;
}

static const char* tev_a_arg(GXTevAlphaArg a, int stage, char* buf,
                             size_t n)
{
    const TEVStage* st = &s_tev[stage];
    switch (a) {
    case GX_CA_APREV:
        snprintf(buf, n, "prev.a");
        break;
    case GX_CA_A0:
        snprintf(buf, n, "%g", s_tevreg[0].a / 255.f);
        break;
    case GX_CA_A1:
        snprintf(buf, n, "%g", s_tevreg[1].a / 255.f);
        break;
    case GX_CA_A2:
        snprintf(buf, n, "%g", s_tevreg[2].a / 255.f);
        break;
    case GX_CA_TEXA: {
        int m = (st->map >= GX_TEXMAP0 && st->map <= GX_TEXMAP7)
                    ? (int) st->map
                    : 0;
        int c = (st->coord >= GX_TEXCOORD0 && st->coord <= GX_TEXCOORD7)
                    ? (int) st->coord
                    : 0;
        snprintf(buf, n, "texture(uTex[%d], vUV[%d]).a", m, c);
        break;
    }
    case GX_CA_RASA:
        snprintf(buf, n, "%s",
                 (st->color == GX_COLOR1 || st->color == GX_COLOR1A1)
                     ? "vC1.a"
                     : "vC0.a");
        break;
    case GX_CA_KONST:
        snprintf(buf, n, "konsta%d", stage);
        break;
    case GX_CA_ZERO:
        snprintf(buf, n, "0.0");
        break;
    default:
        snprintf(buf, n, "0.0");
        break;
    }
    return buf;
}

static void konst_value(GXTevKColorSel csel, GXTevKAlphaSel asel, float* rgb,
                        float* a)
{
    float cr = 1, cg = 1, cb = 1, ca = 1;
    unsigned k = 0;
    // color selector constant part
    switch (csel) {
    case GX_TEV_KCSEL_1:
        cr = cg = cb = 1.f;
        break;
    case GX_TEV_KCSEL_7_8:
        cr = cg = cb = 7.f / 8.f;
        break;
    case GX_TEV_KCSEL_3_4:
        cr = cg = cb = 3.f / 4.f;
        break;
    case GX_TEV_KCSEL_5_8:
        cr = cg = cb = 5.f / 8.f;
        break;
    case GX_TEV_KCSEL_1_2:
        cr = cg = cb = 1.f / 2.f;
        break;
    case GX_TEV_KCSEL_3_8:
        cr = cg = cb = 3.f / 8.f;
        break;
    case GX_TEV_KCSEL_1_4:
        cr = cg = cb = 1.f / 4.f;
        break;
    case GX_TEV_KCSEL_1_8:
        cr = cg = cb = 1.f / 8.f;
        break;
    default:
        if (csel >= GX_TEV_KCSEL_K0 && csel <= GX_TEV_KCSEL_K3) {
            k = (unsigned) (csel - GX_TEV_KCSEL_K0);
            cr = s_kcolor[k].r / 255.f;
            cg = s_kcolor[k].g / 255.f;
            cb = s_kcolor[k].b / 255.f;
            ca = s_kcolor[k].a / 255.f;
        } else if (csel >= GX_TEV_KCSEL_K0_R && csel <= GX_TEV_KCSEL_K3_A) {
            unsigned v = (unsigned) (csel - GX_TEV_KCSEL_K0_R);
            k = v / 4;
            {
                float comp[4] = { s_kcolor[k].r / 255.f,
                                  s_kcolor[k].g / 255.f,
                                  s_kcolor[k].b / 255.f,
                                  s_kcolor[k].a / 255.f };
                float c = comp[v % 4];
                cr = cg = cb = ca = c;
            }
        }
        break;
    }
    // alpha selector overrides alpha
    switch (asel) {
    case GX_TEV_KASEL_1:
        ca = 1.f;
        break;
    case GX_TEV_KASEL_7_8:
        ca = 7.f / 8.f;
        break;
    case GX_TEV_KASEL_3_4:
        ca = 3.f / 4.f;
        break;
    case GX_TEV_KASEL_5_8:
        ca = 5.f / 8.f;
        break;
    case GX_TEV_KASEL_1_2:
        ca = 1.f / 2.f;
        break;
    case GX_TEV_KASEL_3_8:
        ca = 3.f / 8.f;
        break;
    case GX_TEV_KASEL_1_4:
        ca = 1.f / 4.f;
        break;
    case GX_TEV_KASEL_1_8:
        ca = 1.f / 8.f;
        break;
    default:
        if (asel >= GX_TEV_KASEL_K0_R && asel <= GX_TEV_KASEL_K3_A) {
            unsigned v = (unsigned) (asel - GX_TEV_KASEL_K0_R);
            k = v / 4;
            {
                float comp[4] = { s_kcolor[k].r / 255.f,
                                  s_kcolor[k].g / 255.f,
                                  s_kcolor[k].b / 255.f,
                                  s_kcolor[k].a / 255.f };
                ca = comp[v % 4];
            }
        }
        break;
    }
    rgb[0] = cr;
    rgb[1] = cg;
    rgb[2] = cb;
    *a = ca;
}

static const char* tev_bias_str(GXTevBias b)
{
    return (b == GX_TB_ADDHALF) ? " + vec3(0.5)"
           : (b == GX_TB_SUBHALF) ? " - vec3(0.5)"
                                  : "";
}

static const char* tev_scale_str(GXTevScale s)
{
    return (s == GX_CS_SCALE_2) ? " * 2.0"
           : (s == GX_CS_SCALE_4) ? " * 4.0"
           : (s == GX_CS_DIVIDE_2) ? " * 0.5"
                                   : "";
}

// Build fragment shader for current TEV/alpha/fog state. Returns malloc'd
// string (caller frees).
static char* build_fragment_src(void)
{
    // 16 stages x ~600 chars + prologue; generous static budget.
    size_t cap = 65536;
    char* src = (char*) malloc(cap);
    size_t off = 0;
    int i;
    char A[256], B[256], C[256], D[256];
    char aA[256], aB[256], aC[256], aD[256];
    if (!src)
        return NULL;
    off += (size_t) snprintf(
        src + off, cap - off,
        "#version 330 core\n"
        "uniform sampler2D uTex[8];\n"
        "in vec4 vC0;\n in vec4 vC1;\n in vec2 vUV[8];\n in float vFogDepth;\n"
        "uniform vec3 uFogColor;\n uniform float uFogStart;\n uniform float "
        "uFogEnd;\n"
        "out vec4 oColor;\n"
        "void main() {\n"
        " vec4 prev = vec4(0.0, 0.0, 0.0, 1.0);\n");
    for (i = 0; i < s_num_tev && i < 16; i++) {
        const TEVStage* st = &s_tev[i];
        float krgb[3], ka;
        konst_value(st->kcsel, st->kasel, krgb, &ka);
        tev_c_arg(st->ca, i, A, sizeof(A));
        tev_c_arg(st->cb, i, B, sizeof(B));
        tev_c_arg(st->cc, i, C, sizeof(C));
        tev_c_arg(st->cd, i, D, sizeof(D));
        tev_a_arg(st->aa, i, aA, sizeof(aA));
        tev_a_arg(st->ab, i, aB, sizeof(aB));
        tev_a_arg(st->ac, i, aC, sizeof(aC));
        tev_a_arg(st->ad, i, aD, sizeof(aD));
        off += (size_t) snprintf(
            src + off, cap - off,
            " {\n  vec3 konst%d = vec3(%g, %g, %g);\n"
            "  float konsta%d = %g;\n"
            "  vec3 cA = %s;\n  vec3 cB = %s;\n  vec3 cC = %s;\n  vec3 "
            "cD = %s;\n",
            i, krgb[0], krgb[1], krgb[2], i, ka, A, B, C, D);
        if (st->cop == GX_TEV_SUB) {
            off += (size_t) snprintf(src + off, cap - off,
                                     "  vec3 cR = (cD - mix(cA, cB, cC)%s)%s;\n",
                                     tev_bias_str(st->cbias),
                                     tev_scale_str(st->cscale));
        } else if (st->cop >= GX_TEV_COMP_R8_GT) {
            // Compare ops: approximate as select (documented limitation).
            off += (size_t) snprintf(
                src + off, cap - off,
                "  vec3 cR = (cA.r > cB.r) ? cC : vec3(0.0);\n");
        } else {
            off += (size_t) snprintf(src + off, cap - off,
                                     "  vec3 cR = (cD + mix(cA, cB, cC)%s)%s;\n",
                                     tev_bias_str(st->cbias),
                                     tev_scale_str(st->cscale));
        }
        if (st->cclamp)
            off += (size_t) snprintf(src + off, cap - off,
                                     "  cR = clamp(cR, 0.0, 1.0);\n");
        off += (size_t) snprintf(
            src + off, cap - off,
            "  float aA_ = %s;\n  float aB_ = %s;\n  float aC_ = %s;\n  "
            "float aD_ = %s;\n",
            aA, aB, aC, aD);
        if (st->aop == GX_TEV_SUB) {
            off += (size_t) snprintf(
                src + off, cap - off,
                "  float aR = (aD_ - mix(aA_, aB_, aC_)%s)%s;\n",
                (st->abias == GX_TB_ADDHALF)
                    ? " + 0.5"
                    : ((st->abias == GX_TB_SUBHALF) ? " - 0.5" : ""),
                (st->ascale == GX_CS_SCALE_2)
                    ? " * 2.0"
                    : ((st->ascale == GX_CS_SCALE_4)
                           ? " * 4.0"
                           : ((st->ascale == GX_CS_DIVIDE_2) ? " * 0.5"
                                                             : "")));
        } else {
            off += (size_t) snprintf(
                src + off, cap - off,
                "  float aR = (aD_ + mix(aA_, aB_, aC_)%s)%s;\n",
                (st->abias == GX_TB_ADDHALF)
                    ? " + 0.5"
                    : ((st->abias == GX_TB_SUBHALF) ? " - 0.5" : ""),
                (st->ascale == GX_CS_SCALE_2)
                    ? " * 2.0"
                    : ((st->ascale == GX_CS_SCALE_4)
                           ? " * 4.0"
                           : ((st->ascale == GX_CS_DIVIDE_2) ? " * 0.5"
                                                             : "")));
        }
        if (st->aclamp)
            off += (size_t) snprintf(src + off, cap - off,
                                     "  aR = clamp(aR, 0.0, 1.0);\n");
        off += (size_t) snprintf(src + off, cap - off,
                                 "  prev = vec4(cR, aR);\n }\n");
        if (off > cap - 2048)
            break;
    }
    // NOTE: the alpha-compare/fog epilogue is appended by
    // build_frag_epilogue(), which rewrites the tail below.
    off += (size_t) snprintf(src + off, cap - off, " oColor = prev;\n}\n");
    return src;
}

// NOTE: build_fragment_src() above leaves alpha-compare/fog to the draw
// path via uniforms + a small patch step: to keep the generator readable,
// the real compare/fog epilogue is appended by build_frag_epilogue()
// which rewrites the tail " oColor = prev;\n}\n".
static void build_frag_epilogue(char* src, size_t cap)
{
    char* tail = strstr(src, " oColor = prev;\n}\n");
    if (!tail)
        return;
    if (s_hal.alpha_test_enabled || s_hal.fog_enabled) {
        char epi[2048];
        size_t n = 0;
        n += (size_t) snprintf(epi + n, sizeof(epi) - n,
                               " {\n  float _a = prev.a * 255.0;\n");
        if (s_hal.alpha_test_enabled) {
            n += (size_t) snprintf(
                epi + n, sizeof(epi) - n,
                "  if (!(%s)) discard;\n",
                // expand ALPHAREF manually
                (s_hal.alpha_func == GX_NEVER)    ? "false"
                : (s_hal.alpha_func == GX_ALWAYS) ? "true"
                : (s_hal.alpha_func == GX_LESS)   ? "(_a < ALPHAREF)"
                : (s_hal.alpha_func == GX_LEQUAL) ? "(_a <= ALPHAREF)"
                : (s_hal.alpha_func == GX_EQUAL)
                    ? "(abs(_a - ALPHAREF) < 0.5)"
                : (s_hal.alpha_func == GX_GEQUAL) ? "(_a >= ALPHAREF)"
                : (s_hal.alpha_func == GX_GREATER) ? "(_a > ALPHAREF)"
                : (s_hal.alpha_func == GX_NEQUAL)
                    ? "(abs(_a - ALPHAREF) >= 0.5)"
                    : "true");
            // substitute numeric ref
            {
                char num[32];
                char* at;
                snprintf(num, sizeof(num), "%u",
                         (unsigned) s_hal.alpha_ref);
                while ((at = strstr(epi, "ALPHAREF")) != NULL) {
                    char tmp[2048];
                    size_t pre = (size_t) (at - epi);
                    snprintf(tmp, sizeof(tmp), "%.*s%s%s", (int) pre,
                             epi, num, at + 8);
                    strcpy(epi, tmp);
                    n = strlen(epi);
                }
            }
        }
        if (s_hal.fog_enabled) {
            n += (size_t) snprintf(
                epi + n, sizeof(epi) - n,
                "  float ff = clamp((uFogEnd - vFogDepth) / max(uFogEnd - "
                "uFogStart, 1e-3), 0.0, 1.0);\n"
                "  prev.rgb = mix(uFogColor, prev.rgb, ff);\n");
        }
        n += (size_t) snprintf(epi + n, sizeof(epi) - n,
                               " }\n oColor = prev;\n}\n");
        if (strlen(epi) + (size_t) (tail - src) < cap)
            strcpy(tail, epi);
    }
}

static const char* vertex_shader_src(void)
{
    return "#version 330 core\n"
           "layout(location=0) in vec4 aPos;\n"
           "layout(location=1) in vec3 aNrm;\n"
           "layout(location=2) in vec4 aC0;\n"
           "layout(location=3) in vec4 aC1;\n"
           "layout(location=4) in vec4 aUV01;\n"
           "layout(location=5) in vec4 aUV23;\n"
           "layout(location=6) in vec4 aUV45;\n"
           "layout(location=7) in vec4 aUV67;\n"
           "layout(location=8) in float aMtx;\n"
           "uniform mat4 uProj;\n"
           "uniform mat4 uPosMtx[10];\n"
           "uniform mat4 uNrmMtx[10];\n"
           "uniform mat4 uTexMtx[10];\n"
           "uniform vec4 uAmb[2];\n"
           "uniform vec4 uMat[2];\n"
           "uniform int uChanEn[2];\n"
           "uniform int uAmbSrc[2];\n"
           "uniform int uMatSrc[2];\n"
           "uniform int uLightMask[2];\n"
           "uniform int uDiffFn[2];\n"
           "uniform vec3 uLightDir[8];\n"
           "uniform vec3 uLightCol[8];\n"
           "uniform int uTexGenSrc[8];\n"
           "uniform int uTexGenMtx[8];\n"
           "uniform int uTexGenCount;\n"
           "out vec4 vC0;\n out vec4 vC1;\n out vec2 vUV[8];\n out float "
           "vFogDepth;\n"
           "vec2 texgen(int i, vec4 pos, vec3 nrm, vec4 uvs[8]) {\n"
           " int s = uTexGenSrc[i];\n"
           " vec4 v = (s == 0) ? pos : (s == 1) ? vec4(nrm, 1.0) : "
           "uvs[clamp(s - 2, 0, 7)];\n"
           " int m = uTexGenMtx[i];\n"
           " if (m < 0) return v.xy;\n"
           " vec4 t = uTexMtx[m] * v;\n"
           " return t.xy / max(t.w, 1e-6);\n"
           "}\n"
           "void main() {\n"
           " int mi = int(aMtx + 0.5);\n"
           " mi = clamp(mi, 0, 9);\n"
           " vec4 wp = uPosMtx[mi] * aPos;\n"
           " gl_Position = uProj * wp;\n"
           " vec3 n = normalize((uNrmMtx[mi] * vec4(aNrm, 0.0)).xyz + "
           "vec3(1e-8));\n"
           " vec4 uvs[8];\n"
           " uvs[0]=aUV01.xy; uvs[1]=aUV01.zw; uvs[2]=aUV23.xy; "
           "uvs[3]=aUV23.zw;\n"
           " uvs[4]=aUV45.xy; uvs[5]=aUV45.zw; uvs[6]=aUV67.xy; "
           "uvs[7]=aUV67.zw;\n"
           " vec3 amb0 = (uAmbSrc[0]==1) ? aC0.rgb : uAmb[0].rgb;\n"
           " vec3 mat0 = (uMatSrc[0]==1) ? aC0.rgb : uMat[0].rgb;\n"
           " vec3 col0 = amb0;\n"
           " float aa0 = (uAmbSrc[0]==1) ? aC0.a : uAmb[0].a;\n"
           " if (uChanEn[0]==1) {\n"
           "  for (int li=0; li<8; li++) {\n"
           "   if ((uLightMask[0] & (1<<li)) != 0) {\n"
           "    float d = dot(n, normalize(uLightDir[li]));\n"
           "    if (uDiffFn[0]==2) d = clamp(d, 0.0, 1.0);\n"
           "    else if (uDiffFn[0]==0) d = 0.0;\n"
           "    col0 += mat0 * uLightCol[li] * d;\n"
           "   }\n"
           "  }\n"
           " }\n"
           " vec3 amb1 = (uAmbSrc[1]==1) ? aC1.rgb : uAmb[1].rgb;\n"
           " vec3 mat1 = (uMatSrc[1]==1) ? aC1.rgb : uMat[1].rgb;\n"
           " vec3 col1 = amb1;\n"
           " float aa1 = (uAmbSrc[1]==1) ? aC1.a : uAmb[1].a;\n"
           " if (uChanEn[1]==1) {\n"
           "  for (int li=0; li<8; li++) {\n"
           "   if ((uLightMask[1] & (1<<li)) != 0) {\n"
           "    float d = dot(n, normalize(uLightDir[li]));\n"
           "    if (uDiffFn[1]==2) d = clamp(d, 0.0, 1.0);\n"
           "    else if (uDiffFn[1]==0) d = 0.0;\n"
           "    col1 += mat1 * uLightCol[li] * d;\n"
           "   }\n"
           "  }\n"
           " }\n"
           " vC0 = vec4(col0, aa0);\n vC1 = vec4(col1, aa1);\n"
           " for (int ti=0; ti<8; ti++)\n"
           "  vUV[ti] = (ti < uTexGenCount) ? texgen(ti, aPos, aNrm, uvs) "
           ": uvs[ti];\n"
           " vFogDepth = -wp.z;\n"
           "}\n";
}

// ------------------------------------------------------------ textures
static void hal_tex_ensure(const GXTexObj* obj, int unit);

#define TEXOBJ_N 1024
static TexObjExt s_texobj[TEXOBJ_N];

TexObjExt* hal_texobj_ext(const GXTexObj* obj)
{
    int i, free_slot = -1;
    for (i = 0; i < TEXOBJ_N; i++) {
        if (s_texobj[i].used && s_texobj[i].obj == obj)
            return &s_texobj[i];
        if (!s_texobj[i].used && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        free_slot = 0; // table full: recycle slot 0 (should not happen)
    memset(&s_texobj[free_slot], 0, sizeof(TexObjExt));
    s_texobj[free_slot].used = 1;
    s_texobj[free_slot].obj = obj;
    s_texobj[free_slot].tlut_name = 0xFFFFFFFFu;
    return &s_texobj[free_slot];
}

const TexObjExt* hal_texobj_get(const GXTexObj* obj)
{
    int i;
    for (i = 0; i < TEXOBJ_N; i++) {
        if (s_texobj[i].used && s_texobj[i].obj == obj)
            return &s_texobj[i];
    }
    return NULL;
}

static int tex_params_key(const TexObjExt* e)
{
    return (int) (e->wrap_s ^ e->wrap_t ^ e->min_f ^ e->mag_f ^
                  (e->tlut_name & 0xFFFFu));
}

// ------------------------------------------------------------ public HAL API
GXHALContext* gx_hal_get_context(void)
{
    return &s_hal;
}

static void mat_identity(float* m)
{
    int i;
    for (i = 0; i < 16; i++)
        m[i] = (i % 5 == 0) ? 1.f : 0.f;
}

// row-major 3x4 (GX Mtx) -> column-major 4x4 (GL)
static void mat34_to_gl(const float* src34, float* dst44)
{
    dst44[0] = src34[0];
    dst44[1] = src34[4];
    dst44[2] = src34[8];
    dst44[3] = 0;
    dst44[4] = src34[1];
    dst44[5] = src34[5];
    dst44[6] = src34[9];
    dst44[7] = 0;
    dst44[8] = src34[2];
    dst44[9] = src34[6];
    dst44[10] = src34[10];
    dst44[11] = 0;
    dst44[12] = src34[3];
    dst44[13] = src34[7];
    dst44[14] = src34[11];
    dst44[15] = 1;
}

void gx_hal_init(int width, int height, GXRenderModeObj* rm)
{
    int i;
    memset(&s_hal, 0, sizeof(s_hal));
    memset(&s_vtx, 0, sizeof(s_vtx));
    memset(s_verts, 0, sizeof(s_verts));
    memset(s_texobj, 0, sizeof(s_texobj));
    memset(s_texcache, 0, sizeof(s_texcache));
    memset(s_progcache, 0, sizeof(s_progcache));
    s_win_w = width;
    s_win_h = height;
    s_hal.width = width;
    s_hal.height = height;
    s_hal.current_render_mode = rm;
    if (rm) {
        s_hal.framebuffer_width = rm->fbWidth;
        s_hal.framebuffer_height = rm->efbHeight;
    } else {
        s_hal.framebuffer_width = 640;
        s_hal.framebuffer_height = 480;
    }
    s_efb_w = s_hal.framebuffer_width;
    s_efb_h = s_hal.framebuffer_height;

    for (i = 0; i < 16; i++) {
        s_tev[i].coord = (GXTexCoordID) (GX_TEXCOORD0 + (i % 8));
        s_tev[i].map = (GXTexMapID) (GX_TEXMAP0 + (i % 8));
        s_tev[i].color = GX_COLOR0;
        s_tev[i].ca = GX_CC_TEXC;
        s_tev[i].cb = GX_CC_ZERO;
        s_tev[i].cc = GX_CC_ZERO;
        s_tev[i].cd = GX_CC_ZERO;
        s_tev[i].aa = GX_CA_TEXA;
        s_tev[i].ab = GX_CA_ZERO;
        s_tev[i].ac = GX_CA_ZERO;
        s_tev[i].ad = GX_CA_ZERO;
        s_tev[i].cop = GX_TEV_ADD;
        s_tev[i].aop = GX_TEV_ADD;
        s_tev[i].cbias = GX_TB_ZERO;
        s_tev[i].abias = GX_TB_ZERO;
        s_tev[i].cscale = GX_CS_SCALE_1;
        s_tev[i].ascale = GX_CS_SCALE_1;
        s_tev[i].cclamp = GX_TRUE;
        s_tev[i].aclamp = GX_TRUE;
        s_tev[i].kcsel = GX_TEV_KCSEL_1;
        s_tev[i].kasel = GX_TEV_KASEL_1;
    }
    s_tevreg[0].r = s_tevreg[0].g = s_tevreg[0].b = 255;
    s_tevreg[0].a = 255;
    s_tevreg[1].r = s_tevreg[1].g = s_tevreg[1].b = 255;
    s_tevreg[1].a = 255;
    s_tevreg[2].r = s_tevreg[2].g = s_tevreg[2].b = 255;
    s_tevreg[2].a = 255;
    for (i = 0; i < 4; i++) {
        s_kcolor[i].r = s_kcolor[i].g = s_kcolor[i].b = 255;
        s_kcolor[i].a = 255;
    }
    s_konst_def.r = s_konst_def.g = s_konst_def.b = s_konst_def.a = 255;
    for (i = 0; i < 2; i++) {
        s_chan[i].enable = GX_FALSE;
        s_chan[i].amb_src = GX_SRC_REG;
        s_chan[i].mat_src = GX_SRC_REG;
        s_chan[i].amb.r = s_chan[i].amb.g = s_chan[i].amb.b = 255;
        s_chan[i].amb.a = 255;
        s_chan[i].mat.r = s_chan[i].mat.g = s_chan[i].mat.b = 255;
        s_chan[i].mat.a = 255;
    }
    for (i = 0; i < 8; i++) {
        s_lights[i].x = 0;
        s_lights[i].y = 1;
        s_lights[i].z = 0;
        s_lights[i].w = 0;
        s_lights[i].r = s_lights[i].g = s_lights[i].b = 1;
    }
    for (i = 0; i < 10; i++) {
        mat_identity(s_posmtx[i]);
        mat_identity(s_nrmmtx[i]);
        mat_identity(s_texmtx[i]);
    }
    mat_identity(s_proj);
    for (i = 0; i < 8; i++) {
        s_tg_type[i] = GX_TG_MTX2x4;
        s_tg_src[i] = GX_TG_TEX0;
        s_tg_mtx[i] = -1;
    }
    s_hal.cull_mode = GX_CULL_NONE;
    s_hal.depth_func = GX_LEQUAL;
    s_hal.depth_test_enabled = GX_TRUE;
    s_hal.blend_mode = GX_BM_NONE;
    s_hal.viewport_w = (float) width;
    s_hal.viewport_h = (float) height;

    _glGenVertexArrays(1, &s_vao);
    _glGenBuffers(1, &s_vbo);
    _glBindVertexArray(s_vao);
    _glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    _glBufferData(GL_ARRAY_BUFFER, sizeof(s_verts), NULL,
                  GL_DYNAMIC_DRAW);

    // EFB framebuffer
    _glGenFramebuffers(1, &s_efb_fbo);
    _glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
    glGenTextures(1, &s_efb_color);
    glBindTexture(GL_TEXTURE_2D, s_efb_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei) s_efb_w,
                 (GLsizei) s_efb_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    _glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, s_efb_color, 0);
    glGenTextures(1, &s_efb_depth);
    glBindTexture(GL_TEXTURE_2D, s_efb_depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                 (GLsizei) s_efb_w, (GLsizei) s_efb_h, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    _glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_TEXTURE_2D, s_efb_depth, 0);
    _glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
}

void gx_hal_set_efb_size(int w, int h)
{
    if (w <= 0 || h <= 0 || (w == s_efb_w && h == s_efb_h))
        return;
    s_efb_w = w;
    s_efb_h = h;
    s_hal.framebuffer_width = w;
    s_hal.framebuffer_height = h;
    _glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
    glBindTexture(GL_TEXTURE_2D, s_efb_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei) w, (GLsizei) h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, s_efb_depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, (GLsizei) w,
                 (GLsizei) h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT,
                 NULL);
    _glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
}

void gx_hal_bind_efb(void)
{
    _glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
}

void gx_hal_shutdown(void)
{
    unsigned i;
    for (i = 0; i < PROGCACHE_N; i++) {
        if (s_progcache[i].used)
            _glDeleteProgram(s_progcache[i].prog);
    }
    for (i = 0; i < TEXCACHE_N; i++) {
        if (s_texcache[i].gltex)
            glDeleteTextures(1, &s_texcache[i].gltex);
    }
    if (s_efb_fbo)
        _glDeleteFramebuffers(1, &s_efb_fbo);
    if (s_efb_color)
        glDeleteTextures(1, &s_efb_color);
    if (s_efb_depth)
        glDeleteTextures(1, &s_efb_depth);
    if (s_vbo)
        _glDeleteBuffers(1, &s_vbo);
    if (s_vao)
        _glDeleteVertexArrays(1, &s_vao);
    if (s_hal.vertex_buffer) {
        free(s_hal.vertex_buffer);
        s_hal.vertex_buffer = NULL;
    }
    free(s_readback);
    s_readback = NULL;
    s_readback_size = 0;
    s_last_prog = NULL;
}

// ------------------------------------------------------------ program mgmt
// Hash of everything baked into generated shaders (uniform-driven state
// like matrices, lights and bound textures is excluded on purpose).
static unsigned hal_state_hash(void)
{
    unsigned h = 2166136261u;
    const u8* p;
    size_t n;
    int i;
#define MIX(m_, s_)                                                            \
    do {                                                                       \
        p = (const u8*) (m_);                                                  \
        n = (s_);                                                              \
        while (n--)                                                            \
            h = (h ^ *p++) * 16777619u;                                        \
    } while (0)
    for (i = 0; i < s_num_tev && i < 16; i++)
        MIX(&s_tev[i], sizeof(s_tev[i]));
    MIX(&s_num_tev, sizeof(s_num_tev));
    MIX(s_tevreg, sizeof(s_tevreg));
    MIX(s_kcolor, sizeof(s_kcolor));
    // NOTE: hashed field-by-field (never over padding bytes, whose
    // indeterminate values would poison the hash).
    MIX(&s_hal.alpha_test_enabled, sizeof(s_hal.alpha_test_enabled));
    MIX(&s_hal.alpha_func, sizeof(s_hal.alpha_func));
    MIX(&s_hal.alpha_ref, sizeof(s_hal.alpha_ref));
    MIX(&s_hal.fog_enabled, sizeof(s_hal.fog_enabled));
    MIX(&s_hal.fog_start, sizeof(float) * 2);
    MIX(&s_hal.fog_color, sizeof(GXColor));
    MIX(&s_num_chan, sizeof(s_num_chan));
    MIX(&s_num_tg, sizeof(s_num_tg));
#undef MIX
    return h;
}

static ProgCacheEnt* hal_get_program(void)
{
    char* fs;
    const char* vs = vertex_shader_src();
    unsigned vh, fh, sh;
    unsigned i;
    ProgCacheEnt* e;
    sh = hal_state_hash();
    if (s_last_prog && sh == s_last_state_hash)
        return s_last_prog; // fast path: no rebuild, no source gen
    fs = build_fragment_src();
    if (!fs)
        return NULL;
    build_frag_epilogue(fs, 65536);
    vh = fnv1a(vs) ^ (unsigned) (s_num_chan * 31 + s_num_tg * 131);
    fh = fnv1a(fs);
    for (i = 0; i < PROGCACHE_N; i++) {
        if (s_progcache[i].used && s_progcache[i].vhash == vh &&
            s_progcache[i].fhash == fh) {
            free(fs);
            s_last_state_hash = sh;
            s_last_prog = &s_progcache[i];
            return &s_progcache[i];
        }
    }
    e = &s_progcache[s_progcache_next];
    s_progcache_next = (s_progcache_next + 1) % PROGCACHE_N;
    if (e->used)
        _glDeleteProgram(e->prog);
    e->prog = link_program(vs, fs);
    e->vhash = vh;
    e->fhash = fh;
    e->used = 1;
    e->loc_proj = _glGetUniformLocation(e->prog, "uProj");
    e->loc_posmtx = _glGetUniformLocation(e->prog, "uPosMtx");
    e->loc_nrmmtx = _glGetUniformLocation(e->prog, "uNrmMtx");
    e->loc_texmtx = _glGetUniformLocation(e->prog, "uTexMtx");
    e->loc_amb = _glGetUniformLocation(e->prog, "uAmb");
    e->loc_mat = _glGetUniformLocation(e->prog, "uMat");
    e->loc_chanen = _glGetUniformLocation(e->prog, "uChanEn");
    e->loc_ambsrc = _glGetUniformLocation(e->prog, "uAmbSrc");
    e->loc_matsrc = _glGetUniformLocation(e->prog, "uMatSrc");
    e->loc_lightmask = _glGetUniformLocation(e->prog, "uLightMask");
    e->loc_difffn = _glGetUniformLocation(e->prog, "uDiffFn");
    e->loc_lightdir = _glGetUniformLocation(e->prog, "uLightDir");
    e->loc_lightcol = _glGetUniformLocation(e->prog, "uLightCol");
    e->loc_texgensrc = _glGetUniformLocation(e->prog, "uTexGenSrc");
    e->loc_texgenmtx = _glGetUniformLocation(e->prog, "uTexGenMtx");
    e->loc_texgencount = _glGetUniformLocation(e->prog, "uTexGenCount");
    e->loc_fogcolor = _glGetUniformLocation(e->prog, "uFogColor");
    e->loc_fogstart = _glGetUniformLocation(e->prog, "uFogStart");
    e->loc_fogend = _glGetUniformLocation(e->prog, "uFogEnd");
    for (i = 0; i < 8; i++) {
        char name[16];
        snprintf(name, sizeof(name), "uTex[%u]", i);
        e->loc_tex[i] = _glGetUniformLocation(e->prog, name);
    }
    free(fs);
    s_last_state_hash = sh;
    s_last_prog = e;
    return e;
}

static void hal_upload_uniforms(ProgCacheEnt* e)
{
    int i;
    int en[2], asrc[2], msrc[2], mask[2], dfn[2];
    int tgsrc[8], tgmtx[8];
    float ambf[8], matf[8], ldir[24], lcol[24];
    _glUseProgram(e->prog);
    if (e->loc_proj >= 0)
        _glUniformMatrix4fv(e->loc_proj, 1, 0, s_proj);
    if (e->loc_posmtx >= 0)
        _glUniformMatrix4fv(e->loc_posmtx, 10, 0, &s_posmtx[0][0]);
    if (e->loc_nrmmtx >= 0)
        _glUniformMatrix4fv(e->loc_nrmmtx, 10, 0, &s_nrmmtx[0][0]);
    if (e->loc_texmtx >= 0)
        _glUniformMatrix4fv(e->loc_texmtx, 10, 0, &s_texmtx[0][0]);
    for (i = 0; i < 2; i++) {
        en[i] = (i < s_num_chan && s_chan[i].enable) ? 1 : 0;
        asrc[i] = (s_chan[i].amb_src == GX_SRC_VTX) ? 1 : 0;
        msrc[i] = (s_chan[i].mat_src == GX_SRC_VTX) ? 1 : 0;
        mask[i] = (int) s_chan[i].light_mask;
        dfn[i] = (int) s_chan[i].diff_fn;
        ambf[i * 4 + 0] = s_chan[i].amb.r / 255.f;
        ambf[i * 4 + 1] = s_chan[i].amb.g / 255.f;
        ambf[i * 4 + 2] = s_chan[i].amb.b / 255.f;
        ambf[i * 4 + 3] = s_chan[i].amb.a / 255.f;
        matf[i * 4 + 0] = s_chan[i].mat.r / 255.f;
        matf[i * 4 + 1] = s_chan[i].mat.g / 255.f;
        matf[i * 4 + 2] = s_chan[i].mat.b / 255.f;
        matf[i * 4 + 3] = s_chan[i].mat.a / 255.f;
    }
    if (e->loc_amb >= 0)
        _glUniform4fv(e->loc_amb, 2, ambf);
    if (e->loc_mat >= 0)
        _glUniform4fv(e->loc_mat, 2, matf);
    if (e->loc_chanen >= 0)
        _glUniform1iv(e->loc_chanen, 2, en);
    if (e->loc_ambsrc >= 0)
        _glUniform1iv(e->loc_ambsrc, 2, asrc);
    if (e->loc_matsrc >= 0)
        _glUniform1iv(e->loc_matsrc, 2, msrc);
    if (e->loc_lightmask >= 0)
        _glUniform1iv(e->loc_lightmask, 2, mask);
    if (e->loc_difffn >= 0)
        _glUniform1iv(e->loc_difffn, 2, dfn);
    for (i = 0; i < 8; i++) {
        ldir[i * 3 + 0] = s_lights[i].x;
        ldir[i * 3 + 1] = s_lights[i].y;
        ldir[i * 3 + 2] = s_lights[i].z;
        lcol[i * 3 + 0] = s_lights[i].r;
        lcol[i * 3 + 1] = s_lights[i].g;
        lcol[i * 3 + 2] = s_lights[i].b;
        if (s_tg_src[i] == GX_TG_POS)
            tgsrc[i] = 0;
        else if (s_tg_src[i] == GX_TG_NRM)
            tgsrc[i] = 1;
        else if (s_tg_src[i] >= GX_TG_TEX0 && s_tg_src[i] <= GX_TG_TEX7)
            tgsrc[i] = 2 + (s_tg_src[i] - GX_TG_TEX0);
        else if (s_tg_src[i] >= GX_TG_TEXCOORD0)
            tgsrc[i] = 2 + (s_tg_src[i] - GX_TG_TEXCOORD0) % 8;
        else
            tgsrc[i] = -2;
        tgmtx[i] = (i < s_num_tg) ? s_tg_mtx[i] : -1;
    }
    if (e->loc_lightdir >= 0)
        _glUniform3fv(e->loc_lightdir, 8, ldir);
    if (e->loc_lightcol >= 0)
        _glUniform3fv(e->loc_lightcol, 8, lcol);
    if (e->loc_texgensrc >= 0)
        _glUniform1iv(e->loc_texgensrc, 8, tgsrc);
    if (e->loc_texgenmtx >= 0)
        _glUniform1iv(e->loc_texgenmtx, 8, tgmtx);
    if (e->loc_texgencount >= 0)
        _glUniform1i(e->loc_texgencount, s_num_tg);
    for (i = 0; i < 8; i++) {
        if (e->loc_tex[i] >= 0)
            _glUniform1i(e->loc_tex[i], i);
    }
    if (e->loc_fogcolor >= 0)
        _glUniform3f(e->loc_fogcolor, s_hal.fog_color.r / 255.f,
                     s_hal.fog_color.g / 255.f, s_hal.fog_color.b / 255.f);
    if (e->loc_fogstart >= 0)
        _glUniform1f(e->loc_fogstart, s_hal.fog_start);
    if (e->loc_fogend >= 0)
        _glUniform1f(e->loc_fogend, s_hal.fog_end);
}

// ------------------------------------------------------------ GL state apply
static unsigned hal_gl_compare(GXCompare c)
{
    switch (c) {
    case GX_NEVER:
        return GL_NEVER;
    case GX_LESS:
        return GL_LESS;
    case GX_EQUAL:
        return GL_EQUAL;
    case GX_LEQUAL:
        return GL_LEQUAL;
    case GX_GREATER:
        return GL_GREATER;
    case GX_NEQUAL:
        return GL_NOTEQUAL;
    case GX_GEQUAL:
        return GL_GEQUAL;
    default:
        return GL_ALWAYS;
    }
}

static void hal_apply_state(void)
{
    // render into EFB
    _glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
    glViewport((GLint) s_hal.viewport_x,
               (GLint) (s_efb_h - s_hal.viewport_y - s_hal.viewport_h),
               (GLsizei) s_hal.viewport_w, (GLsizei) s_hal.viewport_h);
    if (s_hal.scissor_enabled) {
        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint) s_hal.scissor_x,
                  (GLint) (s_efb_h - s_hal.scissor_y - s_hal.scissor_h),
                  (GLsizei) s_hal.scissor_w, (GLsizei) s_hal.scissor_h);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    if (s_hal.depth_test_enabled) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc((GLenum) hal_gl_compare(s_hal.depth_func));
        glDepthMask(GL_TRUE);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    switch (s_hal.cull_mode) {
    case GX_CULL_FRONT:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        break;
    case GX_CULL_BACK:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        break;
    case GX_CULL_ALL:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT_AND_BACK);
        break;
    default:
        glDisable(GL_CULL_FACE);
        break;
    }
    // GX front = clockwise; GL default CCW -> flip
    glFrontFace(GL_CW);
    switch (s_hal.blend_mode) {
    case GX_BM_BLEND: {
        GLenum s = GL_ONE, d = GL_ZERO;
        glEnable(GL_BLEND);
        _glBlendEquation(GL_FUNC_ADD);
        switch (s_hal.blend_src) {
        case GX_BL_ZERO:
            s = GL_ZERO;
            break;
        case GX_BL_ONE:
            s = GL_ONE;
            break;
        case GX_BL_SRCCLR:
            s = GL_SRC_COLOR;
            break;
        case GX_BL_INVSRCCLR:
            s = GL_ONE_MINUS_SRC_COLOR;
            break;
        case GX_BL_SRCALPHA:
            s = GL_SRC_ALPHA;
            break;
        case GX_BL_INVSRCALPHA:
            s = GL_ONE_MINUS_SRC_ALPHA;
            break;
        case GX_BL_DSTALPHA:
            s = GL_DST_ALPHA;
            break;
        case GX_BL_INVDSTALPHA:
            s = GL_ONE_MINUS_DST_ALPHA;
            break;
        default:
            break;
        }
        switch (s_hal.blend_dst) {
        case GX_BL_ZERO:
            d = GL_ZERO;
            break;
        case GX_BL_ONE:
            d = GL_ONE;
            break;
        case GX_BL_SRCCLR:
            d = GL_DST_COLOR;
            break;
        case GX_BL_INVSRCCLR:
            d = GL_ONE_MINUS_DST_COLOR;
            break;
        case GX_BL_SRCALPHA:
            d = GL_SRC_ALPHA;
            break;
        case GX_BL_INVSRCALPHA:
            d = GL_ONE_MINUS_SRC_ALPHA;
            break;
        case GX_BL_DSTALPHA:
            d = GL_DST_ALPHA;
            break;
        case GX_BL_INVDSTALPHA:
            d = GL_ONE_MINUS_DST_ALPHA;
            break;
        default:
            break;
        }
        glBlendFunc(s, d);
        break;
    }
    case GX_BM_SUBTRACT:
        glEnable(GL_BLEND);
        _glBlendEquation(GL_FUNC_SUBTRACT);
        glBlendFunc(GL_ONE, GL_ONE);
        break;
    case GX_BM_LOGIC:
        // no GL equivalent in core profile; approximate as opaque
    default:
        glDisable(GL_BLEND);
        break;
    }
}

// ------------------------------------------------------------ vertex assembler
static void cur_reset(void)
{
    int i, j;
    s_cur.pos[0] = s_cur.pos[1] = s_cur.pos[2] = 0;
    s_cur.pos[3] = 1;
    s_cur.nrm[0] = s_cur.nrm[1] = s_cur.nrm[2] = 0;
    for (i = 0; i < 4; i++)
        s_cur.c0[i] = s_cur.c1[i] = 1.f;
    for (i = 0; i < 8; i++)
        for (j = 0; j < 2; j++)
            s_cur.uv[i][j] = 0;
    s_cur.mtx = 0;
    s_fed_mask = 0;
}

static void cur_push(void)
{
    if (s_vert_count < MAX_VTX)
        s_verts[s_vert_count++] = s_cur;
    cur_reset();
}

static void cur_fed(unsigned bit)
{
    s_fed_mask |= bit;
    if (s_enabled_mask && (s_fed_mask & s_enabled_mask) == s_enabled_mask)
        cur_push();
}

static float decode_num(const u8* p, GXCompType t, float scale)
{
    switch (t) {
    case GX_U8:
        return p[0] * scale;
    case GX_S8:
        return ((const s8*) p)[0] * scale;
    case GX_U16:
        return (((unsigned) p[0] << 8) | p[1]) * scale;
    case GX_S16: {
        int v = (int) (((unsigned) p[0] << 8) | p[1]);
        if (v >= 32768)
            v -= 65536;
        return v * scale;
    }
    case GX_F32: {
        // big-endian float from game data
        union {
            u32 u;
            float f;
        } v;
        v.u = ((u32) p[0] << 24) | ((u32) p[1] << 16) |
              ((u32) p[2] << 8) | p[3];
        return v.f;
    }
    default:
        return 0;
    }
}

static int comptype_size(GXCompType t)
{
    switch (t) {
    case GX_U8:
    case GX_S8:
        return 1;
    case GX_U16:
    case GX_S16:
        return 2;
    case GX_F32:
        return 4;
    default:
        return 1;
    }
}

// Fetch indexed attribute from bound array.
static void fetch_indexed(GXAttr attr, unsigned idx, float* out_pos,
                          float* out_nrm, float* out_c, float* out_uv,
                          float* out_mtx)
{
    const u8* base = s_vtx.array[attr];
    int stride = s_vtx.stride[attr];
    int cnt = s_vtx.fmt[s_vtxfmt][attr].cnt;
    GXCompType type = s_vtx.fmt[s_vtxfmt][attr].type;
    int frac = s_vtx.fmt[s_vtxfmt][attr].frac;
    const u8* p;
    float scale;
    int comps, k;
    if (!base)
        return;
    p = base + idx * (unsigned) stride;
    scale = (type == GX_F32) ? 1.f : (1.f / (float) (1u << frac));
    comps = 0;
    if (attr == GX_VA_POS)
        comps = (cnt == GX_POS_XYZ) ? 3 : 2;
    else if (attr == GX_VA_NRM)
        comps = 3;
    else if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1)
        comps = (cnt == GX_CLR_RGBA) ? 4 : 3;
    else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7)
        comps = (cnt == GX_TEX_ST) ? 2 : 1;
    else if (attr == GX_VA_PNMTXIDX || (attr >= GX_VA_TEX0MTXIDX &&
                                        attr <= GX_VA_TEX7MTXIDX)) {
        *out_mtx = (float) p[0];
        return;
    }
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        // packed color component types
        if (type == GX_RGB565) {
            unsigned v = ((unsigned) p[0] << 8) | p[1];
            out_c[0] = ((v >> 11) & 0x1F) * (255.f / 31.f) / 255.f;
            out_c[1] = ((v >> 5) & 0x3F) * (255.f / 63.f) / 255.f;
            out_c[2] = (v & 0x1F) * (255.f / 31.f) / 255.f;
            out_c[3] = 1.f;
            return;
        } else if (type == GX_RGBA8) {
            out_c[0] = p[0] / 255.f;
            out_c[1] = p[1] / 255.f;
            out_c[2] = p[2] / 255.f;
            out_c[3] = p[3] / 255.f;
            return;
        } else if (type == GX_RGB8 || type == GX_RGBX8) {
            out_c[0] = p[0] / 255.f;
            out_c[1] = p[1] / 255.f;
            out_c[2] = p[2] / 255.f;
            out_c[3] = 1.f;
            return;
        } else if (type == GX_RGBA4) {
            unsigned v = ((unsigned) p[0] << 8) | p[1];
            out_c[0] = ((v >> 12) & 0xF) / 15.f;
            out_c[1] = ((v >> 8) & 0xF) / 15.f;
            out_c[2] = ((v >> 4) & 0xF) / 15.f;
            out_c[3] = (v & 0xF) / 15.f;
            return;
        } else if (type == GX_RGBA6) {
            u32 v = ((u32) p[0] << 16) | ((u32) p[1] << 8) | p[2];
            out_c[0] = ((v >> 18) & 0x3F) / 63.f;
            out_c[1] = ((v >> 12) & 0x3F) / 63.f;
            out_c[2] = ((v >> 6) & 0x3F) / 63.f;
            out_c[3] = (v & 0x3F) / 63.f;
            return;
        }
    }
    for (k = 0; k < comps; k++) {
        float v = decode_num(p + k * (unsigned) comptype_size(type), type,
                             scale);
        if (attr == GX_VA_POS)
            out_pos[k] = v;
        else if (attr == GX_VA_NRM)
            out_nrm[k] = v;
        else if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1)
            // integer color components are 0..255 (frac is 0 for colors)
            out_c[k] = (type == GX_F32) ? v : v / 255.f;
        else
            out_uv[k] = v;
    }
    if (attr == GX_VA_POS && comps == 2)
        out_pos[2] = 0;
    if ((attr == GX_VA_CLR0 || attr == GX_VA_CLR1) && comps == 3)
        out_c[3] = 1.f;
}

// attribute bit in fed/enabled masks
static unsigned attr_bit(GXAttr a)
{
    if (a == GX_VA_POS)
        return 1u << 0;
    if (a == GX_VA_NRM)
        return 1u << 1;
    if (a == GX_VA_CLR0)
        return 1u << 2;
    if (a == GX_VA_CLR1)
        return 1u << 3;
    if (a >= GX_VA_TEX0 && a <= GX_VA_TEX7)
        return 1u << (4 + (a - GX_VA_TEX0));
    if (a == GX_VA_PNMTXIDX || a == GX_VA_NBT ||
        (a >= GX_VA_TEX0MTXIDX && a <= GX_VA_TEX7MTXIDX))
        return 1u << 12;
    return 0;
}

// --- direct feeds (called by the GXVert shim) ---
static void feed_pos(const float* v, int n)
{
    int i;
    for (i = 0; i < n && i < 3; i++)
        s_cur.pos[i] = v[i];
    cur_fed(attr_bit(GX_VA_POS));
}

static void feed_nrm(const float* v)
{
    s_cur.nrm[0] = v[0];
    s_cur.nrm[1] = v[1];
    s_cur.nrm[2] = v[2];
    cur_fed(attr_bit(GX_VA_NRM));
    // NBT (normal/binormal/tangent for bump) shares the completion bit
    // with matrix indices; feeding NRM also satisfies an enabled NBT so
    // bump-mapped draws can't stall the assembler. The shader itself
    // uses NRM (bump detail is a documented v1 limitation).
    if (s_enabled_mask & attr_bit(GX_VA_NBT))
        cur_fed(attr_bit(GX_VA_NBT));
}

static void feed_clr(GXAttr which, const float* v, int n)
{
    float* dst = (which == GX_VA_CLR1) ? s_cur.c1 : s_cur.c0;
    int i;
    for (i = 0; i < n && i < 4; i++)
        dst[i] = v[i];
    if (n == 3)
        dst[3] = 1.f;
    cur_fed(attr_bit(which));
}

static void feed_uv(int unit, const float* v, int n)
{
    int i;
    if (unit < 0 || unit > 7)
        return;
    for (i = 0; i < n && i < 2; i++)
        s_cur.uv[unit][i] = v[i];
    cur_fed(attr_bit((GXAttr) (GX_VA_TEX0 + unit)));
}

static void feed_mtx(float m)
{
    s_cur.mtx = m;
    cur_fed(attr_bit(GX_VA_PNMTXIDX));
}

static void feed_index(GXAttr attr, unsigned idx)
{
    float recinto[4] = { 0, 0, 0, 1 };
    float tmpc[4] = { 1, 1, 1, 1 };
    float tmpuv[2] = { 0, 0 };
    float tmpm = 0;
    fetch_indexed(attr, idx, recinto, s_cur.nrm, tmpc, tmpuv, &tmpm);
    if (attr == GX_VA_POS) {
        s_cur.pos[0] = recinto[0];
        s_cur.pos[1] = recinto[1];
        s_cur.pos[2] = recinto[2];
        cur_fed(attr_bit(GX_VA_POS));
    } else if (attr == GX_VA_NRM) {
        cur_fed(attr_bit(GX_VA_NRM));
    } else if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        float* dst = (attr == GX_VA_CLR1) ? s_cur.c1 : s_cur.c0;
        dst[0] = tmpc[0];
        dst[1] = tmpc[1];
        dst[2] = tmpc[2];
        dst[3] = tmpc[3];
        cur_fed(attr_bit(attr));
    } else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
        s_cur.uv[attr - GX_VA_TEX0][0] = tmpuv[0];
        s_cur.uv[attr - GX_VA_TEX0][1] = tmpuv[1];
        cur_fed(attr_bit(attr));
    } else {
        s_cur.mtx = tmpm;
        cur_fed(attr_bit(GX_VA_PNMTXIDX));
    }
}

// ------------------------------------------------------------------ GX_BEGIN
void gx_hal_begin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts)
{
    int a;
    (void) nverts;
    s_prim = type;
    s_vtxfmt = vtxfmt;
    s_vert_count = 0;
    s_enabled_mask = 0;
    for (a = 0; a < 32; a++) {
        GXAttrType t = s_vtx.desc[a];
        if (t == GX_DIRECT || t == GX_INDEX8 || t == GX_INDEX16)
            s_enabled_mask |= attr_bit((GXAttr) a);
    }
    cur_reset();
    s_hal.current_primitive_type = (int) type;
    s_hal.current_vtxfmt = vtxfmt;
}

static unsigned hal_prim_gl(GXPrimitive p)
{
    switch (p) {
    case GX_TRIANGLES:
        return GL_TRIANGLES;
    case GX_TRIANGLESTRIP:
        return GL_TRIANGLE_STRIP;
    case GX_TRIANGLEFAN:
        return GL_TRIANGLE_FAN;
    case GX_LINES:
        return GL_LINES;
    case GX_LINESTRIP:
        return GL_LINE_STRIP;
    case GX_POINTS:
        return GL_POINTS;
    default:
        return GL_TRIANGLES;
    }
}

static void hal_flush_draw(void)
{
    ProgCacheEnt* e;
    int i, unit;
    if (s_vert_count <= 0)
        return;
    e = hal_get_program();
    if (!e)
        return;
    hal_apply_state();
    // textures
    for (unit = 0; unit < 8; unit++) {
        _glActiveTexture((GLenum) (GL_TEXTURE0 + unit));
        if (s_hal.texture_objects[unit])
            hal_tex_ensure(s_hal.texture_objects[unit], unit);
        else
            glBindTexture(GL_TEXTURE_2D, 0);
    }
    hal_upload_uniforms(e);
    // upload vertices (expand QUADS)
    {
        HAL_Vertex* draw = s_verts;
        int count = s_vert_count;
        HAL_Vertex* tmp = NULL;
        if (s_prim == GX_QUADS) {
            int quads = s_vert_count / 4;
            tmp = (HAL_Vertex*) malloc(sizeof(HAL_Vertex) *
                                      (size_t) (quads * 6));
            if (tmp) {
                int q;
                for (q = 0; q < quads; q++) {
                    tmp[q * 6 + 0] = s_verts[q * 4 + 0];
                    tmp[q * 6 + 1] = s_verts[q * 4 + 1];
                    tmp[q * 6 + 2] = s_verts[q * 4 + 2];
                    tmp[q * 6 + 3] = s_verts[q * 4 + 0];
                    tmp[q * 6 + 4] = s_verts[q * 4 + 2];
                    tmp[q * 6 + 5] = s_verts[q * 4 + 3];
                }
                draw = tmp;
                count = quads * 6;
            }
        }
        _glBindVertexArray(s_vao);
        _glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        _glBufferData(GL_ARRAY_BUFFER,
                      (GLsizeiptr) (sizeof(HAL_Vertex) * (size_t) count),
                      draw, GL_STREAM_DRAW);
        // layout: pos(4f) nrm(3f) c0(4f) c1(4f) uv(16f) mtx(1f) = 32 floats
        _glEnableVertexAttribArray(0);
        _glVertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(HAL_Vertex),
                               (const void*) 0);
        _glEnableVertexAttribArray(1);
        _glVertexAttribPointer(1, 3, GL_FLOAT, 0, sizeof(HAL_Vertex),
                               (const void*) (sizeof(float) * 4));
        _glEnableVertexAttribArray(2);
        _glVertexAttribPointer(2, 4, GL_FLOAT, 0, sizeof(HAL_Vertex),
                               (const void*) (sizeof(float) * 7));
        _glEnableVertexAttribArray(3);
        _glVertexAttribPointer(3, 4, GL_FLOAT, 0, sizeof(HAL_Vertex),
                               (const void*) (sizeof(float) * 11));
        _glEnableVertexAttribArray(4);
        _glVertexAttribPointer(4, 4, GL_FLOAT, 0, sizeof(HAL_Vertex),
                               (const void*) (sizeof(float) * 15));
        _glEnableVertexAttribArray(5);
        _glVertexAttribPointer(5, 4, GL_FLOAT, 0, sizeof(HAL_Vertex),
                               (const void*) (sizeof(float) * 19));
        _glEnableVertexAttribArray(6);
        _glVertexAttribPointer(6, 4, GL_FLOAT, 0, sizeof(HAL_Vertex),
                               (const void*) (sizeof(float) * 23));
        _glEnableVertexAttribArray(7);
        _glVertexAttribPointer(7, 4, GL_FLOAT, 0, sizeof(HAL_Vertex),
                               (const void*) (sizeof(float) * 27));
        _glEnableVertexAttribArray(8);
        _glVertexAttribPointer(8, 1, GL_FLOAT, 0, sizeof(HAL_Vertex),
                               (const void*) (sizeof(float) * 31));
        glDrawArrays((GLenum) hal_prim_gl(s_prim), 0, (GLsizei) count);
        free(tmp);
    }
    s_vert_count = 0;
    (void) i;
}

void gx_hal_end(void)
{
    hal_flush_draw();
}

// ------------------------------------------------------------ display lists
// Forward: defined alongside the GXVert shim near the end of the file.
static GXAttr clr_target(void);
// Executes a prebuilt GX display list (HSD static geometry). The stream
// layout matches what HSD emits: opcode|vat, BE16 count, then per-vertex
// attribute data in FIFO order. Attribute presence and encoding follow the
// CURRENT VAT state (set up outside the list via GXSetVtxDesc* /
// GXSetVtxAttrFmt*, exactly like retail). Embedded multi-byte values are
// big-endian and decode through the same paths as indexed arrays.
// State opcodes (CP/XF/BP loads) do not appear in HSD lists; anything that
// is not a known draw opcode terminates the stream (safe no-op).
static int dl_need(const u8* dl, u32 nbytes, u32 pos, u32 n)
{
    return pos + n <= nbytes ? 1 : 0;
}

static unsigned dl_u16(const u8* p)
{
    return ((unsigned) p[0] << 8) | p[1];
}

// Feed one DIRECT color from the stream (packed or numeric), advancing *pos.
static void dl_feed_color_direct(GXAttr attr, const u8* dl, u32 nbytes,
                                 u32* pos)
{
    GXCompType type = s_vtx.fmt[s_vtxfmt][attr].type;
    GXCompCnt cnt = s_vtx.fmt[s_vtxfmt][attr].cnt;
    int comps = (cnt == GX_CLR_RGBA) ? 4 : 3;
    float c[4] = { 0, 0, 0, 1.f };
    const u8* p;
    if (!dl_need(dl, nbytes, *pos, 4)) {
        return;
    }
    p = dl + *pos;
    if (type == GX_RGB565) {
        unsigned v = dl_u16(p);
        *pos += 2;
        c[0] = ((v >> 11) & 0x1F) * (255.f / 31.f) / 255.f;
        c[1] = ((v >> 5) & 0x3F) * (255.f / 63.f) / 255.f;
        c[2] = (v & 0x1F) * (255.f / 31.f) / 255.f;
    } else if (type == GX_RGBA8) {
        if (!dl_need(dl, nbytes, *pos, 4))
            return;
        c[0] = p[0] / 255.f;
        c[1] = p[1] / 255.f;
        c[2] = p[2] / 255.f;
        c[3] = p[3] / 255.f;
        *pos += 4;
    } else if (type == GX_RGB8 || type == GX_RGBX8) {
        if (!dl_need(dl, nbytes, *pos, 3))
            return;
        c[0] = p[0] / 255.f;
        c[1] = p[1] / 255.f;
        c[2] = p[2] / 255.f;
        *pos += (type == GX_RGB8) ? 3 : 4;
    } else if (type == GX_RGBA4) {
        unsigned v = dl_u16(p);
        *pos += 2;
        c[0] = ((v >> 12) & 0xF) / 15.f;
        c[1] = ((v >> 8) & 0xF) / 15.f;
        c[2] = ((v >> 4) & 0xF) / 15.f;
        c[3] = (v & 0xF) / 15.f;
    } else if (type == GX_RGBA6) {
        if (!dl_need(dl, nbytes, *pos, 3))
            return;
        {
            u32 v = ((u32) p[0] << 16) | ((u32) p[1] << 8) | p[2];
            c[0] = ((v >> 18) & 0x3F) / 63.f;
            c[1] = ((v >> 12) & 0x3F) / 63.f;
            c[2] = ((v >> 6) & 0x3F) / 63.f;
            c[3] = (v & 0x3F) / 63.f;
        }
        *pos += 3;
    } else {
        // numeric components
        float scale;
        int sz, k;
        if (type == GX_F32)
            scale = 1.f;
        else
            scale = 1.f / (float) (1u << s_vtx.fmt[s_vtxfmt][attr].frac);
        sz = comptype_size(type);
        if (!dl_need(dl, nbytes, *pos, (u32) (comps * sz)))
            return;
        for (k = 0; k < comps; k++) {
            float v = decode_num(p + k * (unsigned) sz, type, scale);
            c[k] = (type == GX_F32) ? v : v / 255.f;
        }
        *pos += (u32) (comps * sz);
    }
    feed_clr(clr_target(), c, comps);
}

// Feed one DIRECT numeric attribute (pos/nrm/tex), advancing *pos.
static void dl_feed_numeric(GXAttr attr, float* dst_slot, int comps,
                            const u8* dl, u32 nbytes, u32* pos)
{
    GXCompType type = s_vtx.fmt[s_vtxfmt][attr].type;
    float scale;
    int sz, k;
    if (type == GX_F32)
        scale = 1.f;
    else
        scale = 1.f / (float) (1u << s_vtx.fmt[s_vtxfmt][attr].frac);
    sz = comptype_size(type);
    if (!dl_need(dl, nbytes, *pos, (u32) (comps * sz)))
        return;
    for (k = 0; k < comps; k++)
        dst_slot[k] = decode_num(dl + *pos + (u32) (k * sz), type, scale);
    *pos += (u32) (comps * sz);
}

static int dl_attr_enabled(GXAttr a)
{
    return (int) a >= 0 && (int) a < 32 && s_vtx.desc[a] != GX_NONE;
}

// (defined near the GXVert shim, far below)
void hal_execute_display_list(const u8* dl, u32 nbytes)
{
    u32 pos = 0;
    if (!dl || nbytes < 3)
        return;
    while (pos + 3 <= nbytes) {
        u8 op = dl[pos];
        GXPrimitive prim;
        GXVtxFmt vat;
        u16 nverts;
        u16 v;
        switch (op & 0xF8) {
        case 0x80:
            prim = GX_QUADS;
            break;
        case 0x90:
            prim = GX_TRIANGLES;
            break;
        case 0x98:
            prim = GX_TRIANGLESTRIP;
            break;
        case 0xA0:
            prim = GX_TRIANGLEFAN;
            break;
        case 0xA8:
            prim = GX_LINES;
            break;
        case 0xB0:
            prim = GX_LINESTRIP;
            break;
        case 0xB8:
            prim = GX_POINTS;
            break;
        default:
            return; // NOP / state op: end of stream
        }
        vat = (GXVtxFmt) (op & 0x07);
        nverts = (u16) (((u16) dl[pos + 1] << 8) | dl[pos + 2]);
        pos += 3;
        gx_hal_begin(prim, vat, nverts);
        for (v = 0; v < nverts; v++) {
            u32 vtx_start = pos; // truncated stream guard, see below
            int a;
            // matrix indices first (FIFO order)
            if (dl_attr_enabled(GX_VA_PNMTXIDX)) {
                if (s_vtx.desc[GX_VA_PNMTXIDX] == GX_DIRECT) {
                    if (!dl_need(dl, nbytes, pos, 1))
                        break;
                    feed_mtx((float) dl[pos++]);
                } else {
                    unsigned idx;
                    if (s_vtx.desc[GX_VA_PNMTXIDX] == GX_INDEX16) {
                        if (!dl_need(dl, nbytes, pos, 2))
                            break;
                        idx = dl_u16(dl + pos);
                        pos += 2;
                    } else {
                        if (!dl_need(dl, nbytes, pos, 1))
                            break;
                        idx = dl[pos++];
                    }
                    feed_index(GX_VA_PNMTXIDX, idx);
                }
            }
            for (a = GX_VA_TEX0MTXIDX; a <= GX_VA_TEX7MTXIDX; a++) {
                if (!dl_attr_enabled((GXAttr) a))
                    continue;
                if (s_vtx.desc[a] == GX_DIRECT) {
                    if (!dl_need(dl, nbytes, pos, 1))
                        break;
                    feed_mtx((float) dl[pos++]);
                } else {
                    unsigned idx;
                    if (s_vtx.desc[a] == GX_INDEX16) {
                        if (!dl_need(dl, nbytes, pos, 2))
                            break;
                        idx = dl_u16(dl + pos);
                        pos += 2;
                    } else {
                        if (!dl_need(dl, nbytes, pos, 1))
                            break;
                        idx = dl[pos++];
                    }
                    feed_index((GXAttr) a, idx);
                }
            }
            if (dl_attr_enabled(GX_VA_POS)) {
                GXAttrType t = s_vtx.desc[GX_VA_POS];
                int comps =
                    (s_vtx.fmt[s_vtxfmt][GX_VA_POS].cnt == GX_POS_XY) ? 2
                                                                     : 3;
                if (t == GX_DIRECT) {
                    float pv[3] = { 0, 0, 0 };
                    dl_feed_numeric(GX_VA_POS, pv, comps, dl, nbytes,
                                    &pos);
                    feed_pos(pv, 3);
                } else {
                    unsigned idx;
                    if (t == GX_INDEX16) {
                        if (!dl_need(dl, nbytes, pos, 2))
                            break;
                        idx = dl_u16(dl + pos);
                        pos += 2;
                    } else {
                        if (!dl_need(dl, nbytes, pos, 1))
                            break;
                        idx = dl[pos++];
                    }
                    feed_index(GX_VA_POS, idx);
                }
            }
            if (dl_attr_enabled(GX_VA_NRM)) {
                GXAttrType t = s_vtx.desc[GX_VA_NRM];
                if (t == GX_DIRECT) {
                    float nv[3] = { 0, 0, 0 };
                    dl_feed_numeric(GX_VA_NRM, nv, 3, dl, nbytes, &pos);
                    feed_nrm(nv);
                } else {
                    unsigned idx;
                    if (t == GX_INDEX16) {
                        if (!dl_need(dl, nbytes, pos, 2))
                            break;
                        idx = dl_u16(dl + pos);
                        pos += 2;
                    } else {
                        if (!dl_need(dl, nbytes, pos, 1))
                            break;
                        idx = dl[pos++];
                    }
                    feed_index(GX_VA_NRM, idx);
                }
            } else if (dl_attr_enabled(GX_VA_NBT)) {
                // NBT supplies 3 normals; the shader uses the first
                // (bump detail is a v1 approximation).
                GXAttrType t = s_vtx.desc[GX_VA_NBT];
                int k;
                for (k = 0; k < 3; k++) {
                    if (t == GX_DIRECT) {
                        float nv[3] = { 0, 0, 0 };
                        dl_feed_numeric(GX_VA_NBT, nv, 3, dl, nbytes,
                                        &pos);
                        if (k == 0)
                            feed_nrm(nv);
                    } else {
                        unsigned idx;
                        if (t == GX_INDEX16) {
                            if (!dl_need(dl, nbytes, pos, 2))
                                break;
                            idx = dl_u16(dl + pos);
                            pos += 2;
                        } else {
                            if (!dl_need(dl, nbytes, pos, 1))
                                break;
                            idx = dl[pos++];
                        }
                        if (k == 0)
                            feed_index(GX_VA_NBT, idx);
                    }
                }
            }
            if (dl_attr_enabled(GX_VA_CLR0) || dl_attr_enabled(GX_VA_CLR1)) {
                // FIFO order: CLR0 data (if enabled) then CLR1 data.
                GXAttr order[2] = { GX_VA_CLR0, GX_VA_CLR1 };
                int k;
                for (k = 0; k < 2; k++) {
                    GXAttr ca = order[k];
                    GXAttrType t;
                    if (!dl_attr_enabled(ca))
                        continue;
                    t = s_vtx.desc[ca];
                    if (t == GX_DIRECT) {
                        dl_feed_color_direct(ca, dl, nbytes, &pos);
                    } else {
                        unsigned idx;
                        if (t == GX_INDEX16) {
                            if (!dl_need(dl, nbytes, pos, 2))
                                break;
                            idx = dl_u16(dl + pos);
                            pos += 2;
                        } else {
                            if (!dl_need(dl, nbytes, pos, 1))
                                break;
                            idx = dl[pos++];
                        }
                        feed_index(ca, idx);
                    }
                }
            }
            for (a = GX_VA_TEX0; a <= GX_VA_TEX7; a++) {
                GXAttrType t;
                int comps;
                if (!dl_attr_enabled((GXAttr) a))
                    continue;
                t = s_vtx.desc[a];
                comps = (s_vtx.fmt[s_vtxfmt][a].cnt == GX_TEX_ST) ? 2 : 1;
                if (t == GX_DIRECT) {
                    float uv[2] = { 0, 0 };
                    dl_feed_numeric((GXAttr) a, uv, comps, dl, nbytes,
                                    &pos);
                    feed_uv(a - GX_VA_TEX0, uv, comps);
                } else {
                    unsigned idx;
                    if (t == GX_INDEX16) {
                        if (!dl_need(dl, nbytes, pos, 2))
                            break;
                        idx = dl_u16(dl + pos);
                        pos += 2;
                    } else {
                        if (!dl_need(dl, nbytes, pos, 1))
                            break;
                        idx = dl[pos++];
                    }
                    feed_index((GXAttr) a, idx);
                }
            }
            if (pos == vtx_start)
                return; // truncated stream: no progress possible
        }
        gx_hal_end();
    }
}

// ------------------------------------------------------------ state setters
void gx_hal_set_vtx_desc(GXAttr attr, GXAttrType type)
{
    if ((int) attr >= 0 && (int) attr < 32)
        s_vtx.desc[attr] = type;
}

void gx_hal_set_vtx_attr_fmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt,
                             GXCompType type, u8 frac)
{
    if ((int) vtxfmt >= 0 && (int) vtxfmt < 8 && (int) attr >= 0 &&
        (int) attr < 32) {
        s_vtx.fmt[vtxfmt][attr].cnt = cnt;
        s_vtx.fmt[vtxfmt][attr].type = type;
        s_vtx.fmt[vtxfmt][attr].frac = frac;
    }
}

void gx_hal_set_array(GXAttr attr, const void* base_ptr, u8 stride)
{
    if ((int) attr >= 0 && (int) attr < 32) {
        s_vtx.array[attr] = (const u8*) base_ptr;
        s_vtx.stride[attr] = stride;
    }
}

void gx_hal_set_viewport(float x, float y, float w, float h)
{
    s_hal.viewport_x = x;
    s_hal.viewport_y = y;
    s_hal.viewport_w = w;
    s_hal.viewport_h = h;
}

void gx_hal_set_scissor(int x, int y, int w, int h)
{
    s_hal.scissor_x = x;
    s_hal.scissor_y = y;
    s_hal.scissor_w = w;
    s_hal.scissor_h = h;
    s_hal.scissor_enabled = (w > 0 && h > 0) ? GX_TRUE : GX_FALSE;
}

void gx_hal_clear(GXColor color, u32 z)
{
    s_hal.clear_color = color;
    s_hal.clear_z = z;
    _glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
    glClearColor(color.r / 255.f, color.g / 255.f, color.b / 255.f,
                 color.a / 255.f);
    glClearDepth((GLclampd) (z & 0xFFFFFF) / (GLclampd) 0xFFFFFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void gx_hal_set_tev_stage(GXTevStageID stage)
{
    s_hal.current_tev_stage = stage;
}

void gx_hal_set_cull_mode(GXCullMode mode)
{
    s_hal.cull_mode = mode;
}

void gx_hal_set_ztest(GXBool enable, GXCompare func, GXBool update)
{
    s_hal.depth_test_enabled = enable;
    s_hal.depth_func = func;
    (void) update;
}

void gx_hal_set_blend_mode(GXBlendMode mode, GXBlendFactor src,
                           GXBlendFactor dst, GXLogicOp logic_op)
{
    s_hal.blend_mode = mode;
    s_hal.blend_src = src;
    s_hal.blend_dst = dst;
    s_hal.logic_op = logic_op;
}

void gx_hal_set_alpha_test(GXBool enable, GXCompare func, u8 ref)
{
    s_hal.alpha_test_enabled = enable;
    s_hal.alpha_func = func;
    s_hal.alpha_ref = ref;
}

void gx_hal_set_fog(GXFogType type, float start, float end, float nearz,
                    float farz, GXColor color)
{
    s_hal.fog_type = type;
    s_hal.fog_start = start;
    s_hal.fog_end = end;
    s_hal.fog_color = color;
    s_hal.fog_enabled = (type == GX_FOG_NONE) ? GX_FALSE : GX_TRUE;
    (void) nearz;
    (void) farz;
}

void gx_hal_set_projection(GXProjectionType type, float left, float right,
                           float top, float bottom, float nearz, float farz)
{
    // Build GL clip-space projection directly (column-major).
    float* m = s_proj;
    int i;
    for (i = 0; i < 16; i++)
        m[i] = 0;
    if (type == GX_ORTHOGRAPHIC) {
        m[0] = 2.f / (right - left);
        m[5] = 2.f / (top - bottom);
        m[10] = -2.f / (farz - nearz);
        m[12] = -(right + left) / (right - left);
        m[13] = -(top + bottom) / (top - bottom);
        m[14] = -(farz + nearz) / (farz - nearz);
        m[15] = 1.f;
    } else {
        m[0] = (2.f * nearz) / (right - left);
        m[5] = (2.f * nearz) / (top - bottom);
        m[8] = (right + left) / (right - left);
        m[9] = (top + bottom) / (top - bottom);
        m[10] = -(farz + nearz) / (farz - nearz);
        m[11] = -1.f;
        m[14] = -(2.f * farz * nearz) / (farz - nearz);
    }
}

void gx_hal_load_projection_mtx(float* mtx, GXProjectionType type)
{
    // GX Mtx44 is row-major; transpose into GL column-major.
    int r, c;
    (void) type;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            s_proj[c * 4 + r] = mtx[r * 4 + c];
}

void gx_hal_load_pos_mtx_imm(float* mtx, u32 mtx_idx)
{
    unsigned slot = (mtx_idx / 3) % 10;
    mat34_to_gl(mtx, s_posmtx[slot]);
}

void gx_hal_load_nrm_mtx_imm(float* mtx, u32 mtx_idx)
{
    unsigned slot = (mtx_idx / 3) % 10;
    mat34_to_gl(mtx, s_nrmmtx[slot]);
}

void gx_hal_load_tex_mtx_imm(float* mtx, u32 mtx_idx)
{
    unsigned slot = ((mtx_idx >= 30 ? mtx_idx - 30 : 0) / 3) % 10;
    mat34_to_gl(mtx, s_texmtx[slot]);
}

void gx_hal_set_num_tex_gens(u8 n)
{
    s_num_tg = n;
    s_hal.num_tex_gens = n;
}

void gx_hal_set_tex_coord_gen(GXTexCoordID dst, GXTexGenType func,
                              GXTexGenSrc src, u32 mtx)
{
    gx_hal_set_tex_coord_gen2(dst, func, src, mtx, GX_FALSE,
                              GX_PTIDENTITY);
}

void gx_hal_set_tex_coord_gen2(GXTexCoordID dst, GXTexGenType func,
                               GXTexGenSrc src, u32 mtx, GXBool normalize,
                               u32 pt_texmtx)
{
    int i = (int) dst;
    (void) normalize;
    (void) pt_texmtx;
    if (i >= 0 && i < 8) {
        s_tg_type[i] = func;
        s_tg_src[i] = src;
        if (func == GX_TG_MTX2x4 || func == GX_TG_MTX3x4)
            s_tg_mtx[i] = (int) ((mtx >= 30 ? mtx - 30 : 0) / 3) % 10;
        else
            s_tg_mtx[i] = -1;
    }
}

void gx_hal_set_tex_coord_scale_manually(GXTexCoordID coord, u8 enable,
                                         u16 ss, u16 ts)
{
    (void) coord;
    (void) enable;
    (void) ss;
    (void) ts;
}

void gx_hal_set_tex_coord_cyl_wrap(GXTexCoordID coord, u8 s_enable,
                                   u8 t_enable)
{
    (void) coord;
    (void) s_enable;
    (void) t_enable;
}

void gx_hal_set_tex_coord_bias(GXTexCoordID coord, u8 s_enable,
                               u8 t_enable)
{
    (void) coord;
    (void) s_enable;
    (void) t_enable;
}

void gx_hal_set_num_chans(u8 n)
{
    s_num_chan = n;
}

void gx_hal_set_chan_ctrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src,
                          GXColorSrc mat_src, u32 light_mask,
                          GXDiffuseFn diff_fn, GXAttnFn attn_fn)
{
    int i = (chan == GX_COLOR1 || chan == GX_ALPHA1) ? 1 : 0;
    s_chan[i].enable = enable;
    s_chan[i].amb_src = amb_src;
    s_chan[i].mat_src = mat_src;
    s_chan[i].light_mask = light_mask;
    s_chan[i].diff_fn = diff_fn;
    s_chan[i].attn_fn = attn_fn;
}

void gx_hal_set_chan_amb_color(GXChannelID chan, GXColor color)
{
    int i = (chan == GX_COLOR1 || chan == GX_ALPHA1) ? 1 : 0;
    s_chan[i].amb = color;
}

void gx_hal_set_chan_mat_color(GXChannelID chan, GXColor color)
{
    int i = (chan == GX_COLOR1 || chan == GX_ALPHA1) ? 1 : 0;
    s_chan[i].mat = color;
}

void gx_hal_invalidate_tex_all(void)
{
    // textures are validated lazily against (data,w,h,fmt); nothing to do.
}

// RGBA8 (bottom-up, GL readback) -> YUYV (top-down, XFB layout).
// The XFB buffers HSD allocates hold 2 bytes/pixel; writing RGBA8 would
// overflow them by 2x, so the copy path converts (BT.601).
static void rgba_to_yuyv(const u8* rgba, u8* yuyv, int w, int h)
{
    int y, x;
    for (y = 0; y < h; y++) {
        const u8* srow = rgba + (size_t) (h - 1 - y) * w * 4; // flip
        u8* drow = yuyv + (size_t) y * w * 2;
        for (x = 0; x < w; x += 2) {
            int r0 = srow[x * 4 + 0], g0 = srow[x * 4 + 1],
                b0 = srow[x * 4 + 2];
            int r1 = srow[x * 4 + 4], g1 = srow[x * 4 + 5],
                b1 = srow[x * 4 + 6];
            int y0 = (77 * r0 + 150 * g0 + 29 * b0) >> 8;
            int y1 = (77 * r1 + 150 * g1 + 29 * b1) >> 8;
            int u = (((-43 * r0 - 84 * g0 + 127 * b0) >> 8) +
                     ((-43 * r1 - 84 * g1 + 127 * b1) >> 8)) /
                        2 +
                    128;
            int v = (((127 * r0 - 106 * g0 - 21 * b0) >> 8) +
                     ((127 * r1 - 106 * g1 - 21 * b1) >> 8)) /
                        2 +
                    128;
            if (y0 < 0)
                y0 = 0;
            if (y0 > 255)
                y0 = 255;
            if (y1 < 0)
                y1 = 0;
            if (y1 > 255)
                y1 = 255;
            if (u < 0)
                u = 0;
            if (u > 255)
                u = 255;
            if (v < 0)
                v = 0;
            if (v > 255)
                v = 255;
            drow[x * 2 + 0] = (u8) y0;
            drow[x * 2 + 1] = (u8) u;
            drow[x * 2 + 2] = (u8) y1;
            drow[x * 2 + 3] = (u8) v;
        }
        // odd width: duplicate the last pixel's luma
        if (w & 1) {
            int r = srow[(w - 1) * 4 + 0], g = srow[(w - 1) * 4 + 1],
                b = srow[(w - 1) * 4 + 2];
            int yy = (77 * r + 150 * g + 29 * b) >> 8;
            if (yy < 0)
                yy = 0;
            if (yy > 255)
                yy = 255;
            drow[(w - 1) * 2 + 0] = (u8) yy;
            drow[(w - 1) * 2 + 1] = 128;
        }
    }
}

void gx_hal_copy_disp(void* dest, GXBool clear)
{
    // Present EFB to the default framebuffer (upscaled blit) and, if the
    // game asked for bytes back, read them down as YUYV into the XFB.
    _glBindFramebuffer(GL_READ_FRAMEBUFFER, s_efb_fbo);
    _glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    _glBlitFramebuffer(0, 0, (GLint) s_efb_w, (GLint) s_efb_h, 0, 0,
                       (GLint) s_win_w, (GLint) s_win_h,
                       GL_COLOR_BUFFER_BIT, GL_LINEAR);
    _glBindFramebuffer(GL_FRAMEBUFFER, s_efb_fbo);
    if (dest) {
        size_t need =
            (size_t) s_efb_w * (size_t) s_efb_h * 4;
        if (need > s_readback_size) {
            free(s_readback);
            s_readback = (u8*) malloc(need ? need : 1);
            s_readback_size = s_readback ? need : 0;
        }
        if (s_readback) {
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, (GLsizei) s_efb_w, (GLsizei) s_efb_h,
                         GL_RGBA, GL_UNSIGNED_BYTE, s_readback);
            rgba_to_yuyv(s_readback, (u8*) dest, s_efb_w, s_efb_h);
        }
    }
    if (clear)
        gx_hal_clear(s_hal.clear_color, s_hal.clear_z);
}

u32 gx_hal_get_tex_buffer_size(u16 width, u16 height, u32 format, u8 mipmap,
                               u8 max_lod)
{
    unsigned size = gx_tex_src_size((int) format, width, height);
    (void) mipmap;
    (void) max_lod;
    return size;
}

// ------------------------------------------------------------ tex objects
// (metadata lives in the TexObjExt side table; see gx_hal_priv.h)
static void hal_tex_ensure(const GXTexObj* obj, int unit)
{
    const TexObjExt* e = hal_texobj_get(obj);
    unsigned w, h, fmt;
    const void* data;
    const void* tlut;
    int tlut_fmt;
    int key, i;
    u8* rgba;
    unsigned gltex;
    if (!e)
        e = hal_texobj_ext(obj);
    w = e->w;
    h = e->h;
    fmt = e->fmt;
    data = e->data;
    tlut = e->tlut;
    tlut_fmt = e->tlut_fmt;
    if (!tlut && e->tlut_name != 0xFFFFFFFFu) {
        // Palettized texture whose TLUT was registered separately via
        // GXLoadTlut(): resolve the name through the API-side registry.
        tlut = hal_tlut_resolve(e->tlut_name, &tlut_fmt);
    }
    if (!data || w == 0 || h == 0) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }
    key = tex_params_key(e);
    for (i = 0; i < TEXCACHE_N; i++) {
        if (s_texcache[i].gltex && s_texcache[i].obj == obj &&
            s_texcache[i].data == data && s_texcache[i].w == w &&
            s_texcache[i].h == h && s_texcache[i].fmt == fmt &&
            s_texcache[i].params_key == key) {
            glBindTexture(GL_TEXTURE_2D, s_texcache[i].gltex);
            return;
        }
    }
    rgba = gx_tex_decode((int) fmt, (const u8*) data, w, h,
                         (const u8*) tlut, tlut_fmt);
    if (!rgba) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }
    glGenTextures(1, &gltex);
    _glActiveTexture((GLenum) (GL_TEXTURE0 + unit));
    gx_tex_upload_gl(gltex, rgba, (int) w, (int) h, (int) e->wrap_s,
                     (int) e->wrap_t, (int) e->min_f, (int) e->mag_f,
                     (int) e->mipmap, 0.f, 1);
    free(rgba);
    {
        TexCacheEnt* c = &s_texcache[s_texcache_next];
        s_texcache_next = (s_texcache_next + 1) % TEXCACHE_N;
        if (c->gltex)
            glDeleteTextures(1, &c->gltex);
        c->obj = obj;
        c->data = data;
        c->w = (u16) w;
        c->h = (u16) h;
        c->fmt = fmt;
        c->gltex = gltex;
        c->params_key = key;
    }
}

void gx_hal_load_tex_obj(GXTexObj* obj, GXTexMapID id)
{
    int unit = (int) id;
    if (unit >= 0 && unit < 8)
        s_hal.texture_objects[unit] = obj;
}

void gx_hal_init_tex_obj(GXTexObj* obj, void* image_ptr, u16 width,
                         u16 height, GXTexFmt format, GXTexWrapMode wrap_s,
                         GXTexWrapMode wrap_t, u8 mipmap)
{
    TexObjExt* e = hal_texobj_ext(obj);
    e->w = width;
    e->h = height;
    e->fmt = (unsigned) format;
    e->wrap_s = (unsigned) wrap_s;
    e->wrap_t = (unsigned) wrap_t;
    e->mipmap = mipmap;
    e->min_f = mipmap ? 5 : 1; // default LOD filters
    e->mag_f = 1;
    e->data = image_ptr;
    e->tlut = NULL;
    e->tlut_fmt = 0;
    e->lod_bias_bits = 0;
}

void gx_hal_init_tex_obj_lod(GXTexObj* obj, GXTexFilter min_filt,
                             GXTexFilter mag_filt, f32 min_lod, f32 max_lod,
                             f32 lod_bias, GXBool bias_clamp,
                             GXBool do_edge_lod, GXAnisotropy max_aniso)
{
    TexObjExt* e = hal_texobj_ext(obj);
    union {
        float f;
        unsigned u;
    } v;
    e->min_f = (unsigned) min_filt;
    e->mag_f = (unsigned) mag_filt;
    v.f = lod_bias;
    e->lod_bias_bits = v.u;
    (void) min_lod;
    (void) max_lod;
    (void) bias_clamp;
    (void) do_edge_lod;
    (void) max_aniso;
}

// ------------------------------------------------------------ swap
void gx_hal_swap_buffers(void)
{
    // VI present happens in hal_video; kept here for API compat.
    glFlush();
}

// ------------------------------------------------------------ internal accessors
// (used by gx_hal_api.c)
TEVStage* hal_tev_stage(int i)
{
    return &s_tev[i & 15];
}
void hal_set_num_tev(int n)
{
    s_num_tev = (n < 1) ? 1 : ((n > 16) ? 16 : n);
    s_hal.num_tev_stages = s_num_tev;
}
int hal_get_num_tev(void)
{
    return s_num_tev;
}
GXColor* hal_tev_reg(int i)
{
    return &s_tevreg[i & 3];
}
GXColor* hal_kcolor(int i)
{
    return &s_kcolor[i & 3];
}
ChanState* hal_chan(int i)
{
    return &s_chan[i & 1];
}
LightState* hal_light(int i)
{
    return &s_lights[i & 7];
}
float* hal_proj(void)
{
    return s_proj;
}
float* hal_posmtx(int i)
{
    return s_posmtx[i & 9];
}
float* hal_nrmmtx(int i)
{
    return s_nrmmtx[i & 9];
}
float* hal_texmtx(int i)
{
    return s_texmtx[i & 9];
}
void hal_set_cur_mtx(int id)
{
    s_cur_mtx = id;
}

// ------------------------------------------------------------ GXVert shim impl
// DIRECT values arrive as host floats already; fixed-point variants are
// scaled by the active vtxfmt (frac). INDEX variants fetch bound arrays.
static float vtx_scale(GXAttr attr)
{
    GXCompType t = s_vtx.fmt[s_vtxfmt][attr].type;
    int frac = s_vtx.fmt[s_vtxfmt][attr].frac;
    if (t == GX_F32)
        return 1.f;
    return 1.f / (float) (1u << frac);
}

void GXPosition3f32(f32 x, f32 y, f32 z)
{
    float v[3] = { x, y, z };
    feed_pos(v, 3);
}
void GXPosition3u8(u8 x, u8 y, u8 z)
{
    float s = vtx_scale(GX_VA_POS);
    float v[3] = { x * s, y * s, z * s };
    feed_pos(v, 3);
}
void GXPosition3s8(s8 x, s8 y, s8 z)
{
    float s = vtx_scale(GX_VA_POS);
    float v[3] = { x * s, y * s, z * s };
    feed_pos(v, 3);
}
void GXPosition3u16(u16 x, u16 y, u16 z)
{
    float s = vtx_scale(GX_VA_POS);
    float v[3] = { x * s, y * s, z * s };
    feed_pos(v, 3);
}
void GXPosition3s16(s16 x, s16 y, s16 z)
{
    float s = vtx_scale(GX_VA_POS);
    float v[3] = { x * s, y * s, z * s };
    feed_pos(v, 3);
}
void GXPosition2f32(f32 x, f32 y)
{
    float v[3] = { x, y, 0 };
    feed_pos(v, 3);
}
void GXPosition2u8(u8 x, u8 y)
{
    float s = vtx_scale(GX_VA_POS);
    float v[3] = { x * s, y * s, 0 };
    feed_pos(v, 3);
}
void GXPosition2s8(s8 x, s8 y)
{
    float s = vtx_scale(GX_VA_POS);
    float v[3] = { x * s, y * s, 0 };
    feed_pos(v, 3);
}
void GXPosition2u16(u16 x, u16 y)
{
    float s = vtx_scale(GX_VA_POS);
    float v[3] = { x * s, y * s, 0 };
    feed_pos(v, 3);
}
void GXPosition2s16(s16 x, s16 y)
{
    float s = vtx_scale(GX_VA_POS);
    float v[3] = { x * s, y * s, 0 };
    feed_pos(v, 3);
}
void GXPosition1x16(u16 x)
{
    feed_index(GX_VA_POS, x);
}
void GXPosition1x8(u8 x)
{
    feed_index(GX_VA_POS, x);
}

void GXNormal3f32(f32 x, f32 y, f32 z)
{
    float v[3] = { x, y, z };
    feed_nrm(v);
}
void GXNormal3s16(s16 x, s16 y, s16 z)
{
    float s = vtx_scale(GX_VA_NRM);
    float v[3] = { x * s, y * s, z * s };
    feed_nrm(v);
}
void GXNormal3s8(s8 x, s8 y, s8 z)
{
    float s = vtx_scale(GX_VA_NRM);
    float v[3] = { x * s, y * s, z * s };
    feed_nrm(v);
}
void GXNormal1x16(u16 x)
{
    feed_index(GX_VA_NRM, x);
}
void GXNormal1x8(u8 x)
{
    feed_index(GX_VA_NRM, x);
}

// Route a color write to the color attribute that is currently "due":
// CLR0 first when enabled, otherwise CLR1. (Some materials enable only
// CLR1; always preferring CLR0 would stall the assembler.)
static GXAttr clr_target(void)
{
    int c0en = (s_enabled_mask & attr_bit(GX_VA_CLR0)) != 0;
    int c1en = (s_enabled_mask & attr_bit(GX_VA_CLR1)) != 0;
    int c0fed = (s_fed_mask & attr_bit(GX_VA_CLR0)) != 0;
    if (c0en && !c0fed)
        return GX_VA_CLR0;
    if (c1en)
        return GX_VA_CLR1;
    return GX_VA_CLR0;
}

void GXColor4u8(u8 r, u8 g, u8 b, u8 a)
{
    float v[4] = { r / 255.f, g / 255.f, b / 255.f, a / 255.f };
    feed_clr(clr_target(), v, 4);
}
void GXColor1u32(u32 c)
{
    // big-endian RGBA8 on the wire; host u32 param is already the value
    float v[4] = { ((c >> 24) & 0xFF) / 255.f, ((c >> 16) & 0xFF) / 255.f,
                   ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f };
    feed_clr(clr_target(), v, 4);
}
void GXColor3u8(u8 r, u8 g, u8 b)
{
    float v[4] = { r / 255.f, g / 255.f, b / 255.f, 1.f };
    feed_clr(clr_target(), v, 3);
}
void GXColor1u16(u16 c)
{
    float v[4] = { ((c >> 11) & 0x1F) * (255.f / 31.f) / 255.f,
                   ((c >> 5) & 0x3F) * (255.f / 63.f) / 255.f,
                   (c & 0x1F) * (255.f / 31.f) / 255.f, 1.f };
    feed_clr(clr_target(), v, 4);
}
void GXColor1x16(u16 x)
{
    feed_index(clr_target(), x);
}
void GXColor1x8(u8 x)
{
    feed_index(clr_target(), x);
}

static int next_tex_unit(void)
{
    // TEXCOORD/FIFO order follows enabled TEX attrs in increasing order;
    // pick the first enabled unit not yet fed this vertex.
    int u;
    for (u = 0; u < 8; u++) {
        if ((s_enabled_mask & attr_bit((GXAttr) (GX_VA_TEX0 + u))) &&
            !(s_fed_mask & attr_bit((GXAttr) (GX_VA_TEX0 + u))))
            return u;
    }
    return 0;
}

void GXTexCoord2f32(f32 s, f32 t)
{
    float v[2] = { s, t };
    feed_uv(next_tex_unit(), v, 2);
}
void GXTexCoord2s16(s16 s, s16 t)
{
    int u = next_tex_unit();
    float sc = vtx_scale((GXAttr) (GX_VA_TEX0 + u));
    float v[2] = { s * sc, t * sc };
    feed_uv(u, v, 2);
}
void GXTexCoord2u16(u16 s, u16 t)
{
    int u = next_tex_unit();
    float sc = vtx_scale((GXAttr) (GX_VA_TEX0 + u));
    float v[2] = { s * sc, t * sc };
    feed_uv(u, v, 2);
}
void GXTexCoord2s8(s8 s, s8 t)
{
    int u = next_tex_unit();
    float sc = vtx_scale((GXAttr) (GX_VA_TEX0 + u));
    float v[2] = { s * sc, t * sc };
    feed_uv(u, v, 2);
}
void GXTexCoord2u8(u8 s, u8 t)
{
    int u = next_tex_unit();
    float sc = vtx_scale((GXAttr) (GX_VA_TEX0 + u));
    float v[2] = { s * sc, t * sc };
    feed_uv(u, v, 2);
}
void GXTexCoord1f32(f32 s)
{
    float v[2] = { s, 0 };
    feed_uv(next_tex_unit(), v, 1);
}
void GXTexCoord1s16(s16 s)
{
    int u = next_tex_unit();
    float sc = vtx_scale((GXAttr) (GX_VA_TEX0 + u));
    float v[2] = { s * sc, 0 };
    feed_uv(u, v, 1);
}
void GXTexCoord1u16(u16 s)
{
    int u = next_tex_unit();
    float sc = vtx_scale((GXAttr) (GX_VA_TEX0 + u));
    float v[2] = { s * sc, 0 };
    feed_uv(u, v, 1);
}
void GXTexCoord1s8(s8 s)
{
    int u = next_tex_unit();
    float sc = vtx_scale((GXAttr) (GX_VA_TEX0 + u));
    float v[2] = { s * sc, 0 };
    feed_uv(u, v, 1);
}
void GXTexCoord1u8(u8 s)
{
    int u = next_tex_unit();
    float sc = vtx_scale((GXAttr) (GX_VA_TEX0 + u));
    float v[2] = { s * sc, 0 };
    feed_uv(u, v, 1);
}
void GXTexCoord1x16(u16 x)
{
    feed_index((GXAttr) (GX_VA_TEX0 + next_tex_unit()), x);
}
void GXTexCoord1x8(u8 x)
{
    feed_index((GXAttr) (GX_VA_TEX0 + next_tex_unit()), x);
}

void GXMatrixIndex1u8(u8 x)
{
    feed_mtx((float) x);
}

void GXCmd1u8(u8 x)
{
    (void) x;
}
void GXCmd1u16(u16 x)
{
    (void) x;
}
void GXCmd1u32(u32 x)
{
    (void) x;
}
void GXParam1u8(u8 x)
{
    (void) x;
}
void GXParam1u16(u16 x)
{
    (void) x;
}
void GXParam1u32(u32 x)
{
    (void) x;
}
void GXParam1s8(s8 x)
{
    (void) x;
}
void GXParam1s16(s16 x)
{
    (void) x;
}
void GXParam1s32(s32 x)
{
    (void) x;
}
void GXParam1f32(f32 x)
{
    (void) x;
}
void GXParam3f32(f32 x, f32 y, f32 z)
{
    (void) x;
    (void) y;
    (void) z;
}
void GXParam4f32(f32 x, f32 y, f32 z, f32 w)
{
    (void) x;
    (void) y;
    (void) z;
    (void) w;
}
