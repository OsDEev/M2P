// SDK2 DVD: GameCube disc filesystem over a host directory.
//
// Layout: sdk2_disc_root() points at the extracted GALE01 root (files like
// "MnSlChr.usd", "PlCo.dat", ...). DVDConvertPathToEntrynum() assigns each
// distinct path a stable entry number; DVDFastOpen()/DVDReadAsyncPrio()
// then work like the retail equivalents, except "async" completes
// synchronously before returning (callbacks still fire, so game state
// machines advance exactly as on hardware).
//
// Low-level DVDLow* / absolute-offset / streaming entry points have no
// host-side meaning and report success with dummy data.

#include "sdk2.h"

#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

// ------------------------------------------------------------ path table
#define DVD_PATH_MAX 1024
#define DVD_ENTRIES_MAX 4096

typedef struct {
    char path[DVD_PATH_MAX]; // normalized, no leading slash
    long size;
    int used;
} DvdEntry;

static DvdEntry s_entries[DVD_ENTRIES_MAX];
static int s_entry_count;
static char s_cwd[DVD_PATH_MAX];

static void path_normalize(const char* in, char* out, size_t n)
{
    // strip leading slashes, convert backslashes, collapse "root/" prefix
    while (*in == '/' || *in == '\\')
        in++;
    strncpy(out, in, n - 1);
    out[n - 1] = '\0';
    {
        char* p = out;
        while (*p) {
            if (*p == '\\')
                *p = '/';
            p++;
        }
    }
}

static void full_path(const char* norm, char* out, size_t n)
{
    const char* root = sdk2_disc_root();
    if (s_cwd[0] && norm[0] != '/' &&
        strncmp(norm, "./", 2) != 0) {
        snprintf(out, n, "%s/%s/%s", root, s_cwd, norm);
    } else {
        snprintf(out, n, "%s/%s", root, norm);
    }
}

static long file_size_of(const char* full)
{
    FILE* f = fopen(full, "rb");
    long sz = -1;
    if (f) {
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fclose(f);
    }
    return sz;
}

s32 DVDConvertPathToEntrynum(const char* pathPtr)
{
    char norm[DVD_PATH_MAX];
    int i;
    if (!pathPtr)
        return -1;
    path_normalize(pathPtr, norm, sizeof(norm));
    for (i = 0; i < s_entry_count; i++) {
        if (strcmp(s_entries[i].path, norm) == 0)
            return i + 1;
    }
    if (s_entry_count >= DVD_ENTRIES_MAX)
        return -1;
    {
        char full[DVD_PATH_MAX * 2];
        long sz;
        full_path(norm, full, sizeof(full));
        sz = file_size_of(full);
        if (sz < 0)
            return -1; // not on disc
        strncpy(s_entries[s_entry_count].path, norm, DVD_PATH_MAX - 1);
        s_entries[s_entry_count].size = sz;
        s_entries[s_entry_count].used = 1;
        return ++s_entry_count; // entry numbers start at 1
    }
}

static DvdEntry* entry_by_num(s32 entrynum)
{
    if (entrynum <= 0 || entrynum > s_entry_count)
        return NULL;
    return &s_entries[entrynum - 1];
}

// ------------------------------------------------------------ file handles
BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo)
{
    DvdEntry* e = entry_by_num(entrynum);
    char full[DVD_PATH_MAX * 2];
    FILE* f;
    if (!e || !fileInfo)
        return FALSE;
    full_path(e->path, full, sizeof(full));
    f = fopen(full, "rb");
    if (!f)
        return FALSE;
    fclose(f);
    memset(fileInfo, 0, sizeof(*fileInfo));
    fileInfo->cb.command = DVD_COMMAND_READ;
    fileInfo->cb.state = DVD_STATE_END;
    fileInfo->startAddr = (u32) entrynum * 0x1000000u; // synthetic
    fileInfo->length = (u32) e->size;
    // stash the entry number for reads (callback field is free to use:
    // retail leaves DVDFileInfo.callback for the caller; we keep our own
    // copy in startAddr-adjacent state instead — use userData).
    fileInfo->cb.userData = (void*) (intptr_t) entrynum;
    return TRUE;
}

BOOL DVDOpen(char* fileName, DVDFileInfo* fileInfo)
{
    s32 n = DVDConvertPathToEntrynum(fileName);
    if (n < 0)
        return FALSE;
    return DVDFastOpen(n, fileInfo);
}

BOOL DVDClose(DVDFileInfo* fileInfo)
{
    (void) fileInfo;
    return TRUE;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length,
                      s32 offset, DVDCallback callback, s32 prio)
{
    FILE* f = NULL;
    char full[DVD_PATH_MAX * 2];
    long got = -1;
    (void) prio;
    if (fileInfo && fileInfo->cb.userData) {
        DvdEntry* e =
            entry_by_num((s32) (intptr_t) fileInfo->cb.userData);
        if (e)
            full_path(e->path, full, sizeof(full));
        else
            full[0] = '\0';
    } else {
        full[0] = '\0';
    }
    if (full[0]) {
        f = fopen(full, "rb");
        if (f) {
            if (offset >= 0)
                fseek(f, offset, SEEK_SET);
            got = (long) fread(addr, 1, (size_t) length, f);
            fclose(f);
        }
    }
    fileInfo->cb.state = (got >= 0) ? DVD_STATE_END
                                    : DVD_STATE_FATAL_ERROR;
    fileInfo->cb.transferredSize =
        (u32) (got >= 0 ? got : 0);
    if (callback)
        callback(got >= 0 ? got : DVD_RESULT_FATAL_ERROR, fileInfo);
    return got >= 0 ? TRUE : FALSE;
}

long DVDReadPrio(struct DVDFileInfo* fileInfo, void* addr, long length,
                 long offset, long prio)
{
    FILE* f = NULL;
    char full[DVD_PATH_MAX * 2];
    long got = -1;
    (void) prio;
    if (fileInfo && fileInfo->cb.userData) {
        DvdEntry* e =
            entry_by_num((s32) (intptr_t) fileInfo->cb.userData);
        if (e)
            full_path(e->path, full, sizeof(full));
        else
            full[0] = '\0';
    } else {
        full[0] = '\0';
    }
    if (full[0]) {
        f = fopen(full, "rb");
        if (f) {
            if (offset >= 0)
                fseek(f, offset, SEEK_SET);
            got = (long) fread(addr, 1, (size_t) length, f);
            fclose(f);
        }
    }
    return got;
}

int DVDSeekAsyncPrio(struct DVDFileInfo* fileInfo, long offset,
                     void (*callback)(long, struct DVDFileInfo*),
                     long prio)
{
    (void) prio;
    fileInfo->cb.state = DVD_STATE_END;
    if (callback)
        callback(offset, fileInfo);
    return 1;
}

long DVDSeekPrio(struct DVDFileInfo* fileInfo, long offset, long prio)
{
    (void) fileInfo;
    (void) prio;
    return offset;
}

long DVDGetFileInfoStatus(struct DVDFileInfo* fileInfo)
{
    return fileInfo ? fileInfo->cb.state : DVD_STATE_FATAL_ERROR;
}

BOOL DVDGetCurrentDir(char* path, u32 maxlen)
{
    if (!path)
        return FALSE;
    strncpy(path, s_cwd[0] ? s_cwd : "/", maxlen - 1);
    path[maxlen - 1] = '\0';
    return TRUE;
}

BOOL DVDChangeDir(char* dirName)
{
    char norm[DVD_PATH_MAX];
    if (!dirName)
        return FALSE;
    path_normalize(dirName, norm, sizeof(norm));
    strncpy(s_cwd, norm, sizeof(s_cwd) - 1);
    return TRUE;
}

int DVDOpenDir(char* dirName, DVDDir* dir)
{
    (void) dirName;
    (void) dir;
    return 0;
}

int DVDReadDir(DVDDir* dir, DVDDirEntry* dirent)
{
    (void) dir;
    (void) dirent;
    return 0;
}

int DVDCloseDir(DVDDir* dir)
{
    (void) dir;
    return 0;
}

void* DVDGetFSTLocation()
{
    return NULL;
}

BOOL DVDPrepareStreamAsync(DVDFileInfo* fileInfo, u32 length, u32 offset,
                           DVDCallback callback)
{
    if (callback)
        callback((s32) length, fileInfo);
    (void) offset;
    return TRUE;
}

s32 DVDPrepareStream(DVDFileInfo* fileInfo, u32 length, u32 offset)
{
    (void) fileInfo;
    (void) offset;
    return (s32) length;
}

s32 DVDGetTransferredSize(DVDFileInfo* fileinfo)
{
    return fileinfo ? (s32) fileinfo->cb.transferredSize : 0;
}

// ------------------------------------------------------------ disc / drive
static DVDDiskID s_disk_id;

void DVDInit()
{
    memset(s_entries, 0, sizeof(s_entries));
    s_entry_count = 0;
    s_cwd[0] = '\0';
    memcpy(s_disk_id.gameName, "GALE", 4);
    memcpy(s_disk_id.company, "01", 2);
    s_disk_id.diskNumber = 0;
    s_disk_id.gameVersion = 0;
    s_disk_id.streaming = 0;
    s_disk_id.streamingBufSize = 0;
}

struct DVDDiskID* DVDGetCurrentDiskID(void)
{
    return &s_disk_id;
}

BOOL DVDCheckDisk(void)
{
    return TRUE;
}

long DVDGetDriveStatus()
{
    return DVD_STATE_END;
}

long DVDGetCommandBlockStatus(struct DVDCommandBlock* block)
{
    return block ? block->state : DVD_STATE_FATAL_ERROR;
}

int DVDSetAutoInvalidation(int autoInval)
{
    static int cur = 0;
    int prev = cur;
    cur = autoInval;
    return prev;
}

void DVDPause()
{
}

void DVDResume()
{
}

int DVDResetRequired()
{
    return 0;
}

void DVDReset()
{
}

void DVDDumpWaitingQueue(void)
{
}

int DVDCancelAsync(struct DVDCommandBlock* block,
                   void (*callback)(long, struct DVDCommandBlock*))
{
    if (block)
        block->state = DVD_STATE_CANCELED;
    if (callback)
        callback(DVD_RESULT_CANCELED, block);
    return 0;
}

long DVDCancel(volatile struct DVDCommandBlock* block)
{
    if (block)
        ((struct DVDCommandBlock*) block)->state = DVD_STATE_CANCELED;
    return DVD_RESULT_CANCELED;
}

int DVDCancelAllAsync(DVDCBCallback callback)
{
    if (callback)
        callback(DVD_RESULT_CANCELED, NULL);
    return 0;
}

long DVDCancelAll(void)
{
    return DVD_RESULT_CANCELED;
}

// absolute-offset / streaming / inquiry: no host equivalent
int DVDReadAbsAsyncPrio(struct DVDCommandBlock* block, void* addr,
                        long length, long offset,
                        void (*callback)(long, struct DVDCommandBlock*),
                        long prio)
{
    (void) addr;
    (void) length;
    (void) offset;
    (void) prio;
    if (block)
        block->state = DVD_STATE_FATAL_ERROR;
    if (callback)
        callback(DVD_RESULT_FATAL_ERROR, block);
    return 0;
}

int DVDSeekAbsAsyncPrio(struct DVDCommandBlock* block, long offset,
                        void (*callback)(long, struct DVDCommandBlock*),
                        long prio)
{
    (void) offset;
    (void) prio;
    if (block)
        block->state = DVD_STATE_FATAL_ERROR;
    if (callback)
        callback(DVD_RESULT_FATAL_ERROR, block);
    return 0;
}

int DVDReadAbsAsyncForBS(struct DVDCommandBlock* block, void* addr,
                         long length, long offset,
                         void (*callback)(long, struct DVDCommandBlock*))
{
    return DVDReadAbsAsyncPrio(block, addr, length, offset, callback, 2);
}

int DVDReadDiskID(struct DVDCommandBlock* block, struct DVDDiskID* diskID,
                  void (*callback)(long, struct DVDCommandBlock*))
{
    if (diskID)
        *diskID = s_disk_id;
    if (block)
        block->state = DVD_STATE_END;
    if (callback)
        callback(0, block);
    return 1;
}

int DVDPrepareStreamAbsAsync(struct DVDCommandBlock* block,
                             unsigned long length, unsigned long offset,
                             void (*callback)(long, struct DVDCommandBlock*))
{
    (void) length;
    (void) offset;
    if (block)
        block->state = DVD_STATE_END;
    if (callback)
        callback(0, block);
    return 1;
}

int DVDCancelStreamAsync(struct DVDCommandBlock* block,
                         void (*callback)(long, struct DVDCommandBlock*))
{
    if (callback)
        callback(0, block);
    return 1;
}

long DVDCancelStream(struct DVDCommandBlock* block)
{
    (void) block;
    return 0;
}

int DVDStopStreamAtEndAsync(struct DVDCommandBlock* block,
                            void (*callback)(long, struct DVDCommandBlock*))
{
    if (callback)
        callback(0, block);
    return 1;
}

long DVDStopStreamAtEnd(struct DVDCommandBlock* block)
{
    (void) block;
    return 0;
}

int DVDGetStreamErrorStatusAsync(struct DVDCommandBlock* block,
                                 void (*callback)(long,
                                                  struct DVDCommandBlock*))
{
    if (callback)
        callback(0, block);
    return 1;
}

long DVDGetStreamErrorStatus(struct DVDCommandBlock* block)
{
    (void) block;
    return 0;
}

int DVDGetStreamPlayAddrAsync(struct DVDCommandBlock* block,
                              void (*callback)(long,
                                               struct DVDCommandBlock*))
{
    if (callback)
        callback(0, block);
    return 1;
}

long DVDGetStreamPlayAddr(struct DVDCommandBlock* block)
{
    (void) block;
    return 0;
}

int DVDGetStreamStartAddrAsync(struct DVDCommandBlock* block,
                               void (*callback)(long,
                                                struct DVDCommandBlock*))
{
    if (callback)
        callback(0, block);
    return 1;
}

long DVDGetStreamStartAddr(struct DVDCommandBlock* block)
{
    (void) block;
    return 0;
}

int DVDGetStreamLengthAsync(struct DVDCommandBlock* block,
                            void (*callback)(long, struct DVDCommandBlock*))
{
    if (callback)
        callback(0, block);
    return 1;
}

long DVDGetStreamLength(struct DVDCommandBlock* block)
{
    (void) block;
    return 0;
}

int DVDChangeDiskAsyncForBS(struct DVDCommandBlock* block,
                            void (*callback)(long, struct DVDCommandBlock*))
{
    if (callback)
        callback(DVD_RESULT_FATAL_ERROR, block);
    return 0;
}

int DVDChangeDiskAsync(struct DVDCommandBlock* block, struct DVDDiskID* id,
                       void (*callback)(long, struct DVDCommandBlock*))
{
    (void) id;
    if (callback)
        callback(DVD_RESULT_FATAL_ERROR, block);
    return 0;
}

long DVDChangeDisk(struct DVDCommandBlock* block, struct DVDDiskID* id)
{
    (void) block;
    (void) id;
    return DVD_RESULT_FATAL_ERROR;
}

int DVDInquiryAsync(struct DVDCommandBlock* block, struct DVDDriveInfo* info,
                    void (*callback)(long, struct DVDCommandBlock*))
{
    if (info)
        memset(info, 0, sizeof(*info));
    if (callback)
        callback(0, block);
    return 1;
}

long DVDInquiry(struct DVDCommandBlock* block, struct DVDDriveInfo* info)
{
    (void) block;
    if (info)
        memset(info, 0, sizeof(*info));
    return 0;
}

void __DVDStoreErrorCode(u32 error)
{
    (void) error;
}

// low-level DI: stubs
int DVDLowRead(void* addr, unsigned long length, unsigned long offset,
               void (*callback)(unsigned long))
{
    (void) addr;
    (void) length;
    (void) offset;
    if (callback)
        callback(0);
    return 0;
}

int DVDLowSeek(unsigned long offset, void (*callback)(unsigned long))
{
    (void) offset;
    if (callback)
        callback(0);
    return 0;
}

int DVDLowWaitCoverClose(void (*callback)(unsigned long))
{
    if (callback)
        callback(0);
    return 0;
}

int DVDLowReadDiskID(struct DVDDiskID* diskID, void (*callback)(unsigned long))
{
    if (diskID)
        *diskID = s_disk_id;
    if (callback)
        callback(0);
    return 0;
}

int DVDLowStopMotor(void (*callback)(unsigned long))
{
    if (callback)
        callback(0);
    return 0;
}

int DVDLowRequestError(void (*callback)(unsigned long))
{
    if (callback)
        callback(0);
    return 0;
}

int DVDLowInquiry(struct DVDDriveInfo* info, void (*callback)(unsigned long))
{
    if (info)
        memset(info, 0, sizeof(*info));
    if (callback)
        callback(0);
    return 0;
}

int DVDLowAudioStream(unsigned long subcmd, unsigned long length,
                      unsigned long offset, void (*callback)(unsigned long))
{
    (void) subcmd;
    (void) length;
    (void) offset;
    if (callback)
        callback(0);
    return 0;
}

int DVDLowRequestAudioStatus(unsigned long subcmd,
                             void (*callback)(unsigned long))
{
    (void) subcmd;
    if (callback)
        callback(0);
    return 0;
}

int DVDLowAudioBufferConfig(int enable, unsigned long size,
                            void (*callback)(unsigned long))
{
    (void) enable;
    (void) size;
    if (callback)
        callback(0);
    return 0;
}

void DVDLowReset()
{
}

void (*DVDLowSetResetCoverCallback(void (*callback)(unsigned long)))(
    unsigned long)
{
    (void) callback;
    return NULL;
}

int DVDLowBreak()
{
    return 0;
}

void (*DVDLowClearCallback())(unsigned long)
{
    return NULL;
}

unsigned long DVDLowGetCoverStatus()
{
    return 0;
}
