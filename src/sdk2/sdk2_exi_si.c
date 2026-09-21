// SDK2 EXI/SI stubs.
//
// The high-level PAD and CARD layers are re-implemented against host
// devices (see hal_input / sdk2_card), so nothing in the PC build performs
// real EXI/SI transfers. These stubs exist only to satisfy the link:
// EXI channels report "no device", SI reports standard GC controllers on
// channels backed by hal_input.

#include <dolphin/types.h>
#include <dolphin/os/OSContext.h>

// si.h relies on ambient OSTime (normally from dolphin/os.h, which we
// deliberately do NOT include here: os.h also pulls os/OSSerial.h,
// which redeclares the same SI entry points with different
// (unsigned long) signatures and breaks the build).
typedef s64 OSTime;

#include <dolphin/exi.h>
#include <dolphin/si.h>

#include <stdarg.h>
#include <stdio.h>

// Declared locally instead of <dolphin/db.h>: db.h pulls db/DBInterface.h
// which includes <dolphin/os.h> (see above).
BOOL DBIsDebuggerPresent(void);
void DBPrintf(char* str, ...);

// ------------------------------------------------------------ debugger stub
BOOL DBIsDebuggerPresent(void)
{
    return FALSE;
}

void DBPrintf(char* str, ...)
{
    va_list ap;
    va_start(ap, str);
    vprintf(str, ap);
    va_end(ap);
    fflush(stdout);
}

extern void hal_input_set_present(int chan, int present);

// ------------------------------------------------------------ EXI
static EXICallback s_exi_cb[3];

EXICallback EXISetExiCallback(s32 channel, EXICallback callback)
{
    EXICallback prev = NULL;
    if (channel >= 0 && channel < 3) {
        prev = s_exi_cb[channel];
        s_exi_cb[channel] = callback;
    }
    return prev;
}

void EXIInit(void)
{
}

BOOL EXILock(s32 channel, u32 device, EXICallback callback)
{
    (void) channel;
    (void) device;
    (void) callback;
    return TRUE;
}

BOOL EXIUnlock(s32 channel)
{
    (void) channel;
    return TRUE;
}

BOOL EXISelect(s32 channel, u32 device, u32 frequency)
{
    (void) channel;
    (void) device;
    (void) frequency;
    return TRUE;
}

BOOL EXIDeselect(s32 channel)
{
    (void) channel;
    return TRUE;
}

BOOL EXIImm(s32 channel, void* buffer, s32 length, u32 type,
            EXICallback callback)
{
    (void) channel;
    (void) buffer;
    (void) length;
    (void) type;
    if (callback)
        callback(channel, NULL);
    return TRUE;
}

BOOL EXIImmEx(s32 channel, void* buffer, s32 length, u32 type)
{
    (void) channel;
    (void) buffer;
    (void) length;
    (void) type;
    return TRUE;
}

BOOL EXIDma(s32 channel, void* buffer, s32 length, u32 type,
            EXICallback callback)
{
    (void) channel;
    (void) buffer;
    (void) length;
    (void) type;
    if (callback)
        callback(channel, NULL);
    return TRUE;
}

BOOL EXISync(s32 channel)
{
    (void) channel;
    return TRUE;
}

BOOL EXIProbe(s32 channel)
{
    // channel 0/1: memory card present (backed by sdk2_card)
    return (channel == 0 || channel == 1) ? TRUE : FALSE;
}

s32 EXIProbeEx(s32 channel)
{
    return EXIProbe(channel) ? 1 : -1;
}

BOOL EXIAttach(s32 channel, EXICallback callback)
{
    (void) channel;
    (void) callback;
    return TRUE;
}

BOOL EXIDetach(s32 channel)
{
    (void) channel;
    return TRUE;
}

u32 EXIGetState(s32 channel)
{
    (void) channel;
    return 0;
}

s32 EXIGetID(s32 channel, u32 device, u32* id)
{
    (void) device;
    if (id)
        *id = 0;
    // memory-card IDs on 0/1 (any plausible ID keeps CARD happy)
    if (channel == 0 || channel == 1) {
        if (id)
            *id = 0x00000002u;
        return 1;
    }
    return 0;
}

void EXIProbeReset(void)
{
}

// ------------------------------------------------------------ SI
BOOL SITransfer(s32 chan, void* output, u32 outputBytes, void* input,
                u32 inputBytes, SICallback callback, OSTime delay)
{
    (void) chan;
    (void) output;
    (void) outputBytes;
    (void) input;
    (void) inputBytes;
    (void) delay;
    if (callback)
        callback(chan, 0, NULL);
    return TRUE;
}

u32 SIGetCommand(long chan)
{
    (void) chan;
    return 0;
}

u32 SIEnablePolling(u32 poll)
{
    return poll;
}

u32 SIDisablePolling(u32 poll)
{
    (void) poll;
    return 0;
}

u32 SISetXY(u32 x, u32 y)
{
    (void) x;
    (void) y;
    return 0;
}

void SITransferCommands(void)
{
}

BOOL SIBusy(void)
{
    return FALSE;
}
