#include "devcom.h"

#include "debug.h"
#include "devcom.static.h"
#include "synth.h"

bool HSD_DevComIsBusy(int idx)
{
    return (bool) devComStatus[idx];
}

static void HSD_DevComUnlink(HSD_DevCom* dc)
{
    HSD_DevCom* curr;
    bool enabled = OSDisableInterrupts();
    int i = dc->dcReq & 3;

    if (devComStatus[i] == dc) {
        devComStatus[i] = dc->next;
        if (HSD_DevCom_804C6330[i] == dc) {
            HSD_DevCom_804C6330[i] = 0;
        }
        goto cleanup;
    }

    for (curr = devComStatus[i]; curr->next != NULL; curr = curr->next) {
        if (curr->next == dc) {
            curr->next = dc->next;
            if (HSD_DevCom_804C6330[i] == dc) {
                HSD_DevCom_804C6330[i] = curr;
            }
            OSRestoreInterrupts(enabled);
            return;
        }
    }
    HSD_ASSERT(0x6E, 0);

cleanup:
    OSRestoreInterrupts(enabled);
}

static void HSD_DevComStdCallback(ARQRequest* request)
{
    int i;

    if (request == &devComARQR[0][0]) {
        i = 0;
    } else if (request == &devComARQR[1][0]) {
        i = 1;
    } else {
        HSD_ASSERT(0xA5, 0);
    }
    aramstate = 0;
    devComRelayBufFlag[i] = false;
    HSD_DevComDVDWakeUp();
    HSD_DevComARAMWakeUp();
}

static inline void HSD_DevComARAMCallback_inline(HSD_DevCom* devcom)
{
    bool enabled = OSDisableInterrupts();
    devcom->next = HSD_DevCom_804D77F0;
    HSD_DevCom_804D77F0 = devcom;
    OSRestoreInterrupts(enabled);
}

static void HSD_DevComARAMCallback(ARQRequest* request)
{
    int i;
    void* buf;

    if (aramDC->type == 0x1A) {
        if (request == &devComARQR[0][0]) {
            i = 0;
        } else {
            i = 1;
        }
        buf = HSD_DevCom_804C6330_bufs[i];
    } else {
        buf = NULL;
    }

    if (aramDC->callback != NULL) {
        aramDC->callback(aramDC->dcReq, aramDC->args, buf, aramDC->cancelflag);
    }

    HSD_DevComUnlink(aramDC);
    HSD_DevComARAMCallback_inline(aramDC);
    HSD_DevComStdCallback(request);
}

static inline int getRelayBufIdx(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (!devComRelayBufFlag[i]) {
            devComRelayBufFlag[i] = true;
            return i;
        }
    }
    return -1;
}

// PC: with synchronous completion, globals like dvdDC/aramDC may be stale
// after a nested callback unlinks or replaces the queue head. Never trust
// them for "still queued?" decisions; walk the queues instead.
static int devcom_still_queued(const HSD_DevCom* dc)
{
    int i;
    const HSD_DevCom* cur;
    for (i = 0; i < 4; i++) {
        for (cur = devComStatus[i]; cur != NULL; cur = cur->next) {
            if (cur == dc) {
                return 1;
            }
        }
    }
    return 0;
}

// PC port note: sdk2 ARQ completes transfers synchronously inside
// ARQPostRequest (retail ARAM DMA is async). The body below is written
// for async completion: it posts, then does transfer bookkeeping. With
// synchronous completion the callback (unlink, queue advance, reentrant
// wakeups) already ran by the time ARQPostRequest returns, so each site:
//  - raises aramstate BEFORE posting (a reentrant wakeup must not
//    re-post the same chunk), and
//  - applies bookkeeping only if its request is still queued
//    (devComStatus[3] == cur); otherwise the nested path already
//    finished everything.
// Multi-chunk requests are drained by recursing (retail would continue
// on the next ARQ interrupt); OSDisableInterrupts is a plain flag on PC,
// so the recursion is safe.
void HSD_DevComARAMWakeUp(void)
{
    bool enabled;
    int req_idx;
    u32 xfer_size2;
    void (*arq_callback)(ARQRequest*);
    void (*arq_callback2)(ARQRequest*);
    HSD_DevCom* cur;

    enabled = OSDisableInterrupts();
    if (aramstate != 0) {
        OSRestoreInterrupts(enabled);
        return;
    }
    aramDC = devComStatus[3];
    if (devComStatus[3] != NULL) {
        cur = aramDC;
        if (aramDC->cancelflag) {
            if (aramDC->callback != NULL) {
                aramDC->callback(aramDC->dcReq, aramDC->args, NULL, true);
            }
            HSD_DevComUnlink(aramDC);
            OSRestoreInterrupts(enabled);
            HSD_DevComARAMWakeUp();
            return;
        }
        req_idx = getRelayBufIdx();
        if (req_idx >= 0) {
            if (aramDC->type == 3) {
                u32 xfer_size;
                if (aramDC->size > DEVCOM_BUF_SIZE) {
                    arq_callback = HSD_DevComStdCallback;
                    xfer_size = DEVCOM_BUF_SIZE;
                } else {
                    arq_callback = HSD_DevComARAMCallback;
                    xfer_size = aramDC->size;
                }
                {
                    int* p = HSD_DevCom_804C6330_bufs[req_idx];
                    int i;
                    for (i = 0x1000; i > 0; i--) {
                        *p++ = 0;
                    }
                }
                DCStoreRange(HSD_DevCom_804C6330_bufs[req_idx],
                             DEVCOM_BUF_SIZE);
                // PC: ARQ completes synchronously; see function note.
                aramstate = 1;
                ARQPostRequest(devComARQR[req_idx], 0, 0, 1,
                               (uintptr_t) HSD_DevCom_804C6330_bufs[req_idx],
                               cur->dest, xfer_size, arq_callback);
                if (devComStatus[3] == cur) {
                    cur->dest += xfer_size;
                    cur->size -= xfer_size;
                    aramstate = 1;
                    if (cur->size > 0) {
                        // More chunks remain; retail would continue on
                        // the next ARQ interrupt, so pump explicitly.
                        aramstate = 0;
                        OSRestoreInterrupts(enabled);
                        HSD_DevComARAMWakeUp();
                        return;
                    }
                }
            } else if (aramDC->type == 0xB) {
                DCStoreRange((void*) cur->src, cur->size);
                // PC: ARQ completes synchronously; see function note.
                aramstate = 1;
                ARQPostRequest(devComARQR[req_idx], 0, 0, 1, cur->src,
                               cur->dest, cur->size,
                               HSD_DevComARAMCallback);
                if (devComStatus[3] == cur) {
                    aramstate = 1;
                }
            } else if (aramDC->type == 0x19) {
                DCInvalidateRange((void*) cur->dest, cur->size);
                // PC: ARQ completes synchronously; see function note.
                aramstate = 1;
                ARQPostRequest(devComARQR[req_idx], 0, 1, 1, cur->src,
                               cur->dest, cur->size,
                               HSD_DevComARAMCallback);
                if (devComStatus[3] == cur) {
                    aramstate = 1;
                }
            } else if (aramDC->type == 0x1A) {
                DCInvalidateRange(HSD_DevCom_804C6330_bufs[req_idx],
                                  DEVCOM_BUF_SIZE);
                // PC: ARQ completes synchronously; see function note.
                aramstate = 1;
                ARQPostRequest(devComARQR[req_idx], 0, 1, 1, cur->src,
                               (uintptr_t) HSD_DevCom_804C6330_bufs[req_idx],
                               cur->size, HSD_DevComARAMCallback);
                if (devComStatus[3] == cur) {
                    aramstate = 1;
                }
            } else if (aramDC->type == 0x1B) {
                DCInvalidateRange(HSD_DevCom_804C6330_bufs[req_idx],
                                  DEVCOM_BUF_SIZE);
                if (cur->size > DEVCOM_BUF_SIZE) {
                    arq_callback2 = HSD_DevComStdCallback;
                    xfer_size2 = DEVCOM_BUF_SIZE;
                } else {
                    arq_callback2 = HSD_DevComARAMCallback;
                    xfer_size2 = cur->size;
                }
                ARQPostRequest(&devComARQR[req_idx][1], 0, 1, 1, cur->src,
                               (uintptr_t) HSD_DevCom_804C6330_bufs[req_idx],
                               xfer_size2, NULL);
                // PC: ARQ completes synchronously; see function note.
                aramstate = 1;
                ARQPostRequest(&devComARQR[req_idx][0], 0, 0, 1,
                               (uintptr_t) HSD_DevCom_804C6330_bufs[req_idx],
                               cur->dest, xfer_size2, arq_callback2);
                if (devComStatus[3] == cur) {
                    cur->src += xfer_size2;
                    cur->dest += xfer_size2;
                    cur->size -= xfer_size2;
                    aramstate = 1;
                    if (cur->size > 0) {
                        // More chunks remain; retail would continue on
                        // the next ARQ interrupt, so pump explicitly.
                        aramstate = 0;
                        OSRestoreInterrupts(enabled);
                        HSD_DevComARAMWakeUp();
                        return;
                    }
                }
            }
        }
    }
    OSRestoreInterrupts(enabled);
}

static void HSD_DevComDVDStdCallback(ARQRequest* request)
{
    int i;
    if (request == &devComARQR[0][0]) {
        i = 0;
    } else if (request == &devComARQR[1][0]) {
        i = 1;
    } else {
        HSD_ASSERT(0x158, 0);
    }
    devComRelayBufFlag[i] = false;
    HSD_DevComDVDWakeUp();
    HSD_DevComARAMWakeUp();
}

static void HSD_DevComDVDARAMEndCallback(ARQRequest* request)
{
    int i;

    HSD_DevComDVDStdCallback(request);

    if (request == &devComARQR[0][0]) {
        i = 0;
    } else {
        i = 1;
    }

    if (HSD_DevCom_804D77FC[i]->callback != NULL && HSD_DevCom_804D7804 == 0) {
        HSD_DevCom_804D77FC[i]->callback(HSD_DevCom_804D77FC[i]->dcReq,
                                         HSD_DevCom_804D77FC[i]->args, NULL,
                                         HSD_DevCom_804D77FC[i]->cancelflag);
    }
    // NOTE: do NOT recycle the dc here. The caller (HSD_DevComDVDCallback)
    // unlinks it from the status queue only after this returns; recycling
    // first would overwrite its `next` link, and the unlink would then
    // re-queue a stale entry (use-after-recycle). Recycling happens in
    // the caller, mirroring the other completion paths.
    HSD_DevCom_804D77FC[i] = NULL;
}

static void HSD_DevComDVDMemCallback(s32 result, DVDFileInfo* unused)
{
    HSD_DevCom* dc;
    bool enabled;

    if (result == -1) {
        HSD_DevCom_804D7804 = 1;
    }
    if (dvdDC->size > 0x80000) {
        dvdDC->src += 0x80000;
        dvdDC->dest += 0x80000;
        dvdDC->size -= 0x80000;
        HSD_DevCom_804D77F5 = 0;
        HSD_DevComDVDWakeUp();
        return;
    }
    if (dvdDC->callback != NULL && HSD_DevCom_804D7804 == 0) {
        dvdDC->callback(dvdDC->dcReq, dvdDC->args, NULL, dvdDC->cancelflag);
    }
    HSD_DevComUnlink(dvdDC);
    dc = dvdDC;
    enabled = OSDisableInterrupts();
    dc->next = HSD_DevCom_804D77F0;
    HSD_DevCom_804D77F0 = dc;
    OSRestoreInterrupts(enabled);
    HSD_DevCom_804D77F5 = 0;
    HSD_DevComDVDWakeUp();
}

static void HSD_DevComDVDCallback(s32 result, DVDFileInfo* unused)
{
    HSD_DevCom* dc;
    s32 enabled;
    u16 type;

    PAD_STACK(8);

    if (result == -1) {
        HSD_DevCom_804D7804 = 1;
    }
    type = dvdDC->type;
    if (type == 0x22) {
        HSD_ASSERT(0x18C, dvdDC->size <= DEVCOM_BUF_SIZE);
        HSD_ASSERT(0x18D, dvdDC->callback);
        if (HSD_DevCom_804D7804 == 0) {
            dvdDC->callback(dvdDC->dcReq, dvdDC->args,
                            HSD_DevCom_804C6330_bufs[HSD_DevCom_804D77F6],
                            dvdDC->cancelflag);
        }
        HSD_DevComUnlink(dvdDC);
        dc = dvdDC;
        enabled = OSDisableInterrupts();
        dc->next = HSD_DevCom_804D77F0;
        HSD_DevCom_804D77F0 = dc;
        OSRestoreInterrupts(enabled);
        HSD_DevCom_804D77F5 = 0;
        devComRelayBufFlag[HSD_DevCom_804D77F6] = false;
        HSD_DevComDVDWakeUp();
        HSD_DevComARAMWakeUp();
    } else if (type == 0x23) {
        HSD_DevCom_804D77F7 = HSD_DevCom_804D77F6;
        if (dvdDC->size > DEVCOM_BUF_SIZE) {
            ARQPostRequest(
                devComARQR[HSD_DevCom_804D77F7], 0, 0, 1,
                (uintptr_t) HSD_DevCom_804C6330_bufs[HSD_DevCom_804D77F7],
                dvdDC->dest, DEVCOM_BUF_SIZE, HSD_DevComDVDStdCallback);
            dvdDC->src += DEVCOM_BUF_SIZE;
            dvdDC->dest += DEVCOM_BUF_SIZE;
            dvdDC->size -= DEVCOM_BUF_SIZE;
            HSD_DevCom_804D77F5 = 0;
            HSD_DevComDVDWakeUp();
        } else {
            HSD_DevCom_804D77FC[HSD_DevCom_804D77F7] = dvdDC;
            ARQPostRequest(
                devComARQR[HSD_DevCom_804D77F7], 0, 0, 1,
                (uintptr_t) HSD_DevCom_804C6330_bufs[HSD_DevCom_804D77F7],
                dvdDC->dest, dvdDC->size, HSD_DevComDVDARAMEndCallback);
            HSD_DevComUnlink(dvdDC);
            // Recycle only after unlink (see EndCallback note); the other
            // completion paths use the same unlink-then-recycle order.
            dc = dvdDC;
            enabled = OSDisableInterrupts();
            dc->next = HSD_DevCom_804D77F0;
            HSD_DevCom_804D77F0 = dc;
            OSRestoreInterrupts(enabled);
            HSD_DevCom_804D77F5 = 0;
            HSD_DevComDVDWakeUp();
        }
    }
}

void HSD_DevComDVDWakeUp(void)
{
    bool enabled = OSDisableInterrupts();
    int i;
    int buf_idx;

    if (HSD_DevCom_804D77F5 != 0) {
        OSRestoreInterrupts(enabled);
        return;
    }
    for (i = 0; i < 3; i++) {
        if ((dvdDC = devComStatus[i])) {
            if (dvdDC->cancelflag) {
                if (dvdDC->callback != NULL) {
                    dvdDC->callback(dvdDC->dcReq, dvdDC->args, NULL, true);
                }
                HSD_DevComUnlink(dvdDC);
                OSRestoreInterrupts(enabled);
                HSD_DevComDVDWakeUp();
                return;
            }
            DVDFastOpen(dvdDC->file, &fileinfo);
            if (dvdDC->type == 0x21) {
                HSD_DevCom* dc = dvdDC;
                // PC: DVD completes synchronously (sdk2 reads host files
                // inline); claim busy BEFORE posting so a reentrant wakeup
                // cannot re-post the same chunk (the advance below runs
                // only after the nested callback returns).
                HSD_DevCom_804D77F5 = 1;
                DVDReadAsyncPrio(&fileinfo, (void*) dc->dest,
                                 MIN(dc->size, 0x80000), (s32) dc->src,
                                 HSD_DevComDVDMemCallback, 2);
                if (devcom_still_queued(dc)) {
                    // Still queued: the nested callback advanced one chunk
                    // but its re-wake was blocked; retail would continue
                    // on the next DVD interrupt, so pump explicitly.
                    // (Nested completion paths clear 77F5 themselves.)
                    HSD_DevCom_804D77F5 = 0;
                    OSRestoreInterrupts(enabled);
                    HSD_DevComDVDWakeUp();
                    return;
                }
                OSRestoreInterrupts(enabled);
                return;
            }
            buf_idx = getRelayBufIdx();
            if (buf_idx >= 0) {
                HSD_DevCom* dc = dvdDC;
                HSD_DevCom_804D77F6 = buf_idx;
                // PC: same sync-completion reasoning as above.
                HSD_DevCom_804D77F5 = 1;
                DVDReadAsyncPrio(&fileinfo, HSD_DevCom_804C6330_bufs[buf_idx],
                                 MIN(dc->size, DEVCOM_BUF_SIZE), dc->src,
                                 HSD_DevComDVDCallback, 2);
                if (devcom_still_queued(dc)) {
                    HSD_DevCom_804D77F5 = 0;
                    OSRestoreInterrupts(enabled);
                    HSD_DevComDVDWakeUp();
                    return;
                }
                OSRestoreInterrupts(enabled);
                return;
            }
        }
    }
    OSRestoreInterrupts(enabled);
}

static inline int HSD_DevComGetDestType(int type)
{
    return type & 7;
}

#define INIT_N_DEVCOMS 16

static inline void DevComLinkNext(HSD_DevCom* dc)
{
    int i;
    for (i = 1; i < INIT_N_DEVCOMS - 1; i++) {
        dc[i].next = &dc[i] + 1;
    }
    dc[i].next = NULL;
}

int HSD_DevComRequest(int file, uintptr_t src, uintptr_t dest, size_t size,
                      int type, int pri, HSD_DevComCallback cb, uintptr_t args)
{
    bool enabled;
    HSD_DevCom* dc;
    int result;

    enabled = OSDisableInterrupts();

    if ((dc = HSD_DevCom_804D77F0)) {
        HSD_DevCom_804D77F0 = dc->next;
        OSRestoreInterrupts(enabled);
    } else {
        dc = HSD_AudioMalloc(sizeof(HSD_DevCom) * INIT_N_DEVCOMS);
        DevComLinkNext(dc);

        HSD_DevCom_804D77F0 = &dc[1];
        OSRestoreInterrupts(enabled);
    }

    HSD_ASSERT(0x1ED, dc);
    HSD_ASSERT(0x1EE,
        !(HSD_DevComGetDestType(type) == DEVCOMDEST_SBUF
            && size > DEVCOM_BUF_SIZE));

    HSD_ASSERT(0x1EF, src % 32 == 0);
    HSD_ASSERT(0x1F0, dest % 32 == 0);
    HSD_ASSERT(0x1F1, size % 32 == 0);
    HSD_ASSERT(0x1F2, size != 0);

    pri = (type & 0x38) == 0x20 ? pri : 3;

    dc->file = file;
    dc->src = src;
    dc->dest = dest;
    dc->size = size;
    dc->type = type;
    dc->cancelflag = false;
    dc->callback = cb;

    enabled = OSDisableInterrupts();
    result = HSD_DevCom_804D6050 + pri;
    dc->dcReq = result;
    HSD_DevCom_804D6050 += 4;
    if (HSD_DevCom_804C6330[pri] != NULL) {
        HSD_DevCom_804C6330[pri]->next = dc;
    }
    HSD_DevCom_804C6330[pri] = dc;
    dc->next = NULL;
    if (devComStatus[pri] == NULL) {
        devComStatus[pri] = dc;
        HSD_DevComDVDWakeUp();
        HSD_DevComARAMWakeUp();
    }
    OSRestoreInterrupts(enabled);

    return result;
}

static inline HSD_DevCom* HSD_DevComCancelEx_inline(int dcReq)
{
    HSD_DevCom* cur = devComStatus[dcReq & 3];
    while (cur != NULL) {
        if (cur->dcReq == dcReq) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

int HSD_DevComCancelEx(int dcReq, u32 flags, HSD_DevComCallback cb,
                       uintptr_t args)
{
    HSD_DevCom* dc;
    bool enabled = OSDisableInterrupts();

    if ((dc = HSD_DevComCancelEx_inline(dcReq))) {
        int tmp = dcReq & 3;
        if (flags & 1) {
            dc->callback = cb;
        }
        if (flags & 2) {
            dc->args = args;
        }
        if (devComStatus[tmp] == dc) {
            dc->cancelflag = true;
        } else {
            if (dc->callback != NULL) {
                dc->callback(dc->dcReq, dc->args, NULL, true);
            }
            HSD_DevComUnlink(dc);
        }
    } else {
        int i;
        for (i = 0; i < 2; i++) {
            HSD_DevCom* dc = HSD_DevCom_804D77FC[i];
            if (dc != NULL && dc->dcReq == dcReq) {
                dc->callback = cb;
                dc->args = args;
                HSD_DevCom_804D77FC[i]->cancelflag = true;
            }
        }
    }
    OSRestoreInterrupts(enabled);
    return 0;
}
