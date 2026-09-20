// AI emulation + output backend.
//
// Timing model: the stream sample counter advances with the wall clock
// while "playing". hal_audio_pump() (called once per VI frame) asks the
// AX mixer for the samples due since the last pump and hands them to the
// backend. On Windows the backend is WinMM waveOut; elsewhere samples are
// counted but discarded (null sink) — the game still runs at full speed
// with correct AI/DMA timing.

#include "hal_audio.h"

#include <dolphin/ai.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

#define AI_RATE 32000
#define AI_CHANNELS 2
#define AI_FRAME_SAMPLES 512 // ~16 ms at 32 kHz
#define AI_NUM_BUFS 4

static HAL_AudioConfig s_cfg;
static int s_ready;

// DMA state
static u32 s_dma_addr;
static u32 s_dma_len;
static u32 s_dma_left;
static int s_dma_enabled;
static AIDCallback s_dma_cb;

// stream state
static AISCallback s_stream_cb;
static u32 s_stream_count;
static u32 s_stream_trigger;
static u32 s_stream_state = AI_STREAM_STOP;
static u32 s_stream_rate = AI_SAMPLERATE_32KHZ;
static u32 s_dsp_rate = AI_SAMPLERATE_32KHZ;
static u8 s_vol_l = 255, s_vol_r = 255;

// playback clock
static double s_clock_base; // hal time at which s_clock_samples was true
static unsigned s_clock_samples;

// output buffer (submitted, not yet consumed)
#define RING_FRAMES (AI_RATE * 2) // 2 seconds
static s16 s_ring[RING_FRAMES * AI_CHANNELS];
static unsigned s_ring_w, s_ring_r;

static double now_sec(void);

#ifdef _WIN32
static HWAVEOUT s_wo;
static WAVEHDR s_hdrs[AI_NUM_BUFS];
static s16 s_wobuf[AI_NUM_BUFS][AI_FRAME_SAMPLES * AI_CHANNELS];
static int s_wo_next;

static void CALLBACK wave_cb(HWAVEOUT wo, UINT msg, DWORD_PTR inst,
                             DWORD_PTR p1, DWORD_PTR p2)
{
    (void) wo;
    (void) inst;
    (void) p2;
    if (msg == WOM_DONE) {
        WAVEHDR* h = (WAVEHDR*) p1;
        h->dwFlags &= ~WHDR_DONE;
        // buffer returned to the free pool (polled in pump)
    }
}
#endif

static double now_sec(void)
{
    extern double hal_video_time(void);
    return hal_video_time();
}

int hal_audio_init(const HAL_AudioConfig* cfg)
{
    if (s_ready)
        return 1;
    if (cfg)
        s_cfg = *cfg;
    else {
        memset(&s_cfg, 0, sizeof(s_cfg));
        s_cfg.sample_rate = AI_RATE;
        s_cfg.volume = 80;
        s_cfg.enabled = 1;
    }
    s_clock_base = now_sec();
    s_clock_samples = 0;
    s_ring_w = s_ring_r = 0;
#ifdef _WIN32
    s_wo = NULL;
    if (s_cfg.enabled) {
        WAVEFORMATEX fmt;
        MMRESULT r;
        int i;
        memset(&fmt, 0, sizeof(fmt));
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = AI_CHANNELS;
        fmt.nSamplesPerSec = AI_RATE;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = AI_CHANNELS * 2;
        fmt.nAvgBytesPerSec = AI_RATE * AI_CHANNELS * 2;
        fmt.cbSize = 0;
        r = waveOutOpen(&s_wo, WAVE_MAPPER, &fmt, (DWORD_PTR) wave_cb, 0,
                        CALLBACK_FUNCTION);
        if (r != MMSYSERR_NOERROR) {
            fprintf(stderr, "[hal] waveOutOpen failed (%u), null sink\n",
                    (unsigned) r);
            s_wo = NULL;
        } else {
            for (i = 0; i < AI_NUM_BUFS; i++) {
                memset(&s_hdrs[i], 0, sizeof(WAVEHDR));
                s_hdrs[i].lpData = (LPSTR) s_wobuf[i];
                s_hdrs[i].dwBufferLength =
                    (DWORD) sizeof(s_wobuf[i]);
                waveOutPrepareHeader(s_wo, &s_hdrs[i],
                                     (UINT) sizeof(WAVEHDR));
            }
            s_wo_next = 0;
            waveOutSetVolume(s_wo, 0xFFFFFFFF);
        }
    }
#endif
    s_ready = 1;
    printf("[hal] audio: %s @ %d Hz\n",
           s_cfg.enabled ? "enabled" : "null sink", AI_RATE);
    return 1;
}

void hal_audio_shutdown(void)
{
    if (!s_ready)
        return;
#ifdef _WIN32
    if (s_wo) {
        int i;
        waveOutReset(s_wo);
        for (i = 0; i < AI_NUM_BUFS; i++)
            waveOutUnprepareHeader(s_wo, &s_hdrs[i],
                                   (UINT) sizeof(WAVEHDR));
        waveOutClose(s_wo);
        s_wo = NULL;
    }
#endif
    s_ready = 0;
}

void hal_audio_submit(const s16* frames, unsigned nframes)
{
    unsigned i;
    for (i = 0; i < nframes; i++) {
        unsigned next = (s_ring_w + 1) % RING_FRAMES;
        if (next == s_ring_r)
            break; // overrun: drop (mixer is ahead of playback)
        s_ring[s_ring_w * AI_CHANNELS + 0] = frames[i * 2 + 0];
        s_ring[s_ring_w * AI_CHANNELS + 1] = frames[i * 2 + 1];
        s_ring_w = next;
    }
}

u32 hal_audio_stream_count(void)
{
    return s_stream_count;
}

// Mixer entry, implemented by sdk2_ax.c (software AX voices). Declared in
// hal_audio.h; no default here to avoid duplicate symbols.
extern void hal_audio_mix_request(s16* out, unsigned nframes);

static unsigned ring_available(void)
{
    return (s_ring_w + RING_FRAMES - s_ring_r) % RING_FRAMES;
}

static void ring_consume(s16* out, unsigned nframes)
{
    unsigned i;
    for (i = 0; i < nframes; i++) {
        if (s_ring_r == s_ring_w) {
            out[i * 2 + 0] = 0;
            out[i * 2 + 1] = 0; // underrun: silence
        } else {
            // apply volume 0..100
            int l = s_ring[s_ring_r * AI_CHANNELS + 0];
            int r = s_ring[s_ring_r * AI_CHANNELS + 1];
            l = l * s_cfg.volume / 100;
            r = r * s_cfg.volume / 100;
            out[i * 2 + 0] = (s16) l;
            out[i * 2 + 1] = (s16) r;
            s_ring_r = (s_ring_r + 1) % RING_FRAMES;
        }
    }
}

// Called once per VI frame from hal_video_present().
void hal_audio_pump(void)
{
    double now;
    unsigned due, total;
    static s16 mixbuf[AI_FRAME_SAMPLES * 4 * AI_CHANNELS];
    if (!s_ready)
        return;
    now = now_sec();
    total = (unsigned) ((now - s_clock_base) * AI_RATE);
    due = total - s_clock_samples;
    if (due == 0)
        return;
    if (due > AI_FRAME_SAMPLES * 4)
        due = AI_FRAME_SAMPLES * 4; // clamp after hitches
    if (s_stream_state == AI_STREAM_START)
        s_stream_count += due;
    // virtual DMA consumption
    if (s_dma_enabled && s_dma_left > 0) {
        unsigned bytes = due * AI_CHANNELS * 2;
        if (bytes >= s_dma_left) {
            s_dma_left = 0;
            s_dma_enabled = 0;
            if (s_dma_cb)
                s_dma_cb();
        } else {
            s_dma_left -= bytes;
        }
    }
    // render fresh samples through the AX mixer, then play/count them
    hal_audio_mix_request(mixbuf, due);
    hal_audio_submit(mixbuf, due);
#ifdef _WIN32
    if (s_wo) {
        // feed every free waveOut buffer from the ring
        int i;
        for (i = 0; i < AI_NUM_BUFS; i++) {
            WAVEHDR* h = &s_hdrs[s_wo_next];
            s_wo_next = (s_wo_next + 1) % AI_NUM_BUFS;
            if (h->dwFlags & WHDR_PREPARED) {
                if (!(h->dwFlags & WHDR_DONE) && (h->dwFlags & WHDR_INQUEUE))
                    continue;
                {
                    unsigned avail = ring_available();
                    unsigned want =
                        AI_FRAME_SAMPLES < avail ? AI_FRAME_SAMPLES
                                                 : avail;
                    if (want == 0)
                        continue;
                    ring_consume(s_wobuf[s_wo_next == 0 ? AI_NUM_BUFS - 1
                                                       : s_wo_next - 1],
                                 want);
                    h->dwBufferLength =
                        (DWORD) (want * AI_CHANNELS * sizeof(s16));
                    waveOutWrite(s_wo, h, (UINT) sizeof(WAVEHDR));
                }
            }
        }
    } else
#endif
    {
        // null sink: drop everything, keep timing
        s_ring_r = s_ring_w;
    }
    s_clock_samples = total;
}

// ------------------------------------------------------------------ AI API
AIDCallback AIRegisterDMACallback(AIDCallback callback)
{
    AIDCallback prev = s_dma_cb;
    s_dma_cb = callback;
    return prev;
}

void AIInitDMA(u32 start_addr, u32 length)
{
    s_dma_addr = start_addr;
    s_dma_len = length;
    s_dma_left = length;
}

BOOL AIGetDMAEnableFlag(void)
{
    return s_dma_enabled ? TRUE : FALSE;
}

void AIStartDMA(void)
{
    s_dma_enabled = 1;
    s_dma_left = s_dma_len;
}

void AIStopDMA(void)
{
    s_dma_enabled = 0;
}

u32 AIGetDMABytesLeft(void)
{
    return s_dma_left;
}

u32 AIGetDMAStartAddr(void)
{
    return s_dma_addr;
}

u32 AIGetDMALength(void)
{
    return s_dma_len;
}

static int s_ai_init;

BOOL AICheckInit(void)
{
    return s_ai_init ? TRUE : FALSE;
}

AISCallback AIRegisterStreamCallback(AISCallback callback)
{
    AISCallback prev = s_stream_cb;
    s_stream_cb = callback;
    return prev;
}

u32 AIGetStreamSampleCount(void)
{
    return s_stream_count;
}

void AIResetStreamSampleCount(void)
{
    s_stream_count = 0;
}

void AISetStreamTrigger(u32 trigger)
{
    s_stream_trigger = trigger;
}

u32 AIGetStreamTrigger(void)
{
    return s_stream_trigger;
}

void AISetStreamPlayState(u32 state)
{
    s_stream_state = state;
    if (state == AI_STREAM_START)
        s_clock_base = now_sec() - s_clock_samples / (double) AI_RATE;
}

u32 AIGetStreamPlayState(void)
{
    return s_stream_state;
}

void AISetDSPSampleRate(u32 rate)
{
    s_dsp_rate = rate;
}

u32 AIGetDSPSampleRate(void)
{
    return s_dsp_rate;
}

void AISetStreamSampleRate(u32 rate)
{
    s_stream_rate = rate;
}

u32 AIGetStreamSampleRate(void)
{
    return s_stream_rate;
}

void AISetStreamVolLeft(u8 vol)
{
    s_vol_l = vol;
}

u8 AIGetStreamVolLeft(void)
{
    return s_vol_l;
}

void AISetStreamVolRight(u8 vol)
{
    s_vol_r = vol;
}

u8 AIGetStreamVolRight(void)
{
    return s_vol_r;
}

void AIInit(u8* stack)
{
    (void) stack;
    s_ai_init = 1;
    s_stream_state = AI_STREAM_START;
}

void AIReset(void)
{
    s_dma_enabled = 0;
    s_dma_left = 0;
}
