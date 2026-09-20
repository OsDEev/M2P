#ifndef _DOLPHIN_GX_GXVERT_PC_H_
#define _DOLPHIN_GX_GXVERT_PC_H_

// PC-port replacement for <dolphin/gx/GXVert.h>.
//
// Original header writes vertex data straight into the GP FIFO at the
// physical address 0xCC008000 via `GXWGFifo`. On PC that address does not
// exist, so every build with -DMELEE_PC_PORT must resolve THIS file instead
// of libs/dolphin/include/dolphin/gx/GXVert.h (see CMakeLists.txt: the
// src/hal/shim include dir is placed BEFORE libs/dolphin/include).
//
// All functions forward into the HAL vertex assembler in gx_hal.c.

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// FIFO helpers (command/param streams are not used by the game renderer;
// provided only so translation units that reference them still link).
void GXCmd1u8(u8 x);
void GXCmd1u16(u16 x);
void GXCmd1u32(u32 x);
void GXParam1u8(u8 x);
void GXParam1u16(u16 x);
void GXParam1u32(u32 x);
void GXParam1s8(s8 x);
void GXParam1s16(s16 x);
void GXParam1s32(s32 x);
void GXParam1f32(f32 x);
void GXParam3f32(f32 x, f32 y, f32 z);
void GXParam4f32(f32 x, f32 y, f32 z, f32 w);

// Position
void GXPosition3f32(f32 x, f32 y, f32 z);
void GXPosition3u8(u8 x, u8 y, u8 z);
void GXPosition3s8(s8 x, s8 y, s8 z);
void GXPosition3u16(u16 x, u16 y, u16 z);
void GXPosition3s16(s16 x, s16 y, s16 z);
void GXPosition2f32(f32 x, f32 y);
void GXPosition2u8(u8 x, u8 y);
void GXPosition2s8(s8 x, s8 y);
void GXPosition2u16(u16 x, u16 y);
void GXPosition2s16(s16 x, s16 y);
void GXPosition1x16(u16 x);
void GXPosition1x8(u8 x);

// Normal
void GXNormal3f32(f32 x, f32 y, f32 z);
void GXNormal3s16(s16 x, s16 y, s16 z);
void GXNormal3s8(s8 x, s8 y, s8 z);
void GXNormal1x16(u16 x);
void GXNormal1x8(u8 x);

// Color
void GXColor4u8(u8 r, u8 g, u8 b, u8 a);
void GXColor1u32(u32 c);
void GXColor3u8(u8 r, u8 g, u8 b);
void GXColor1u16(u16 c);
void GXColor1x16(u16 x);
void GXColor1x8(u8 x);

// TexCoord
void GXTexCoord2f32(f32 s, f32 t);
void GXTexCoord2s16(s16 s, s16 t);
void GXTexCoord2u16(u16 s, u16 t);
void GXTexCoord2s8(s8 s, s8 t);
void GXTexCoord2u8(u8 s, u8 t);
void GXTexCoord1f32(f32 s);
void GXTexCoord1s16(s16 s);
void GXTexCoord1u16(u16 s);
void GXTexCoord1s8(s8 s);
void GXTexCoord1u8(u8 s);
void GXTexCoord1x16(u16 x);
void GXTexCoord1x8(u8 x);

// Matrix index (skin matrices)
void GXMatrixIndex1u8(u8 x);

#ifdef __cplusplus
}
#endif

#endif
