// Public GameCube GX API implemented on top of the OpenGL HAL backend.
//
// Every function below has the exact signature from libs/dolphin/include,
// so game code (melee/*, sysdolphin/*) links unchanged. Behavior notes:
// - FIFO / display-list / perf / "poke" (CPU->EFB) APIs are emulated or
//   stubbed: display lists execute immediately instead of recording.
// - GXLightObj / GXTlutObj are opaque in the original headers; this file
//   owns their PC-side layout (see HalLightObj / s_tlut_* below).

#include "gx_hal.h"
#include "gx_hal_priv.h"
#include "gl_loader.h"

#include <dolphin/gx.h>
#include <dolphin/os/OSThread.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Forward declarations (used before definition below).
void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b,
                     GXTevColorArg c, GXTevColorArg d);
void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b,
                     GXTevAlphaArg c, GXTevAlphaArg d);
void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                     GXTevScale scale, GXBool clamp, GXTevRegID out_reg);
void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                     GXTevScale scale, GXBool clamp, GXTevRegID out_reg);
void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz,
                   f32 farz);

#define HAL_PI 3.14159265358979323846f

// ------------------------------------------------------------ draw sync state
static GXDrawSyncCallback s_sync_cb;
static GXDrawDoneCallback s_done_cb;
static u16 s_draw_token;

// ------------------------------------------------------------ fifo state
static GXFifoObj s_cpu_fifo;
static GXFifoObj s_gp_fifo;
static GXFifoObj* s_cur_cpu = NULL;
static GXFifoObj* s_cur_gp = NULL;
static GXBreakPtCallback s_break_cb;
static u32 s_overflow;

// ------------------------------------------------------------ misc GX state
static GXBool s_color_update = GX_TRUE;
static GXBool s_alpha_update = GX_TRUE;
static GXBool s_zcomp_before_tex = GX_TRUE;
static GXPixelFmt s_pix_fmt = GX_PF_RGB8_Z24;
static GXZFmt16 s_z_fmt;
static GXBool s_dither = GX_FALSE;
static GXBool s_dst_alpha_en = GX_FALSE;
static u8 s_dst_alpha;
static GXBool s_odd_mask = GX_TRUE, s_even_mask = GX_TRUE;
static GXBool s_field_mode, s_half_aspect;
static GXBool s_coplanar = GX_FALSE;
static GXClipMode s_clip = GX_CLIP_ENABLE;
static s32 s_scissor_ox, s_scissor_oy;
static u8 s_line_width = 6;
static GXTexOffset s_line_off = GX_TO_ZERO;
static u8 s_point_size = 6;
static GXTexOffset s_point_off = GX_TO_ZERO;
static float s_viewport[6];
static GXWarningLevel s_verify_level = GX_WARN_NONE;
static GXVerifyCallback s_verify_cb;

// copy/filter state
static u16 s_copy_src_l, s_copy_src_t, s_copy_src_w, s_copy_src_h;
static u16 s_copy_dst_w, s_copy_dst_h;
static GXTexFmt s_copy_dst_fmt;
static GXBool s_copy_dst_mip;
static GXCopyMode s_copy_field = GX_COPY_PROGRESSIVE;
static GXFBClamp s_copy_clamp = GX_CLAMP_NONE;
static GXColor s_copy_clear_c;
static u32 s_copy_clear_z = 0xFFFFFFu;
static GXBool s_copy_aa;
static u8 s_copy_samp[12][2];
static GXBool s_copy_vf;
static u8 s_copy_vfilter[7];
static GXGamma s_copy_gamma = GX_GM_1_0;

// display-list recording (executed immediately on PC)
static int s_in_disp_list;

// alpha compare full state
static GXCompare s_ac0 = GX_ALWAYS, s_ac1 = GX_ALWAYS;
static u8 s_aref0, s_aref1;
static GXAlphaOp s_aop = GX_AOP_AND;

// z-texture
static GXZTexOp s_ztex_op = GX_ZT_DISABLE;
static GXTexFmt s_ztex_fmt;
static u32 s_ztex_bias;

// tev clamp / swap tables
static GXTevSwapSel s_swap_ras[16], s_swap_tex[16];
static int s_swap_table[4][4];

// PC-side light object layout (64 bytes == sizeof(GXLightObj))
typedef struct {
    float px, py, pz, pw;
    float cr, cg, cb, cpad;
    float a0, a1, a2, apad;
    float k0, k1, k2, kpad;
} HalLightObj;

// Spot state (cutoff angle + falloff fn) doesn't fit the 64-byte object,
// so it lives in a side table keyed by object pointer (same pattern as
// the TLUT registry below). Default cutoff 180 = spot disabled.
typedef struct {
    const GXLightObj* obj;
    f32 cutoff;
    GXSpotFn spot;
    int used;
} LightSpotSlot;
#define LIGHTSPOT_N 32
static LightSpotSlot s_spots[LIGHTSPOT_N];

static void hal_light_set_spot(const GXLightObj* obj, f32 cutoff,
                               GXSpotFn spot)
{
    int i, free_slot = -1;
    for (i = 0; i < LIGHTSPOT_N; i++) {
        if (s_spots[i].used && s_spots[i].obj == obj) {
            s_spots[i].cutoff = cutoff;
            s_spots[i].spot = spot;
            return;
        }
        if (!s_spots[i].used && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        free_slot = 0;
    s_spots[free_slot].used = 1;
    s_spots[free_slot].obj = obj;
    s_spots[free_slot].cutoff = cutoff;
    s_spots[free_slot].spot = spot;
}

static f32 hal_light_get_cutoff(const GXLightObj* obj)
{
    int i;
    for (i = 0; i < LIGHTSPOT_N; i++) {
        if (s_spots[i].used && s_spots[i].obj == obj)
            return s_spots[i].cutoff;
    }
    return 180.f;
}

// PC-side TLUT object (12 bytes == sizeof(GXTlutObj))
typedef struct {
    const void* data;
    unsigned fmt;
    unsigned entries;
} HalTlutObj;

typedef struct {
    const GXTlutObj* obj;
    HalTlutObj info;
    int used;
} TlutObjSlot;
#define TLUTOBJ_N 64
static TlutObjSlot s_tlutobj[TLUTOBJ_N];

typedef struct {
    const void* data;
    int fmt;
    int entries;
    int used;
} TlutNameSlot;
static TlutNameSlot s_tlut_by_name[20]; // GX_TLUT0..15 + BIGTLUT0..3

const void* hal_tlut_resolve(unsigned name, int* fmt_out)
{
    if (name < 20 && s_tlut_by_name[name].used) {
        if (fmt_out)
            *fmt_out = s_tlut_by_name[name].fmt;
        return s_tlut_by_name[name].data;
    }
    return NULL;
}

static HalTlutObj* tlutobj_ext(const GXTlutObj* o)
{
    int i, free_slot = -1;
    for (i = 0; i < TLUTOBJ_N; i++) {
        if (s_tlutobj[i].used && s_tlutobj[i].obj == o)
            return &s_tlutobj[i].info;
        if (!s_tlutobj[i].used && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        free_slot = 0;
    memset(&s_tlutobj[free_slot], 0, sizeof(TlutObjSlot));
    s_tlutobj[free_slot].used = 1;
    s_tlutobj[free_slot].obj = o;
    return &s_tlutobj[free_slot].info;
}

static const HalTlutObj* tlutobj_get(const GXTlutObj* o)
{
    int i;
    for (i = 0; i < TLUTOBJ_N; i++) {
        if (s_tlutobj[i].used && s_tlutobj[i].obj == o)
            return &s_tlutobj[i].info;
    }
    return NULL;
}

// tex region dummies (TMEM does not exist on PC)
static GXTexRegion s_tex_region_dummy;
static GXTlutRegion s_tlut_region_dummy;
static GXTexRegionCallback s_tex_region_cb;
static GXTlutRegionCallback s_tlut_region_cb;

// ================================================================ GXManage
BOOL IsWriteGatherBufferEmpty(void)
{
    return TRUE;
}

static GXFifoObj s_init_fifo;

GXFifoObj* GXInit(void* base, u32 size)
{
    (void) base;
    (void) size;
    memset(&s_init_fifo, 0, sizeof(s_init_fifo));
    s_cur_cpu = &s_init_fifo;
    s_cur_gp = &s_init_fifo;
    return &s_init_fifo;
}

void GXSetMisc(GXMiscToken token, u32 val)
{
    (void) token;
    (void) val;
}

void GXFlush(void)
{
    glFlush();
}

void GXResetWriteGatherPipe(void)
{
}

void GXAbortFrame(void)
{
    // Next GXBegin() resets the assembler; nothing to flush.
}

void GXSetDrawSync(u16 token)
{
    s_draw_token = token;
    if (s_sync_cb)
        s_sync_cb(token);
}

u16 GXReadDrawSync(void)
{
    return s_draw_token;
}

void GXSetDrawDone(void)
{
    glFinish();
    if (s_done_cb)
        s_done_cb();
}

void GXWaitDrawDone(void)
{
    glFinish();
}

void GXDrawDone(void)
{
    glFinish();
    if (s_done_cb)
        s_done_cb();
}

void GXPixModeSync(void)
{
}

void GXTexModeSync(void)
{
}

GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback cb)
{
    GXDrawSyncCallback prev = s_sync_cb;
    s_sync_cb = cb;
    return prev;
}

GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb)
{
    GXDrawDoneCallback prev = s_done_cb;
    s_done_cb = cb;
    return prev;
}

// command-list register id tables (BP addresses, informational on PC)
u8 GXTexMode0Ids[8] = { 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87 };
u8 GXTexMode1Ids[8] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7 };
u8 GXTexImage0Ids[8] = { 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF };
u8 GXTexImage1Ids[8] = { 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7 };
u8 GXTexImage2Ids[8] = { 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF };
u8 GXTexImage3Ids[8] = { 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7 };
u8 GXTexTlutIds[8] = { 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F };

// ================================================================ GXFifo
void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size)
{
    (void) fifo;
    (void) base;
    (void) size;
}

void GXInitFifoPtrs(GXFifoObj* fifo, void* readPtr, void* writePtr)
{
    (void) fifo;
    (void) readPtr;
    (void) writePtr;
}

void GXInitFifoLimits(GXFifoObj* fifo, u32 hiWatermark, u32 loWatermark)
{
    (void) fifo;
    (void) hiWatermark;
    (void) loWatermark;
}

void GXSetCPUFifo(GXFifoObj* fifo)
{
    s_cur_cpu = fifo ? fifo : &s_cpu_fifo;
}

void GXSetGPFifo(GXFifoObj* fifo)
{
    s_cur_gp = fifo ? fifo : &s_gp_fifo;
}

void GXSaveCPUFifo(GXFifoObj* fifo)
{
    if (fifo)
        *fifo = *s_cur_cpu;
}

void GXSaveGPFifo(GXFifoObj* fifo)
{
    if (fifo)
        *fifo = *s_cur_gp;
}

void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* readIdle,
                   GXBool* cmdIdle, GXBool* brkpt)
{
    if (overhi)
        *overhi = GX_FALSE;
    if (underlow)
        *underlow = GX_FALSE;
    if (readIdle)
        *readIdle = GX_TRUE;
    if (cmdIdle)
        *cmdIdle = GX_TRUE;
    if (brkpt)
        *brkpt = GX_FALSE;
}

void GXGetFifoStatus(GXFifoObj* fifo, GXBool* overhi, GXBool* underflow,
                     u32* fifoCount, GXBool* cpuWrite, GXBool* gpRead,
                     GXBool* fifowrap)
{
    (void) fifo;
    if (overhi)
        *overhi = GX_FALSE;
    if (underflow)
        *underflow = GX_FALSE;
    if (fifoCount)
        *fifoCount = 0;
    if (cpuWrite)
        *cpuWrite = GX_TRUE;
    if (gpRead)
        *gpRead = GX_TRUE;
    if (fifowrap)
        *fifowrap = GX_FALSE;
}

void GXGetFifoPtrs(GXFifoObj* fifo, void** readPtr, void** writePtr)
{
    (void) fifo;
    if (readPtr)
        *readPtr = NULL;
    if (writePtr)
        *writePtr = NULL;
}

void* GXGetFifoBase(GXFifoObj* fifo)
{
    (void) fifo;
    return NULL;
}

u32 GXGetFifoSize(GXFifoObj* fifo)
{
    (void) fifo;
    return 0;
}

void GXGetFifoLimits(GXFifoObj* fifo, u32* hi, u32* lo)
{
    (void) fifo;
    if (hi)
        *hi = 0;
    if (lo)
        *lo = 0;
}

GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback cb)
{
    GXBreakPtCallback prev = s_break_cb;
    s_break_cb = cb;
    return prev;
}

void GXEnableBreakPt(void* break_pt)
{
    (void) break_pt;
}

void GXDisableBreakPt(void)
{
}

OSThread* GXSetCurrentGXThread(void)
{
    return NULL;
}

OSThread* GXGetCurrentGXThread(void)
{
    return NULL;
}

GXFifoObj* GXGetCPUFifo(void)
{
    return s_cur_cpu ? s_cur_cpu : &s_cpu_fifo;
}

GXFifoObj* GXGetGPFifo(void)
{
    return s_cur_gp ? s_cur_gp : &s_gp_fifo;
}

u32 GXGetOverflowCount(void)
{
    return s_overflow;
}

u32 GXResetOverflowCount(void)
{
    u32 v = s_overflow;
    s_overflow = 0;
    return v;
}

volatile void* GXRedirectWriteGatherPipe(void* ptr)
{
    return (volatile void*) ptr;
}

void GXRestoreWriteGatherPipe(void)
{
}

// ================================================================ GXGeometry
void GXSetVtxDesc(GXAttr attr, GXAttrType type)
{
    gx_hal_set_vtx_desc(attr, type);
}

void GXSetVtxDescv(const GXVtxDescList* attrPtr)
{
    GXClearVtxDesc();
    if (!attrPtr)
        return;
    while (attrPtr->attr != GX_VA_NULL) {
        gx_hal_set_vtx_desc(attrPtr->attr, attrPtr->type);
        attrPtr++;
    }
}

void GXClearVtxDesc(void)
{
    int a;
    for (a = 0; a < 32; a++)
        gx_hal_set_vtx_desc((GXAttr) a, GX_NONE);
}

void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt,
                     GXCompType type, u8 frac)
{
    gx_hal_set_vtx_attr_fmt(vtxfmt, attr, cnt, type, frac);
}

void GXSetVtxAttrFmtv(GXVtxFmt vtxfmt, const GXVtxAttrFmtList* list)
{
    if (!list)
        return;
    while (list->attr != GX_VA_NULL) {
        gx_hal_set_vtx_attr_fmt(vtxfmt, list->attr, list->cnt,
                                list->type, list->frac);
        list++;
    }
}

void GXSetArray(GXAttr attr, const void* base_ptr, u8 stride)
{
    gx_hal_set_array(attr, base_ptr, stride);
}

void GXInvalidateVtxCache(void)
{
}

void GXSetTexCoordGen2(GXTexCoordID dst_coord, GXTexGenType func,
                       GXTexGenSrc src_param, u32 mtx, GXBool normalize,
                       u32 pt_texmtx)
{
    gx_hal_set_tex_coord_gen2(dst_coord, func, src_param, mtx, normalize,
                              pt_texmtx);
}

void GXSetNumTexGens(u8 nTexGens)
{
    gx_hal_set_num_tex_gens(nTexGens);
}

void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts)
{
    gx_hal_begin(type, vtxfmt, nverts);
}

void GXSetLineWidth(u8 width, GXTexOffset texOffsets)
{
    s_line_width = width;
    s_line_off = texOffsets;
}

void GXSetPointSize(u8 pointSize, GXTexOffset texOffsets)
{
    s_point_size = pointSize;
    s_point_off = texOffsets;
}

void GXEnableTexOffsets(GXTexCoordID coord, u8 line_enable,
                        u8 point_enable)
{
    (void) coord;
    (void) line_enable;
    (void) point_enable;
}

// GXEnd is an empty inline in the original header (vertices flow through
// the GP FIFO). The PC shim header declares it as a real function; it
// flushes the HAL vertex assembler here.
void GXEnd(void)
{
    gx_hal_end();
}

// ================================================================ GXTransform
void GXProject(f32 x, f32 y, f32 z, f32 mtx[3][4], f32* pm, f32* vp,
               f32* sx, f32* sy, f32* sz)
{
    // view transform (row-major 3x4) then projection + viewport map
    float vx = mtx[0][0] * x + mtx[0][1] * y + mtx[0][2] * z + mtx[0][3];
    float vy = mtx[1][0] * x + mtx[1][1] * y + mtx[1][2] * z + mtx[1][3];
    float vz = mtx[2][0] * x + mtx[2][1] * y + mtx[2][2] * z + mtx[2][3];
    // pm is a GX projection (row-major 4x4-ish, GX_PROJECTION_SZ=7 floats
    // for perspective). Approximate with a plain perspective divide using
    // the projection stored in the HAL.
    float* proj = hal_proj();
    float cx = proj[0] * vx + proj[4] * vy + proj[8] * vz + proj[12];
    float cy = proj[1] * vx + proj[5] * vy + proj[9] * vz + proj[13];
    float cw = proj[3] * vx + proj[7] * vy + proj[11] * vz + proj[15];
    (void) pm;
    if (cw == 0)
        cw = 1e-6f;
    {
        float nx = cx / cw, ny = cy / cw;
        *sx = vp[0] + (1.f + nx) * vp[2] * 0.5f;
        *sy = vp[1] + (1.f - ny) * vp[3] * 0.5f;
        *sz = vz;
    }
}

void GXSetProjection(f32 mtx[4][4], GXProjectionType type)
{
    gx_hal_load_projection_mtx(&mtx[0][0], type);
}

void GXSetProjectionv(f32* ptr)
{
    gx_hal_load_projection_mtx(ptr, GX_PERSPECTIVE);
}

void GXLoadPosMtxImm(f32 mtx[3][4], u32 id)
{
    gx_hal_load_pos_mtx_imm(&mtx[0][0], id);
}

void GXLoadPosMtxIndx(u16 mtx_indx, u32 id)
{
    // Indexed matrix store has no PC equivalent; game code always pairs
    // this with an immediate load path on first use. No-op.
    (void) mtx_indx;
    (void) id;
}

void GXLoadNrmMtxImm(f32 mtx[3][4], u32 id)
{
    gx_hal_load_nrm_mtx_imm(&mtx[0][0], id);
}

void GXLoadNrmMtxImm3x3(f32 mtx[3][3], u32 id)
{
    float m34[12];
    m34[0] = mtx[0][0];
    m34[1] = mtx[0][1];
    m34[2] = mtx[0][2];
    m34[3] = 0;
    m34[4] = mtx[1][0];
    m34[5] = mtx[1][1];
    m34[6] = mtx[1][2];
    m34[7] = 0;
    m34[8] = mtx[2][0];
    m34[9] = mtx[2][1];
    m34[10] = mtx[2][2];
    m34[11] = 0;
    gx_hal_load_nrm_mtx_imm(m34, id);
}

void GXLoadNrmMtxIndx3x3(u16 mtx_indx, u32 id)
{
    (void) mtx_indx;
    (void) id;
}

void GXSetCurrentMtx(u32 id)
{
    hal_set_cur_mtx((int) id);
}

void GXLoadTexMtxImm(f32 mtx[][4], u32 id, GXTexMtxType type)
{
    float m34[12];
    if (type == GX_MTX2x4) {
        m34[0] = mtx[0][0];
        m34[1] = mtx[0][1];
        m34[2] = mtx[0][2];
        m34[3] = mtx[0][3];
        m34[4] = mtx[1][0];
        m34[5] = mtx[1][1];
        m34[6] = mtx[1][2];
        m34[7] = mtx[1][3];
        m34[8] = 0;
        m34[9] = 0;
        m34[10] = 0;
        m34[11] = 1;
    } else {
        memcpy(m34, mtx, sizeof(m34));
    }
    gx_hal_load_tex_mtx_imm(m34, id);
}

void GXLoadTexMtxIndx(u16 mtx_indx, u32 id, GXTexMtxType type)
{
    (void) mtx_indx;
    (void) id;
    (void) type;
}

void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz,
                         f32 farz, u32 field)
{
    (void) field;
    GXSetViewport(left, top, wd, ht, nearz, farz);
}

void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz)
{
    gx_hal_set_viewport(left, top, wd, ht);
    s_viewport[0] = left;
    s_viewport[1] = top;
    s_viewport[2] = wd;
    s_viewport[3] = ht;
    s_viewport[4] = nearz;
    s_viewport[5] = farz;
    glDepthRange((GLclampd) nearz, (GLclampd) farz);
}

void GXSetScissorBoxOffset(s32 x_off, s32 y_off)
{
    s_scissor_ox = x_off;
    s_scissor_oy = y_off;
}

void GXSetClipMode(GXClipMode mode)
{
    s_clip = mode;
}

// ================================================================ GXPixel
void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz,
              GXColor color)
{
    gx_hal_set_fog(type, startz, endz, nearz, farz, color);
}

void GXInitFogAdjTable(GXFogAdjTable* table, u16 width, f32 projmtx[4][4])
{
    (void) table;
    (void) width;
    (void) projmtx;
}

void GXSetFogRangeAdj(GXBool enable, u16 center, GXFogAdjTable* table)
{
    (void) enable;
    (void) center;
    (void) table;
}

void GXSetBlendMode(GXBlendMode type, GXBlendFactor src_factor,
                    GXBlendFactor dst_factor, GXLogicOp op)
{
    gx_hal_set_blend_mode(type, src_factor, dst_factor, op);
}

static void apply_color_mask(void)
{
    glColorMask((GLboolean) (s_color_update ? GL_TRUE : GL_FALSE),
                (GLboolean) (s_color_update ? GL_TRUE : GL_FALSE),
                (GLboolean) (s_color_update ? GL_TRUE : GL_FALSE),
                (GLboolean) (s_alpha_update ? GL_TRUE : GL_FALSE));
}

void GXSetColorUpdate(GXBool update_enable)
{
    s_color_update = update_enable;
    apply_color_mask();
}

void GXSetAlphaUpdate(GXBool update_enable)
{
    s_alpha_update = update_enable;
    apply_color_mask();
}

void GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable)
{
    gx_hal_set_ztest(compare_enable, func, update_enable);
}

void GXSetZCompLoc(GXBool before_tex)
{
    s_zcomp_before_tex = before_tex;
}

void GXSetPixelFmt(GXPixelFmt pix_fmt, GXZFmt16 z_fmt)
{
    s_pix_fmt = pix_fmt;
    s_z_fmt = z_fmt;
}

void GXSetDither(GXBool dither)
{
    s_dither = dither;
    if (dither)
        glEnable(GL_DITHER);
    else
        glDisable(GL_DITHER);
}

void GXSetDstAlpha(GXBool enable, u8 alpha)
{
    s_dst_alpha_en = enable;
    s_dst_alpha = alpha;
}

void GXSetFieldMask(GXBool odd_mask, GXBool even_mask)
{
    s_odd_mask = odd_mask;
    s_even_mask = even_mask;
}

void GXSetFieldMode(GXBool field_mode, GXBool half_aspect_ratio)
{
    s_field_mode = field_mode;
    s_half_aspect = half_aspect_ratio;
}

// ================================================================ GXCull
void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht)
{
    gx_hal_set_scissor((int) (left + (u32) s_scissor_ox),
                       (int) (top + (u32) s_scissor_oy), (int) wd,
                       (int) ht);
}

void GXSetCullMode(GXCullMode mode)
{
    gx_hal_set_cull_mode(mode);
}

void GXSetCoPlanar(GXBool enable)
{
    s_coplanar = enable;
}

// ================================================================ GXLighting
void GXInitLightAttn(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2, f32 k0,
                     f32 k1, f32 k2)
{
    HalLightObj* o = (HalLightObj*) lt_obj;
    o->a0 = a0;
    o->a1 = a1;
    o->a2 = a2;
    o->k0 = k0;
    o->k1 = k1;
    o->k2 = k2;
}

void GXInitLightAttnA(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2)
{
    HalLightObj* o = (HalLightObj*) lt_obj;
    o->a0 = a0;
    o->a1 = a1;
    o->a2 = a2;
}

void GXInitLightAttnK(GXLightObj* lt_obj, f32 k0, f32 k1, f32 k2)
{
    HalLightObj* o = (HalLightObj*) lt_obj;
    o->k0 = k0;
    o->k1 = k1;
    o->k2 = k2;
}

void GXInitLightSpot(GXLightObj* lt_obj, f32 cutoff, GXSpotFn spot_func)
{
    (void) lt_obj;
    hal_light_set_spot(lt_obj, cutoff, spot_func);
}

void GXInitLightDistAttn(GXLightObj* lt_obj, f32 ref_dist, f32 ref_br,
                         GXDistAttnFn dist_func)
{
    (void) lt_obj;
    (void) ref_dist;
    (void) ref_br;
    (void) dist_func;
}

void GXInitLightPos(GXLightObj* lt_obj, f32 x, f32 y, f32 z)
{
    HalLightObj* o = (HalLightObj*) lt_obj;
    o->px = x;
    o->py = y;
    o->pz = z;
    o->pw = 1.f;
}

void GXInitLightDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz)
{
    HalLightObj* o = (HalLightObj*) lt_obj;
    // Directional: store negated direction as position-at-infinity.
    o->px = -nx;
    o->py = -ny;
    o->pz = -nz;
    o->pw = 0.f;
}

void GXInitSpecularDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz)
{
    GXInitLightDir(lt_obj, nx, ny, nz);
}

void GXInitSpecularDirHA(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz, f32 hx,
                         f32 hy, f32 hz)
{
    (void) hx;
    (void) hy;
    (void) hz;
    GXInitLightDir(lt_obj, nx, ny, nz);
}

void GXInitLightColor(GXLightObj* lt_obj, GXColor color)
{
    HalLightObj* o = (HalLightObj*) lt_obj;
    o->cr = color.r / 255.f;
    o->cg = color.g / 255.f;
    o->cb = color.b / 255.f;
}

static int light_id_to_index(GXLightID light)
{
    int i;
    for (i = 0; i < 8; i++) {
        if (light & (GXLightID) (1u << i))
            return i;
    }
    return 0;
}

void GXLoadLightObjImm(GXLightObj* lt_obj, GXLightID light)
{
    const HalLightObj* o = (const HalLightObj*) lt_obj;
    LightState* dst = hal_light(light_id_to_index(light));
    dst->x = o->px;
    dst->y = o->py;
    dst->z = o->pz;
    dst->w = o->pw;
    dst->r = o->cr;
    dst->g = o->cg;
    dst->b = o->cb;
    dst->a0 = o->a0;
    dst->a1 = o->a1;
    dst->a2 = o->a2;
    dst->k0 = o->k0;
    dst->k1 = o->k1;
    dst->k2 = o->k2;
    dst->cutoff = hal_light_get_cutoff(lt_obj);
}

void GXLoadLightObjIndx(u32 lt_obj_indx, GXLightID light)
{
    (void) lt_obj_indx;
    (void) light;
}

void GXSetChanAmbColor(GXChannelID chan, GXColor amb_color)
{
    gx_hal_set_chan_amb_color(chan, amb_color);
}

void GXSetChanMatColor(GXChannelID chan, GXColor mat_color)
{
    gx_hal_set_chan_mat_color(chan, mat_color);
}

void GXSetNumChans(u8 nChans)
{
    gx_hal_set_num_chans(nChans);
}

void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src,
                   GXColorSrc mat_src, u32 light_mask, GXDiffuseFn diff_fn,
                   GXAttnFn attn_fn)
{
    gx_hal_set_chan_ctrl(chan, enable, amb_src, mat_src, light_mask,
                         diff_fn, attn_fn);
}

// ================================================================ GXTev
void GXSetTevOp(GXTevStageID id, GXTevMode mode)
{
    TEVStage* st = hal_tev_stage(id);
    switch (mode) {
    case GX_MODULATE: // out = tex * ras
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC,
                        GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA,
                        GX_CA_ZERO);
        break;
    case GX_DECAL: // out = ras*(1-texA) + tex*texA
        GXSetTevColorIn(id, GX_CC_RASC, GX_CC_TEXC, GX_CC_TEXA,
                        GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_RASA, GX_CA_TEXA, GX_CA_TEXA,
                        GX_CA_ZERO);
        break;
    case GX_BLEND: // out = ras*(1-konst) + tex*konst
        GXSetTevColorIn(id, GX_CC_RASC, GX_CC_TEXC, GX_CC_KONST,
                        GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_RASA, GX_CA_TEXA, GX_CA_KONST,
                        GX_CA_ZERO);
        break;
    case GX_REPLACE:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO,
                        GX_CC_TEXC);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO,
                        GX_CA_TEXA);
        break;
    case GX_PASSCLR:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO,
                        GX_CC_RASC);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO,
                        GX_CA_RASA);
        break;
    default:
        break;
    }
    GXSetTevColorOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    GXSetTevAlphaOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                    GX_TEVPREV);
    (void) st;
}

void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b,
                     GXTevColorArg c, GXTevColorArg d)
{
    TEVStage* st = hal_tev_stage(stage);
    st->ca = a;
    st->cb = b;
    st->cc = c;
    st->cd = d;
}

void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b,
                     GXTevAlphaArg c, GXTevAlphaArg d)
{
    TEVStage* st = hal_tev_stage(stage);
    st->aa = a;
    st->ab = b;
    st->ac = c;
    st->ad = d;
}

void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                     GXTevScale scale, GXBool clamp, GXTevRegID out_reg)
{
    TEVStage* st = hal_tev_stage(stage);
    st->cop = op;
    st->cbias = bias;
    st->cscale = scale;
    st->cclamp = clamp;
    st->cout = out_reg;
}

void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                     GXTevScale scale, GXBool clamp, GXTevRegID out_reg)
{
    TEVStage* st = hal_tev_stage(stage);
    st->aop = op;
    st->abias = bias;
    st->ascale = scale;
    st->aclamp = clamp;
    st->aout = out_reg;
}

void GXSetTevColor(GXTevRegID id, GXColor color)
{
    if (id >= GX_TEVREG0 && id <= GX_TEVREG2)
        *hal_tev_reg(id - GX_TEVREG0) = color;
}

void GXSetTevColorS10(GXTevRegID id, GXColorS10 color)
{
    GXColor c;
    c.r = (u8) (color.r < 0 ? 0 : (color.r > 255 ? 255 : color.r));
    c.g = (u8) (color.g < 0 ? 0 : (color.g > 255 ? 255 : color.g));
    c.b = (u8) (color.b < 0 ? 0 : (color.b > 255 ? 255 : color.b));
    c.a = (u8) (color.a < 0 ? 0 : (color.a > 255 ? 255 : color.a));
    GXSetTevColor(id, c);
}

void GXSetTevKColor(GXTevKColorID id, GXColor color)
{
    if (id >= GX_KCOLOR0 && id <= GX_KCOLOR3)
        *hal_kcolor(id) = color;
}

void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel)
{
    hal_tev_stage(stage)->kcsel = sel;
}

void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel)
{
    hal_tev_stage(stage)->kasel = sel;
}

void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel,
                      GXTevSwapSel tex_sel)
{
    if ((int) stage >= 0 && (int) stage < 16) {
        s_swap_ras[stage] = ras_sel;
        s_swap_tex[stage] = tex_sel;
    }
}

void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red,
                           GXTevColorChan green, GXTevColorChan blue,
                           GXTevColorChan alpha)
{
    if ((int) table >= 0 && (int) table < 4) {
        s_swap_table[table][0] = red;
        s_swap_table[table][1] = green;
        s_swap_table[table][2] = blue;
        s_swap_table[table][3] = alpha;
    }
}

void GXSetTevClampMode(int a, int b)
{
    (void) a;
    (void) b;
}

void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op,
                       GXCompare comp1, u8 ref1)
{
    s_ac0 = comp0;
    s_aref0 = ref0;
    s_aop = op;
    s_ac1 = comp1;
    s_aref1 = ref1;
    // Fragment generator handles a single compare; combine here:
    // if both are ALWAYS -> disabled; if comp1 is ALWAYS -> use comp0.
    if (comp0 == GX_ALWAYS && comp1 == GX_ALWAYS) {
        gx_hal_set_alpha_test(GX_FALSE, GX_ALWAYS, 0);
    } else if (comp1 == GX_ALWAYS) {
        gx_hal_set_alpha_test(GX_TRUE, comp0, ref0);
    } else if (comp0 == GX_ALWAYS) {
        gx_hal_set_alpha_test(GX_TRUE, comp1, ref1);
    } else {
        // Two-sided compare with AND/OR: approximate with comp0
        // (documented limitation; Melee UI uses single compares).
        (void) op;
        gx_hal_set_alpha_test(GX_TRUE, comp0, ref0);
    }
}

void GXSetZTexture(GXZTexOp op, GXTexFmt fmt, u32 bias)
{
    s_ztex_op = op;
    s_ztex_fmt = fmt;
    s_ztex_bias = bias;
}

void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map,
                   GXChannelID color)
{
    TEVStage* st = hal_tev_stage(stage);
    st->coord = coord;
    st->map = map;
    st->color = color;
}

void GXSetNumTevStages(u8 nStages)
{
    hal_set_num_tev(nStages);
}

// indirect stages: approximated as direct (documented limitation)
void GXSetTevIndirect(GXTevStageID tev_stage, GXIndTexStageID ind_stage,
                      GXIndTexFormat format, GXIndTexBiasSel bias_sel,
                      GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s,
                      GXIndTexWrap wrap_t, GXBool add_prev, GXBool utc_lod,
                      GXIndTexAlphaSel alpha_sel)
{
    (void) tev_stage;
    (void) ind_stage;
    (void) format;
    (void) bias_sel;
    (void) matrix_sel;
    (void) wrap_s;
    (void) wrap_t;
    (void) add_prev;
    (void) utc_lod;
    (void) alpha_sel;
}

void GXSetIndTexMtx(GXIndTexMtxID mtx_id, f32 offset[2][3], s8 scale_exp)
{
    (void) mtx_id;
    (void) offset;
    (void) scale_exp;
}

void GXSetIndTexCoordScale(GXIndTexStageID ind_state, GXIndTexScale scale_s,
                           GXIndTexScale scale_t)
{
    (void) ind_state;
    (void) scale_s;
    (void) scale_t;
}

void GXSetIndTexOrder(GXIndTexStageID ind_stage, GXTexCoordID tex_coord,
                      GXTexMapID tex_map)
{
    (void) ind_stage;
    (void) tex_coord;
    (void) tex_map;
}

void GXSetNumIndStages(u8 nIndStages)
{
    (void) nIndStages;
}

void GXSetTevDirect(GXTevStageID tev_stage)
{
    (void) tev_stage;
}

void GXSetTevIndWarp(GXTevStageID tev_stage, GXIndTexStageID ind_stage,
                     u8 signed_offset, u8 replace_mode,
                     GXIndTexMtxID matrix_sel)
{
    (void) tev_stage;
    (void) ind_stage;
    (void) signed_offset;
    (void) replace_mode;
    (void) matrix_sel;
}

void GXSetTevIndTile(GXTevStageID tev_stage, GXIndTexStageID ind_stage,
                     u16 tilesize_s, u16 tilesize_t, u16 tilespacing_s,
                     u16 tilespacing_t, GXIndTexFormat format,
                     GXIndTexMtxID matrix_sel, GXIndTexBiasSel bias_sel,
                     GXIndTexAlphaSel alpha_sel)
{
    (void) tev_stage;
    (void) ind_stage;
    (void) tilesize_s;
    (void) tilesize_t;
    (void) tilespacing_s;
    (void) tilespacing_t;
    (void) format;
    (void) matrix_sel;
    (void) bias_sel;
    (void) alpha_sel;
}

void GXSetTevIndBumpST(GXTevStageID tev_stage, GXIndTexStageID ind_stage,
                       GXIndTexMtxID matrix_sel)
{
    (void) tev_stage;
    (void) ind_stage;
    (void) matrix_sel;
}

void GXSetTevIndBumpXYZ(GXTevStageID tev_stage, GXIndTexStageID ind_stage,
                        GXIndTexMtxID matrix_sel)
{
    (void) tev_stage;
    (void) ind_stage;
    (void) matrix_sel;
}

void GXSetTevIndRepeat(GXTevStageID tev_stage)
{
    (void) tev_stage;
}

// ================================================================ GXTexture
u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, u8 mipmap,
                       u8 max_lod)
{
    return gx_hal_get_tex_buffer_size(width, height, format, mipmap,
                                      max_lod);
}

void GXInitTexObj(GXTexObj* obj, void* image_ptr, u16 width, u16 height,
                  GXTexFmt format, GXTexWrapMode wrap_s,
                  GXTexWrapMode wrap_t, u8 mipmap)
{
    gx_hal_init_tex_obj(obj, image_ptr, width, height, format, wrap_s,
                        wrap_t, mipmap);
}

void GXInitTexObjCI(GXTexObj* obj, void* image_ptr, u16 width, u16 height,
                    GXTexFmt format, GXTexWrapMode wrap_s,
                    GXTexWrapMode wrap_t, u8 mipmap, u32 tlut_name)
{
    gx_hal_init_tex_obj(obj, image_ptr, width, height, format, wrap_s,
                        wrap_t, mipmap);
    hal_texobj_ext(obj)->tlut_name = tlut_name;
}

void GXInitTexObjLOD(GXTexObj* obj, GXTexFilter min_filt,
                     GXTexFilter mag_filt, f32 min_lod, f32 max_lod,
                     f32 lod_bias, GXBool bias_clamp, GXBool do_edge_lod,
                     GXAnisotropy max_aniso)
{
    gx_hal_init_tex_obj_lod(obj, min_filt, mag_filt, min_lod, max_lod,
                            lod_bias, bias_clamp, do_edge_lod, max_aniso);
}

void GXInitTexObjData(GXTexObj* obj, void* image_ptr)
{
    hal_texobj_ext(obj)->data = image_ptr;
}

void GXInitTexObjWrapMode(GXTexObj* obj, GXTexWrapMode s, GXTexWrapMode t)
{
    TexObjExt* e = hal_texobj_ext(obj);
    e->wrap_s = (unsigned) s;
    e->wrap_t = (unsigned) t;
}

void GXInitTexObjTlut(GXTexObj* obj, u32 tlut_name)
{
    hal_texobj_ext(obj)->tlut_name = tlut_name;
}

void GXInitTexObjUserData(GXTexObj* obj, void* user_data)
{
    hal_texobj_ext(obj)->userdata = user_data;
}

void* GXGetTexObjUserData(const GXTexObj* obj)
{
    const TexObjExt* e = hal_texobj_get(obj);
    return e ? e->userdata : NULL;
}

void GXLoadTexObjPreLoaded(GXTexObj* obj, GXTexRegion* region,
                           GXTexMapID id)
{
    (void) region;
    gx_hal_load_tex_obj(obj, id);
}

void GXLoadTexObj(GXTexObj* obj, GXTexMapID id)
{
    gx_hal_load_tex_obj(obj, id);
}

void GXInitTlutObj(GXTlutObj* tlut_obj, void* lut, GXTlutFmt fmt,
                   u16 n_entries)
{
    HalTlutObj* e = tlutobj_ext(tlut_obj);
    e->data = lut;
    e->fmt = (unsigned) fmt;
    e->entries = n_entries;
}

void GXLoadTlut(GXTlutObj* tlut_obj, u32 tlut_name)
{
    const HalTlutObj* e = tlutobj_get(tlut_obj);
    if (e && tlut_name < 20) {
        s_tlut_by_name[tlut_name].data = e->data;
        s_tlut_by_name[tlut_name].fmt = (int) e->fmt;
        s_tlut_by_name[tlut_name].entries = (int) e->entries;
        s_tlut_by_name[tlut_name].used = 1;
    }
}

void GXInitTexCacheRegion(GXTexRegion* region, u8 is_32b_mipmap,
                          u32 tmem_even, GXTexCacheSize size_even,
                          u32 tmem_odd, GXTexCacheSize size_odd)
{
    (void) region;
    (void) is_32b_mipmap;
    (void) tmem_even;
    (void) size_even;
    (void) tmem_odd;
    (void) size_odd;
}

void GXInitTexPreLoadRegion(GXTexRegion* region, u32 tmem_even,
                            u32 size_even, u32 tmem_odd, u32 size_odd)
{
    (void) region;
    (void) tmem_even;
    (void) size_even;
    (void) tmem_odd;
    (void) size_odd;
}

void GXInitTlutRegion(GXTlutRegion* region, u32 tmem_addr,
                      GXTlutSize tlut_size)
{
    (void) region;
    (void) tmem_addr;
    (void) tlut_size;
}

void GXInvalidateTexRegion(GXTexRegion* region)
{
    (void) region;
}

void GXInvalidateTexAll(void)
{
    gx_hal_invalidate_tex_all();
}

GXTexRegionCallback GXSetTexRegionCallback(GXTexRegionCallback f)
{
    GXTexRegionCallback prev = s_tex_region_cb;
    s_tex_region_cb = f;
    return prev;
}

GXTlutRegionCallback GXSetTlutRegionCallback(GXTlutRegionCallback f)
{
    GXTlutRegionCallback prev = s_tlut_region_cb;
    s_tlut_region_cb = f;
    return prev;
}

void GXPreLoadEntireTexture(GXTexObj* tex_obj, GXTexRegion* region)
{
    (void) region;
    // force decode+upload now (unit 0 scratch)
    gx_hal_load_tex_obj(tex_obj, GX_TEXMAP0);
}

void GXSetTexCoordScaleManually(GXTexCoordID coord, u8 enable, u16 ss,
                                u16 ts)
{
    gx_hal_set_tex_coord_scale_manually(coord, enable, ss, ts);
}

void GXSetTexCoordCylWrap(GXTexCoordID coord, u8 s_enable, u8 t_enable)
{
    gx_hal_set_tex_coord_cyl_wrap(coord, s_enable, t_enable);
}

void GXSetTexCoordBias(GXTexCoordID coord, u8 s_enable, u8 t_enable)
{
    gx_hal_set_tex_coord_bias(coord, s_enable, t_enable);
}

// ================================================================ GXGet
void GXGetVtxDesc(GXAttr attr, GXAttrType* type)
{
    // state lives in the backend; expose via a query helper would need
    // new API — report DIRECT for POS (safe default), NONE otherwise.
    (void) attr;
    if (type)
        *type = GX_NONE;
}

void GXGetVtxDescv(GXVtxDescList* vcd)
{
    if (vcd) {
        vcd->attr = GX_VA_NULL;
        vcd->type = GX_NONE;
    }
}

void GXGetVtxAttrFmt(GXVtxFmt fmt, GXAttr attr, GXCompCnt* cnt,
                     GXCompType* type, u8* frac)
{
    (void) fmt;
    (void) attr;
    if (cnt)
        *cnt = 0;
    if (type)
        *type = 0;
    if (frac)
        *frac = 0;
}

void GXGetVtxAttrFmtv(GXVtxFmt fmt, GXVtxAttrFmtList* vat)
{
    (void) fmt;
    if (vat)
        vat->attr = GX_VA_NULL;
}

void GXGetLineWidth(u8* width, GXTexOffset* texOffsets)
{
    if (width)
        *width = s_line_width;
    if (texOffsets)
        *texOffsets = s_line_off;
}

void GXGetPointSize(u8* pointSize, GXTexOffset* texOffsets)
{
    if (pointSize)
        *pointSize = s_point_size;
    if (texOffsets)
        *texOffsets = s_point_off;
}

void GXGetCullMode(GXCullMode* mode)
{
    if (mode)
        *mode = gx_hal_get_context()->cull_mode;
}

void GXGetLightAttnA(GXLightObj* lt_obj, f32* a0, f32* a1, f32* a2)
{
    const HalLightObj* o = (const HalLightObj*) lt_obj;
    if (a0)
        *a0 = o->a0;
    if (a1)
        *a1 = o->a1;
    if (a2)
        *a2 = o->a2;
}

void GXGetLightAttnK(GXLightObj* lt_obj, f32* k0, f32* k1, f32* k2)
{
    const HalLightObj* o = (const HalLightObj*) lt_obj;
    if (k0)
        *k0 = o->k0;
    if (k1)
        *k1 = o->k1;
    if (k2)
        *k2 = o->k2;
}

void GXGetLightPos(GXLightObj* lt_obj, f32* x, f32* y, f32* z)
{
    const HalLightObj* o = (const HalLightObj*) lt_obj;
    if (x)
        *x = o->px;
    if (y)
        *y = o->py;
    if (z)
        *z = o->pz;
}

void GXGetLightDir(GXLightObj* lt_obj, f32* nx, f32* ny, f32* nz)
{
    const HalLightObj* o = (const HalLightObj*) lt_obj;
    if (nx)
        *nx = -o->px;
    if (ny)
        *ny = -o->py;
    if (nz)
        *nz = -o->pz;
}

void GXGetLightColor(GXLightObj* lt_obj, GXColor* color)
{
    const HalLightObj* o = (const HalLightObj*) lt_obj;
    if (color) {
        color->r = (u8) (o->cr * 255.f);
        color->g = (u8) (o->cg * 255.f);
        color->b = (u8) (o->cb * 255.f);
        color->a = 255;
    }
}

GXBool GXGetTexObjMipMap(const GXTexObj* to)
{
    const TexObjExt* e = hal_texobj_get(to);
    return e ? (GXBool) e->mipmap : GX_FALSE;
}

GXTexFmt GXGetTexObjFmt(const GXTexObj* to)
{
    const TexObjExt* e = hal_texobj_get(to);
    return e ? (GXTexFmt) e->fmt : 0;
}

u16 GXGetTexObjWidth(const GXTexObj* to)
{
    const TexObjExt* e = hal_texobj_get(to);
    return e ? (u16) e->w : 0;
}

u16 GXGetTexObjHeight(const GXTexObj* to)
{
    const TexObjExt* e = hal_texobj_get(to);
    return e ? (u16) e->h : 0;
}

GXTexWrapMode GXGetTexObjWrapS(const GXTexObj* to)
{
    const TexObjExt* e = hal_texobj_get(to);
    return e ? (GXTexWrapMode) e->wrap_s : GX_CLAMP;
}

GXTexWrapMode GXGetTexObjWrapT(const GXTexObj* to)
{
    const TexObjExt* e = hal_texobj_get(to);
    return e ? (GXTexWrapMode) e->wrap_t : GX_CLAMP;
}

void* GXGetTexObjData(const GXTexObj* to)
{
    const TexObjExt* e = hal_texobj_get(to);
    return e ? (void*) e->data : NULL;
}

void GXGetTexObjAll(const GXTexObj* obj, void** image_ptr, u16* width,
                    u16* height, GXTexFmt* format, GXTexWrapMode* wrap_s,
                    GXTexWrapMode* wrap_t, u8* mipmap)
{
    const TexObjExt* e = hal_texobj_get(obj);
    if (!e)
        return;
    if (image_ptr)
        *image_ptr = (void*) e->data;
    if (width)
        *width = (u16) e->w;
    if (height)
        *height = (u16) e->h;
    if (format)
        *format = (GXTexFmt) e->fmt;
    if (wrap_s)
        *wrap_s = (GXTexWrapMode) e->wrap_s;
    if (wrap_t)
        *wrap_t = (GXTexWrapMode) e->wrap_t;
    if (mipmap)
        *mipmap = (u8) e->mipmap;
}

void GXGetTexObjLODAll(const GXTexObj* tex_obj, GXTexFilter* min_filt,
                       GXTexFilter* mag_filt, f32* min_lod, f32* max_lod,
                       f32* lod_bias, u8* bias_clamp, u8* do_edge_lod,
                       GXAnisotropy* max_aniso)
{
    const TexObjExt* e = hal_texobj_get(tex_obj);
    if (!e)
        return;
    if (min_filt)
        *min_filt = (GXTexFilter) e->min_f;
    if (mag_filt)
        *mag_filt = (GXTexFilter) e->mag_f;
    if (min_lod)
        *min_lod = 0;
    if (max_lod)
        *max_lod = 0;
    if (lod_bias) {
        union {
            unsigned u;
            float f;
        } v;
        v.u = e->lod_bias_bits;
        *lod_bias = v.f;
    }
    if (bias_clamp)
        *bias_clamp = 0;
    if (do_edge_lod)
        *do_edge_lod = 0;
    if (max_aniso)
        *max_aniso = GX_ANISO_1;
}

GXTexFilter GXGetTexObjMinFilt(const GXTexObj* tex_obj)
{
    const TexObjExt* e = hal_texobj_get(tex_obj);
    return e ? (GXTexFilter) e->min_f : GX_LINEAR;
}

GXTexFilter GXGetTexObjMagFilt(const GXTexObj* tex_obj)
{
    const TexObjExt* e = hal_texobj_get(tex_obj);
    return e ? (GXTexFilter) e->mag_f : GX_LINEAR;
}

f32 GXGetTexObjMinLOD(const GXTexObj* tex_obj)
{
    (void) tex_obj;
    return 0;
}

f32 GXGetTexObjMaxLOD(const GXTexObj* tex_obj)
{
    (void) tex_obj;
    return 0;
}

f32 GXGetTexObjLODBias(const GXTexObj* tex_obj)
{
    const TexObjExt* e = hal_texobj_get(tex_obj);
    if (!e)
        return 0;
    {
        union {
            unsigned u;
            float f;
        } v;
        v.u = e->lod_bias_bits;
        return v.f;
    }
}

GXBool GXGetTexObjBiasClamp(const GXTexObj* tex_obj)
{
    (void) tex_obj;
    return GX_FALSE;
}

GXBool GXGetTexObjEdgeLOD(const GXTexObj* tex_obj)
{
    (void) tex_obj;
    return GX_FALSE;
}

GXAnisotropy GXGetTexObjMaxAniso(const GXTexObj* tex_obj)
{
    (void) tex_obj;
    return GX_ANISO_1;
}

u32 GXGetTexObjTlut(const GXTexObj* tex_obj)
{
    const TexObjExt* e = hal_texobj_get(tex_obj);
    return e ? e->tlut_name : 0xFFFFFFFFu;
}

void GXGetTlutObjAll(const GXTlutObj* tlut_obj, void** data,
                     GXTlutFmt* format, u16* numEntries)
{
    const HalTlutObj* e = tlutobj_get(tlut_obj);
    if (!e)
        return;
    if (data)
        *data = (void*) e->data;
    if (format)
        *format = (GXTlutFmt) e->fmt;
    if (numEntries)
        *numEntries = (u16) e->entries;
}

void* GXGetTlutObjData(const GXTlutObj* tlut_obj)
{
    const HalTlutObj* e = tlutobj_get(tlut_obj);
    return e ? (void*) e->data : NULL;
}

GXTlutFmt GXGetTlutObjFmt(const GXTlutObj* tlut_obj)
{
    const HalTlutObj* e = tlutobj_get(tlut_obj);
    return e ? (GXTlutFmt) e->fmt : GX_TL_IA8;
}

u16 GXGetTlutObjNumEntries(const GXTlutObj* tlut_obj)
{
    const HalTlutObj* e = tlutobj_get(tlut_obj);
    return e ? (u16) e->entries : 0;
}

void GXGetTexRegionAll(const GXTexRegion* region, u8* is_cached,
                       u8* is_32b_mipmap, u32* tmem_even, u32* size_even,
                       u32* tmem_odd, u32* size_odd)
{
    (void) region;
    if (is_cached)
        *is_cached = 0;
    if (is_32b_mipmap)
        *is_32b_mipmap = 0;
    if (tmem_even)
        *tmem_even = 0;
    if (size_even)
        *size_even = 0;
    if (tmem_odd)
        *tmem_odd = 0;
    if (size_odd)
        *size_odd = 0;
}

void GXGetTlutRegionAll(const GXTlutRegion* region, u32* tmem_addr,
                        GXTlutSize* tlut_size)
{
    (void) region;
    if (tmem_addr)
        *tmem_addr = 0;
    if (tlut_size)
        *tlut_size = GX_TLUT_256;
}

void GXGetProjectionv(f32* ptr)
{
    if (ptr)
        memcpy(ptr, hal_proj(), sizeof(float) * 16);
}

void GXGetViewportv(f32* vp)
{
    if (vp)
        memcpy(vp, s_viewport, sizeof(s_viewport));
}

void GXGetScissor(u32* left, u32* top, u32* wd, u32* ht)
{
    GXHALContext* c = gx_hal_get_context();
    if (left)
        *left = (u32) c->scissor_x;
    if (top)
        *top = (u32) c->scissor_y;
    if (wd)
        *wd = (u32) c->scissor_w;
    if (ht)
        *ht = (u32) c->scissor_h;
}

// ================================================================ GXFrameBuffer
void GXAdjustForOverscan(GXRenderModeObj* rmin, GXRenderModeObj* rmout,
                         u16 hor, u16 ver)
{
    if (!rmin || !rmout)
        return;
    *rmout = *rmin;
    if (rmout->viWidth > hor)
        rmout->viWidth -= hor;
    if (rmout->viHeight > ver)
        rmout->viHeight -= ver;
    rmout->viXOrigin += hor / 2;
    rmout->viYOrigin += ver / 2;
}

void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht)
{
    s_copy_src_l = left;
    s_copy_src_t = top;
    s_copy_src_w = wd;
    s_copy_src_h = ht;
}

void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht)
{
    s_copy_src_l = left;
    s_copy_src_t = top;
    s_copy_src_w = wd;
    s_copy_src_h = ht;
}

void GXSetDispCopyDst(u16 wd, u16 ht)
{
    s_copy_dst_w = wd;
    s_copy_dst_h = ht;
}

void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap)
{
    s_copy_dst_w = wd;
    s_copy_dst_h = ht;
    s_copy_dst_fmt = fmt;
    s_copy_dst_mip = mipmap;
}

void GXSetDispCopyFrame2Field(GXCopyMode mode)
{
    s_copy_field = mode;
}

void GXSetCopyClamp(GXFBClamp clamp)
{
    s_copy_clamp = clamp;
}

u32 GXSetDispCopyYScale(f32 vscale)
{
    (void) vscale;
    return 1;
}

void GXSetCopyClear(GXColor clear_clr, u32 clear_z)
{
    s_copy_clear_c = clear_clr;
    s_copy_clear_z = clear_z;
}

void GXSetCopyFilter(GXBool aa, const u8 sample_pattern[12][2], GXBool vf,
                     const u8 vfilter[7])
{
    s_copy_aa = aa;
    if (sample_pattern)
        memcpy(s_copy_samp, sample_pattern, sizeof(s_copy_samp));
    s_copy_vf = vf;
    if (vfilter)
        memcpy(s_copy_vfilter, vfilter, sizeof(s_copy_vfilter));
}

void GXSetDispCopyGamma(GXGamma gamma)
{
    s_copy_gamma = gamma;
}

void GXCopyDisp(void* dest, GXBool clear)
{
    gx_hal_copy_disp(dest, clear);
}

void GXCopyTex(void* dest, GXBool clear)
{
    gx_hal_copy_disp(dest, clear);
}

void GXClearBoundingBox(void)
{
}

void GXReadBoundingBox(u16* left, u16* top, u16* right, u16* bottom)
{
    if (left)
        *left = 0;
    if (top)
        *top = 0;
    if (right)
        *right = 0;
    if (bottom)
        *bottom = 0;
}

// ================================================================ GXDispList
void GXBeginDisplayList(void* list, u32 size)
{
    (void) list;
    (void) size;
    s_in_disp_list++;
}

u32 GXEndDisplayList(void)
{
    if (s_in_disp_list > 0)
        s_in_disp_list--;
    return 0;
}

void GXCallDisplayList(void* list, u32 nbytes)
{
    // Static PObj geometry arrives as prebuilt GX command streams;
    // execute them through the HAL vertex assembler (see gx_hal.c).
    // (Nothing in the game records lists at runtime, so the record path
    // stays a no-op.)
    if (list && nbytes >= 3)
        hal_execute_display_list((const u8*) list, nbytes);
}

// ================================================================ GXDraw (shapes)
void GXDrawCube(void)
{
    static const float n[6][3] = { { 0, 0, 1 },  { 0, 0, -1 }, { 1, 0, 0 },
                                   { -1, 0, 0 }, { 0, 1, 0 },  { 0, -1, 0 } };
    static const float q[6][4][3] = {
        { { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } },
        { { 1, -1, -1 }, { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 } },
        { { 1, -1, 1 }, { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 } },
        { { -1, -1, -1 }, { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 } },
        { { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 }, { -1, 1, -1 } },
        { { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 } },
    };
    int f, v;
    GXBegin(GX_QUADS, GX_VTXFMT0, 24);
    for (f = 0; f < 6; f++) {
        for (v = 0; v < 4; v++) {
            GXNormal3f32(n[f][0], n[f][1], n[f][2]);
            GXPosition3f32(q[f][v][0], q[f][v][1], q[f][v][2]);
        }
    }
    gx_hal_end();
}

void GXDrawSphere(u8 numMajor, u8 numMinor)
{
    int stacks = (numMinor < 4) ? 4 : numMinor;
    int slices = (numMajor < 4) ? 4 : numMajor;
    int i, j;
    GXBegin(GX_TRIANGLES, GX_VTXFMT0,
            (u16) (stacks * slices * 6));
    for (i = 0; i < stacks; i++) {
        for (j = 0; j < slices; j++) {
            float t0 = (float) i / stacks, t1 = (float) (i + 1) / stacks;
            float p0 = (float) j / slices, p1 = (float) (j + 1) / slices;
            float v[4][3];
            int k;
            float th, ph;
            th = t0 * 3.14159265f;
            ph = p0 * 2.f * 3.14159265f;
            v[0][0] = sinf(th) * cosf(ph);
            v[0][1] = cosf(th);
            v[0][2] = sinf(th) * sinf(ph);
            th = t1 * 3.14159265f;
            v[1][0] = sinf(th) * cosf(ph);
            v[1][1] = cosf(th);
            v[1][2] = sinf(th) * sinf(ph);
            ph = p1 * 2.f * 3.14159265f;
            v[2][0] = sinf(th) * cosf(ph);
            v[2][1] = cosf(th);
            v[2][2] = sinf(th) * sinf(ph);
            th = t0 * 3.14159265f;
            v[3][0] = sinf(th) * cosf(ph);
            v[3][1] = cosf(th);
            v[3][2] = sinf(th) * sinf(ph);
            {
                int idx[6] = { 0, 1, 2, 0, 2, 3 };
                for (k = 0; k < 6; k++) {
                    GXNormal3f32(v[idx[k]][0], v[idx[k]][1],
                                 v[idx[k]][2]);
                    GXPosition3f32(v[idx[k]][0], v[idx[k]][1],
                                   v[idx[k]][2]);
                }
            }
        }
    }
    gx_hal_end();
}

void GXDrawCylinder(u8 numEdges)
{
    int n = (numEdges < 3) ? 3 : numEdges;
    int i;
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16) (n * 6));
    for (i = 0; i < n; i++) {
        float a0 = (float) i / n * 2.f * 3.14159265f;
        float a1 = (float) (i + 1) / n * 2.f * 3.14159265f;
        float x0 = cosf(a0), z0 = sinf(a0);
        float x1 = cosf(a1), z1 = sinf(a1);
        GXNormal3f32(x0, 0, z0);
        GXPosition3f32(x0, -1, z0);
        GXNormal3f32(x1, 0, z1);
        GXPosition3f32(x1, -1, z1);
        GXNormal3f32(x1, 0, z1);
        GXPosition3f32(x1, 1, z1);
        GXNormal3f32(x0, 0, z0);
        GXPosition3f32(x0, -1, z0);
        GXNormal3f32(x1, 0, z1);
        GXPosition3f32(x1, 1, z1);
        GXNormal3f32(x0, 0, z0);
        GXPosition3f32(x0, 1, z0);
    }
    gx_hal_end();
}

void GXDrawTorus(f32 rc, u8 numc, u8 numt)
{
    int nc = (numc < 4) ? 4 : numc, nt = (numt < 4) ? 4 : numt;
    int i, j, k;
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16) (nc * nt * 6));
    for (i = 0; i < nc; i++) {
        for (j = 0; j < nt; j++) {
            float u0 = (float) i / nc * 2.f * 3.14159265f;
            float u1 = (float) (i + 1) / nc * 2.f * 3.14159265f;
            float v0 = (float) j / nt * 2.f * 3.14159265f;
            float v1 = (float) (j + 1) / nt * 2.f * 3.14159265f;
            float p[4][3], nn[4][3];
            float uu[4] = { u0, u1, u1, u0 }, vv[4] = { v0, v0, v1, v1 };
            for (k = 0; k < 4; k++) {
                float cu = cosf(uu[k]), su = sinf(uu[k]);
                float cv = cosf(vv[k]), sv = sinf(vv[k]);
                nn[k][0] = cu * cv;
                nn[k][1] = sv;
                nn[k][2] = su * cv;
                p[k][0] = (1.f + rc * cv) * cu;
                p[k][1] = rc * sv;
                p[k][2] = (1.f + rc * cv) * su;
            }
            {
                int idx[6] = { 0, 1, 2, 0, 2, 3 };
                for (k = 0; k < 6; k++) {
                    GXNormal3f32(nn[idx[k]][0], nn[idx[k]][1],
                                 nn[idx[k]][2]);
                    GXPosition3f32(p[idx[k]][0], p[idx[k]][1],
                                   p[idx[k]][2]);
                }
            }
        }
    }
    gx_hal_end();
}

void GXDrawDodeca(void)
{
    GXDrawSphere(8, 6);
}

void GXDrawOctahedron(void)
{
    GXDrawSphere(4, 3);
}

void GXDrawIcosahedron(void)
{
    GXDrawSphere(10, 8);
}

void GXDrawSphere1(u8 depth)
{
    u8 n = (u8) (6 + depth * 2);
    GXDrawSphere(n, n);
}

u32 GXGenNormalTable(u8 depth, f32* table)
{
    u32 n = 0;
    int stacks = 4 + depth, slices = 6 + depth * 2;
    int i, j;
    if (!table)
        return 0;
    for (i = 0; i <= stacks && n < 512; i++) {
        for (j = 0; j < slices && n < 512; j++) {
            float th = (float) i / stacks * 3.14159265f;
            float ph = (float) j / slices * 2.f * 3.14159265f;
            table[n * 3 + 0] = sinf(th) * cosf(ph);
            table[n * 3 + 1] = cosf(th);
            table[n * 3 + 2] = sinf(th) * sinf(ph);
            n++;
        }
    }
    return n;
}

// ================================================================ GXPerf
void GXSetGPMetric(GXPerf0 perf0, GXPerf1 perf1)
{
    (void) perf0;
    (void) perf1;
}

void GXReadGPMetric(u32* cnt0, u32* cnt1)
{
    if (cnt0)
        *cnt0 = 0;
    if (cnt1)
        *cnt1 = 0;
}

void GXClearGPMetric(void)
{
}

u32 GXReadGP0Metric(void)
{
    return 0;
}

u32 GXReadGP1Metric(void)
{
    return 0;
}

void GXReadMemMetric(u32* cp_req, u32* tc_req, u32* cpu_rd_req,
                     u32* cpu_wr_req, u32* dsp_req, u32* io_req,
                     u32* vi_req, u32* pe_req, u32* rf_req, u32* fi_req)
{
    if (cp_req)
        *cp_req = 0;
    if (tc_req)
        *tc_req = 0;
    if (cpu_rd_req)
        *cpu_rd_req = 0;
    if (cpu_wr_req)
        *cpu_wr_req = 0;
    if (dsp_req)
        *dsp_req = 0;
    if (io_req)
        *io_req = 0;
    if (vi_req)
        *vi_req = 0;
    if (pe_req)
        *pe_req = 0;
    if (rf_req)
        *rf_req = 0;
    if (fi_req)
        *fi_req = 0;
}

void GXClearMemMetric(void)
{
}

void GXReadPixMetric(u32* top_pixels_in, u32* top_pixels_out,
                     u32* bot_pixels_in, u32* bot_pixels_out,
                     u32* clr_pixels_in, u32* copy_clks)
{
    if (top_pixels_in)
        *top_pixels_in = 0;
    if (top_pixels_out)
        *top_pixels_out = 0;
    if (bot_pixels_in)
        *bot_pixels_in = 0;
    if (bot_pixels_out)
        *bot_pixels_out = 0;
    if (clr_pixels_in)
        *clr_pixels_in = 0;
    if (copy_clks)
        *copy_clks = 0;
}

void GXClearPixMetric(void)
{
}

void GXSetVCacheMetric(GXVCachePerf attr)
{
    (void) attr;
}

void GXReadVCacheMetric(u32* check, u32* miss, u32* stall)
{
    if (check)
        *check = 0;
    if (miss)
        *miss = 0;
    if (stall)
        *stall = 0;
}

void GXClearVCacheMetric(void)
{
}

void GXInitXfRasMetric(void)
{
}

void GXReadXfRasMetric(u32* xf_wait_in, u32* xf_wait_out, u32* ras_busy,
                       u32* clocks)
{
    if (xf_wait_in)
        *xf_wait_in = 0;
    if (xf_wait_out)
        *xf_wait_out = 0;
    if (ras_busy)
        *ras_busy = 0;
    if (clocks)
        *clocks = 0;
}

u32 GXReadClksPerVtx(void)
{
    return 0;
}

// ================================================================ GXCpu2Efb
// CPU access to the EFB. Peek reads back the current EFB framebuffer;
// Poke has no meaningful GL equivalent mid-frame and is a no-op.
void GXPokeAlphaMode(GXCompare func, u8 threshold)
{
    (void) func;
    (void) threshold;
}

void GXPokeAlphaRead(GXAlphaReadMode mode)
{
    (void) mode;
}

void GXPokeAlphaUpdate(GXBool update_enable)
{
    (void) update_enable;
}

void GXPokeBlendMode(GXBlendMode type, GXBlendFactor src_factor,
                     GXBlendFactor dst_factor, GXLogicOp op)
{
    (void) type;
    (void) src_factor;
    (void) dst_factor;
    (void) op;
}

void GXPokeColorUpdate(GXBool update_enable)
{
    (void) update_enable;
}

void GXPokeDstAlpha(GXBool enable, u8 alpha)
{
    (void) enable;
    (void) alpha;
}

void GXPokeDither(GXBool dither)
{
    (void) dither;
}

void GXPokeZMode(GXBool compare_enable, GXCompare func, GXBool update_enable)
{
    (void) compare_enable;
    (void) func;
    (void) update_enable;
}

void GXPeekARGB(u16 x, u16 y, u32* color)
{
    u8 px[4] = { 0, 0, 0, 255 };
    GXHALContext* c = gx_hal_get_context();
    if ((int) x < c->framebuffer_width &&
        (int) y < c->framebuffer_height) {
        // NOTE: caller must be between frames, not mid-draw.
        gx_hal_bind_efb();
        glReadPixels((GLint) x, (GLint) y, 1, 1, GL_RGBA,
                     GL_UNSIGNED_BYTE, px);
    }
    if (color)
        *color = ((u32) px[3] << 24) | ((u32) px[0] << 16) |
                 ((u32) px[1] << 8) | px[2];
}

void GXPokeARGB(u16 x, u16 y, u32 color)
{
    (void) x;
    (void) y;
    (void) color;
}

void GXPeekZ(u16 x, u16 y, u32* z)
{
    float d = 1.f;
    GXHALContext* c = gx_hal_get_context();
    if ((int) x < c->framebuffer_width &&
        (int) y < c->framebuffer_height) {
        gx_hal_bind_efb();
        glReadPixels((GLint) x, (GLint) y, 1, 1, GL_DEPTH_COMPONENT,
                     GL_FLOAT, &d);
    }
    if (z)
        *z = (u32) (d * 16777215.f);
}

void GXPokeZ(u16 x, u16 y, u32 z)
{
    (void) x;
    (void) y;
    (void) z;
}

u32 GXCompressZ16(u32 z24, GXZFmt16 zfmt)
{
    (void) zfmt;
    return z24 >> 8;
}

u32 GXDecompressZ16(u32 z16, GXZFmt16 zfmt)
{
    (void) zfmt;
    return z16 << 8;
}

// ================================================================ GXVerify
void GXSetVerifyLevel(GXWarningLevel level)
{
    s_verify_level = level;
}

GXVerifyCallback GXSetVerifyCallback(GXVerifyCallback cb)
{
    GXVerifyCallback prev = s_verify_cb;
    s_verify_cb = cb;
    return prev;
}

// NOTE: <dolphin/gx.h> redeclares GXSetDrawSyncCallback with an
// unsigned-short spelling of the same type; the single definition above
// (GXManage spelling) satisfies both declarations.
