#ifndef _MELEE_HAL_INPUT_H_
#define _MELEE_HAL_INPUT_H_

// GameCube PAD emulation over GLFW joysticks + keyboard.
// Keyboard map is remappable through the settings module (defaults are
// documented in hal_input.c). Rumble state is tracked; physical rumble
// needs an SDL-backed build (documented limitation).

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Key binding ids (settings key: input.key.<name>)
typedef enum {
    HAL_KEY_STICK_UP,
    HAL_KEY_STICK_DOWN,
    HAL_KEY_STICK_LEFT,
    HAL_KEY_STICK_RIGHT,
    HAL_KEY_CSTICK_UP,
    HAL_KEY_CSTICK_DOWN,
    HAL_KEY_CSTICK_LEFT,
    HAL_KEY_CSTICK_RIGHT,
    HAL_KEY_A,
    HAL_KEY_B,
    HAL_KEY_X,
    HAL_KEY_Y,
    HAL_KEY_Z,
    HAL_KEY_L,
    HAL_KEY_R,
    HAL_KEY_START,
    HAL_KEY_DUP,
    HAL_KEY_DDOWN,
    HAL_KEY_DLEFT,
    HAL_KEY_DRIGHT,
    HAL_KEY_COUNT
} HalKeyId;

int hal_input_init(void);
void hal_input_shutdown(void);
void hal_input_poll(void); // call once per frame (after present)
int hal_input_key_glfw(int id); // current GLFW key for a binding id
void hal_input_set_key(int id, int glfw_key);
unsigned hal_input_motor(int chan); // last PADControlMotor command
void hal_input_set_present(int chan, int present); // controller plugged?

#ifdef __cplusplus
}
#endif

#endif
