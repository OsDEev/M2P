// GLFW window + VI emulation. The game drives frames through:
//   VIConfigure(rm) -> GXCopyDisp(xfb) + VISetNextFrameBuffer(xfb) ->
//   VIWaitForRetrace()/present. Our GXCopyDisp() already blits the EFB to
//   the default framebuffer, so present() only swaps buffers, runs retrace
//   callbacks and throttles to the VI rate (60 Hz NTSC / 50 Hz PAL).

#include "hal_video.h"
#include "gx_hal.h"
#include "gl_loader.h"

#include <dolphin/gx/GXStruct.h>
#include <dolphin/vi/vitypes.h>

#include <GLFW/glfw3.h>

#include <stdio.h>
#include <string.h>

void hal_input_attach_window(void* win);
void hal_audio_pump(void);

static void (*s_frame_hook)(void);

void hal_video_set_frame_hook(void (*fn)(void))
{
    s_frame_hook = fn;
}

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

static GLFWwindow* s_win;
static HAL_VideoConfig s_cfg;
static GXRenderModeObj* s_rm;
static void* s_next_fb;
static void* s_next_right_fb;
static int s_black = 1;
static int s_is_3d;
static unsigned s_retrace;
static unsigned s_field; // toggles for interlace
static VIRetraceCallback s_pre_cb;
static VIRetraceCallback s_post_cb;
static double s_last_present;
static int s_video_hz = 60;
static int s_have_preconfig;

void hal_video_preconfig(const HAL_VideoConfig* cfg)
{
    if (cfg) {
        s_cfg = *cfg;
        s_have_preconfig = 1;
    }
}
static u16 s_pan_x, s_pan_y, s_pan_w, s_pan_h;

static void sleep_ms(double ms)
{
    if (ms <= 0)
        return;
#ifdef _WIN32
    Sleep((DWORD) ms);
#else
    {
        struct timespec ts;
        ts.tv_sec = (time_t) (ms / 1000.0);
        ts.tv_nsec = (long) ((ms - ts.tv_sec * 1000.0) * 1000000.0);
        nanosleep(&ts, NULL);
    }
#endif
}

double hal_video_time(void)
{
    if (s_win)
        return glfwGetTime();
#ifdef _WIN32
    return GetTickCount64() / 1000.0;
#else
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec / 1e9;
    }
#endif
}

static void apply_hz_from_rm(void)
{
    if (!s_rm) {
        s_video_hz = 60;
        return;
    }
    switch (s_rm->viTVmode) {
    case VI_TVMODE_PAL_INT:
    case VI_TVMODE_PAL_DS:
    case VI_TVMODE_DEBUG_PAL_INT:
    case VI_TVMODE_DEBUG_PAL_DS:
    case VI_TVMODE_EURGB60_INT:
    case VI_TVMODE_EURGB60_DS:
        // EURGB60 is 60 Hz despite the PAL name
        if (s_rm->viTVmode == VI_TVMODE_PAL_INT ||
            s_rm->viTVmode == VI_TVMODE_PAL_DS ||
            s_rm->viTVmode == VI_TVMODE_DEBUG_PAL_INT ||
            s_rm->viTVmode == VI_TVMODE_DEBUG_PAL_DS)
            s_video_hz = 50;
        else
            s_video_hz = 60;
        break;
    default:
        s_video_hz = 60;
        break;
    }
}

int hal_video_init(const HAL_VideoConfig* cfg)
{
    int w, h;
    if (s_win)
        return 1;
    if (cfg) {
        s_cfg = *cfg;
        s_have_preconfig = 1;
    } else if (!s_have_preconfig) {
        memset(&s_cfg, 0, sizeof(s_cfg));
        s_cfg.scale = 2;
        s_cfg.vsync = 1;
    }
    if (!glfwInit()) {
        fprintf(stderr, "[hal] glfwInit failed\n");
        return 0;
    }
    w = s_cfg.width > 0 ? s_cfg.width : 640 * (s_cfg.scale > 0 ? s_cfg.scale : 1);
    h = s_cfg.height > 0 ? s_cfg.height : 480 * (s_cfg.scale > 0 ? s_cfg.scale : 1);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    s_win = glfwCreateWindow(w, h, "Super Smash Bros. Melee (PC port)",
                             s_cfg.fullscreen ? glfwGetPrimaryMonitor() : NULL,
                             NULL);
    if (!s_win) {
        fprintf(stderr, "[hal] glfwCreateWindow failed\n");
        glfwTerminate();
        return 0;
    }
    glfwMakeContextCurrent(s_win);
    hal_input_attach_window(s_win);
    if (s_cfg.vsync)
        glfwSwapInterval(1);
    else
        glfwSwapInterval(0);
    if (!gl_loader_init((void* (*)(const char*)) glfwGetProcAddress)) {
        fprintf(stderr, "[hal] GL loader failed: %s\n", gl_loader_error());
        glfwDestroyWindow(s_win);
        s_win = NULL;
        glfwTerminate();
        return 0;
    }
    {
        const char* ver = (const char*) glGetString(GL_VERSION);
        printf("[hal] OpenGL: %s\n", ver ? ver : "unknown");
    }
    // Default render mode until VIConfigure arrives.
    extern GXRenderModeObj GXNtsc480IntDf;
    s_rm = &GXNtsc480IntDf;
    apply_hz_from_rm();
    gx_hal_init(w, h, s_rm);
    s_last_present = hal_video_time();
    return 1;
}

void hal_video_shutdown(void)
{
    if (!s_win)
        return;
    gx_hal_shutdown();
    glfwDestroyWindow(s_win);
    s_win = NULL;
    glfwTerminate();
}

void hal_video_present(void)
{
    double now, frame, wait;
    if (!s_win)
        return;
    if (s_pre_cb)
        s_pre_cb(s_retrace);
    glfwSwapBuffers(s_win);
    glfwPollEvents();
    if (s_frame_hook)
        s_frame_hook();
    hal_audio_pump();
    s_retrace++;
    s_field ^= 1;
    if (s_post_cb)
        s_post_cb(s_retrace);
    // throttle to VI rate when vsync is off (vsync on already throttles)
    if (!s_cfg.vsync) {
        now = hal_video_time();
        frame = 1.0 / s_video_hz;
        wait = (s_last_present + frame - now) * 1000.0;
        if (wait > 0 && wait < 100)
            sleep_ms(wait);
        s_last_present = hal_video_time();
    } else {
        s_last_present = hal_video_time();
    }
}

int hal_video_should_close(void)
{
    return s_win ? glfwWindowShouldClose(s_win) : 1;
}

void hal_video_get_size(int* w, int* h)
{
    if (s_win)
        glfwGetFramebufferSize(s_win, w, h);
    else {
        if (w)
            *w = 0;
        if (h)
            *h = 0;
    }
}

void* hal_video_get_proc(const char* name)
{
    return (void*) glfwGetProcAddress(name);
}

unsigned hal_vi_retrace_count(void)
{
    return s_retrace;
}

void* hal_video_window_ptr(void)
{
    return s_win;
}

// ------------------------------------------------------------------ VI API
VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb)
{
    VIRetraceCallback prev = s_pre_cb;
    s_pre_cb = cb;
    return prev;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{
    VIRetraceCallback prev = s_post_cb;
    s_post_cb = cb;
    return prev;
}

void VIInit(void)
{
    HAL_VideoConfig cfg = s_cfg;
    if (cfg.scale <= 0)
        cfg.scale = 2;
    hal_video_init(&cfg);
    s_retrace = 0;
}

void VIWaitForRetrace(void)
{
    // Single-threaded port: the retrace wait IS the frame boundary.
    // Present, run callbacks, pump audio and throttle here.
    hal_video_present();
}

void VIConfigure(GXRenderModeObj* rm)
{
    int w, h;
    if (!rm)
        return;
    s_rm = rm;
    apply_hz_from_rm();
    if (!s_win)
        return;
    // Resize the EFB to the new render mode.
    w = rm->fbWidth;
    h = rm->efbHeight;
    gx_hal_set_efb_size(w, h);
    (void) w;
    (void) h;
}

void VIConfigurePan(u16 xOrg, u16 yOrg, u16 width, u16 height)
{
    s_pan_x = xOrg;
    s_pan_y = yOrg;
    s_pan_w = width;
    s_pan_h = height;
}

void VIFlush(void)
{
    glFlush();
}

void VISetNextFrameBuffer(void* fb)
{
    s_next_fb = fb;
}

void VISetNextRightFrameBuffer(void* fb)
{
    s_next_right_fb = fb;
}

void VISetBlack(BOOL black)
{
    s_black = black ? 1 : 0;
    if (black && s_win) {
        int w, h;
        glfwGetFramebufferSize(s_win, &w, &h);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glfwSwapBuffers(s_win);
    }
}

void VISet3D(BOOL threeD)
{
    s_is_3d = threeD ? 1 : 0;
}

u32 VIGetRetraceCount(void)
{
    return s_retrace;
}

u32 VIGetNextField(void)
{
    return s_field;
}

u32 VIGetCurrentLine(void)
{
    // approximate from time within the frame
    double now = hal_video_time();
    double phase = (now - s_last_present) * s_video_hz;
    if (phase < 0)
        phase = 0;
    if (phase > 1)
        phase = 1;
    return (u32) (phase * (s_rm ? s_rm->viHeight : 480));
}

u32 VIGetTvFormat(void)
{
    if (!s_rm)
        return VI_NTSC;
    switch (s_rm->viTVmode) {
    case VI_TVMODE_PAL_INT:
    case VI_TVMODE_PAL_DS:
    case VI_TVMODE_DEBUG_PAL_INT:
    case VI_TVMODE_DEBUG_PAL_DS:
        return VI_PAL;
    case VI_TVMODE_MPAL_INT:
    case VI_TVMODE_MPAL_DS:
        return VI_MPAL;
    case VI_TVMODE_EURGB60_INT:
    case VI_TVMODE_EURGB60_DS:
        return VI_EURGB60;
    default:
        return VI_NTSC;
    }
}

u32 VIGetDTVStatus(void)
{
    return 0;
}
