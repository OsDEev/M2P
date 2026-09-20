// No-ImGui frontend: lifecycle stubs + settings instance.
// The console editor (gui_console.c) is still available via --configure.

#include "gui.h"

#include <string.h>

static MeleeSettings s_settings;
static int s_have_settings;
static char s_ini_path[1024];

MeleeSettings* gui_settings(void)
{
    if (!s_have_settings) {
        melee_settings_defaults(&s_settings);
        s_have_settings = 1;
    }
    return &s_settings;
}

void gui_set_ini_path(const char* path)
{
    if (path) {
        strncpy(s_ini_path, path, sizeof(s_ini_path) - 1);
        s_ini_path[sizeof(s_ini_path) - 1] = '\0';
    }
}

void gui_init(void* window)
{
    (void) window;
}

void gui_shutdown(void)
{
}

void gui_frame(void)
{
}

void gui_toggle(void)
{
}

int gui_is_open(void)
{
    return 0;
}
