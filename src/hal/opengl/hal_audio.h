#ifndef _MELEE_HAL_AUDIO_H_
#define _MELEE_HAL_AUDIO_H_

// Audio Interface (AI) emulation + streamed output backend.
//
// Data path: AX mixer (sdk2_ax.c) renders 32 kHz stereo s16 frames into the
// ring buffer via hal_audio_submit(); the backend thread/process drains it:
//   - Windows: WinMM waveOut
//   - other platforms: null sink (timing only) unless MELEE_ALSA/PULSE
//     backends are enabled at build time (see hal_audio.c).
// AIDCallback (DMA) and AISCallback (stream) hooks fire from the drain path
// so game timing (AIGetDMABytesLeft etc.) stays consistent.

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate; // 32000 or 48000 (game uses 32000)
    int volume;      // 0..100
    int enabled;     // 0 = null sink
} HAL_AudioConfig;

int hal_audio_init(const HAL_AudioConfig* cfg);
void hal_audio_shutdown(void);

// Push interleaved stereo s16 frames (32 kHz). Thread-safe.
void hal_audio_submit(const s16* frames, unsigned nframes);

// Consume timing: how many stream samples have been "played".
u32 hal_audio_stream_count(void);

// AX mixer hook: sdk2_ax.c renders into this when the backend needs data.
// Default implementation mixes silence; sdk2_ax overrides it.
void hal_audio_mix_request(s16* out, unsigned nframes);

#ifdef __cplusplus
}
#endif

#endif
