#ifndef _MELEE_PC_ENDIAN_H_
#define _MELEE_PC_ENDIAN_H_

// Stage-2 endian helpers: game data files are big-endian, the PC is
// little-endian. These alignment-safe readers/writers convert at the
// boundary where file bytes become runtime values.
//
// Usage rule (keeps upstream divergence minimal):
//   - file data is NEVER mutated in place (it may be shared);
//   - scalars are converted when COPIED into heap/runtime structs;
//   - relocation-written pointers are native already: read directly;
//   - single bytes and C strings need no conversion.
// Include via <pc/pc_endian.h> (the src/ include dir covers it).

#include <dolphin/types.h>

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline u16 pc_swap16(u16 v)
{
    return (u16) ((v << 8) | (v >> 8));
}

static inline u32 pc_swap32(u32 v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

static inline f32 pc_swapf(f32 v)
{
    union {
        f32 f;
        u32 u;
    } in, out;
    in.f = v;
    out.u = pc_swap32(in.u);
    return out.f;
}

// Big-endian loads (alignment-safe).
static inline u16 pc_rb16(const void* p)
{
    u8 b[2];
    memcpy(b, p, 2);
    return (u16) (((u16) b[0] << 8) | b[1]);
}

static inline u32 pc_rb32(const void* p)
{
    u8 b[4];
    memcpy(b, p, 4);
    return ((u32) b[0] << 24) | ((u32) b[1] << 16) | ((u32) b[2] << 8) |
           b[3];
}

static inline f32 pc_rf32(const void* p)
{
    union {
        f32 f;
        u32 u;
    } v;
    v.u = pc_rb32(p);
    return v.f;
}

// Big-endian stores (alignment-safe, for normalizing heap copies).
static inline void pc_wb16(void* p, u16 v)
{
    u8 b[2];
    b[0] = (u8) (v >> 8);
    b[1] = (u8) v;
    memcpy(p, b, 2);
}

static inline void pc_wb32(void* p, u32 v)
{
    u8 b[4];
    b[0] = (u8) (v >> 24);
    b[1] = (u8) (v >> 16);
    b[2] = (u8) (v >> 8);
    b[3] = (u8) v;
    memcpy(p, b, 4);
}

static inline void pc_wf32(void* p, f32 v)
{
    union {
        f32 f;
        u32 u;
    } u;
    u.f = v;
    pc_wb32(p, u.u);
}

#ifdef __cplusplus
}
#endif

#endif
