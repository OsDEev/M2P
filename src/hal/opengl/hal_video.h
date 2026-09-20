#ifndef _MELEE_HAL_VIDEO_H_
#define _MELEE_HAL_VIDEO_H_

// Window + Video Interface (VI) emulation.
// The GLFW window is created lazily on the first VIInit() so the game boot
// sequence (OSInit -> VIInit -> ...) works unchanged.

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width;      // window width (0 = from render mode)
    int height;     // window height
    int fullscreen; // 0 windowed, 1 fullscreen
    int vsync;      // 0 off, 1 on
    int scale;      // window scale vs 640x480 (used when width==0)
} HAL_VideoConfig;

// Stored and applied by the next init (used before the lazy VIInit).
void hal_video_preconfig(const HAL_VideoConfig* cfg);
int hal_video_init(const HAL_VideoConfig* cfg);
void hal_video_shutdown(void);
void hal_video_present(void); // swap + poll + retrace tick + throttle
int hal_video_should_close(void);
void hal_video_get_size(int* w, int* h);
void* hal_video_get_proc(const char* name);
double hal_video_time(void); // seconds, monotonic
unsigned hal_vi_retrace_count(void);
void* hal_video_window_ptr(void); // GLFWwindow* (NULL before first init)
// Per-frame hook (used by the settings GUI overlay). Called inside
// hal_video_present() after buffer swap, with the GL context current.
void hal_video_set_frame_hook(void (*fn)(void));

#ifdef __cplusplus
}
#endif

#endif
