#ifndef _MELEE_SETTINGS_H_
#define _MELEE_SETTINGS_H_

// Port settings: plain struct + tiny INI persistence (no dependencies).
// Layers above (hal_*, sdk2_*, gui) read from here; nothing reads back.

#ifdef __cplusplus
extern "C" {
#endif

#define MELEE_KEY_SLOTS 20

typedef struct {
    // video
    int win_width;   // 0 = auto from scale
    int win_height;  // 0 = auto from scale
    int win_scale;   // 1..4 (auto size = 640*scale x 480*scale)
    int fullscreen;  // 0/1
    int vsync;       // 0/1
    // render
    int anisotropy;  // 1..16 (reserved for the sampler upgrade)
    int show_fps;    // 0/1 overlay (ImGui build only)
    // audio
    int audio_enabled; // 0/1
    int volume;        // 0..100
    // input: GLFW key codes per HalKeyId order (see hal_input.h)
    int keys[MELEE_KEY_SLOTS];
    int pad_present[4];
    // paths
    char disc_root[1024];
    char card_a[1024];
    char card_b[1024];
    char user_dir[1024];
    // system
    int mem_size_mb; // 24 or 48
    int language;    // 0 English, 1 German, 2 French, 3 Spanish, 4 Italian,
                     // 5 Dutch
    int gui_startup; // 0/1 show settings window on boot
} MeleeSettings;

void melee_settings_defaults(MeleeSettings* s);
int melee_settings_load(const char* path, MeleeSettings* s); // 1 ok
int melee_settings_save(const char* path, const MeleeSettings* s); // 1 ok
void melee_settings_path_default(char* out, unsigned n); // user_dir ini

#ifdef __cplusplus
}
#endif

#endif
