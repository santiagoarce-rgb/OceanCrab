#pragma once
// F1 enhancements menu (Dear ImGui), in the style of the Harbour Masters ports:
// a menu bar over the game image. Settings apply live when possible and are
// saved to the .ini; the rest are marked "restart".

union SDL_Event;
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace spongebob {

void menu_init(SDL_Window *window, SDL_Renderer *renderer);
void menu_shutdown();

// Feed every SDL event. F1 toggles the menu.
void menu_handle_event(const SDL_Event &event);

// Draw the menu (if open) after the game image, before SDL_RenderPresent.
void menu_render();

// True while the menu is open: the game must not receive keyboard/mouse input.
[[nodiscard]] bool menu_open();

// Set when the user picks Quit in the menu.
[[nodiscard]] bool menu_quit_requested();

// The game texture, so live display options (filter) can be applied to it.
void menu_set_game_texture(SDL_Texture *texture);

}  // namespace spongebob
