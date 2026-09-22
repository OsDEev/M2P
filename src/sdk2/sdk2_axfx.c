// SDK2 AXFX: software aux-bus effects (reverb/chorus/delay).
//
// The game (axdriver) registers these callbacks directly on the aux buses
// and tunes them with host-order params from HSD_AudioSFXGetDefaultAuxParam
// (reverb time/predelay/damping/mix, chorus base/variation/period, delay
// taps). This file implements the DSP in portable C so aux sends actually
// produce wet returns instead of silence.
//
// Conventions (shared with the sdk2_ax mixer):
// - AXFX_BUFFERUPDATE holds three s32 buses (left/right/surround) with
//   s16-range samples; effects process in place (float internally).
// - Delay-line memory comes from the __AXFXAlloc hooks (game heap), sized
//   to match HSD_AudioGetAuxHeapSize budgets.
// - Comb feedback derives from the reverb `time` (T60) parameter; damping
//   is a one-pole lowpass in the feedback loop; `mix` scales the return.

#include <dolphin/axfx.h>

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// FxComb/FxAllpass name the REVSTD delay-line type, but REVHI lines are
// layout-identical (verified below), so HI paths cast explicitly.
_Static_assert(sizeof(struct AXFX_REVSTD_DELAYLINE) ==
                   sizeof(struct AXFX_REVHI_DELAYLINE),
               "HI/STD delay lines must match");
_Static_assert(offsetof(struct AXFX_REVSTD_DELAYLINE, inputs) ==
                   offsetof(struct AXFX_REVHI_DELAYLINE, inputs),
               "HI/STD delay lines must match");

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define AXFX_RATE 32000

// Length-explicit processing entries (the Callbacks above forward here;
// sdk2_ax.c sets axfx_block_len before each aux call).
void AXFXReverbStdProcess(struct AXFX_REVERBSTD* rev, long* lr, long* rr,
                          long* sr, int n);
void AXFXReverbHiProcess(struct AXFX_REVERBHI* rev, long* lr, long* rr,
                         long* sr, int n);
void AXFXChorusProcess(struct AXFX_CHORUS* c, long* lr, long* rr, long* sr,
                       int n);
void AXFXDelayProcess(struct AXFX_DELAY* d, long* lr, long* rr, long* sr,
                      int n);

static float clampf(float v, float lo, float hi)
{
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    return v;
}

// ------------------------------------------------------------ primitives
typedef struct {
    float* buf;
    int len;
    int pos;
} FxLine;

static float* fx_alloc(int nfloats)
{
    float* p;
    if (nfloats < 16)
        nfloats = 16;
    if (__AXFXAlloc)
        p = (float*) __AXFXAlloc((unsigned long) nfloats * sizeof(float));
    else
        p = (float*) malloc(sizeof(float) * (size_t) nfloats);
    if (p)
        memset(p, 0, sizeof(float) * (size_t) nfloats);
    return p;
}

static void fx_free(float* p)
{
    if (!p)
        return;
    if (__AXFXFree)
        __AXFXFree(p);
    else
        free(p);
}

// Comb with damped feedback. inPoint/outPoint double as positions.
typedef struct {
    struct AXFX_REVSTD_DELAYLINE* d;
    float fb;
    float damp;
} FxComb;

static float fx_comb(FxComb* c, float x)
{
    float y = c->d->inputs[c->d->outPoint];
    // lowpass in the feedback path (Freeverb-style damping)
    c->d->lastOutput =
        c->d->lastOutput * c->damp + y * (1.f - c->damp);
    c->d->inputs[c->d->inPoint] = x + c->d->lastOutput * c->fb;
    c->d->inPoint++;
    if (c->d->inPoint >= c->d->length)
        c->d->inPoint = 0;
    c->d->outPoint++;
    if (c->d->outPoint >= c->d->length)
        c->d->outPoint = 0;
    return y;
}

typedef struct {
    struct AXFX_REVSTD_DELAYLINE* d;
    float fb;
} FxAllpass;

static float fx_allpass(FxAllpass* a, float x)
{
    float bufout = a->d->inputs[a->d->outPoint];
    float y = -x + bufout;
    a->d->inputs[a->d->inPoint] = x + bufout * a->fb;
    a->d->inPoint++;
    if (a->d->inPoint >= a->d->length)
        a->d->inPoint = 0;
    a->d->outPoint++;
    if (a->d->outPoint >= a->d->length)
        a->d->outPoint = 0;
    return y;
}

static void fx_line_setup(struct AXFX_REVSTD_DELAYLINE* d, float* mem,
                          int len)
{
    d->inputs = mem;
    d->length = len;
    d->inPoint = 0;
    d->outPoint = 0;
    d->lastOutput = 0;
}

// T60 feedback for a delay line of `len` samples at 32 kHz.
static float fx_fb_for_time(int len, float time_sec)
{
    float t;
    if (time_sec <= 0.05f)
        return 0;
    t = (float) len / (float) AXFX_RATE / time_sec;
    return clampf(powf(10.f, -3.f * t), 0.f, 0.96f);
}

// ------------------------------------------------------------ reverb std
// Topology per channel (matches the heap budget in
// HSD_AudioGetAuxHeapSize): predelay -> 2 combs -> 2 allpasses.
static void revstd_setup(struct AXFX_REVERBSTD* rev)
{
    static const int comb_len[2] = { 0x6FD + 2, 0x7CF + 2 };
    static const int ap_len[2] = { 0x1B1 + 2, 0x95 + 2 };
    int pre = (int) (clampf(rev->preDelay, 0.f, 0.5f) * AXFX_RATE) + 2;
    int ch, i;
    if (!rev)
        return;
    for (ch = 0; ch < 3; ch++) {
        for (i = 0; i < 2; i++) {
            if (!rev->rv.C[ch * 2 + i].inputs) {
                rev->rv.C[ch * 2 + i].inputs =
                    fx_alloc(comb_len[i]);
            }
            if (!rev->rv.AP[ch * 2 + i].inputs) {
                rev->rv.AP[ch * 2 + i].inputs = fx_alloc(ap_len[i]);
            }
        }
        if (!rev->rv.preDelayLine[ch])
            rev->rv.preDelayLine[ch] = fx_alloc(pre);
        rev->rv.preDelayPtr[ch] = rev->rv.preDelayLine[ch];
        rev->rv.preDelayTime = pre;
    }
    rev->rv.allPassCoeff = 0.5f;
}

static void revstd_free(struct AXFX_REVERBSTD* rev)
{
    int ch, i;
    if (!rev)
        return;
    for (ch = 0; ch < 3; ch++) {
        for (i = 0; i < 2; i++) {
            fx_free(rev->rv.C[ch * 2 + i].inputs);
            rev->rv.C[ch * 2 + i].inputs = NULL;
            fx_free(rev->rv.AP[ch * 2 + i].inputs);
            rev->rv.AP[ch * 2 + i].inputs = NULL;
        }
        fx_free(rev->rv.preDelayLine[ch]);
        rev->rv.preDelayLine[ch] = NULL;
        rev->rv.preDelayPtr[ch] = NULL;
    }
}

static float revstd_channel(struct AXFX_REVERBSTD* rev, int ch, float x)
{
    FxComb c0, c1;
    FxAllpass a0, a1;
    float fb, wet, y;
    float* pre;
    int prelen;
    if (!rev->rv.C[ch * 2].inputs)
        return 0;
    // predelay (read pointer stored in preDelayPtr)
    pre = rev->rv.preDelayLine[ch];
    prelen = (int) rev->rv.preDelayTime;
    if (!pre || prelen < 2)
        return 0;
    {
        float* rp = rev->rv.preDelayPtr[ch];
        long idx = (long) (rp - pre);
        float delayed;
        if (idx < 0 || idx >= prelen)
            idx = 0;
        delayed = pre[idx];
        pre[idx] = x;
        idx++;
        if (idx >= prelen)
            idx = 0;
        rev->rv.preDelayPtr[ch] = pre + idx;
        x = delayed;
    }
    fb = fx_fb_for_time((int) rev->rv.C[ch * 2].length, rev->time);
    c0.d = &rev->rv.C[ch * 2];
    c0.fb = fb;
    c0.damp = clampf(rev->damping, 0.f, 0.99f);
    c1.d = &rev->rv.C[ch * 2 + 1];
    c1.fb = fx_fb_for_time((int) rev->rv.C[ch * 2 + 1].length,
                           rev->time);
    c1.damp = c0.damp;
    y = fx_comb(&c0, x) + fx_comb(&c1, x);
    y *= 0.5f;
    a0.d = &rev->rv.AP[ch * 2];
    a0.fb = rev->rv.allPassCoeff;
    a1.d = &rev->rv.AP[ch * 2 + 1];
    a1.fb = rev->rv.allPassCoeff;
    y = fx_allpass(&a1, fx_allpass(&a0, y));
    wet = clampf(rev->mix, 0.f, 1.f);
    return y * wet;
}

int AXFXReverbStdInit(struct AXFX_REVERBSTD* rev)
{
    if (!rev)
        return 0;
    // The game memcpys a param template over this struct; delay-line
    // descriptors arrive as stack garbage, and the `if (!inputs)` guards
    // in setup cannot tell garbage from allocated lines. Clear runtime
    // state first (params live outside rv) so setup allocates everything.
    memset(&rev->rv, 0, sizeof(rev->rv));
    revstd_free(rev); // idempotent re-init
    revstd_setup(rev);
    if (!rev->rv.C[0].inputs)
        return 0;
    return 1;
}

int AXFXReverbStdShutdown(struct AXFX_REVERBSTD* rev)
{
    revstd_free(rev);
    return 1;
}

int AXFXReverbStdSettings(struct AXFX_REVERBSTD* rev)
{
    if (!rev)
        return 0;
    if (!rev->rv.C[0].inputs)
        revstd_setup(rev); // params changed pre-init: prepare lines
    return 1;
}

// Block length for the length-less AXFX callbacks. The mixer (the sole
// caller) sets it before every aux call; retail always processes full
// 160-sample DSP frames.
unsigned axfx_block_len = 160;

void AXFXReverbStdCallback(struct AXFX_BUFFERUPDATE* bufferUpdate,
                           struct AXFX_REVERBSTD* reverb)
{
    if (!bufferUpdate)
        return;
    AXFXReverbStdProcess(reverb, bufferUpdate->left, bufferUpdate->right,
                         bufferUpdate->surround, (int) axfx_block_len);
}

// Block variant used by the mixer (length-explicit).
void AXFXReverbStdProcess(struct AXFX_REVERBSTD* rev, long* lr,
                          long* rr, long* sr, int n)
{
    int i;
    if (!rev || rev->tempDisableFX)
        return; // dry passthrough: bus untouched
    if (!rev->rv.C[0].inputs)
        revstd_setup(rev);
    for (i = 0; i < n; i++) {
        float l = lr[i] / 32767.f;
        float r = rr ? rr[i] / 32767.f : l;
        float s = sr ? sr[i] / 32767.f : (l + r) * 0.5f;
        l = revstd_channel(rev, 0, l);
        r = revstd_channel(rev, 1, r);
        s = revstd_channel(rev, 2, s);
        lr[i] = (long) (clampf(l, -1.f, 1.f) * 32767.f);
        if (rr)
            rr[i] = (long) (clampf(r, -1.f, 1.f) * 32767.f);
        if (sr)
            sr[i] = (long) (clampf(s, -1.f, 1.f) * 32767.f);
    }
}

// ------------------------------------------------------------ reverb hi
// Per channel: predelay -> 4 combs -> 2 allpasses + crosstalk.
static void revhi_setup(struct AXFX_REVERBHI* rev)
{
    static const int comb_len[4] = { 0x6FD + 2, 0x7CF + 2, 0x91D + 2,
                                     0x1B1 + 2 };
    static const int ap_len[3] = { 0x95 + 2, 0x2F + 2, 0x49 + 2 };
    int pre = (int) (clampf(rev->preDelay, 0.f, 0.5f) * AXFX_RATE) + 2;
    int ch, i;
    if (!rev)
        return;
    for (ch = 0; ch < 3; ch++) {
        for (i = 0; i < 4 && ch * 4 + i < 9; i++) {
            if (!rev->rv.C[ch * 4 + i].inputs) {
                rev->rv.C[ch * 4 + i].inputs =
                    fx_alloc(comb_len[i]);
            }
        }
        for (i = 0; i < 2 && ch * 2 + i < 9; i++) {
            if (!rev->rv.AP[ch * 2 + i].inputs) {
                rev->rv.AP[ch * 2 + i].inputs = fx_alloc(ap_len[i]);
            }
        }
        if (!rev->rv.preDelayLine[ch])
            rev->rv.preDelayLine[ch] = fx_alloc(pre);
        rev->rv.preDelayPtr[ch] = rev->rv.preDelayLine[ch];
        rev->rv.preDelayTime = pre;
    }
    rev->rv.allPassCoeff = 0.5f;
}

static void revhi_free(struct AXFX_REVERBHI* rev)
{
    int i;
    if (!rev)
        return;
    for (i = 0; i < 9; i++) {
        fx_free(rev->rv.C[i].inputs);
        rev->rv.C[i].inputs = NULL;
        fx_free(rev->rv.AP[i].inputs);
        rev->rv.AP[i].inputs = NULL;
    }
    for (i = 0; i < 3; i++) {
        fx_free(rev->rv.preDelayLine[i]);
        rev->rv.preDelayLine[i] = NULL;
        rev->rv.preDelayPtr[i] = NULL;
    }
}

static float revhi_channel(struct AXFX_REVERBHI* rev, int ch, float x)
{
    FxComb c[4];
    FxAllpass a[2];
    float y = 0;
    float* pre;
    int prelen, i;
    if (ch < 0 || ch > 2 || !rev->rv.C[ch * 4].inputs)
        return 0;
    pre = rev->rv.preDelayLine[ch];
    prelen = (int) rev->rv.preDelayTime;
    if (pre && prelen >= 2) {
        float* rp = rev->rv.preDelayPtr[ch];
        long idx = (long) (rp - pre);
        float delayed;
        if (idx < 0 || idx >= prelen)
            idx = 0;
        delayed = pre[idx];
        pre[idx] = x;
        idx++;
        if (idx >= prelen)
            idx = 0;
        rev->rv.preDelayPtr[ch] = pre + idx;
        x = delayed;
    }
    for (i = 0; i < 4; i++) {
        c[i].d = (struct AXFX_REVSTD_DELAYLINE*) &rev->rv.C[ch * 4 + i];
        c[i].fb = fx_fb_for_time((int) c[i].d->length, rev->time);
        c[i].damp = clampf(rev->damping, 0.f, 0.99f);
        y += fx_comb(&c[i], x);
    }
    y *= 0.25f;
    for (i = 0; i < 2; i++) {
        a[i].d = (struct AXFX_REVSTD_DELAYLINE*) &rev->rv.AP[ch * 2 + i];
        a[i].fb = rev->rv.allPassCoeff;
        y = fx_allpass(&a[i], y);
    }
    return y * clampf(rev->mix, 0.f, 1.f);
}

int AXFXReverbHiInit(struct AXFX_REVERBHI* rev)
{
    if (!rev)
        return 0;
    // Same garbage-descriptor hazard as reverb-std: clear runtime state
    // (params live outside rv) before free/setup.
    memset(&rev->rv, 0, sizeof(rev->rv));
    revhi_free(rev);
    revhi_setup(rev);
    if (!rev->rv.C[0].inputs)
        return 0;
    return 1;
}

int AXFXReverbHiShutdown(struct AXFX_REVERBHI* rev)
{
    revhi_free(rev);
    return 1;
}

int AXFXReverbHiSettings(struct AXFX_REVERBHI* rev)
{
    if (!rev)
        return 0;
    if (!rev->rv.C[0].inputs)
        revhi_setup(rev);
    return 1;
}

void AXFXReverbHiCallback(struct AXFX_BUFFERUPDATE* bufferUpdate,
                          struct AXFX_REVERBHI* reverb)
{
    if (!bufferUpdate)
        return;
    AXFXReverbHiProcess(reverb, bufferUpdate->left, bufferUpdate->right,
                        bufferUpdate->surround, (int) axfx_block_len);
}

void AXFXReverbHiProcess(struct AXFX_REVERBHI* rev, long* lr, long* rr,
                         long* sr, int n)
{
    int i;
    float cross;
    if (!rev || rev->tempDisableFX)
        return;
    if (!rev->rv.C[0].inputs)
        revhi_setup(rev);
    cross = clampf(rev->crosstalk, 0.f, 1.f) * 0.5f;
    for (i = 0; i < n; i++) {
        float l = lr[i] / 32767.f;
        float r = rr ? rr[i] / 32767.f : l;
        float s = sr ? sr[i] / 32767.f : (l + r) * 0.5f;
        l = revhi_channel(rev, 0, l);
        r = revhi_channel(rev, 1, r);
        s = revhi_channel(rev, 2, s);
        // crosstalk between L/R returns
        {
            float tl = l + (r - l) * cross;
            float tr = r + (l - r) * cross;
            l = tl;
            r = tr;
        }
        lr[i] = (long) (clampf(l, -1.f, 1.f) * 32767.f);
        if (rr)
            rr[i] = (long) (clampf(r, -1.f, 1.f) * 32767.f);
        if (sr)
            sr[i] = (long) (clampf(s, -1.f, 1.f) * 32767.f);
    }
}

void DoCrossTalk(long* l, long* r, float cross, float invcross)
{
    if (!l || !r)
        return;
    {
        float fl = *l / 32767.f, fr = *r / 32767.f;
        float tl = fl * invcross + fr * cross;
        float tr = fr * invcross + fl * cross;
        *l = (long) (clampf(tl, -1.f, 1.f) * 32767.f);
        *r = (long) (clampf(tr, -1.f, 1.f) * 32767.f);
    }
}

// ------------------------------------------------------------ chorus
// Variable short delay with sinusoidal LFO. Line memory lives in
// src.smpBase (allocated here); position runs in currentPosLo.
static void chorus_setup(struct AXFX_CHORUS* c)
{
    int need;
    if (!c)
        return;
    need = (int) c->baseDelay + (int) c->variation + 64;
    if (need < 64)
        need = 64;
    if (need > 8192)
        need = 8192;
    if (!c->work.src.smpBase) {
        c->work.src.smpBase = (long*) fx_alloc(need);
        c->work.currentPosLo = 0;
    }
    c->work.src.old = NULL;
}

static void chorus_free(struct AXFX_CHORUS* c)
{
    if (!c)
        return;
    fx_free((float*) c->work.src.smpBase);
    c->work.src.smpBase = NULL;
    c->work.currentPosLo = 0;
}

int AXFXChorusInit(struct AXFX_CHORUS* c)
{
    if (!c)
        return 0;
    // Same garbage-descriptor hazard: clear work state (params
    // baseDelay/variation/period live outside work) before free/setup.
    memset(&c->work, 0, sizeof(c->work));
    chorus_free(c);
    chorus_setup(c);
    if (!c->work.src.smpBase)
        return 0;
    return 1;
}

int AXFXChorusShutdown(struct AXFX_CHORUS* c)
{
    chorus_free(c);
    return 1;
}

int AXFXChorusSettings(struct AXFX_CHORUS* c)
{
    if (!c)
        return 0;
    if (!c->work.src.smpBase)
        chorus_setup(c);
    return 1;
}

void AXFXChorusCallback(struct AXFX_BUFFERUPDATE* bufferUpdate,
                        struct AXFX_CHORUS* chorus)
{
    if (!bufferUpdate)
        return;
    AXFXChorusProcess(chorus, bufferUpdate->left, bufferUpdate->right,
                      bufferUpdate->surround, (int) axfx_block_len);
}

void AXFXChorusProcess(struct AXFX_CHORUS* c, long* lr, long* rr,
                       long* sr, int n)
{
    float* line;
    int len, i;
    unsigned period;
    if (!c || (!c->baseDelay && !c->variation))
        return; // bypass: bus untouched
    if (!c->work.src.smpBase)
        chorus_setup(c);
    line = (float*) c->work.src.smpBase;
    if (!line)
        return;
    len = (int) c->baseDelay + (int) c->variation + 64;
    if (len < 64)
        len = 64;
    if (len > 8192)
        len = 8192;
    period = c->period ? c->period : 1;
    for (i = 0; i < n; i++) {
        float lfo, off, rpos;
        int r0, r1;
        float fr, s;
        // LFO phase runs in currentPosLo's high bits is overkill;
        // use a per-block phase derived from the write position.
        unsigned wpos = c->work.currentPosLo % (unsigned) len;
        lfo = sinf(2.f * (float) M_PI *
                   ((float) (c->work.currentPosLo % period) /
                    (float) period));
        off = (float) c->baseDelay +
              (float) c->variation * (0.5f + 0.5f * lfo);
        if (off < 1.f)
            off = 1.f;
        if (off > len - 1)
            off = (float) (len - 1);
        // process L into line, read all three buses from it (mono line
        // shared; stereo image comes from the dry mix, standard for
        // aux chorus returns)
        line[wpos] = lr[i] / 32767.f;
        rpos = (float) wpos - off;
        while (rpos < 0)
            rpos += len;
        r0 = (int) rpos % len;
        r1 = (r0 + 1) % len;
        fr = rpos - (float) ((int) rpos);
        s = line[r0] + (line[r1] - line[r0]) * fr;
        lr[i] = (long) (clampf(s, -1.f, 1.f) * 32767.f);
        if (rr)
            rr[i] = (long) (clampf(s, -1.f, 1.f) * 32767.f);
        if (sr)
            sr[i] = (long) (clampf(s, -1.f, 1.f) * 32767.f);
        c->work.currentPosLo++;
    }
}

// ------------------------------------------------------------ delay
// Three taps (L/R/S) with per-tap time/feedback/output. Buffers live in
// left/right/sur; positions in currentPos[].
static void delay_setup(struct AXFX_DELAY* d)
{
    int i;
    if (!d)
        return;
    for (i = 0; i < 3; i++) {
        int len = (int) d->delay[i];
        if (len < 8)
            len = 8;
        if (len > 96000)
            len = 96000; // 3 s @ 32 kHz
        if (!d->left && i == 0)
            d->left = (long*) fx_alloc(len);
        if (!d->right && i == 1)
            d->right = (long*) fx_alloc(len);
        if (!d->sur && i == 2)
            d->sur = (long*) fx_alloc(len);
        d->currentSize[i] = (u32) len;
        d->currentPos[i] = 0;
        d->currentFeedback[i] = 0;
        d->currentOutput[i] = 0;
    }
}

static void delay_free(struct AXFX_DELAY* d)
{
    if (!d)
        return;
    fx_free((float*) d->left);
    fx_free((float*) d->right);
    fx_free((float*) d->sur);
    d->left = d->right = d->sur = NULL;
}

int AXFXDelayInit(struct AXFX_DELAY* d)
{
    if (!d)
        return 0;
    // Same garbage-descriptor hazard: clear only the line pointers
    // (delay[]/feedback[]/output[] are params consumed by setup).
    d->left = d->right = d->sur = NULL;
    delay_free(d);
    delay_setup(d);
    if (!d->left)
        return 0;
    return 1;
}

int AXFXDelayShutdown(struct AXFX_DELAY* d)
{
    delay_free(d);
    return 1;
}

int AXFXDelaySettings(struct AXFX_DELAY* d)
{
    if (!d)
        return 0;
    if (!d->left)
        delay_setup(d);
    return 1;
}

void AXFXDelayCallback(struct AXFX_BUFFERUPDATE* bufferUpdate,
                       struct AXFX_DELAY* delay)
{
    if (!bufferUpdate)
        return;
    AXFXDelayProcess(delay, bufferUpdate->left, bufferUpdate->right,
                     bufferUpdate->surround, (int) axfx_block_len);
}

void AXFXDelayProcess(struct AXFX_DELAY* d, long* lr, long* rr, long* sr,
                      int n)
{
    long* bus[3] = { lr, rr, sr };
    float* line[3];
    int i, k;
    if (!d)
        return;
    if (!d->left)
        delay_setup(d);
    line[0] = (float*) d->left;
    line[1] = (float*) d->right;
    line[2] = (float*) d->sur;
    for (i = 0; i < 3; i++) {
        float fb, wet;
        int len;
        if (!bus[i] || !line[i])
            continue;
        len = (int) d->currentSize[i];
        if (len < 8)
            continue;
        fb = clampf(d->feedback[i] / 128.f, 0.f, 0.95f);
        wet = clampf(d->output[i] / 128.f, 0.f, 1.f);
        if (wet <= 0)
            continue;
        for (k = 0; k < n; k++) {
            unsigned wpos = d->currentPos[i] % (unsigned) len;
            float x = bus[i][k] / 32767.f;
            float y = line[i][wpos];
            line[i][wpos] = x + y * fb;
            d->currentPos[i]++;
            bus[i][k] = (long) (clampf(y * wet, -1.f, 1.f) * 32767.f);
        }
    }
}

// ------------------------------------------------------------ hooks
void* (*__AXFXAlloc)(unsigned long);
void (*__AXFXFree)(void*);

void* AXFXAllocFunction(unsigned long size)
{
    if (__AXFXAlloc)
        return __AXFXAlloc(size);
    return malloc(size ? size : 1);
}

void AXFXFreeFunction(void* ptr)
{
    if (__AXFXFree)
        __AXFXFree(ptr);
    else
        free(ptr);
}

void AXFXSetHooks(void* (*alloc_hook)(unsigned long),
                  void (*free_hook)(void*))
{
    __AXFXAlloc = alloc_hook;
    __AXFXFree = free_hook;
}
