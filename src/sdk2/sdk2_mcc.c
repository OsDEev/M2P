// SDK2 MCC/FIO: debug communication channel stubs.
//
// MCC is the USB-Gecko/modem debug link used by hsd_392C/3933 and the
// gmmain USB-server wait. It only runs on non-master builds with a debug
// host attached, so on PC every MCC call reports "no debug hardware"
// (Init/Open return 0 = failure, which is exactly what that code checks
// for before giving up and booting the game normally).
//
// FIO (file IO over the debug link) is instead backed by host files in
// the user directory, so debug screenshot dumps (hsd_3933) land on disk
// instead of vanishing.

#include "sdk2.h"

#include <dolphin/mcc.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------ MCC (no debug HW)
int MCCStreamOpen(enum MCC_CHANNEL chID, u8 blockSize)
{
    (void) chID;
    (void) blockSize;
    return 0;
}

int MCCStreamClose(enum MCC_CHANNEL chID)
{
    (void) chID;
    return 0;
}

int MCCStreamWrite(enum MCC_CHANNEL chID, void* data, u32 dataBlockSize)
{
    (void) chID;
    (void) data;
    (void) dataBlockSize;
    return 0;
}

u32 MCCStreamRead(enum MCC_CHANNEL chID, void* data)
{
    (void) chID;
    (void) data;
    return 0;
}

int MCCInit(enum MCC_EXI exiChannel, u8 timeout,
            MCC_CBSysEvent callbackSysEvent)
{
    (void) exiChannel;
    (void) timeout;
    (void) callbackSysEvent;
    return 0; // failure: no debug hardware (callers test == 0)
}

void MCCExit(void)
{
}

int MCCPing(void)
{
    return 0;
}

int MCCEnumDevices(MCC_CBEnumDevices callbackEnumDevices)
{
    (void) callbackEnumDevices;
    return 0;
}

u8 MCCGetFreeBlocks(enum MCC_MODE mode)
{
    (void) mode;
    return 0;
}

u8 MCCGetLastError(void)
{
    return 1; // nonzero = "not initialized"
}

int MCCGetChannelInfo(enum MCC_CHANNEL chID, MCC_Info* info)
{
    (void) chID;
    if (info) {
        memset(info, 0, sizeof(*info));
        info->connect = MCC_CONNECT_DISCONNECT;
    }
    return 0;
}

int MCCGetConnectionStatus(enum MCC_CHANNEL chID, enum MCC_CONNECT* connect)
{
    (void) chID;
    if (connect)
        *connect = MCC_CONNECT_DISCONNECT;
    return 0;
}

int MCCNotify(enum MCC_CHANNEL chID, u32 notify)
{
    (void) chID;
    (void) notify;
    return 0;
}

u32 MCCSetChannelEventMask(enum MCC_CHANNEL chID, u32 event)
{
    (void) chID;
    (void) event;
    return 0;
}

int MCCOpen(enum MCC_CHANNEL chID, u8 blockSize, MCC_CBEvent callbackEvent)
{
    (void) chID;
    (void) blockSize;
    (void) callbackEvent;
    return 0; // failure: callers test == 0
}

int MCCClose(enum MCC_CHANNEL chID)
{
    (void) chID;
    return 0;
}

int MCCLock(enum MCC_CHANNEL chID)
{
    (void) chID;
    return 0;
}

int MCCUnlock(enum MCC_CHANNEL chID)
{
    (void) chID;
    return 0;
}

int MCCRead(enum MCC_CHANNEL chID, u32 offset, void* data, long size,
            enum MCC_SYNC_STATE async)
{
    (void) chID;
    (void) offset;
    (void) data;
    (void) size;
    (void) async;
    return 0;
}

int MCCWrite(enum MCC_CHANNEL chID, u32 offset, void* data, long size,
             enum MCC_SYNC_STATE async)
{
    (void) chID;
    (void) offset;
    (void) data;
    (void) size;
    (void) async;
    return 0;
}

int MCCCheckAsyncDone()
{
    return 0;
}

// ------------------------------------------------------------ FIO (host files)
#define FIO_HANDLES_MAX 16

static FILE* s_fio[FIO_HANDLES_MAX];
static int s_fio_init;

static void fio_path(const char* filename, char* out, unsigned n)
{
    const char* user = sdk2_user_dir();
    // strip drive/absolute prefixes: debug filenames are GC-side paths
    while (*filename == '/' || *filename == '\\')
        filename++;
    if (filename[0] && filename[1] == ':')
        filename += 2;
    while (*filename == '/' || *filename == '\\')
        filename++;
    snprintf(out, n, "%s/%s", user, filename);
}

int FIOInit(enum MCC_EXI exiChannel, enum MCC_CHANNEL chID, u8 blockSize)
{
    (void) exiChannel;
    (void) chID;
    (void) blockSize;
    memset(s_fio, 0, sizeof(s_fio));
    s_fio_init = 1;
    return 1;
}

void FIOExit(void)
{
    int i;
    for (i = 0; i < FIO_HANDLES_MAX; i++) {
        if (s_fio[i]) {
            fclose(s_fio[i]);
            s_fio[i] = NULL;
        }
    }
    s_fio_init = 0;
}

int FIOQuery(void)
{
    return s_fio_init ? 1 : 0;
}

int FIOFopen(const char* filename, u32 mode)
{
    char full[2048];
    FILE* f;
    int i;
    (void) mode;
    if (!filename)
        return -1;
    fio_path(filename, full, sizeof(full));
    f = fopen(full, "r+b");
    if (!f)
        f = fopen(full, "w+b");
    if (!f)
        return -1;
    for (i = 0; i < FIO_HANDLES_MAX; i++) {
        if (!s_fio[i]) {
            s_fio[i] = f;
            return i + 1; // handles are 1-based (0/-1 = error)
        }
    }
    fclose(f);
    return -1;
}

int FIOFclose(int handle)
{
    if (handle <= 0 || handle > FIO_HANDLES_MAX || !s_fio[handle - 1])
        return -1;
    fclose(s_fio[handle - 1]);
    s_fio[handle - 1] = NULL;
    return 0;
}

u32 FIOFread(int handle, void* data, u32 size)
{
    if (handle <= 0 || handle > FIO_HANDLES_MAX || !s_fio[handle - 1] ||
        !data) {
        return 0;
    }
    return (u32) fread(data, 1, size, s_fio[handle - 1]);
}

u32 FIOFwrite(int handle, void* data, u32 size)
{
    if (handle <= 0 || handle > FIO_HANDLES_MAX || !s_fio[handle - 1] ||
        !data) {
        return 0;
    }
    return (u32) fwrite(data, 1, size, s_fio[handle - 1]);
}

u32 FIOFseek(int handle, long offset, u32 mode)
{
    static const int whence[] = { SEEK_SET, SEEK_CUR, SEEK_END };
    if (handle <= 0 || handle > FIO_HANDLES_MAX || !s_fio[handle - 1])
        return 0xFFFFFFFFu;
    if (mode > 2)
        mode = 0;
    if (fseek(s_fio[handle - 1], offset, whence[mode]) != 0)
        return 0xFFFFFFFFu;
    return (u32) ftell(s_fio[handle - 1]);
}

int FIOFprintf(int handle, const char* format, ...)
{
    va_list ap;
    int n;
    if (handle <= 0 || handle > FIO_HANDLES_MAX || !s_fio[handle - 1] ||
        !format) {
        return -1;
    }
    va_start(ap, format);
    n = vfprintf(s_fio[handle - 1], format, ap);
    va_end(ap);
    return n;
}

int FIOFflush(int handle)
{
    if (handle <= 0 || handle > FIO_HANDLES_MAX || !s_fio[handle - 1])
        return -1;
    return fflush(s_fio[handle - 1]);
}

int FIOFstat(int handle, struct FIO_Stat* stat)
{
    long cur, end;
    if (handle <= 0 || handle > FIO_HANDLES_MAX || !s_fio[handle - 1] ||
        !stat) {
        return -1;
    }
    cur = ftell(s_fio[handle - 1]);
    fseek(s_fio[handle - 1], 0, SEEK_END);
    end = ftell(s_fio[handle - 1]);
    fseek(s_fio[handle - 1], cur, SEEK_SET);
    memset(stat, 0, sizeof(*stat));
    stat->fileSizeLow = (u32) end;
    return 0;
}

int FIOFerror(int handle)
{
    if (handle <= 0 || handle > FIO_HANDLES_MAX || !s_fio[handle - 1])
        return -1;
    return ferror(s_fio[handle - 1]);
}

int FIOFindFirst(const char* filename, struct FIO_Finddata* finddata)
{
    (void) filename;
    (void) finddata;
    return -1;
}

int FIOFindNext(struct FIO_Finddata* finddata)
{
    (void) finddata;
    return -1;
}

u32 FIOGetAsyncBufferSize(void)
{
    return 0;
}

int FIOFreadAsync(int handle, void* data, u32 size)
{
    return (int) FIOFread(handle, data, size);
}

int FIOFwriteAsync(int handle, void* data, u32 size)
{
    return (int) FIOFwrite(handle, data, size);
}

int FIOCheckAsyncDone(u32* result)
{
    if (result)
        *result = 0;
    return 1;
}
