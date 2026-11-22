#pragma once
// Cheats and testing helpers, toggled from the F1 menu (Cheats tab) and
// hotkeys, applied once per frame by the profile (cheats_tick in
// spongebob_profile.cpp) using the reverse-engineered layouts in game/.

#include <atomic>

namespace spongebob {

struct CheatState {
    // Menu toggles.
    std::atomic<bool> infinite_health{false};
    std::atomic<bool> infinite_lives{false};
    std::atomic<bool> moon_jump_enabled{true};   // J held = moon jump
    std::atomic<bool> turbo_enabled{true};       // Tab held = no frame pacing
    // Live key state / one-shot requests (set by the window layer).
    std::atomic<bool> moon_jump_held{false};
    std::atomic<bool> turbo_held{false};
    std::atomic<bool> save_position_request{false};  // F5
    std::atomic<bool> load_position_request{false};  // F7
    // Feedback for the menu.
    std::atomic<bool> position_saved{false};
};

CheatState &cheats();

}  // namespace spongebob
