#include "hal_rumble.h"

#include <dolphin/pad.h>

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef __linux__
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#define RUMBLE_CHANS 4

static int s_enabled = 1;
static int s_last[RUMBLE_CHANS];

// Forward declarations: backends live below, callers above (MSVC
// requires a declaration before use even for file-local functions).
static void rumble_backend_set(int chan, int on);
#ifdef _WIN32
void rumble_win_init(void);
void rumble_win_shutdown(void);
#endif
#ifdef __linux__
void rumble_evdev_shutdown(void);
#endif

void hal_rumble_init(void)
{
    int i;
    for (i = 0; i < RUMBLE_CHANS; i++)
        s_last[i] = -1;
#ifdef _WIN32
    rumble_win_init();
#endif
}

void hal_rumble_shutdown(void)
{
    int i;
    for (i = 0; i < RUMBLE_CHANS; i++) {
        if (s_last[i] == 1)
            hal_rumble_sync(i, PAD_MOTOR_STOP, 1);
        s_last[i] = -1;
    }
#ifdef _WIN32
    rumble_win_shutdown();
#endif
#ifdef __linux__
    rumble_evdev_shutdown();
#endif
}

void hal_rumble_set_enabled(int on)
{
    int i;
    s_enabled = on ? 1 : 0;
    if (!s_enabled) {
        // cut all motors immediately
        for (i = 0; i < RUMBLE_CHANS; i++) {
            if (s_last[i] == 1) {
                rumble_backend_set(i, 0);
                s_last[i] = 0;
            }
        }
    }
}

void hal_rumble_sync(int chan, unsigned motor_command, int present)
{
    int want;
    if (chan < 0 || chan >= RUMBLE_CHANS)
        return;
    want = (s_enabled && present && motor_command == PAD_MOTOR_RUMBLE)
               ? 1
               : 0;
    if (want == s_last[chan])
        return;
    s_last[chan] = want;
    rumble_backend_set(chan, want);
}

// ------------------------------------------------------------ Windows/XInput
#ifdef _WIN32

typedef struct {
    unsigned short wButtons;
    unsigned char bLeftTrigger;
    unsigned char bRightTrigger;
    short sThumbLX;
    short sThumbLY;
    short sThumbRX;
    short sThumbRY;
} WIN_XINPUT_STATE;

typedef struct {
    unsigned short wLeftMotorSpeed;
    unsigned short wRightMotorSpeed;
} WIN_XINPUT_VIBRATION;

typedef unsigned long(__stdcall* XInputSetStateFn)(unsigned long,
                                                   WIN_XINPUT_VIBRATION*);

static HMODULE s_xinput;
static XInputSetStateFn s_XInputSetState;

void rumble_win_init(void)
{
    static const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll",
                                  "xinput9_1_0.dll" };
    unsigned i;
    if (s_XInputSetState)
        return;
    for (i = 0; i < 3; i++) {
        s_xinput = LoadLibraryA(dlls[i]);
        if (!s_xinput)
            continue;
        s_XInputSetState = (XInputSetStateFn) GetProcAddress(
            s_xinput, "XInputSetState");
        if (s_XInputSetState)
            return;
        FreeLibrary(s_xinput);
        s_xinput = NULL;
    }
}

void rumble_win_shutdown(void)
{
    if (s_xinput) {
        FreeLibrary(s_xinput);
        s_xinput = NULL;
        s_XInputSetState = NULL;
    }
}

static void rumble_backend_set(int chan, int on)
{
    if (s_XInputSetState && chan >= 0 && chan < 4) {
        WIN_XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed = on ? 65535 : 0;
        vib.wRightMotorSpeed = on ? 65535 : 0;
        s_XInputSetState((unsigned long) chan, &vib);
    }
}

#else // ------------------------------------------------------ non-Windows

#ifndef __linux__
static void rumble_backend_set(int chan, int on)
{
    (void) chan;
    (void) on;
}
#endif

#endif // _WIN32

// ------------------------------------------------------------ Linux/evdev
#ifdef __linux__

#define EVDEV_MAX 32

typedef struct {
    int fd;
    int ffid;
    int bound; // 1 once effect uploaded
} EvdevSlot;

// Static init to fd=-1: slot fds must never alias stdin (fd 0).
static EvdevSlot s_evdev[RUMBLE_CHANS] = {
    { -1, -1, 0 }, { -1, -1, 0 }, { -1, -1, 0 }, { -1, -1, 0 },
};

static void evdev_slot_close(EvdevSlot* s)
{
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    s->ffid = -1;
    s->bound = 0;
}

static int evdev_has_rumble(int fd)
{
    unsigned long ffbits[4] = { 0, 0, 0, 0 };
    // EVIOCGBIT(EV_FF, len): which force effects are supported
    if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffbits)), ffbits) < 0)
        return 0;
    return (ffbits[0] & (1u << FF_RUMBLE)) != 0;
}

static int evdev_try_bind(EvdevSlot* s)
{
    // Bind event devices in enumeration order (documented mapping).
    static int next_dev = 0;
    char path[64];
    for (; next_dev < EVDEV_MAX; next_dev++) {
        int fd;
        snprintf(path, sizeof(path), "/dev/input/event%d", next_dev);
        fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0)
            continue;
        next_dev++;
        if (!evdev_has_rumble(fd)) {
            close(fd);
            continue;
        }
        s->fd = fd;
        s->ffid = -1;
        s->bound = 0;
        return 1;
    }
    return 0;
}

static int evdev_upload(EvdevSlot* s)
{
    struct ff_effect e;
    memset(&e, 0, sizeof(e));
    e.type = FF_RUMBLE;
    e.id = -1;
    e.u.rumble.strong_magnitude = 0xFFFF;
    e.u.rumble.weak_magnitude = 0xFFFF;
    e.replay.length = 0; // play until explicitly stopped
    e.replay.delay = 0;
    if (ioctl(s->fd, EVIOCSFF, &e) < 0)
        return 0;
    s->ffid = e.id;
    s->bound = 1;
    return 1;
}

static void rumble_backend_set(int chan, int on)
{
    EvdevSlot* s;
    struct input_event ev;
    if (chan < 0 || chan >= RUMBLE_CHANS)
        return;
    s = &s_evdev[chan];
    if (s->fd < 0) {
        if (!evdev_try_bind(s))
            return; // no (more) rumble devices; silent
    }
    if (!s->bound && !evdev_upload(s))
        return;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_FF;
    ev.code = (unsigned short) s->ffid;
    ev.value = on ? 1 : 0;
    (void) write(s->fd, &ev, sizeof(ev));
}

void rumble_evdev_shutdown(void)
{
    int i;
    for (i = 0; i < RUMBLE_CHANS; i++)
        evdev_slot_close(&s_evdev[i]);
}

#endif // __linux__
