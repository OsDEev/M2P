#ifndef _MELEE_HAL_RUMBLE_H_
#define _MELEE_HAL_RUMBLE_H_

// Force-feedback output for PADControlMotor state.
//
// Backends (best effort, failures are silent):
//   - Windows: XInput (dynamically loaded, no link dependency),
//     channel N drives XInput user N.
//   - Linux: evdev FF_RUMBLE via /dev/input/event* (in enumeration
//     order; needs read/write permission, usually the `input` group).
//   - elsewhere: no-op.
//
// Called from hal_input_poll() once per frame; state changes only hit
// the OS when the motor command actually flips.

#ifdef __cplusplus
extern "C" {
#endif

void hal_rumble_init(void);
void hal_rumble_shutdown(void);
void hal_rumble_set_enabled(int on);
void hal_rumble_sync(int chan, unsigned motor_command, int present);

#ifdef __cplusplus
}
#endif

#endif
