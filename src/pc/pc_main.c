// Melee PC port entry point.
//
// Boot order:
//   1. CLI + melee_pc.ini settings
//   2. sdk2 configuration (disc root, cards, user dir, RAM size)
//   3. HAL preconfig (window, audio, keymap)
//   4. window + settings GUI (unless --no-gui)
//   5. melee_main() — the game's main() from gm/gmmain.c, renamed via
//      -Dmain=melee_main for the PC build
//
// The game drives frames through VI; hal_video presents, pumps audio and
// runs the GUI overlay hook every retrace.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui/gui.h"
#include "hal/hal.h"
#include "sdk2/sdk2.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

// game entry (gm/gmmain.c:main, renamed by -Dmain=melee_main)
int melee_main(void);

static void print_help(const char* prog)
{
    printf("Usage: %s [options]\n"
           "\n"
           "  --disc PATH      disc root directory (GALE01 files)\n"
           "  --user PATH      user directory (saves, ini)\n"
           "  --card-a PATH    memory card A image\n"
           "  --card-b PATH    memory card B image\n"
           "  --width N        window width (0 = auto)\n"
           "  --height N       window height (0 = auto)\n"
           "  --fullscreen     start fullscreen\n"
           "  --no-gui         disable the settings overlay\n"
           "  --configure      edit settings in the terminal and continue\n"
           "  --save-settings  write melee_pc.ini and exit\n"
           "  --help           this text\n"
           "\n"
           "In-game: F1 toggles the settings overlay (ImGui build).\n",
           prog);
}

static void make_dir(const char* path)
{
    if (!path || !path[0])
        return;
#ifdef _WIN32
    CreateDirectoryA(path, NULL);
#else
    mkdir(path, 0755);
#endif
}

static void dir_of(const char* file, char* out, unsigned n)
{
    const char* s1 = strrchr(file, '/');
    const char* s2 = strrchr(file, '\\');
    const char* s = (s1 > s2) ? s1 : s2;
    if (!s) {
        out[0] = '\0';
        return;
    }
    {
        size_t len = (size_t) (s - file);
        if (len >= n)
            len = n - 1;
        memcpy(out, file, len);
        out[len] = '\0';
    }
}

int main(int argc, char** argv)
{
    MeleeSettings* s;
    char ini_path[1024] = { 0 };
    char user_dir[1024] = { 0 };
    int do_configure = 0;
    int do_save_exit = 0;
    int no_gui = 0;
    int i;

    melee_settings_path_default(ini_path, sizeof(ini_path));
    s = gui_settings();
    melee_settings_load(ini_path, s); // missing file -> defaults stay

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
            strncpy(s->disc_root, argv[++i], sizeof(s->disc_root) - 1);
        } else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            strncpy(s->user_dir, argv[++i], sizeof(s->user_dir) - 1);
        } else if (strcmp(argv[i], "--card-a") == 0 && i + 1 < argc) {
            strncpy(s->card_a, argv[++i], sizeof(s->card_a) - 1);
        } else if (strcmp(argv[i], "--card-b") == 0 && i + 1 < argc) {
            strncpy(s->card_b, argv[++i], sizeof(s->card_b) - 1);
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            s->win_width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            s->win_height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            s->fullscreen = 1;
        } else if (strcmp(argv[i], "--no-gui") == 0) {
            no_gui = 1;
        } else if (strcmp(argv[i], "--configure") == 0) {
            do_configure = 1;
        } else if (strcmp(argv[i], "--save-settings") == 0) {
            do_save_exit = 1;
        } else {
            printf("unknown option: %s (see --help)\n", argv[i]);
            return 1;
        }
    }

    // user dir defaults next to the ini when unset
    if (!s->user_dir[0]) {
        dir_of(ini_path, user_dir, sizeof(user_dir));
        strncpy(s->user_dir, user_dir[0] ? user_dir : ".",
                sizeof(s->user_dir) - 1);
    }
    make_dir(s->user_dir);

    if (do_configure) {
        gui_console_edit(ini_path);
        melee_settings_load(ini_path, s); // re-read what was saved
    }
    if (do_save_exit) {
        dir_of(ini_path, user_dir, sizeof(user_dir));
        make_dir(user_dir);
        if (!melee_settings_save(ini_path, s)) {
            printf("failed to save %s\n", ini_path);
            return 1;
        }
        printf("saved %s\n", ini_path);
        return 0;
    }

    // ---- sdk2 configuration ----
    sdk2_set_disc_root(s->disc_root[0] ? s->disc_root : ".");
    sdk2_set_card_path(0, s->card_a[0] ? s->card_a : "melee_card_A.mci");
    sdk2_set_card_path(1, s->card_b[0] ? s->card_b : "melee_card_B.mci");
    sdk2_set_user_dir(s->user_dir);
    sdk2_set_mem_size_mb((unsigned) s->mem_size_mb);

    // ---- HAL preconfig ----
    {
        HAL_VideoConfig vc;
        HAL_AudioConfig ac;
        int k;
        memset(&vc, 0, sizeof(vc));
        vc.width = s->win_width;
        vc.height = s->win_height;
        vc.scale = (s->win_scale >= 1 && s->win_scale <= 4)
                       ? s->win_scale
                       : 2;
        vc.fullscreen = s->fullscreen;
        vc.vsync = s->vsync;
        hal_video_preconfig(&vc);

        memset(&ac, 0, sizeof(ac));
        ac.sample_rate = 32000;
        ac.volume = (s->volume >= 0 && s->volume <= 100) ? s->volume
                                                        : 80;
        ac.enabled = s->audio_enabled;
        if (!hal_audio_init(&ac)) {
            printf("audio init failed, continuing silent\n");
        }

        for (k = 0; k < MELEE_KEY_SLOTS; k++)
            hal_input_set_key(k, s->keys[k]);
        for (k = 0; k < 4; k++)
            hal_input_set_present(k, s->pad_present[k]);
        hal_input_set_rumble_enabled(s->rumble);
    }

    // ---- window + GUI ----
    {
        // create the window now so the GUI has a GL context; VIInit()
        // later reuses it (preconfig from settings is already stored).
        if (!hal_video_init(NULL)) {
            printf("video init failed\n");
            return 1;
        }
        gui_set_ini_path(ini_path);
        if (!no_gui) {
            gui_init(hal_video_window_ptr());
            hal_video_set_frame_hook(gui_frame);
        }
    }

    printf("[pc] disc root : %s\n", sdk2_disc_root());
    printf("[pc] user dir  : %s\n", sdk2_user_dir());
    printf("[pc] booting Melee...\n");
    fflush(stdout);

    melee_main(); // does not return (game loop)

    gui_shutdown();
    hal_audio_shutdown();
    hal_video_shutdown();
    return 0;
}
