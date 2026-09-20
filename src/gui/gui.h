#ifndef _MELEE_GUI_H_
#define _MELEE_GUI_H_

// Settings GUI. Two frontends share this interface:
//   - gui_imgui.cpp  (Dear ImGui overlay; needs MELEE_HAVE_IMGUI)
//   - gui_console.c  (--configure terminal editor; always available)
// Only ONE frontend is linked (CMake picks by MELEE_HAVE_IMGUI).

#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

// Live settings instance owned by the GUI module.
MeleeSettings* gui_settings(void);
// Where "Save" writes (set by pc_main).
void gui_set_ini_path(const char* path);

// ImGui overlay lifecycle (window = GLFWwindow*). No-ops in console mode.
void gui_init(void* window);
void gui_shutdown(void);
void gui_frame(void);   // called once per VI frame (GL context current)
void gui_toggle(void);  // F1 in-game
int gui_is_open(void);

// Console editor (blocking). Returns 1 if settings were saved.
int gui_console_edit(const char* ini_path);

#ifdef __cplusplus
}
#endif

#endif
