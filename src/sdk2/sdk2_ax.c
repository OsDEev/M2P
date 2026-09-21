// SDK2 AX: software voice mixer replacing the DSP.
//
// The game (HSD synth + axdriver) drives up to 64 AXVPB voices with
// ADPCM/PCM samples resident in ARAM. This mixer renders them to 32 kHz
// stereo s16 and feeds hal_audio. It runs inside hal_audio_mix_request()
// (called from the audio pump, once per VI frame) and fires the AX user
// callback every 160 samples (5 ms AX frame, as on hardware).
//
// Endianness: sample bytes and struct fields that travel from disc via
// memcpy are big-endian on the wire; multi-byte fields are loaded with
// explicit BE accessors here. Fields the game computes natively
// (volumes, ratios) use native loads. See README_PC_PORT.md (Stage 2)
// for the remaining game-side SFX-entry parsing work.
//
// Aux-bus effects (reverb/chorus/delay) run for real in sdk2_axfx.c.
// Not yet emulated: ITD, FIR/4-tap SRC (linear instead), DPOP removal,
// aux-send volume deltas (static sends).

#include <dolphin/ax.h>
#include <dolphin/axfx.h>
#include <dolphin/os.h>
#include <pc/pc_endian.h> // Stage 2: PB address fields are big-endian

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void hal_audio_submit(const s16* frames, unsigned nframes);

#define AX_FRAME 160 // samples per AX frame @ 32 kHz

static AXVPB s_voices[AX_MAX_VOICES];
static int s_voice_used[AX_MAX_VOICES];
static u32 s_ax_mode;
static void (*s_user_cb)(void);
static void (*s_aux_a_cb)(void*, void*);
static void* s_aux_a_ctx;
static void (*s_aux_b_cb)(void*, void*);
static void* s_aux_b_ctx;
static u32 s_max_dsp_cycles = 0xFFFFFFFFu;
static int s_ax_init;

// per-voice render cursor (host-side; the DSP PB copy is write-only)
typedef struct {
    float fpos;   // fractional ARAM byte address (PCM cursor)
    s16 yn1, yn2; // ADPCM history
    float vol;    // current VE volume (0..1)
    int active;
    // ADPCM frame cache: one decoded 8-byte frame + position in samples.
    // (ADPCM frames hold 14 samples in bytes 1..7; byte 0 is the header,
    // so naive byte*2 indexing would play the header as audio.)
    u32 ad_frame;   // base byte address of the cached frame
    s16 ad_buf[14]; // decoded samples
    s16 ad_prev;    // last sample before ad_buf[0] (for interpolation)
    float ad_pos;   // 0..14 position inside ad_buf
    int ad_valid;
} VoiceState;

static VoiceState s_vs[AX_MAX_VOICES];

static u16 be16(const u8* p)
{
    return (u16) (((unsigned) p[0] << 8) | p[1]);
}

static u32 be32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) |
           ((u32) p[2] << 8) | p[3];
}

static float be_float(const u8* p)
{
    union {
        u32 u;
        float f;
    } v;
    v.u = be32(p);
    return v.f;
}

// ------------------------------------------------------------ ADPCM decode
// GC DSP-ADPCM: 8-byte frames, header byte (scale[7:4], coef[3:0]) +
// 14 4-bit samples. y[n] = (x[n]*S << 11 + 1024 + c1*y1 + c2*y2) >> 11.
// Full-frame decode into out[14]; updates history. `frame` points at
// 8 ARAM bytes; coefs are 16 s16 BE values (8 pairs).
static void adpcm_frame(const u8* frame, const u8* coefs_be, int* yn1,
                        int* yn2, s16* out)
{
    unsigned hdr = frame[0];
    int scale = 1 << (hdr >> 4);
    unsigned ci = (hdr & 7) * 2;
    int c1 = (s16) be16(coefs_be + ci * 2);
    int c2 = (s16) be16(coefs_be + ci * 2 + 2);
    int i;
    for (i = 0; i < 14; i++) {
        unsigned byte = frame[1 + i / 2];
        int nib = (i & 1) ? (byte & 0xF) : (byte >> 4);
        int s = (nib >= 8) ? nib - 16 : nib;
        int sample = (((s * scale) << 11) + 1024 + c1 * *yn1 + c2 * *yn2) >> 11;
        if (sample > 32767)
            sample = 32767;
        if (sample < -32768)
            sample = -32768;
        *yn2 = *yn1;
        *yn1 = sample;
        out[i] = (s16) sample;
    }
    (void) yn1;
    (void) yn2;
}

// ------------------------------------------------------------ voice alloc
void AXInit(void)
{
    memset(s_voices, 0, sizeof(s_voices));
    memset(s_voice_used, 0, sizeof(s_voice_used));
    memset(s_vs, 0, sizeof(s_vs));
    s_ax_init = 1;
}

void AXQuit(void)
{
    s_ax_init = 0;
    memset(s_voice_used, 0, sizeof(s_voice_used));
    memset(s_vs, 0, sizeof(s_vs));
}

AXVPB* AXAcquireVoice(u32 priority, void (*callback)(void*),
                      u32 userContext)
{
    int i, victim = -1;
    for (i = 0; i < AX_MAX_VOICES; i++) {
        if (!s_voice_used[i]) {
            s_voice_used[i] = 1;
            memset(&s_voices[i], 0, sizeof(AXVPB));
            memset(&s_vs[i], 0, sizeof(VoiceState));
            s_voices[i].priority = (int) priority;
            s_voices[i].callback = callback;
            s_voices[i].userContext = userContext;
            s_voices[i].index = (u32) i;
            return &s_voices[i];
        }
    }
    // steal the lowest-priority voice below the request
    for (i = 0; i < AX_MAX_VOICES; i++) {
        if ((u32) s_voices[i].priority < priority &&
            (victim < 0 ||
             s_voices[i].priority < s_voices[victim].priority)) {
            victim = i;
        }
    }
    if (victim < 0)
        return NULL;
    if (s_voices[victim].callback)
        s_voices[victim].callback((void*) (uintptr_t) s_voices[victim].userContext);
    memset(&s_voices[victim], 0, sizeof(AXVPB));
    memset(&s_vs[victim], 0, sizeof(VoiceState));
    s_voices[victim].priority = (int) priority;
    s_voices[victim].callback = callback;
    s_voices[victim].userContext = userContext;
    s_voices[victim].index = (u32) victim;
    return &s_voices[victim];
}

void AXFreeVoice(AXVPB* p)
{
    int i;
    if (!p)
        return;
    i = (int) (p - s_voices);
    if (i < 0 || i >= AX_MAX_VOICES)
        return;
    s_voice_used[i] = 0;
    memset(&s_vs[i], 0, sizeof(VoiceState));
    memset(p, 0, sizeof(AXVPB));
}

void AXSetVoicePriority(AXVPB* p, u32 priority)
{
    if (p)
        p->priority = (int) priority;
}

void AXRegisterAuxACallback(void (*callback)(void*, void*), void* context)
{
    s_aux_a_cb = callback;
    s_aux_a_ctx = context;
}

void AXRegisterAuxBCallback(void (*callback)(void*, void*), void* context)
{
    s_aux_b_cb = callback;
    s_aux_b_ctx = context;
}

void AXSetMode(u32 mode)
{
    s_ax_mode = mode;
}

u32 AXGetMode(void)
{
    return s_ax_mode;
}

extern AXPROFILE __AXLocalProfile;
extern DSPTaskInfo task;
extern u16 ax_dram_image[8192];
AXPROFILE __AXLocalProfile;
DSPTaskInfo task;
u16 ax_dram_image[8192];
u16 axDspSlaveLength = AX_DSP_SLAVE_LENGTH;
u16 axDspSlave[AX_DSP_SLAVE_LENGTH];

void AXRegisterCallback(void (*callback)())
{
    s_user_cb = callback;
}

void AXInitProfile(AXPROFILE* profile, u32 maxProfiles)
{
    (void) profile;
    (void) maxProfiles;
}

u32 AXGetProfile(void)
{
    return 0;
}

// ------------------------------------------------------------ setters (stage into PB)
void AXSetVoiceSrcType(AXVPB* p, u32 type)
{
    (void) p;
    (void) type; // linear SRC is used for all types
}

void AXSetVoiceState(AXVPB* p, u16 state)
{
    int i;
    if (!p)
        return;
    p->pb.state = state;
    i = (int) (p - s_voices);
    if (i >= 0 && i < AX_MAX_VOICES)
        s_vs[i].active = (state != 0);
}

void AXSetVoiceType(AXVPB* p, u16 type)
{
    if (p)
        p->pb.type = type;
}

void AXSetVoiceMix(AXVPB* p, AXPBMIX* mix)
{
    if (p && mix)
        p->pb.mix = *mix;
}

void AXSetVoiceItdOn(AXVPB* p)
{
    (void) p;
}

void AXSetVoiceItdTarget(AXVPB* p, u16 lShift, u16 rShift)
{
    (void) p;
    (void) lShift;
    (void) rShift;
}

void AXSetVoiceUpdateIncrement(AXVPB* p)
{
    (void) p;
}

void AXSetVoiceUpdateWrite(AXVPB* p, u16 param, u16 data)
{
    (void) p;
    (void) param;
    (void) data;
}

void AXSetVoiceDpop(AXVPB* p, AXPBDPOP* dpop)
{
    if (p && dpop)
        p->pb.dpop = *dpop;
}

void AXSetVoiceVe(AXVPB* p, AXPBVE* ve)
{
    int i;
    if (!p || !ve)
        return;
    p->pb.ve = *ve;
    i = (int) (p - s_voices);
    if (i >= 0 && i < AX_MAX_VOICES)
        s_vs[i].vol = ve->currentVolume / 32767.f;
}

void AXSetVoiceVeDelta(AXVPB* p, s16 delta)
{
    if (p)
        p->pb.ve.currentDelta = delta;
}

void AXSetVoiceFir(AXVPB* p, AXPBFIR* fir)
{
    if (p && fir)
        p->pb.fir = *fir;
}

void AXSetVoiceAddr(AXVPB* p, AXPBADDR* addr)
{
    int i;
    if (!p || !addr)
        return;
    p->pb.addr = *addr;
    i = (int) (p - s_voices);
    if (i >= 0 && i < AX_MAX_VOICES) {
        // NOTE: entry bytes are big-endian; the u16 fields above were
        // copied raw, so reassemble the cursor from the source bytes.
        // AXPBADDR layout: loopFlag(2) format(2) loopHi(2) loopLo(2)
        // endHi(2) endLo(2) curHi(2) curLo(2) -> cur bytes at [12..16)
        const u8* b = (const u8*) addr;
        s_vs[i].fpos = (float) be32(b + 12);
        s_vs[i].yn1 = (s16) be16((const u8*) &p->pb.adpcm.yn1);
        s_vs[i].yn2 = (s16) be16((const u8*) &p->pb.adpcm.yn2);
        s_vs[i].ad_valid = 0;
    }
}

// The address setters below receive native u32 values but store big-endian
// bytes, keeping PB address fields uniformly BE (entry copies and setter
// stores alike) for the BE-reading mixer and the game's BE readers.

void AXSetVoiceLoop(AXVPB* p, u16 loop)
{
    u8* b;
    if (!p)
        return;
    b = (u8*) &p->pb.addr;
    pc_wb16(b + 0, loop);
}

void AXSetVoiceLoopAddr(AXVPB* p, u32 addr)
{
    u8* b;
    if (!p)
        return;
    b = (u8*) &p->pb.addr;
    pc_wb16(b + 4, (u16) (addr >> 16));
    pc_wb16(b + 6, (u16) addr);
}

void AXSetVoiceEndAddr(AXVPB* p, u32 addr)
{
    u8* b;
    if (!p)
        return;
    b = (u8*) &p->pb.addr;
    pc_wb16(b + 8, (u16) (addr >> 16));
    pc_wb16(b + 10, (u16) addr);
}

void AXSetVoiceCurrentAddr(AXVPB* p, u32 addr)
{
    int i;
    u8* b;
    if (!p)
        return;
    b = (u8*) &p->pb.addr;
    pc_wb16(b + 12, (u16) (addr >> 16));
    pc_wb16(b + 14, (u16) addr);
    i = (int) (p - s_voices);
    if (i >= 0 && i < AX_MAX_VOICES) {
        s_vs[i].fpos = (float) addr;
        s_vs[i].ad_valid = 0;
    }
}

void AXSetVoiceAdpcm(AXVPB* p, AXPBADPCM* adpcm)
{
    if (p && adpcm)
        p->pb.adpcm = *adpcm;
}

void AXSetVoiceSrc(AXVPB* p, AXPBSRC* src_)
{
    if (p && src_)
        p->pb.src = *src_;
}

void AXSetVoiceSrcRatio(AXVPB* p, float ratio)
{
    if (!p)
        return;
    // retail stores 16.16 fixed ratio across ratioHi/Lo; keep the float
    // in currentAddressFrac-adjacent staging? No — store natively in src
    // as fixed point like hardware:
    {
        u32 fixed = (u32) (ratio * 65536.f);
        p->pb.src.ratioHi = (u16) (fixed >> 16);
        p->pb.src.ratioLo = (u16) fixed;
    }
}

void AXSetVoiceAdpcmLoop(AXVPB* p, AXPBADPCMLOOP* adpcmloop)
{
    if (p && adpcmloop)
        p->pb.adpcmLoop = *adpcmloop;
}

void AXSetMaxDspCycles(u32 cycles)
{
    s_max_dsp_cycles = cycles;
}

u32 AXGetMaxDspCycles(void)
{
    return s_max_dsp_cycles;
}

u32 AXGetDspCycles(void)
{
    return 0;
}

// NOTE: AXFX hooks + effect implementations live in sdk2_axfx.c.
// ------------------------------------------------------------ mixer core
extern void* sdk2_ar_host(u32 aram_addr);
extern u32 sdk2_ar_size(void);

// Fetch one source sample as float (-1..1), advancing the voice cursor.
// Sets *ended when a one-shot voice reaches its end address.
static float voice_sample(int vi, int* ended)
{
    AXVPB* v = &s_voices[vi];
    VoiceState* st = &s_vs[vi];
    const u8* ab = (const u8*) &v->pb.addr;
    unsigned format = be16(ab + 2);
    unsigned loop_flag = be16(ab + 0);
    u32 loop_addr = be32(ab + 4);
    u32 end_addr = be32(ab + 8);
    u8* aram = (u8*) sdk2_ar_host(0);
    float ratio;
    float out = 0;
    *ended = 0;
    if (!aram)
        return 0;
    {
        u32 rf = ((u32) v->pb.src.ratioHi << 16) | v->pb.src.ratioLo;
        ratio = rf ? (rf / 65536.f) : 1.f;
        if (ratio <= 0)
            ratio = 1.f;
        if (ratio > 8)
            ratio = 8; // sanity clamp
    }
    if (format == 0x00) {
        // ADPCM via the per-voice frame cache (see VoiceState).
        const u8* coefs = (const u8*) &v->pb.adpcm.a[0][0];
        const u8* lb = (const u8*) &v->pb.adpcmLoop;
        int ip;
        float fr;
        s16 a, b;
        if (!st->ad_valid) {
            u32 fb = (u32) st->fpos & ~7u;
            float rel;
            if (fb >= end_addr) {
                if (loop_flag == 1 && end_addr > loop_addr) {
                    fb = loop_addr & ~7u;
                    st->yn1 = (s16) be16(lb + 2);
                    st->yn2 = (s16) be16(lb + 4);
                } else {
                    *ended = 1;
                    return 0;
                }
            }
            {
                int yn1 = st->yn1, yn2 = st->yn2;
                adpcm_frame(aram + fb, coefs, &yn1, &yn2, st->ad_buf);
                st->yn1 = (s16) yn1;
                st->yn2 = (s16) yn2;
            }
            st->ad_frame = fb;
            st->ad_prev = st->ad_buf[0];
            rel = st->fpos - (float) fb;
            st->ad_pos = (rel >= 1.f) ? (rel - 1.f) * 2.f : 0.f;
            if (st->ad_pos < 0)
                st->ad_pos = 0;
            if (st->ad_pos > 13)
                st->ad_pos = 13;
            st->ad_valid = 1;
        }
        ip = (int) st->ad_pos;
        fr = st->ad_pos - ip;
        a = (ip <= 0) ? st->ad_prev : st->ad_buf[ip - 1];
        b = st->ad_buf[ip < 14 ? ip : 13];
        out = (a + (b - a) * fr) / 32768.f;
        st->ad_pos += ratio;
        while (st->ad_pos >= 14.f) {
            u32 nf = st->ad_frame + 8;
            st->ad_pos -= 14.f;
            if (nf >= end_addr) {
                if (loop_flag == 1 && end_addr > loop_addr) {
                    nf = loop_addr & ~7u;
                    st->yn1 = (s16) be16(lb + 2);
                    st->yn2 = (s16) be16(lb + 4);
                    st->ad_prev = st->ad_buf[13];
                } else {
                    *ended = 1;
                    return out;
                }
            } else {
                st->ad_prev = st->ad_buf[13];
            }
            {
                int yn1 = st->yn1, yn2 = st->yn2;
                adpcm_frame(aram + nf, coefs, &yn1, &yn2, st->ad_buf);
                st->yn1 = (s16) yn1;
                st->yn2 = (s16) yn2;
            }
            st->ad_frame = nf;
        }
    } else if (format == 0x0A) {
        // 16-bit PCM, big-endian: 1 sample per 2 bytes.
        float pos = (st->fpos) / 2.f;
        int i0 = (int) pos;
        float fr = pos - i0;
        u32 b0 = (u32) (i0 * 2), b1 = b0 + 2;
        s16 s0 = 0, s1 = 0;
        if (b0 + 2 > end_addr) {
            if (loop_flag == 1 && end_addr > loop_addr) {
                b0 = loop_addr;
                b1 = b0 + 2;
                st->fpos = (float) b0;
            } else {
                *ended = 1;
                return 0;
            }
        }
        s0 = (s16) be16(aram + b0);
        s1 = (b1 + 2 <= end_addr) ? (s16) be16(aram + b1) : s0;
        out = (s0 + (s1 - s0) * fr) / 32768.f;
        st->fpos += ratio * 2.f;
    } else if (format == 0x19) {
        // 8-bit PCM: 1 sample per byte.
        float pos = st->fpos;
        int i0 = (int) pos;
        float fr = pos - i0;
        u32 b0 = (u32) i0;
        s8 s0 = 0, s1 = 0;
        if (b0 + 1 > end_addr) {
            if (loop_flag == 1 && end_addr > loop_addr) {
                b0 = loop_addr;
                st->fpos = (float) b0;
            } else {
                *ended = 1;
                return 0;
            }
        }
        s0 = (s8) aram[b0];
        s1 = (b0 + 1 < end_addr) ? (s8) aram[b0 + 1] : s0;
        out = (s0 + (s1 - s0) * fr) / 128.f;
        st->fpos += ratio;
    }
    {
        // Write back the play cursor (BE bytes) for the game's position
        // trackers (bank-range cutoff, stream master clock).
        u8* abw = (u8*) &v->pb.addr;
        u32 cb = (u32) st->fpos;
        pc_wb16(abw + 12, (u16) (cb >> 16));
        pc_wb16(abw + 14, (u16) cb);
    }
    return out;
}

// Render nframes stereo s16 @ 32 kHz (the hal_audio_mix_request entry).
// Processed in AX_FRAME (160-sample) chunks: voices render main + aux
// sends, registered aux callbacks process their buses via the AXFX
// effects (sdk2_axfx.c), and wet returns fold back into main stereo.
extern unsigned axfx_block_len; // owned by sdk2_axfx.c
void hal_audio_mix_request(s16* out, unsigned nframes)
{
    static float mL[AX_FRAME], mR[AX_FRAME];
    static float aAL[AX_FRAME], aAR[AX_FRAME];
    static float aBL[AX_FRAME], aBR[AX_FRAME];
    static long auxA[3][AX_FRAME], auxB[3][AX_FRAME];
    static unsigned since_frame_cb;
    unsigned done = 0;
    while (done < nframes) {
        unsigned chunk = nframes - done;
        unsigned f;
        int vi;
        if (chunk > AX_FRAME)
            chunk = AX_FRAME;
        for (f = 0; f < chunk; f++)
            mL[f] = mR[f] = aAL[f] = aAR[f] = aBL[f] = aBR[f] = 0;
        for (vi = 0; vi < AX_MAX_VOICES; vi++) {
            AXVPB* v;
            VoiceState* st;
            if (!s_voice_used[vi])
                continue;
            v = &s_voices[vi];
            st = &s_vs[vi];
            if (!st->active || v->pb.state == 0)
                continue;
            for (f = 0; f < chunk; f++) {
                float s, vl, vr, ve;
                int ended = 0;
                s = voice_sample(vi, &ended);
                if (ended) {
                    st->active = 0;
                    v->pb.state = 0;
                    if (v->callback)
                        v->callback(
                            (void*) (uintptr_t) v->userContext);
                    break;
                }
                // VE volume ramp (native u16 volume + s16 delta/sample)
                ve = v->pb.ve.currentVolume / 32767.f;
                ve += v->pb.ve.currentDelta / 32767.f;
                if (ve < 0)
                    ve = 0;
                if (ve > 1)
                    ve = 1;
                v->pb.ve.currentVolume = (u16) (ve * 32767.f);
                // mix volumes are native u16 (game-computed); aux send
                // deltas are not animated in v1 (static sends).
                vl = (v->pb.mix.vL / 32768.f) * ve;
                vr = (v->pb.mix.vR / 32768.f) * ve;
                mL[f] += s * vl;
                mR[f] += s * vr;
                aAL[f] += s * (v->pb.mix.vAuxAL / 32768.f) * ve;
                aAR[f] += s * (v->pb.mix.vAuxAR / 32768.f) * ve;
                aBL[f] += s * (v->pb.mix.vAuxBL / 32768.f) * ve;
                aBR[f] += s * (v->pb.mix.vAuxBR / 32768.f) * ve;
            }
        }
        // aux buses through registered effects (in place, s32 s16-range)
        axfx_block_len = chunk;
        if (s_aux_a_cb) {
            struct AXFX_BUFFERUPDATE upd;
            for (f = 0; f < chunk; f++) {
                auxA[0][f] = (long) (aAL[f] * 32767.f);
                auxA[1][f] = (long) (aAR[f] * 32767.f);
                auxA[2][f] =
                    (long) ((aAL[f] + aAR[f]) * 0.5f * 32767.f);
            }
            upd.left = auxA[0];
            upd.right = auxA[1];
            upd.surround = auxA[2];
            s_aux_a_cb(&upd, s_aux_a_ctx);
            for (f = 0; f < chunk; f++) {
                mL[f] += auxA[0][f] / 32767.f;
                mR[f] += auxA[1][f] / 32767.f;
            }
        }
        if (s_aux_b_cb) {
            struct AXFX_BUFFERUPDATE upd;
            for (f = 0; f < chunk; f++) {
                auxB[0][f] = (long) (aBL[f] * 32767.f);
                auxB[1][f] = (long) (aBR[f] * 32767.f);
                auxB[2][f] =
                    (long) ((aBL[f] + aBR[f]) * 0.5f * 32767.f);
            }
            upd.left = auxB[0];
            upd.right = auxB[1];
            upd.surround = auxB[2];
            s_aux_b_cb(&upd, s_aux_b_ctx);
            for (f = 0; f < chunk; f++) {
                mL[f] += auxB[0][f] / 32767.f;
                mR[f] += auxB[1][f] / 32767.f;
            }
        }
        for (f = 0; f < chunk; f++) {
            float L = mL[f], R = mR[f];
            if (L > 1)
                L = 1;
            if (L < -1)
                L = -1;
            if (R > 1)
                R = 1;
            if (R < -1)
                R = -1;
            out[(done + f) * 2 + 0] = (s16) (L * 32767.f);
            out[(done + f) * 2 + 1] = (s16) (R * 32767.f);
        }
        done += chunk;
        since_frame_cb += chunk;
        while (since_frame_cb >= AX_FRAME) {
            since_frame_cb -= AX_FRAME;
            if (s_user_cb)
                s_user_cb();
        }
    }
}
