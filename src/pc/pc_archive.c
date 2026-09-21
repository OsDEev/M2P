// PC-port replacement for sysdolphin/baselib/archive.c (excluded from the
// PC build: it runs in place assuming host-order file bytes).
//
// The .dat archive format stores every multi-byte field big-endian; on PC
// the file image cannot be mutated in place (it may be shared), so this
// implementation mirrors the retail operations exactly but converts each
// header/table/slot read with pc_rb32. Relocated pointer slots become
// native host addresses, exactly like retail after Locate().
//
// Relocation arithmetic wraps at 32 bits on both GC (PPC) and PC (32-bit
// x86 build), so `base + file_offset` matches retail bit-for-bit.

#include <stdint.h>
#include <string.h>

#include <sysdolphin/baselib/archive.h>

#include <dolphin/os.h>
#include <pc/pc_endian.h>

static inline void pc_Locate(HSD_Archive* archive)
{
    u32 i;

    for (i = 0; i < archive->header.nb_reloc; i++) {
        u32 slot_off = pc_rb32(&archive->reloc_info[i].offset);
        u8* slot = archive->data + slot_off;
        u32 file_off = pc_rb32(slot);
        // 32-bit wrap matches PPC `*ptr += (u32) data`.
        u32 host = (u32) ((uintptr_t) archive->data + file_off);

        memcpy(slot, &host, sizeof(host));
    }
}

s32 HSD_ArchiveParse(HSD_Archive* archive, u8* src, size_t file_size)
{
    u32 offset;

    if (archive == NULL) {
        return -1;
    }

    memset(archive, 0, sizeof(HSD_Archive));
    archive->flags |= 1;
    memcpy(&archive->header, src, sizeof(HSD_ArchiveHeader));
    // Header arrives big-endian; convert the scalar fields in place (the
    // struct itself lives in caller memory, not in the file image).
    archive->header.file_size = pc_rb32(&archive->header.file_size);
    archive->header.data_size = pc_rb32(&archive->header.data_size);
    archive->header.nb_reloc = pc_rb32(&archive->header.nb_reloc);
    archive->header.nb_public = pc_rb32(&archive->header.nb_public);
    archive->header.nb_extern = pc_rb32(&archive->header.nb_extern);

    if (archive->header.file_size != file_size) {
        OSReport("HSD_ArchiveParse: byte-order mismatch! Please check data "
                 "format %x %x\n",
                 archive->header.file_size, file_size);
        return -1;
    }

    offset = sizeof(HSD_ArchiveHeader);
    if (archive->header.data_size != 0) { // Body Size
        archive->data = src + sizeof(HSD_ArchiveHeader);
        offset = archive->header.data_size + sizeof(HSD_ArchiveHeader);
    }
    if (archive->header.nb_reloc != 0) { // Relocation Size
        archive->reloc_info =
            (HSD_ArchiveRelocationInfo*) ((uintptr_t) src + offset);
        offset = offset +
                 archive->header.nb_reloc * sizeof(HSD_ArchiveRelocationInfo);
    }
    if (archive->header.nb_public != 0) { // Root Size
        archive->public_info =
            (HSD_ArchivePublicInfo*) ((uintptr_t) src + offset);
        offset =
            offset + archive->header.nb_public * sizeof(HSD_ArchivePublicInfo);
    }
    if (archive->header.nb_extern != 0) { // XRef Size
        archive->extern_info =
            (HSD_ArchiveExternInfo*) ((uintptr_t) src + offset);
        offset =
            offset + archive->header.nb_extern * sizeof(HSD_ArchiveExternInfo);
    }
    if (offset < archive->header.file_size) { // File Size
        archive->symbols = (char*) ((uintptr_t) src + offset);
    }

    archive->top_ptr = (void*) src;
    pc_Locate(archive);

    return 0;
}

void* HSD_ArchiveGetPublicAddress(HSD_Archive* archive, const char* symbols)
{
    u32 i;

    for (i = 0; i < archive->header.nb_public; i++) {
        u32 sym_off = pc_rb32(&archive->public_info[i].symbol);
        int comparison = strcmp(archive->symbols + sym_off, symbols);

        if (comparison == 0) {
            // If both strings are equal, we've found the node
            return archive->data + pc_rb32(&archive->public_info[i].offset);
        }
    }

    return NULL;
}

char* HSD_ArchiveGetExtern(HSD_Archive* archive, int offset)
{
    if (offset < 0 || archive->header.nb_extern <= (unsigned) offset) {
        return NULL;
    }

    return archive->symbols +
           pc_rb32(&archive->extern_info[offset].symbol);
}

void HSD_ArchiveLocateExtern(HSD_Archive* archive, const char* symbols,
                             void* addr)
{
    u32 offset = 0xFFFFFFFFu;
    u32 i;

    for (i = 0; i < archive->header.nb_extern; i++) {
        u32 sym_off = pc_rb32(&archive->extern_info[i].symbol);
        int comparison = strcmp(symbols, archive->symbols + sym_off);

        if (comparison == 0) {
            offset = pc_rb32(&archive->extern_info[i].offset);
            break;
        }
    }

    if (offset == 0xFFFFFFFFu) {
        return;
    }

    // Extern slots are never covered by the relocation table (retail walks
    // them as raw file offsets), so convert each link from big-endian.
    while (offset != 0xFFFFFFFFu && offset < archive->header.data_size) {
        u8* slot = archive->data + offset;
        u32 next = pc_rb32(slot);
        u32 v = (u32) (uintptr_t) addr;

        memcpy(slot, &v, sizeof(v));
        offset = next;
    }
}
