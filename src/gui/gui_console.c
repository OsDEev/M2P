// Console settings editor (--configure). Always linked; the lifecycle
// (gui_init/frame/...) lives in gui_imgui.cpp (ImGui build) or
// gui_nogui.c (no-ImGui build).

#include "gui.h"

#include <stdio.h>
#include <string.h>

static int ask_int(const char* prompt, int cur, int lo, int hi)
{
    char buf[128];
    printf("%s [%d]: ", prompt, cur);
    fflush(stdout);
    if (!fgets(buf, sizeof(buf), stdin))
        return cur;
    if (buf[0] == '\n' || buf[0] == '\0')
        return cur;
    {
        int v = cur;
        if (sscanf(buf, "%d", &v) == 1) {
            if (v < lo)
                v = lo;
            if (v > hi)
                v = hi;
            return v;
        }
    }
    return cur;
}

static void ask_str(const char* prompt, char* dst, unsigned n)
{
    char buf[2048];
    printf("%s [%s]: ", prompt, dst);
    fflush(stdout);
    if (!fgets(buf, sizeof(buf), stdin))
        return;
    if (buf[0] == '\n' || buf[0] == '\0')
        return;
    buf[strcspn(buf, "\r\n")] = '\0';
    strncpy(dst, buf, n - 1);
    dst[n - 1] = '\0';
}

int gui_console_edit(const char* ini_path)
{
    MeleeSettings* s = gui_settings();
    char path[1024] = { 0 };
    int done = 0;
    if (ini_path) {
        strncpy(path, ini_path, sizeof(path) - 1);
        gui_set_ini_path(path);
    }
    printf("=== Melee PC port: settings (%s) ===\n",
           path[0] ? path : "<unsaved>");
    while (!done) {
        char buf[64];
        int choice = 0;
        printf("\n"
               " 1 video      (scale=%d vsync=%d fullscreen=%d)\n"
               " 2 audio      (enabled=%d volume=%d)\n"
               " 3 paths      (disc='%s')\n"
               " 4 system     (mem=%dMB lang=%d)\n"
               " 5 save\n"
               " 6 save & exit\n"
               " 0 exit without saving\n"
               "> ",
               s->win_scale, s->vsync, s->fullscreen, s->audio_enabled,
               s->volume, s->disc_root, s->mem_size_mb, s->language);
        fflush(stdout);
        if (!fgets(buf, sizeof(buf), stdin))
            break;
        if (sscanf(buf, "%d", &choice) != 1)
            continue;
        switch (choice) {
        case 1:
            s->win_scale = ask_int("window scale 1..4", s->win_scale, 1, 4);
            s->win_width = ask_int("width 0=auto", s->win_width, 0, 7680);
            s->win_height =
                ask_int("height 0=auto", s->win_height, 0, 4320);
            s->vsync = ask_int("vsync 0/1", s->vsync, 0, 1);
            s->fullscreen = ask_int("fullscreen 0/1", s->fullscreen, 0, 1);
            break;
        case 2:
            s->audio_enabled =
                ask_int("audio enabled 0/1", s->audio_enabled, 0, 1);
            s->volume = ask_int("volume 0..100", s->volume, 0, 100);
            break;
        case 3:
            ask_str("disc root", s->disc_root, sizeof(s->disc_root));
            ask_str("card A", s->card_a, sizeof(s->card_a));
            ask_str("card B", s->card_b, sizeof(s->card_b));
            ask_str("user dir", s->user_dir, sizeof(s->user_dir));
            break;
        case 4: {
            int m = ask_int("RAM MB (24/48)", s->mem_size_mb, 24, 48);
            s->mem_size_mb = (m == 48) ? 48 : 24;
            s->language = ask_int("language 0..5", s->language, 0, 5);
            break;
        }
        case 5:
            if (melee_settings_save(path[0] ? path : "melee_pc.ini", s))
                printf("saved.\n");
            else
                printf("SAVE FAILED.\n");
            break;
        case 6:
            if (melee_settings_save(path[0] ? path : "melee_pc.ini", s))
                printf("saved.\n");
            else
                printf("SAVE FAILED.\n");
            done = 1;
            break;
        default:
            done = 1;
            break;
        }
    }
    return 1;
}
