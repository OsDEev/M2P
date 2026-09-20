#ifndef _DOLPHIN_GX_GXGEOMETRY_PC_H_
#define _DOLPHIN_GX_GXGEOMETRY_PC_H_

// PC-port replacement for <dolphin/gx/GXGeometry.h>.
// Identical API, except GXEnd() is a real function that flushes the HAL
// vertex assembler (the original is an empty inline in release builds
// because vertices travel through the GP FIFO, which does not exist on PC).

#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXStruct.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GXSETARRAY(attr, data, size, stride, le)                              \
    GXSetArray((attr), (data), (stride))

void GXSetVtxDesc(GXAttr attr, GXAttrType type);
void GXSetVtxDescv(const GXVtxDescList* attrPtr);
void GXClearVtxDesc(void);
void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt,
                     GXCompType type, u8 frac);
void GXSetVtxAttrFmtv(GXVtxFmt vtxfmt, const GXVtxAttrFmtList* list);
void GXSetArray(GXAttr attr, const void* base_ptr, u8 stride);
void GXInvalidateVtxCache(void);
void GXSetTexCoordGen2(GXTexCoordID dst_coord, GXTexGenType func,
                       GXTexGenSrc src_param, u32 mtx, GXBool normalize,
                       u32 pt_texmtx);
void GXSetNumTexGens(u8 nTexGens);

static inline void GXSetTexCoordGen(GXTexCoordID dst_coord, GXTexGenType func,
                                    GXTexGenSrc src_param, u32 mtx)
{
    GXSetTexCoordGen2(dst_coord, func, src_param, mtx, GX_FALSE,
                      GX_PTIDENTITY);
}

void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts);
void GXEnd(void);
void GXSetLineWidth(u8 width, GXTexOffset texOffsets);
void GXSetPointSize(u8 pointSize, GXTexOffset texOffsets);
void GXEnableTexOffsets(GXTexCoordID coord, u8 line_enable,
                        u8 point_enable);

#ifdef __cplusplus
}
#endif

#endif
