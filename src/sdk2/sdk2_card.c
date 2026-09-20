// SDK2 CARD: memory-card slots backed by host container files.
//
// Model: each slot (0 = A, 1 = B) persists to a single container file
// (default "melee_card_A.mci" / "..._B.mci", overridable via settings).
// Files are stored at file granularity with their CARDStat metadata;
// block-level FAT details are abstracted away (free-space accounting is
// simulated: 1019 blocks total, 8 KiB each).
//
// Async variants complete synchronously and then fire the callback,
// exactly like the DVD layer, so game state machines advance unchanged.

#include "sdk2.h"

#include <dolphin/card.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CARD_SLOTS 2
#define CARD_FILES_MAX 127
#define CARD_BLOCK_SIZE 8192
#define CARD_BLOCKS_TOTAL 1019

typedef struct {
    char name[CARD_FILENAME_MAX + 1];
    u8* data;
    u32 size;   // allocated size (create size)
    u32 length; // logical length (max written)
    u32 time;
    u8 gameName[4];
    u8 company[2];
    u8 bannerFormat;
    u16 iconFormat;
    u16 iconSpeed;
    u32 commentAddr;
    u8 permission;
    int used;
} CardFile;

typedef struct {
    int mounted;
    int present;
    s32 last_result;
    s32 xferred;
    CardFile files[CARD_FILES_MAX];
} CardSlot;

static CardSlot s_slots[CARD_SLOTS];

static const char* slot_path(int chan)
{
    return sdk2_card_path(chan);
}

// ------------------------------------------------------------ persistence
static void card_save(int chan)
{
    CardSlot* s = &s_slots[chan];
    FILE* f = fopen(slot_path(chan), "wb");
    int i;
    u32 n = 0;
    if (!f)
        return;
    fwrite("M2PCARD1", 1, 8, f);
    for (i = 0; i < CARD_FILES_MAX; i++) {
        if (s->files[i].used)
            n++;
    }
    fwrite(&n, 4, 1, f);
    for (i = 0; i < CARD_FILES_MAX; i++) {
        CardFile* cf = &s->files[i];
        u8 hdr[64];
        if (!cf->used)
            continue;
        memset(hdr, 0, sizeof(hdr));
        memcpy(hdr + 0, cf->name, CARD_FILENAME_MAX);
        memcpy(hdr + 32, &cf->size, 4);
        memcpy(hdr + 36, &cf->length, 4);
        memcpy(hdr + 40, &cf->time, 4);
        memcpy(hdr + 44, cf->gameName, 4);
        memcpy(hdr + 48, cf->company, 2);
        hdr[50] = cf->bannerFormat;
        hdr[51] = cf->permission;
        memcpy(hdr + 52, &cf->iconFormat, 2);
        memcpy(hdr + 54, &cf->iconSpeed, 2);
        memcpy(hdr + 56, &cf->commentAddr, 4);
        fwrite(hdr, 1, sizeof(hdr), f);
        if (cf->size)
            fwrite(cf->data, 1, cf->size, f);
    }
    fclose(f);
}

static void card_load(int chan)
{
    CardSlot* s = &s_slots[chan];
    FILE* f = fopen(slot_path(chan), "rb");
    char magic[8];
    u32 n = 0, i;
    if (!f)
        return; // fresh card
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "M2PCARD1", 8) != 0) {
        fclose(f);
        return;
    }
    if (fread(&n, 4, 1, f) != 1) {
        fclose(f);
        return;
    }
    for (i = 0; i < n && i < CARD_FILES_MAX * 2; i++) {
        u8 hdr[64];
        CardFile* cf = NULL;
        int slot = -1, k;
        u32 size;
        if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
            break;
        for (k = 0; k < CARD_FILES_MAX; k++) {
            if (!s->files[k].used) {
                slot = k;
                break;
            }
        }
        if (slot < 0)
            break;
        cf = &s->files[slot];
        memcpy(&size, hdr + 32, 4);
        if (size > 16 * 1024 * 1024)
            break;
        memset(cf, 0, sizeof(*cf));
        memcpy(cf->name, hdr, CARD_FILENAME_MAX);
        cf->name[CARD_FILENAME_MAX] = '\0';
        cf->size = size;
        memcpy(&cf->length, hdr + 36, 4);
        memcpy(&cf->time, hdr + 40, 4);
        memcpy(cf->gameName, hdr + 44, 4);
        memcpy(cf->company, hdr + 48, 2);
        cf->bannerFormat = hdr[50];
        cf->permission = hdr[51];
        memcpy(&cf->iconFormat, hdr + 52, 2);
        memcpy(&cf->iconSpeed, hdr + 54, 2);
        memcpy(&cf->commentAddr, hdr + 56, 4);
        if (size) {
            cf->data = (u8*) malloc(size ? size : 1);
            if (!cf->data)
                break;
            if (fread(cf->data, 1, size, f) != size) {
                free(cf->data);
                memset(cf, 0, sizeof(*cf));
                break;
            }
        }
        cf->used = 1;
    }
    fclose(f);
}

static CardFile* find_file(int chan, const char* name)
{
    int i;
    for (i = 0; i < CARD_FILES_MAX; i++) {
        if (s_slots[chan].files[i].used &&
            strncmp(s_slots[chan].files[i].name, name,
                    CARD_FILENAME_MAX) == 0) {
            return &s_slots[chan].files[i];
        }
    }
    return NULL;
}

static CardFile* file_by_no(int chan, s32 fileNo)
{
    if (fileNo < 0 || fileNo >= CARD_FILES_MAX)
        return NULL;
    if (!s_slots[chan].files[fileNo].used)
        return NULL;
    return &s_slots[chan].files[fileNo];
}

static int file_no_of(int chan, CardFile* cf)
{
    return (int) (cf - s_slots[chan].files);
}

static u32 now_gc_time(void)
{
    time_t now = time(NULL);
    if (now < 946684800LL)
        return 0;
    return (u32) (now - 946684800LL);
}

// ------------------------------------------------------------ bios / mount
void CARDInit(void)
{
    int i;
    memset(s_slots, 0, sizeof(s_slots));
    for (i = 0; i < CARD_SLOTS; i++) {
        s_slots[i].present = 1;
        s_slots[i].last_result = CARD_RESULT_READY;
    }
}

BOOL CARDProbe(long chan)
{
    if (chan < 0 || chan >= CARD_SLOTS)
        return FALSE;
    return s_slots[chan].present ? TRUE : FALSE;
}

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize)
{
    if (chan < 0 || chan >= CARD_SLOTS || !s_slots[chan].present)
        return CARD_RESULT_NOCARD;
    if (memSize)
        *memSize = CARD_BLOCKS_TOTAL * CARD_BLOCK_SIZE; // bytes
    if (sectorSize)
        *sectorSize = CARD_BLOCK_SIZE;
    return CARD_RESULT_READY;
}

long CARDGetMemSize(long chan, unsigned short* size)
{
    if (size)
        *size = (unsigned short) (CARD_BLOCKS_TOTAL * CARD_BLOCK_SIZE /
                                  1024);
    (void) chan;
    return CARD_RESULT_READY;
}

long CARDGetEncoding(long chan, unsigned short* encode)
{
    if (encode)
        *encode = 0;
    (void) chan;
    return CARD_RESULT_READY;
}

s32 CARDGetSectorSize(s32 chan, u32* size)
{
    if (size)
        *size = CARD_BLOCK_SIZE;
    (void) chan;
    return CARD_RESULT_READY;
}

s32 CARDMount(s32 chan, void* workArea, CARDCallback detachCallback)
{
    (void) workArea;
    (void) detachCallback;
    if (chan < 0 || chan >= CARD_SLOTS)
        return CARD_RESULT_NOCARD;
    if (!s_slots[chan].mounted) {
        card_load(chan);
        s_slots[chan].mounted = 1;
    }
    s_slots[chan].last_result = CARD_RESULT_READY;
    return CARD_RESULT_READY;
}

s32 CARDMountAsync(s32 chan, void* workArea, CARDCallback detachCallback,
                   CARDCallback attachCallback)
{
    s32 r = CARDMount(chan, workArea, detachCallback);
    if (attachCallback)
        attachCallback(chan, r);
    return r;
}

s32 CARDUnmount(s32 chan)
{
    if (chan < 0 || chan >= CARD_SLOTS)
        return CARD_RESULT_NOCARD;
    if (s_slots[chan].mounted)
        card_save(chan);
    s_slots[chan].mounted = 0;
    return CARD_RESULT_READY;
}

s32 CARDCheckAsync(s32 chan, CARDCallback callback)
{
    s32 r =
        (chan >= 0 && chan < CARD_SLOTS) ? CARD_RESULT_READY
                                         : CARD_RESULT_NOCARD;
    if (callback)
        callback(chan, r);
    return r;
}

long CARDCheck(long chan)
{
    return (chan >= 0 && chan < CARD_SLOTS) ? CARD_RESULT_READY
                                            : CARD_RESULT_NOCARD;
}

long CARDFormat(long chan)
{
    int i;
    if (chan < 0 || chan >= CARD_SLOTS)
        return CARD_RESULT_NOCARD;
    for (i = 0; i < CARD_FILES_MAX; i++) {
        free(s_slots[chan].files[i].data);
        memset(&s_slots[chan].files[i], 0, sizeof(CardFile));
    }
    card_save(chan);
    return CARD_RESULT_READY;
}

s32 CARDFormatAsync(s32 chan, CARDCallback callback)
{
    s32 r = (s32) CARDFormat(chan);
    if (callback)
        callback(chan, r);
    return r;
}

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed)
{
    int i, n = 0;
    if (chan < 0 || chan >= CARD_SLOTS)
        return CARD_RESULT_NOCARD;
    for (i = 0; i < CARD_FILES_MAX; i++) {
        if (s_slots[chan].files[i].used)
            n++;
    }
    if (byteNotUsed)
        *byteNotUsed =
            (s32) ((CARD_FILES_MAX - n) * CARD_BLOCK_SIZE);
    if (filesNotUsed)
        *filesNotUsed = (s32) (CARD_FILES_MAX - n);
    return CARD_RESULT_READY;
}

s32 CARDGetResultCode(s32 chan)
{
    if (chan < 0 || chan >= CARD_SLOTS)
        return CARD_RESULT_NOCARD;
    return s_slots[chan].last_result;
}

// ------------------------------------------------------------ open/close
s32 CARDOpen(s32 chan, const char* fileName, CARDFileInfo* fileInfo)
{
    CardFile* cf;
    if (chan < 0 || chan >= CARD_SLOTS || !fileName || !fileInfo)
        return CARD_RESULT_NOCARD;
    cf = find_file(chan, fileName);
    if (!cf) {
        s_slots[chan].last_result = CARD_RESULT_NOENT;
        return CARD_RESULT_NOENT;
    }
    fileInfo->chan = chan;
    fileInfo->fileNo = file_no_of(chan, cf);
    fileInfo->offset = 0;
    fileInfo->length = (s32) cf->length;
    fileInfo->iBlock = 0;
    s_slots[chan].last_result = CARD_RESULT_READY;
    return CARD_RESULT_READY;
}

s32 CARDFastOpen(s32 chan, s32 fileNo, CARDFileInfo* fileInfo)
{
    CardFile* cf;
    if (chan < 0 || chan >= CARD_SLOTS || !fileInfo)
        return CARD_RESULT_NOCARD;
    cf = file_by_no(chan, fileNo);
    if (!cf) {
        s_slots[chan].last_result = CARD_RESULT_NOENT;
        return CARD_RESULT_NOENT;
    }
    fileInfo->chan = chan;
    fileInfo->fileNo = fileNo;
    fileInfo->offset = 0;
    fileInfo->length = (s32) cf->length;
    fileInfo->iBlock = 0;
    s_slots[chan].last_result = CARD_RESULT_READY;
    return CARD_RESULT_READY;
}

s32 CARDClose(CARDFileInfo* fileInfo)
{
    (void) fileInfo;
    return CARD_RESULT_READY;
}

s32 CARDCancel(CARDFileInfo* fileInfo)
{
    (void) fileInfo;
    return CARD_RESULT_READY;
}

// ------------------------------------------------------------ create/delete
s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size,
                    CARDFileInfo* fileInfo, CARDCallback callback)
{
    CardFile* cf = NULL;
    int i;
    s32 r;
    if (chan < 0 || chan >= CARD_SLOTS || !fileName) {
        r = CARD_RESULT_NOCARD;
        goto done;
    }
    if (strlen(fileName) > CARD_FILENAME_MAX) {
        r = CARD_RESULT_NAMETOOLONG;
        goto done;
    }
    if (find_file(chan, fileName)) {
        r = CARD_RESULT_EXIST;
        goto done;
    }
    for (i = 0; i < CARD_FILES_MAX; i++) {
        if (!s_slots[chan].files[i].used) {
            cf = &s_slots[chan].files[i];
            break;
        }
    }
    if (!cf) {
        r = CARD_RESULT_LIMIT;
        goto done;
    }
    memset(cf, 0, sizeof(*cf));
    strncpy(cf->name, fileName, CARD_FILENAME_MAX);
    cf->size = size;
    cf->length = 0;
    cf->time = now_gc_time();
    cf->permission = CARD_ATTR_PUBLIC;
    cf->data = (u8*) calloc(1, size ? size : 1);
    if (size && !cf->data) {
        r = CARD_RESULT_INSSPACE;
        goto done;
    }
    cf->used = 1;
    if (fileInfo) {
        fileInfo->chan = chan;
        fileInfo->fileNo = file_no_of(chan, cf);
        fileInfo->offset = 0;
        fileInfo->length = 0;
        fileInfo->iBlock = 0;
    }
    card_save(chan);
    r = CARD_RESULT_READY;
done:
    if (chan >= 0 && chan < CARD_SLOTS)
        s_slots[chan].last_result = r;
    if (callback)
        callback(chan, r);
    return r;
}

long CARDCreate(long chan, char* fileName, unsigned long size,
                struct CARDFileInfo* fileInfo)
{
    return CARDCreateAsync((s32) chan, fileName, (u32) size, fileInfo,
                           NULL);
}

static s32 card_delete(int chan, CardFile* cf)
{
    free(cf->data);
    memset(cf, 0, sizeof(*cf));
    card_save(chan);
    return CARD_RESULT_READY;
}

s32 CARDDeleteAsync(s32 chan, char* fileName, CARDCallback callback)
{
    CardFile* cf;
    s32 r;
    if (chan < 0 || chan >= CARD_SLOTS) {
        r = CARD_RESULT_NOCARD;
        goto done;
    }
    cf = find_file(chan, fileName);
    if (!cf) {
        r = CARD_RESULT_NOENT;
        goto done;
    }
    r = card_delete(chan, cf);
done:
    if (chan >= 0 && chan < CARD_SLOTS)
        s_slots[chan].last_result = r;
    if (callback)
        callback(chan, r);
    return r;
}

s32 CARDDelete(s32 chan, char* fileName)
{
    return CARDDeleteAsync(chan, fileName, NULL);
}

s32 CARDFastDeleteAsync(s32 chan, s32 fileNo, CARDCallback callback)
{
    CardFile* cf;
    s32 r;
    if (chan < 0 || chan >= CARD_SLOTS) {
        r = CARD_RESULT_NOCARD;
        goto done;
    }
    cf = file_by_no(chan, fileNo);
    if (!cf) {
        r = CARD_RESULT_NOENT;
        goto done;
    }
    r = card_delete(chan, cf);
done:
    if (chan >= 0 && chan < CARD_SLOTS)
        s_slots[chan].last_result = r;
    if (callback)
        callback(chan, r);
    return r;
}

long CARDFastDelete(long chan, long fileNo)
{
    return CARDFastDeleteAsync((s32) chan, (s32) fileNo, NULL);
}

s32 CARDRenameAsync(s32 chan, const char* oldName, const char* newName,
                    CARDCallback callback)
{
    CardFile* cf;
    s32 r;
    if (chan < 0 || chan >= CARD_SLOTS) {
        r = CARD_RESULT_NOCARD;
        goto done;
    }
    cf = find_file(chan, oldName);
    if (!cf) {
        r = CARD_RESULT_NOENT;
        goto done;
    }
    if (find_file(chan, newName)) {
        r = CARD_RESULT_EXIST;
        goto done;
    }
    strncpy(cf->name, newName, CARD_FILENAME_MAX);
    card_save(chan);
    r = CARD_RESULT_READY;
done:
    if (chan >= 0 && chan < CARD_SLOTS)
        s_slots[chan].last_result = r;
    if (callback)
        callback(chan, r);
    return r;
}

s32 CARDRename(s32 chan, char* oldName, char* newName)
{
    return CARDRenameAsync(chan, oldName, newName, NULL);
}

// ------------------------------------------------------------ read/write
static CardFile* info_file(CARDFileInfo* fi)
{
    if (!fi || fi->chan < 0 || fi->chan >= CARD_SLOTS)
        return NULL;
    return file_by_no(fi->chan, fi->fileNo);
}

long CARDRead(struct CARDFileInfo* fileInfo, void* buf, long length,
              long offset)
{
    CardFile* cf = info_file(fileInfo);
    if (!cf || !buf || length < 0 || offset < 0)
        return CARD_RESULT_FATAL_ERROR;
    if ((u32) offset >= cf->length) {
        s_slots[fileInfo->chan].xferred = 0;
        return CARD_RESULT_READY;
    }
    {
        u32 avail = cf->length - (u32) offset;
        u32 n = (u32) length < avail ? (u32) length : avail;
        memcpy(buf, cf->data + offset, n);
        s_slots[fileInfo->chan].xferred = (s32) n;
        s_slots[fileInfo->chan].last_result = CARD_RESULT_READY;
        return CARD_RESULT_READY;
    }
}

s32 CARDReadAsync(CARDFileInfo* fileInfo, void* buf, s32 length,
                  s32 offset, CARDCallback callback)
{
    s32 r = (s32) CARDRead(fileInfo, buf, length, offset);
    if (callback)
        callback(fileInfo ? fileInfo->chan : 0, r);
    return r;
}

s32 CARDWrite(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset)
{
    CardFile* cf = info_file(fileInfo);
    u32 end;
    if (!cf || !buf || length < 0 || offset < 0)
        return CARD_RESULT_FATAL_ERROR;
    end = (u32) offset + (u32) length;
    if (end > cf->size) {
        // grow (retail would fail with LIMIT; grow to keep saves working)
        u8* nd = (u8*) realloc(cf->data, end);
        if (!nd)
            return CARD_RESULT_INSSPACE;
        memset(nd + cf->size, 0, end - cf->size);
        cf->data = nd;
        cf->size = end;
    }
    memcpy(cf->data + offset, buf, (size_t) length);
    if (end > cf->length)
        cf->length = end;
    cf->time = now_gc_time();
    s_slots[fileInfo->chan].xferred = length;
    s_slots[fileInfo->chan].last_result = CARD_RESULT_READY;
    card_save(fileInfo->chan);
    return CARD_RESULT_READY;
}

s32 CARDWriteAsync(CARDFileInfo* fileInfo, void* buf, s32 length,
                   s32 offset, CARDCallback callback)
{
    s32 r = CARDWrite(fileInfo, buf, length, offset);
    if (callback)
        callback(fileInfo ? fileInfo->chan : 0, r);
    return r;
}

long CARDGetXferredBytes(long chan)
{
    if (chan < 0 || chan >= CARD_SLOTS)
        return 0;
    return s_slots[chan].xferred;
}

// ------------------------------------------------------------ status
s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat)
{
    CardFile* cf;
    if (chan < 0 || chan >= CARD_SLOTS)
        return CARD_RESULT_NOCARD;
    cf = file_by_no(chan, fileNo);
    if (!cf || !stat)
        return CARD_RESULT_NOENT;
    memset(stat, 0, sizeof(*stat));
    memcpy(stat->fileName, cf->name, CARD_FILENAME_MAX);
    stat->length = cf->size;
    stat->time = cf->time;
    memcpy(stat->gameName, cf->gameName, 4);
    memcpy(stat->company, cf->company, 2);
    stat->bannerFormat = cf->bannerFormat;
    stat->iconAddr = 0xFFFFFFFFu;
    stat->iconFormat = cf->iconFormat;
    stat->iconSpeed = cf->iconSpeed;
    stat->commentAddr = cf->commentAddr;
    stat->offsetBanner = 0;
    stat->offsetBannerTlut = 0;
    stat->offsetData = 0;
    return CARD_RESULT_READY;
}

long CARDSetStatus(long chan, long fileNo, struct CARDStat* stat)
{
    CardFile* cf;
    if (chan < 0 || chan >= CARD_SLOTS || !stat)
        return CARD_RESULT_NOCARD;
    cf = file_by_no((s32) chan, (s32) fileNo);
    if (!cf)
        return CARD_RESULT_NOENT;
    memcpy(cf->gameName, stat->gameName, 4);
    memcpy(cf->company, stat->company, 2);
    cf->bannerFormat = stat->bannerFormat;
    cf->iconFormat = stat->iconFormat;
    cf->iconSpeed = stat->iconSpeed;
    cf->commentAddr = stat->commentAddr;
    cf->time = stat->time;
    card_save((s32) chan);
    return CARD_RESULT_READY;
}

s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CARDStat* stat,
                       CARDCallback callback)
{
    s32 r = (s32) CARDSetStatus(chan, fileNo, stat);
    if (callback)
        callback(chan, r);
    return r;
}
