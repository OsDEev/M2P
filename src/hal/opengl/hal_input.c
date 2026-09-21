// PAD emulation: up to 4 GLFW joysticks + 1 keyboard-as-controller.
// Default keyboard map (remappable via settings `input.key.*`):
//   left stick WASD | c-stick arrows | A J | B K | X L | Y I |
//   L U | R O | Z P | Start Enter | d-pad TFGH.
// Triggers are digital on keyboard (0/255); analog on real gamepads.

#include "hal_input.h"
#include "hal_rumble.h"

#include <dolphin/pad.h>

#include <GLFW/glfw3.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static GLFWwindow* s_win;
static int s_present[PAD_MAX_CONTROLLERS];
static unsigned s_motor[PAD_MAX_CONTROLLERS];
static unsigned long s_mask = 0xFFFFFFFFUL;
static int s_keys[HAL_KEY_COUNT];
static int s_keys_configured; // set by hal_input_set_key (pc_main settings)

static const struct {
    const char* name;
    int key;
} s_key_names[] = {
    { "space", GLFW_KEY_SPACE },         { "enter", GLFW_KEY_ENTER },
    { "escape", GLFW_KEY_ESCAPE },       { "up", GLFW_KEY_UP },
    { "down", GLFW_KEY_DOWN },           { "left", GLFW_KEY_LEFT },
    { "right", GLFW_KEY_RIGHT },         { "lshift", GLFW_KEY_LEFT_SHIFT },
    { "rshift", GLFW_KEY_RIGHT_SHIFT },  { "tab", GLFW_KEY_TAB },
    { "a", GLFW_KEY_A },                 { "b", GLFW_KEY_B },
    { "c", GLFW_KEY_C },                 { "d", GLFW_KEY_D },
    { "e", GLFW_KEY_E },                 { "f", GLFW_KEY_F },
    { "g", GLFW_KEY_G },                 { "h", GLFW_KEY_H },
    { "i", GLFW_KEY_I },                 { "j", GLFW_KEY_J },
    { "k", GLFW_KEY_K },                 { "l", GLFW_KEY_L },
    { "m", GLFW_KEY_M },                 { "n", GLFW_KEY_N },
    { "o", GLFW_KEY_O },                 { "p", GLFW_KEY_P },
    { "q", GLFW_KEY_Q },                 { "r", GLFW_KEY_R },
    { "s", GLFW_KEY_S },                 { "t", GLFW_KEY_T },
    { "u", GLFW_KEY_U },                 { "v", GLFW_KEY_V },
    { "w", GLFW_KEY_W },                 { "x", GLFW_KEY_X },
    { "y", GLFW_KEY_Y },                 { "z", GLFW_KEY_Z },
    { "0", GLFW_KEY_0 },                 { "1", GLFW_KEY_1 },
    { "2", GLFW_KEY_2 },                 { "3", GLFW_KEY_3 },
    { "4", GLFW_KEY_4 },                 { "5", GLFW_KEY_5 },
    { "6", GLFW_KEY_6 },                 { "7", GLFW_KEY_7 },
    { "8", GLFW_KEY_8 },                 { "9", GLFW_KEY_9 },
    { "f1", GLFW_KEY_F1 },               { "f2", GLFW_KEY_F2 },
    { "f3", GLFW_KEY_F3 },               { "f4", GLFW_KEY_F4 },
    { "f5", GLFW_KEY_F5 },               { "f6", GLFW_KEY_F6 },
    { "f7", GLFW_KEY_F7 },               { "f8", GLFW_KEY_F8 },
    { "f9", GLFW_KEY_F9 },               { "f10", GLFW_KEY_F10 },
    { "f11", GLFW_KEY_F11 },             { "f12", GLFW_KEY_F12 },
};

static const char* s_key_ids[HAL_KEY_COUNT] = {
    "stick_up", "stick_down", "stick_left", "stick_right",
    "c_up",     "c_down",     "c_left",     "c_right",
    "a",        "b",          "x",          "y",
    "z",        "l",          "r",          "start",
    "dup",      "ddown",      "dleft",      "dright",
};

int hal_input_key_from_name(const char* name)
{
    unsigned i;
    if (!name)
        return GLFW_KEY_UNKNOWN;
    for (i = 0; i < sizeof(s_key_names) / sizeof(s_key_names[0]); i++) {
        if (strcmp(name, s_key_names[i].name) == 0)
            return s_key_names[i].key;
    }
    return GLFW_KEY_UNKNOWN;
}

const char* hal_input_key_to_name(int key)
{
    unsigned i;
    for (i = 0; i < sizeof(s_key_names) / sizeof(s_key_names[0]); i++) {
        if (key == s_key_names[i].key)
            return s_key_names[i].name;
    }
    return "?";
}

const char* hal_input_key_setting(int id)
{
    if (id < 0 || id >= HAL_KEY_COUNT)
        return "";
    return s_key_ids[id];
}

int hal_input_init(void)
{
    hal_rumble_init();
    if (s_keys_configured)
        return 1; // keymap already pushed from settings by pc_main
    // default map
    s_keys[HAL_KEY_STICK_UP] = GLFW_KEY_W;
    s_keys[HAL_KEY_STICK_DOWN] = GLFW_KEY_S;
    s_keys[HAL_KEY_STICK_LEFT] = GLFW_KEY_A;
    s_keys[HAL_KEY_STICK_RIGHT] = GLFW_KEY_D;
    s_keys[HAL_KEY_CSTICK_UP] = GLFW_KEY_UP;
    s_keys[HAL_KEY_CSTICK_DOWN] = GLFW_KEY_DOWN;
    s_keys[HAL_KEY_CSTICK_LEFT] = GLFW_KEY_LEFT;
    s_keys[HAL_KEY_CSTICK_RIGHT] = GLFW_KEY_RIGHT;
    s_keys[HAL_KEY_A] = GLFW_KEY_J;
    s_keys[HAL_KEY_B] = GLFW_KEY_K;
    s_keys[HAL_KEY_X] = GLFW_KEY_L;
    s_keys[HAL_KEY_Y] = GLFW_KEY_I;
    s_keys[HAL_KEY_Z] = GLFW_KEY_P;
    s_keys[HAL_KEY_L] = GLFW_KEY_U;
    s_keys[HAL_KEY_R] = GLFW_KEY_O;
    s_keys[HAL_KEY_START] = GLFW_KEY_ENTER;
    s_keys[HAL_KEY_DUP] = GLFW_KEY_T;
    s_keys[HAL_KEY_DDOWN] = GLFW_KEY_G;
    s_keys[HAL_KEY_DLEFT] = GLFW_KEY_F;
    s_keys[HAL_KEY_DRIGHT] = GLFW_KEY_H;
    s_present[0] = 1; // keyboard always drives chan 0 unless overridden
    s_present[1] = 1;
    s_present[2] = 1;
    s_present[3] = 1;
    return 1;
}

void hal_input_shutdown(void)
{
    hal_rumble_shutdown();
    s_win = NULL;
}

void hal_input_attach_window(void* win)
{
    s_win = (GLFWwindow*) win;
}

void hal_input_poll(void)
{
    // Pads are sampled on demand in PADRead(); here we only push
    // force-feedback state (cheap: backends send on change only).
    int i;
    for (i = 0; i < PAD_MAX_CONTROLLERS; i++)
        hal_rumble_sync(i, s_motor[i], s_present[i]);
}

int hal_input_key_glfw(int id)
{
    if (id < 0 || id >= HAL_KEY_COUNT)
        return GLFW_KEY_UNKNOWN;
    return s_keys[id];
}

void hal_input_set_key(int id, int glfw_key)
{
    if (id >= 0 && id < HAL_KEY_COUNT) {
        s_keys[id] = glfw_key;
        s_keys_configured = 1;
    }
}

unsigned hal_input_motor(int chan)
{
    if (chan < 0 || chan >= PAD_MAX_CONTROLLERS)
        return PAD_MOTOR_STOP;
    return s_motor[chan];
}

void hal_input_set_present(int chan, int present)
{
    if (chan >= 0 && chan < PAD_MAX_CONTROLLERS)
        s_present[chan] = present ? 1 : 0;
}

void hal_input_set_rumble_enabled(int on)
{
    hal_rumble_set_enabled(on);
}

static int key_down(int glfw_key)
{
    if (!s_win || glfw_key == GLFW_KEY_UNKNOWN)
        return 0;
    return glfwGetKey(s_win, glfw_key) == GLFW_PRESS;
}

static float joy_axis(int joy, int axis)
{
    int count = 0;
    const float* axes = glfwGetJoystickAxes(joy, &count);
    if (!axes || axis < 0 || axis >= count)
        return 0;
    {
        float v = axes[axis];
        if (fabsf(v) < 0.12f) // deadzone
            return 0;
        return v;
    }
}

static unsigned char joy_button(int joy, int btn)
{
    int count = 0;
    const unsigned char* b = glfwGetJoystickButtons(joy, &count);
    if (!b || btn < 0 || btn >= count)
        return 0;
    return b[btn];
}

static float joy_trigger(int joy, int axis)
{
    int count = 0;
    const float* axes = glfwGetJoystickAxes(joy, &count);
    if (!axes || axis < 0 || axis >= count)
        return 0;
    {
        float v = (axes[axis] + 1.f) * 0.5f; // -1..1 -> 0..1
        return v < 0.05f ? 0 : v;
    }
}

static void read_keyboard(PADStatus* st)
{
    int sx = 0, sy = 0, cx = 0, cy = 0;
    if (key_down(s_keys[HAL_KEY_STICK_LEFT]))
        sx -= 127;
    if (key_down(s_keys[HAL_KEY_STICK_RIGHT]))
        sx += 127;
    if (key_down(s_keys[HAL_KEY_STICK_UP]))
        sy += 127;
    if (key_down(s_keys[HAL_KEY_STICK_DOWN]))
        sy -= 127;
    if (key_down(s_keys[HAL_KEY_CSTICK_LEFT]))
        cx -= 127;
    if (key_down(s_keys[HAL_KEY_CSTICK_RIGHT]))
        cx += 127;
    if (key_down(s_keys[HAL_KEY_CSTICK_UP]))
        cy += 127;
    if (key_down(s_keys[HAL_KEY_CSTICK_DOWN]))
        cy -= 127;
    st->stickX = (s8) sx;
    st->stickY = (s8) sy;
    st->substickX = (s8) cx;
    st->substickY = (s8) cy;
    if (key_down(s_keys[HAL_KEY_A]))
        st->button |= PAD_BUTTON_A;
    if (key_down(s_keys[HAL_KEY_B]))
        st->button |= PAD_BUTTON_B;
    if (key_down(s_keys[HAL_KEY_X]))
        st->button |= PAD_BUTTON_X;
    if (key_down(s_keys[HAL_KEY_Y]))
        st->button |= PAD_BUTTON_Y;
    if (key_down(s_keys[HAL_KEY_Z]))
        st->button |= PAD_TRIGGER_Z;
    if (key_down(s_keys[HAL_KEY_START]))
        st->button |= PAD_BUTTON_START;
    if (key_down(s_keys[HAL_KEY_L])) {
        st->button |= PAD_TRIGGER_L;
        st->triggerLeft = 255;
    }
    if (key_down(s_keys[HAL_KEY_R])) {
        st->button |= PAD_TRIGGER_R;
        st->triggerRight = 255;
    }
    if (key_down(s_keys[HAL_KEY_DUP]))
        st->button |= PAD_BUTTON_UP;
    if (key_down(s_keys[HAL_KEY_DDOWN]))
        st->button |= PAD_BUTTON_DOWN;
    if (key_down(s_keys[HAL_KEY_DLEFT]))
        st->button |= PAD_BUTTON_LEFT;
    if (key_down(s_keys[HAL_KEY_DRIGHT]))
        st->button |= PAD_BUTTON_RIGHT;
    st->analogA = (st->button & PAD_BUTTON_A) ? 255 : 0;
    st->analogB = (st->button & PAD_BUTTON_B) ? 255 : 0;
}

static void read_joystick(int joy, PADStatus* st)
{
    float ax = joy_axis(joy, 0), ay = joy_axis(joy, 1);
    float cx = joy_axis(joy, 2), cy = joy_axis(joy, 3);
    float tl = joy_trigger(joy, 4), tr = joy_trigger(joy, 5);
    st->stickX = (s8) (ax * 127.f);
    st->stickY = (s8) (-ay * 127.f);
    st->substickX = (s8) (cx * 127.f);
    st->substickY = (s8) (-cy * 127.f);
    // standard mapping: 0 A, 1 B, 2 X, 3 Y, 4 L, 5 R, 6 Z, 7 Start
    if (joy_button(joy, 0))
        st->button |= PAD_BUTTON_A;
    if (joy_button(joy, 1))
        st->button |= PAD_BUTTON_B;
    if (joy_button(joy, 2))
        st->button |= PAD_BUTTON_X;
    if (joy_button(joy, 3))
        st->button |= PAD_BUTTON_Y;
    if (joy_button(joy, 4) || tl > 0) {
        st->button |= PAD_TRIGGER_L;
        st->triggerLeft = (u8) (tl * 255.f);
    }
    if (joy_button(joy, 5) || tr > 0) {
        st->button |= PAD_TRIGGER_R;
        st->triggerRight = (u8) (tr * 255.f);
    }
    if (joy_button(joy, 6))
        st->button |= PAD_TRIGGER_Z;
    if (joy_button(joy, 7))
        st->button |= PAD_BUTTON_START;
    // d-pad on hat/buttons 12..15 (common layouts)
    if (joy_button(joy, 12))
        st->button |= PAD_BUTTON_UP;
    if (joy_button(joy, 13))
        st->button |= PAD_BUTTON_DOWN;
    if (joy_button(joy, 14))
        st->button |= PAD_BUTTON_LEFT;
    if (joy_button(joy, 15))
        st->button |= PAD_BUTTON_RIGHT;
    st->analogA = (st->button & PAD_BUTTON_A) ? 255 : 0;
    st->analogB = (st->button & PAD_BUTTON_B) ? 255 : 0;
}

// ------------------------------------------------------------------ PAD API
int PADReset(unsigned long mask)
{
    s_mask = mask;
    return 1;
}

BOOL PADRecalibrate(u32 mask)
{
    (void) mask;
    return TRUE;
}

BOOL PADInit(void)
{
    hal_input_init();
    return TRUE;
}

u32 PADRead(PADStatus* status)
{
    int i;
    u32 mask = 0;
    int joy_ids[4] = { GLFW_JOYSTICK_1, GLFW_JOYSTICK_2, GLFW_JOYSTICK_3,
                       GLFW_JOYSTICK_4 };
    for (i = 0; i < PAD_MAX_CONTROLLERS; i++) {
        PADStatus* st = &status[i];
        memset(st, 0, sizeof(*st));
        st->err = PAD_ERR_NO_CONTROLLER;
        if (!s_present[i])
            continue;
        if (i == 0 && s_win) {
            // channel 0: keyboard merged with joystick 1
            read_keyboard(st);
            if (glfwJoystickPresent(joy_ids[0])) {
                PADStatus js;
                memset(&js, 0, sizeof(js));
                read_joystick(joy_ids[0], &js);
                if (js.button || js.stickX || js.stickY || js.substickX ||
                    js.substickY || js.triggerLeft || js.triggerRight) {
                    st->button |= js.button;
                    if (js.stickX)
                        st->stickX = js.stickX;
                    if (js.stickY)
                        st->stickY = js.stickY;
                    if (js.substickX)
                        st->substickX = js.substickX;
                    if (js.substickY)
                        st->substickY = js.substickY;
                    if (js.triggerLeft)
                        st->triggerLeft = js.triggerLeft;
                    if (js.triggerRight)
                        st->triggerRight = js.triggerRight;
                }
            }
            st->err = PAD_ERR_NONE;
            mask |= (u32) PAD_CHAN0_BIT >> i;
        } else if (glfwJoystickPresent(joy_ids[i])) {
            read_joystick(joy_ids[i], st);
            st->err = PAD_ERR_NONE;
            mask |= (u32) PAD_CHAN0_BIT >> i;
        }
    }
    return mask;
}

void PADSetSamplingRate(unsigned long msec)
{
    (void) msec;
}

void __PADTestSamplingRate(unsigned long tvmode)
{
    (void) tvmode;
}

void PADControlAllMotors(const u32* commandArray)
{
    int i;
    if (!commandArray)
        return;
    for (i = 0; i < PAD_MAX_CONTROLLERS; i++)
        s_motor[i] = commandArray[i];
}

void PADControlMotor(s32 chan, u32 command)
{
    if (chan >= 0 && chan < PAD_MAX_CONTROLLERS)
        s_motor[chan] = command;
}

void PADSetSpec(u32 spec)
{
    (void) spec;
}

unsigned long PADGetSpec(void)
{
    return 0;
}

int PADGetType(long chan, unsigned long* type)
{
    if (chan < 0 || chan >= PAD_MAX_CONTROLLERS)
        return PAD_ERR_NO_CONTROLLER;
    if (type)
        *type = 0; // standard controller
    return s_present[chan] ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER;
}

BOOL PADSync(void)
{
    return TRUE;
}

void PADSetAnalogMode(u32 mode)
{
    (void) mode;
}

BOOL __PADDisableRecalibration(int arg0)
{
    (void) arg0;
    return TRUE;
}

void SIRefreshSamplingRate(void)
{
}

void PADClamp(PADStatus* status)
{
    int i;
    for (i = 0; i < PAD_MAX_CONTROLLERS; i++) {
        PADStatus* st = &status[i];
        if (st->stickX > 127)
            st->stickX = 127;
        if (st->stickX < -128)
            st->stickX = -128;
        if (st->stickY > 127)
            st->stickY = 127;
        if (st->stickY < -128)
            st->stickY = -128;
        if (st->substickX > 127)
            st->substickX = 127;
        if (st->substickX < -128)
            st->substickX = -128;
        if (st->substickY > 127)
            st->substickY = 127;
        if (st->substickY < -128)
            st->substickY = -128;
    }
}
