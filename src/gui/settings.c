// Minimal INI persistence for MeleeSettings. Format:
//
//   [video]
//   width=0
//   ...
//   [input.key]
//   stick_up=w
//   ...
//
// Unknown keys are ignored (forward compatibility). Missing file -> caller
// keeps defaults.

#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

// Key names must match HalKeyId order in hal_input.h, and the default
// codes below must match hal_input_init()'s map (settings.c owns no
// dependency on hal_input, so keep the two tables in sync by hand).
static const char* s_key_ids[MELEE_KEY_SLOTS] = {
    "stick_up", "stick_down", "stick_left", "stick_right", "c_up",
    "c_down",   "c_left",     "c_right",    "a",            "b",
    "x",        "y",          "z",          "l",            "r",
    "start",    "dup",        "ddown",      "dleft",        "dright",
};

// default GLFW key codes (hal_input defaults; duplicated here so settings
// works without linking hal_input)
enum {
    K_W = 87,
    K_A = 65,
    K_S = 83,
    K_D = 68,
    K_T = 84,
    K_F = 70,
    K_G = 71,
    K_H = 72,
    K_I = 73,
    K_J = 74,
    K_K = 75,
    K_L = 76,
    K_O = 79,
    K_P = 80,
    K_U = 85,
    K_UP = 265,
    K_DOWN = 264,
    K_LEFT = 263,
    K_RIGHT = 262,
    K_ENTER = 257,
};

void melee_settings_defaults(MeleeSettings* s)
{
    static const int defkeys[MELEE_KEY_SLOTS] = {
        K_W, K_S, K_A, K_D, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_J, K_K,
        K_L, K_I, K_P, K_U, K_O,  K_ENTER, K_T,   K_G,      K_F, K_H,
    };
    int i;
    memset(s, 0, sizeof(*s));
    s->win_width = 0;
    s->win_height = 0;
    s->win_scale = 2;
    s->fullscreen = 0;
    s->vsync = 1;
    s->anisotropy = 4;
    s->show_fps = 0;
    s->audio_enabled = 1;
    s->volume = 80;
    for (i = 0; i < MELEE_KEY_SLOTS; i++)
        s->keys[i] = defkeys[i];
    for (i = 0; i < 4; i++)
        s->pad_present[i] = 1;
    s->mem_size_mb = 24;
    s->language = 0;
    s->gui_startup = 0;
}

void melee_settings_path_default(char* out, unsigned n)
{
#ifdef _WIN32
    char appdata[1024] = { 0 };
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0,
                                   appdata))) {
        snprintf(out, n, "%s\\melee-pc\\melee_pc.ini", appdata);
        return;
    }
    snprintf(out, n, "melee_pc.ini");
#else
    const char* home = getenv("HOME");
    if (home && home[0]) {
        snprintf(out, n, "%s/.melee-pc/melee_pc.ini", home);
        return;
    }
    snprintf(out, n, "melee_pc.ini");
#endif
}

static void trim(char* s)
{
    size_t n;
    while (*s == ' ' || *s == '\t')
        memmove(s, s + 1, strlen(s));
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[n - 1] = '\0';
        n--;
    }
}

static int parse_int(const char* v, int lo, int hi, int fallback)
{
    char* end = NULL;
    long x = strtol(v, &end, 10);
    if (end == v)
        return fallback;
    if (x < lo)
        x = lo;
    if (x > hi)
        x = hi;
    return (int) x;
}

// key name <-> code (subset sufficient for defaults + common remaps)
static int key_from_name(const char* name)
{
    static const struct {
        const char* n;
        int k;
    } tab[] = {
        { "space", 32 },   { "enter", K_ENTER }, { "escape", 256 },
        { "up", K_UP },    { "down", K_DOWN },   { "left", K_LEFT },
        { "right", K_RIGHT }, { "tab", 258 },    { "lshift", 340 },
        { "rshift", 344 },
    };
    unsigned i;
    char single[2] = { 0, 0 };
    if (!name || !name[0])
        return -1;
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (strcmp(name, tab[i].n) == 0)
            return tab[i].k;
    }
    if (name[0] >= 'a' && name[0] <= 'z' && name[1] == '\0') {
        single[0] = name[0];
        return single[0] - 32; // GLFW: 'A'..'Z' = 65..90
    }
    if (name[0] >= '0' && name[0] <= '9' && name[1] == '\0')
        return name[0]; // GLFW: '0'..'9' = 48..57
    if (name[0] == 'f' && name[1] >= '1' && name[1] <= '9' &&
        name[2] == '\0') {
        return 289 + (name[1] - '1'); // F1..F9 = 290..
    }
    if (strcmp(name, "f10") == 0)
        return 299;
    if (strcmp(name, "f11") == 0)
        return 300;
    if (strcmp(name, "f12") == 0)
        return 301;
    return -1;
}

static const char* key_to_name(int k, char* buf, size_t n)
{
    static const struct {
        const char* n;
        int k;
    } tab[] = {
        { "space", 32 },   { "enter", K_ENTER }, { "escape", 256 },
        { "up", K_UP },    { "down", K_DOWN },   { "left", K_LEFT },
        { "right", K_RIGHT }, { "tab", 258 },    { "lshift", 340 },
        { "rshift", 344 },
    };
    unsigned i;
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (tab[i].k == k)
            return tab[i].n;
    }
    if (k >= 'A' && k <= 'Z') {
        buf[0] = (char) (k + 32);
        buf[1] = '\0';
        return buf;
    }
    if (k >= '0' && k <= '9') {
        buf[0] = (char) k;
        buf[1] = '\0';
        return buf;
    }
    if (k >= 290 && k <= 301) {
        snprintf(buf, n, "f%d", k - 289);
        return buf;
    }
    snprintf(buf, n, "%d", k);
    return buf;
}

int melee_settings_load(const char* path, MeleeSettings* s)
{
    FILE* f = fopen(path, "r");
    char line[2048];
    char section[64] = "";
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        char* eq;
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';')
            continue;
        if (line[0] == '[') {
            char* e = strchr(line, ']');
            if (e) {
                size_t n = (size_t) (e - line - 1);
                if (n >= sizeof(section))
                    n = sizeof(section) - 1;
                memcpy(section, line + 1, n);
                section[n] = '\0';
            }
            continue;
        }
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        {
            char* k = line;
            char* v = eq + 1;
            int i;
            trim(k);
            trim(v);
            if (strcmp(section, "video") == 0) {
                if (strcmp(k, "width") == 0)
                    s->win_width = parse_int(v, 0, 7680, 0);
                else if (strcmp(k, "height") == 0)
                    s->win_height = parse_int(v, 0, 4320, 0);
                else if (strcmp(k, "scale") == 0)
                    s->win_scale = parse_int(v, 1, 4, 2);
                else if (strcmp(k, "fullscreen") == 0)
                    s->fullscreen = parse_int(v, 0, 1, 0);
                else if (strcmp(k, "vsync") == 0)
                    s->vsync = parse_int(v, 0, 1, 1);
            } else if (strcmp(section, "render") == 0) {
                if (strcmp(k, "anisotropy") == 0)
                    s->anisotropy = parse_int(v, 1, 16, 4);
                else if (strcmp(k, "show_fps") == 0)
                    s->show_fps = parse_int(v, 0, 1, 0);
            } else if (strcmp(section, "audio") == 0) {
                if (strcmp(k, "enabled") == 0)
                    s->audio_enabled = parse_int(v, 0, 1, 1);
                else if (strcmp(k, "volume") == 0)
                    s->volume = parse_int(v, 0, 100, 80);
            } else if (strcmp(section, "input") == 0) {
                if (strncmp(k, "key.", 4) == 0) {
                    for (i = 0; i < MELEE_KEY_SLOTS; i++) {
                        if (strcmp(k + 4, s_key_ids[i]) == 0) {
                            int code = key_from_name(v);
                            if (code >= 0)
                                s->keys[i] = code;
                            break;
                        }
                    }
                } else if (strncmp(k, "pad", 3) == 0 && k[3] >= '0' &&
                           k[3] <= '3' && k[4] == '\0') {
                    s->pad_present[k[3] - '0'] = parse_int(v, 0, 1, 1);
                }
            } else if (strcmp(section, "paths") == 0) {
                if (strcmp(k, "disc") == 0) {
                    strncpy(s->disc_root, v, sizeof(s->disc_root) - 1);
                } else if (strcmp(k, "card_a") == 0) {
                    strncpy(s->card_a, v, sizeof(s->card_a) - 1);
                } else if (strcmp(k, "card_b") == 0) {
                    strncpy(s->card_b, v, sizeof(s->card_b) - 1);
                } else if (strcmp(k, "user") == 0) {
                    strncpy(s->user_dir, v, sizeof(s->user_dir) - 1);
                }
            } else if (strcmp(section, "system") == 0) {
                if (strcmp(k, "mem_mb") == 0) {
                    int m = parse_int(v, 24, 48, 24);
                    s->mem_size_mb = (m == 48) ? 48 : 24;
                } else if (strcmp(k, "language") == 0) {
                    s->language = parse_int(v, 0, 5, 0);
                } else if (strcmp(k, "gui_startup") == 0) {
                    s->gui_startup = parse_int(v, 0, 1, 0);
                }
            }
        }
    }
    fclose(f);
    return 1;
}

int melee_settings_save(const char* path, const MeleeSettings* s)
{
    FILE* f = fopen(path, "w");
    int i;
    char kb[32];
    if (!f)
        return 0;
    fprintf(f, "# Melee PC port settings (INI)\n");
    fprintf(f, "[video]\nwidth=%d\nheight=%d\nscale=%d\nfullscreen=%d\n"
               "vsync=%d\n",
            s->win_width, s->win_height, s->win_scale, s->fullscreen,
            s->vsync);
    fprintf(f, "[render]\nanisotropy=%d\nshow_fps=%d\n", s->anisotropy,
            s->show_fps);
    fprintf(f, "[audio]\nenabled=%d\nvolume=%d\n", s->audio_enabled,
            s->volume);
    fprintf(f, "[input]\n");
    for (i = 0; i < MELEE_KEY_SLOTS; i++) {
        fprintf(f, "key.%s=%s\n", s_key_ids[i],
                key_to_name(s->keys[i], kb, sizeof(kb)));
    }
    for (i = 0; i < 4; i++)
        fprintf(f, "pad%d=%d\n", i, s->pad_present[i]);
    fprintf(f, "[paths]\ndisc=%s\ncard_a=%s\ncard_b=%s\nuser=%s\n",
            s->disc_root, s->card_a, s->card_b, s->user_dir);
    fprintf(f, "[system]\nmem_mb=%d\nlanguage=%d\ngui_startup=%d\n",
            s->mem_size_mb, s->language, s->gui_startup);
    fclose(f);
    return 1;
}
