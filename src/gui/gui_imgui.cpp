// Dear ImGui settings overlay for the Melee PC port.
//
// Wiring: pc_main creates the window, calls gui_init(window), then each VI
// frame hal_video calls gui_frame() with the GL context current.
// Toggle with F1 (or --no-gui to disable entirely).
//
// Built only when MELEE_HAVE_IMGUI is defined; CMake fetches ImGui +
// backends automatically. Requires C++17.

#include "gui.h"

#ifdef MELEE_HAVE_IMGUI

#include "hal/opengl/hal_input.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstring>

static MeleeSettings s_settings;
static int s_have_settings = 0;
static GLFWwindow* s_win = nullptr;
static bool s_open = false;
static char s_ini_path[1024] = { 0 };
static int s_capture_key = -1; // binding slot being remapped, or -1

static const char* kKeyNames[MELEE_KEY_SLOTS] = {
    "Stick Up",   "Stick Down", "Stick Left", "Stick Right", "C-Stick Up",
    "C-Stick Dn", "C-Stick L",  "C-Stick R",  "A",           "B",
    "X",          "Y",          "Z",          "L",           "R",
    "Start",      "D-Up",       "D-Down",     "D-Left",      "D-Right",
};

static const char* GlfwKeyName(int k)
{
    const char* n = glfwGetKeyName(k, 0);
    static char buf[32];
    if (n && n[0]) {
        snprintf(buf, sizeof(buf), "%s", n);
        return buf;
    }
    switch (k) {
    case GLFW_KEY_UP:
        return "Up";
    case GLFW_KEY_DOWN:
        return "Down";
    case GLFW_KEY_LEFT:
        return "Left";
    case GLFW_KEY_RIGHT:
        return "Right";
    case GLFW_KEY_ENTER:
        return "Enter";
    case GLFW_KEY_ESCAPE:
        return "Esc";
    case GLFW_KEY_SPACE:
        return "Space";
    case GLFW_KEY_TAB:
        return "Tab";
    default:
        break;
    }
    snprintf(buf, sizeof(buf), "key %d", k);
    return buf;
}

MeleeSettings* gui_settings(void)
{
    if (!s_have_settings) {
        melee_settings_defaults(&s_settings);
        s_have_settings = 1;
    }
    return &s_settings;
}

void gui_init(void* window)
{
    s_win = (GLFWwindow*) window;
    if (!s_win)
        return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(s_win, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    if (gui_settings()->gui_startup)
        s_open = true;
}

void gui_shutdown(void)
{
#ifdef MELEE_HAVE_IMGUI
    if (!s_win)
        return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    s_win = nullptr;
#endif
}

void gui_toggle(void)
{
    s_open = !s_open;
}

int gui_is_open(void)
{
    return s_open ? 1 : 0;
}

void gui_frame(void)
{
    static bool f1_was_down = false;
    bool f1_down;
    if (!s_win)
        return;
    f1_down = glfwGetKey(s_win, GLFW_KEY_F1) == GLFW_PRESS;
    if (f1_down && !f1_was_down)
        gui_toggle();
    f1_was_down = f1_down;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    if (s_settings.show_fps) {
        ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_FirstUseEver);
        ImGui::Begin("FPS", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoBackground);
        ImGui::Text("%.1f fps", ImGui::GetIO().Framerate);
        ImGui::End();
    }

    if (s_open) {
        MeleeSettings* s = gui_settings();
        ImGui::SetNextWindowSize(ImVec2(560, 480), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Melee PC — Settings (F1 to hide)", &s_open)) {
            if (ImGui::BeginTabBar("tabs")) {
                if (ImGui::BeginTabItem("Video")) {
                    ImGui::SliderInt("Window scale", &s->win_scale, 1, 4);
                    ImGui::InputInt("Width (0=auto)", &s->win_width);
                    ImGui::InputInt("Height (0=auto)", &s->win_height);
                    bool fs = s->fullscreen != 0;
                    bool vs = s->vsync != 0;
                    if (ImGui::Checkbox("Fullscreen (restart)", &fs))
                        s->fullscreen = fs ? 1 : 0;
                    if (ImGui::Checkbox("VSync", &vs))
                        s->vsync = vs ? 1 : 0;
                    ImGui::TextWrapped("Resolution changes apply on "
                                       "restart.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Render")) {
                    ImGui::SliderInt("Anisotropy", &s->anisotropy, 1, 16);
                    bool fps = s->show_fps != 0;
                    if (ImGui::Checkbox("Show FPS", &fps))
                        s->show_fps = fps ? 1 : 0;
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Audio")) {
                    bool ae = s->audio_enabled != 0;
                    if (ImGui::Checkbox("Enabled (restart)", &ae))
                        s->audio_enabled = ae ? 1 : 0;
                    ImGui::SliderInt("Volume", &s->volume, 0, 100);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Input")) {
                    ImGui::Text("Controller 1 = keyboard + gamepad 1");
                    for (int i = 0; i < 4; i++) {
                        char label[32];
                        bool pe = s->pad_present[i] != 0;
                        snprintf(label, sizeof(label), "Pad %d", i + 1);
                        if (ImGui::Checkbox(label, &pe))
                            s->pad_present[i] = pe ? 1 : 0;
                        if (i < 3)
                            ImGui::SameLine();
                    }
                    bool rb = s->rumble != 0;
                    if (ImGui::Checkbox("Rumble (XInput/evdev)",
                                        &rb)) {
                        s->rumble = rb ? 1 : 0;
                        hal_input_set_rumble_enabled(s->rumble);
                    }
                    ImGui::Separator();
                    for (int i = 0; i < MELEE_KEY_SLOTS; i++) {
                        char btn[64];
                        snprintf(btn, sizeof(btn), "%-10s : %s",
                                 kKeyNames[i], GlfwKeyName(s->keys[i]));
                        if (s_capture_key == i)
                            snprintf(btn, sizeof(btn), "%-10s : <press>",
                                     kKeyNames[i]);
                        char id[16];
                        snprintf(id, sizeof(id), "##k%d", i);
                        ImGui::PushID(i);
                        if (ImGui::Button(btn))
                            s_capture_key = i;
                        ImGui::PopID();
                        if ((i % 2) == 0)
                            ImGui::SameLine();
                    }
                    if (s_capture_key >= 0) {
                        ImGui::Text("Press a key for %s (Esc cancels)",
                                    kKeyNames[s_capture_key]);
                        for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST;
                             k++) {
                            if (k == GLFW_KEY_F1)
                                continue; // reserved for GUI toggle
                            if (glfwGetKey(s_win, k) == GLFW_PRESS) {
                                if (k == GLFW_KEY_ESCAPE) {
                                    s_capture_key = -1;
                                } else {
                                    s->keys[s_capture_key] = k;
                                    s_capture_key = -1;
                                }
                                break;
                            }
                        }
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Paths")) {
                    ImGui::InputText("Disc root", s->disc_root,
                                     sizeof(s->disc_root));
                    ImGui::InputText("Card A", s->card_a,
                                     sizeof(s->card_a));
                    ImGui::InputText("Card B", s->card_b,
                                     sizeof(s->card_b));
                    ImGui::InputText("User dir", s->user_dir,
                                     sizeof(s->user_dir));
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("System")) {
                    bool mem48 = (s->mem_size_mb == 48);
                    if (ImGui::Checkbox("48 MB RAM (restart)", &mem48))
                        s->mem_size_mb = mem48 ? 48 : 24;
                    const char* langs[] = { "English", "German",
                                            "French",  "Spanish",
                                            "Italian", "Dutch" };
                    ImGui::Combo("Language", &s->language, langs, 6);
                    bool gs = s->gui_startup != 0;
                    if (ImGui::Checkbox("Open GUI on boot", &gs))
                        s->gui_startup = gs ? 1 : 0;
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::Separator();
            if (ImGui::Button("Save")) {
                if (!s_ini_path[0])
                    melee_settings_path_default(s_ini_path,
                                                sizeof(s_ini_path));
                if (melee_settings_save(s_ini_path, s))
                    ImGui::SameLine(), ImGui::Text("saved.");
                else
                    ImGui::SameLine(), ImGui::Text("SAVE FAILED.");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("F1 hides • most video changes need "
                                "restart");
        }
        ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// Console editor lives in gui_console.c (always linked, used by
// --configure even in ImGui builds).
void gui_set_ini_path(const char* path)
{
    if (path) {
        strncpy(s_ini_path, path, sizeof(s_ini_path) - 1);
    }
}

#endif // MELEE_HAVE_IMGUI
