// PC endian handling, stage 1: archive structure.
//
// Game data files are big-endian. The archive envelope (header + relocation
// / public / extern tables) has a FIXED layout, so it can be converted
// losslessly: this file replaces src/sysdolphin/baselib/archive.c in the
// PC build (see CMakeLists.txt) with an endian-aware implementation.
//
// What it does: parse headers and tables with explicit big-endian loads,
// relocate pointers into host order. After this, archive structure,
// symbols and pointer relocation are correct on little-endian hosts.
//
// What it does NOT do (Stage 2, see README_PC_PORT.md): the DATA payloads
// themselves (model/anim/SFX structs with embedded floats and ints) are
// still big-endian in memory. Each HSD object loader needs endian-aware
// field access before a given asset class renders/plays correctly.

#include <stdint.h>
#include <string.h>

#include <Runtime/platform.h>

#include <dolphin/os.h>
#include <sysdolphin/baselib/archive.h>

static u32 rb32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) |
           ((u32) p[2] << 8) | p[3];
}

static void locate(HSD_Archive* archive)
{
    u32 i;
    u8* data = archive->data;
    for (i = 0; i < archive->header.nb_reloc; i++) {
        // NOTE: reloc_info lives in the file buffer (big-endian); read
        // the offset with an explicit BE load.
        const u8* e =
            (const u8*) archive->reloc_info +
            i * sizeof(HSD_ArchiveRelocationInfo);
        u32 off = rb32(e);
        u8* slot = data + off;
        u32 file_off = rb32(slot);
        u32 host_ptr = file_off + (u32) (uintptr_t) data;
        // From here on the slot holds a NATIVE pointer: game code reads
        // relocated slots with plain loads. Low-memory invariant: data
        // buffers come from the game heap below 4 GB, so the (u32) cast
        // loses no bits.
        memcpy(slot, &host_ptr, 4);
    }
}

s32 HSD_ArchiveParse(HSD_Archive* archive, u8* src, size_t file_size)
{
    u32 offset;
    u32 file_sz, data_sz, nb_reloc, nb_public, nb_extern;

    if (archive == NULL) {
        return -1;
    }

    memset(archive, 0, sizeof(HSD_Archive));
    archive->flags |= 1;

    file_sz = rb32(src + 0x00);
    data_sz = rb32(src + 0x04);
    nb_reloc = rb32(src + 0x08);
    nb_public = rb32(src + 0x0C);
    nb_extern = rb32(src + 0x10);
    if (file_sz != file_size) {
        OSReport("HSD_ArchiveParse: size mismatch %x %x\n", file_sz,
                 (u32) file_size);
        return -1;
    }
    // normalize the header copy in place (host order from here on)
    memcpy(archive, src, sizeof(HSD_ArchiveHeader));
    archive->header.file_size = file_sz;
    archive->header.data_size = data_sz;
    archive->header.nb_reloc = nb_reloc;
    archive->header.nb_public = nb_public;
    archive->header.nb_extern = nb_extern;

    offset = sizeof(HSD_ArchiveHeader);
    if (data_sz != 0) {
        archive->data = src + sizeof(HSD_ArchiveHeader);
        offset = data_sz + sizeof(HSD_ArchiveHeader);
    }
    if (nb_reloc != 0) {
        archive->reloc_info =
            (HSD_ArchiveRelocationInfo*) ((uintptr_t) src + offset);
        offset += nb_reloc * sizeof(HSD_ArchiveRelocationInfo);
    }
    if (nb_public != 0) {
        archive->public_info =
            (HSD_ArchivePublicInfo*) ((uintptr_t) src + offset);
        offset += nb_public * sizeof(HSD_ArchivePublicInfo);
    }
    if (nb_extern != 0) {
        archive->extern_info =
            (HSD_ArchiveExternInfo*) ((uintptr_t) src + offset);
        offset += nb_extern * sizeof(HSD_ArchiveExternInfo);
    }
    if (offset < file_sz) {
        archive->symbols = (char*) ((uintptr_t) src + offset);
    }

    archive->top_ptr = (void*) src;
    locate(archive);
    return 0;
}

void* HSD_ArchiveGetPublicAddress(HSD_Archive* archive, const char* symbols)
{
    u32 i;
    for (i = 0; i < archive->header.nb_public; i++) {
        const u8* e =
            (const u8*) archive->public_info +
            i * sizeof(HSD_ArchivePublicInfo);
        u32 off = rb32(e + 0);
        u32 sym = rb32(e + 4);
        if (strcmp(archive->symbols + sym, symbols) == 0) {
            return archive->data + off;
        }
    }
    return NULL;
}

char* HSD_ArchiveGetExtern(HSD_Archive* archive, int offset)
{
    const u8* e;
    if (offset < 0 || archive->header.nb_extern <= (unsigned) offset) {
        return NULL;
    }
    e = (const u8*) archive->extern_info +
        (unsigned) offset * sizeof(HSD_ArchiveExternInfo);
    return archive->symbols + rb32(e + 4);
}

void HSD_ArchiveLocateExtern(HSD_Archive* archive, const char* symbols,
                             void* addr)
{
    u32 raw = 0xFFFFFFFFu;
    u32 i;
    u32 guard;
    for (i = 0; i < archive->header.nb_extern; i++) {
        const u8* e =
            (const u8*) archive->extern_info +
            i * sizeof(HSD_ArchiveExternInfo);
        if (strcmp(symbols, archive->symbols + rb32(e + 4)) == 0) {
            raw = rb32(e + 0);
            break;
        }
    }
    if (raw == 0xFFFFFFFFu) {
        return;
    }
    // Extern slots hold big-endian file offsets (they are not covered by
    // the relocation table). Walk the chain with BE loads, patching each
    // slot with the native host pointer. The iteration cap guarantees
    // termination if LocateExtern ever runs twice on one archive.
    guard = archive->header.data_size / 4 + 16;
    while (raw != 0xFFFFFFFFu && raw < archive->header.data_size &&
           guard-- > 0) {
        u8* slot = archive->data + raw;
        u32 next = rb32(slot);
        *(u32*) slot = (u32) (uintptr_t) addr;
        raw = next;
    }
}
