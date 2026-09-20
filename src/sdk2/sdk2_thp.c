// SDK2 THP: movie player stubs.
//
// The original THP decoder is paired-single ASM and isn't portable, and a
// full JPEG/ADPCM THP pipeline is out of scope for v1. These stubs keep the
// exact signatures from <dolphin/thp/thp.h> so lbmthp links and runs:
// frames "decode" instantly and playback proceeds (blank video, silence).
// Movies are optional content; the game never hangs waiting for them.

#include <dolphin/thp/thp.h>

#include <stddef.h>

void THPInit(void)
{
}

THPFileInfo* THPVideoDecode(void* header, void* status_out,
                            THPFileInfo* work, void* data,
                            THPDec_8032FD40_Data* desc)
{
    (void) header;
    (void) status_out;
    (void) data;
    (void) desc;
    // Pretend the frame decoded into `data`; lbmthp converts the (zeroed)
    // YUV planes to texture and advances. Arenas come from zero-filled
    // pages, so movies render black instead of garbage.
    return work;
}

s32 THPDec_803302EC(u8** data)
{
    (void) data;
    return 0;
}

s32 THPDec_8032FD40(THPDec_8032FD40_Data* arg0, u16 arg1)
{
    (void) arg0;
    (void) arg1;
    return 0;
}

s32 THPDec_8032F8D4(u8* data, THPDec_8032FD40_Data* out)
{
    (void) data;
    (void) out;
    return 0;
}

u8 THPDec_80330158(THPFileInfo* info)
{
    (void) info;
    return 1; // "frame ready"
}

void THPDec_80331340(THPFileInfo* info, void* y, void* u, void* v)
{
    // 640x480 YUV->texture path. Planes are already zeroed; nothing to do.
    (void) info;
    (void) y;
    (void) u;
    (void) v;
}

void THPDec_803313D0(THPFileInfo* info, void* y, void* u, void* v, u32 w)
{
    (void) info;
    (void) y;
    (void) u;
    (void) v;
    (void) w;
}

void THPDec_803300E0(THPFileInfo* info)
{
    (void) info;
}
