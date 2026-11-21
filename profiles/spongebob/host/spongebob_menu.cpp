#include "spongebob_menu.hpp"

#include "spongebob_render_config.hpp"
#include "ge_gpu_backend.hpp"

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <array>
#include <cstdlib>
#include <string>

namespace spongebob {
namespace {

SDL_Window *g_window{};
SDL_Renderer *g_renderer{};
SDL_Texture *g_game_texture{};
bool g_ready{};
bool g_open{};
bool g_quit{};
bool g_restart_needed{};
bool g_debug_graphics_window{};

// The values the running renderer was created with; changing them needs a restart.
struct StartupRendering {
    InternalResolutionMode mode{};
    std::uint32_t scale{};
    std::uint32_t msaa{};
    std::uint32_t anisotropy{};
} g_startup{};

void apply_texture_filter() {
    if (g_game_texture == nullptr) return;
    const bool nearest = spongebob_render_configuration().display.upscale_filter == DisplayUpscaleFilter::Nearest;
    SDL_SetTextureScaleMode(g_game_texture, nearest ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
}

void refresh_restart_flag() {
    const RenderingConfiguration &r = spongebob_render_configuration().rendering;
    g_restart_needed = r.internal_resolution_mode != g_startup.mode || r.internal_scale != g_startup.scale ||
                       r.msaa != g_startup.msaa || r.anisotropic_filtering != g_startup.anisotropy;
}

void restart_marker() {
    ImGui::SameLine();
    ImGui::TextDisabled("(restart)");
}

// A combo over discrete values; returns true when the selection changed.
template <std::size_t N>
bool value_combo(const char *label, std::uint32_t &value, const std::array<std::uint32_t, N> &values,
                 const std::array<const char *, N> &names) {
    std::size_t current = 0u;
    for (std::size_t i = 0u; i < N; ++i)
        if (values[i] == value) current = i;
    bool changed = false;
    if (ImGui::BeginCombo(label, names[current])) {
        for (std::size_t i = 0u; i < N; ++i) {
            if (ImGui::Selectable(names[i], i == current)) {
                changed = values[i] != value;
                value = values[i];
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void graphics_menu() {
    SpongebobConfiguration &config = spongebob_render_configuration_mutable();
    DisplayConfiguration &display = config.display;
    RenderingConfiguration &rendering = config.rendering;

    ImGui::SeparatorText("Rendering");
    if (!ge_gpu_backend_device_name().empty()) ImGui::TextDisabled("GPU: %s", ge_gpu_backend_device_name().c_str());
    // Internal resolution: PSP native or an integer multiple of 480x272.
    std::uint32_t scale = rendering.internal_resolution_mode == InternalResolutionMode::Scale
                              ? rendering.internal_scale : 1u;
    ImGui::SetNextItemWidth(200.0f);
    if (value_combo("Internal resolution", scale, std::array<std::uint32_t, 6>{1u, 2u, 3u, 4u, 5u, 6u},
                    std::array<const char *, 6>{"1x  PSP (480x272)", "2x  (960x544)", "3x  (1440x816)",
                                                "4x  (1920x1088)", "5x  (2400x1360)", "6x  (2880x1632)"})) {
        rendering.internal_resolution_mode = scale == 1u ? InternalResolutionMode::PspNative
                                                         : InternalResolutionMode::Scale;
        rendering.internal_scale = scale;
        save_spongebob_setting("Rendering", "InternalResolutionMode", scale == 1u ? "PSP" : "Scale");
        save_spongebob_setting("Rendering", "InternalScale", std::to_string(scale));
    }
    restart_marker();
    ImGui::SetNextItemWidth(200.0f);
    if (value_combo("Anti-aliasing (MSAA)", rendering.msaa, std::array<std::uint32_t, 4>{1u, 2u, 4u, 8u},
                    std::array<const char *, 4>{"Off", "2x", "4x", "8x"}))
        save_spongebob_setting("Rendering", "MSAA", std::to_string(rendering.msaa));
    restart_marker();
    ImGui::SetNextItemWidth(200.0f);
    if (value_combo("Anisotropic filtering", rendering.anisotropic_filtering,
                    std::array<std::uint32_t, 5>{1u, 2u, 4u, 8u, 16u},
                    std::array<const char *, 5>{"Off", "2x", "4x", "8x", "16x"}))
        save_spongebob_setting("Rendering", "AnisotropicFiltering",
                               std::to_string(rendering.anisotropic_filtering));
    restart_marker();

    ImGui::SetNextItemWidth(200.0f);
    if (value_combo("Draw distance", rendering.draw_distance_scale, std::array<std::uint32_t, 4>{1u, 2u, 4u, 8u},
                    std::array<const char *, 4>{"1x  (original)", "2x", "4x", "8x"}))
        save_spongebob_setting("Rendering", "DrawDistance", std::to_string(rendering.draw_distance_scale));
    ImGui::SameLine();
    ImGui::TextDisabled("(live)");

    ImGui::SeparatorText("Screen");
    bool smooth = display.upscale_filter == DisplayUpscaleFilter::Bilinear;
    if (ImGui::Checkbox("Smooth scaling (bilinear)", &smooth)) {
        display.upscale_filter = smooth ? DisplayUpscaleFilter::Bilinear : DisplayUpscaleFilter::Nearest;
        apply_texture_filter();
        save_spongebob_setting("Display", "UpscaleFilter", smooth ? "Bilinear" : "Nearest");
    }
    bool keep_aspect = display.aspect_mode == DisplayAspectMode::Preserve;
    if (ImGui::Checkbox("Keep aspect ratio", &keep_aspect)) {
        display.aspect_mode = keep_aspect ? DisplayAspectMode::Preserve : DisplayAspectMode::Stretch;
        save_spongebob_setting("Display", "AspectRatio", keep_aspect ? "Preserve" : "Stretch");
    }
    ImGui::BeginDisabled(!keep_aspect);
    if (ImGui::Checkbox("Integer scaling", &display.integer_scale))
        save_spongebob_setting("Display", "IntegerScale", display.integer_scale ? "true" : "false");
    ImGui::EndDisabled();
    bool fullscreen = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    if (ImGui::Checkbox("Fullscreen (F11)", &fullscreen)) {
        SDL_SetWindowFullscreen(g_window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0u);
        display.fullscreen = fullscreen;
        save_spongebob_setting("Display", "Fullscreen", fullscreen ? "true" : "false");
    }
    if (ImGui::Checkbox("Show FPS", &display.show_fps))
        save_spongebob_setting("Display", "ShowFPS", display.show_fps ? "true" : "false");

    refresh_restart_flag();
    if (g_restart_needed) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Saved. Restart the game to apply the marked options.");
    }
}

void draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("Ocean Crab")) {
        ImGui::TextDisabled("SpongeBob's Truth or Square, native PC recomp");
        ImGui::Separator();
        if (ImGui::MenuItem("Close menu", "F1")) g_open = false;
        if (ImGui::MenuItem("Quit")) g_quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Graphics")) {
        graphics_menu();
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

}  // namespace

void menu_init(SDL_Window *window, SDL_Renderer *renderer) {
    if (g_ready || window == nullptr || renderer == nullptr) return;
    g_window = window;
    g_renderer = renderer;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;  // our settings live in SpongebobNative.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 6.0f;
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
    const RenderingConfiguration &r = spongebob_render_configuration().rendering;
    g_startup = {r.internal_resolution_mode, r.internal_scale, r.msaa, r.anisotropic_filtering};
    g_ready = true;
    // PSPRECOMP_MENU_OPEN=1 starts with the menu open (=graphics also shows the
    // Graphics panel as a window), for headless screenshots.
    if (const char *open = std::getenv("PSPRECOMP_MENU_OPEN")) {
        g_open = true;
        g_debug_graphics_window = std::string(open) == "graphics";
    }
}

void menu_shutdown() {
    if (!g_ready) return;
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    g_ready = false;
}

void menu_handle_event(const SDL_Event &event) {
    if (!g_ready) return;
    if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_F1 && event.key.repeat == 0) {
        g_open = !g_open;
        return;
    }
    ImGui_ImplSDL2_ProcessEvent(&event);
}

void menu_render() {
    if (!g_ready || !g_open) return;
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    draw_menu_bar();
    if (g_debug_graphics_window) {
        ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_Once);
        if (ImGui::Begin("Graphics", &g_debug_graphics_window, ImGuiWindowFlags_AlwaysAutoResize)) graphics_menu();
        ImGui::End();
    }
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_renderer);
}

bool menu_open() { return g_ready && g_open; }

bool menu_quit_requested() { return g_quit; }

void menu_set_game_texture(SDL_Texture *texture) {
    g_game_texture = texture;
    apply_texture_filter();
}

}  // namespace spongebob
