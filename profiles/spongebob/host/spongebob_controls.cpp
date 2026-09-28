#include "spongebob_controls.hpp"
#include "spongebob_mouse.hpp"
#include "spongebob_frame_limit.hpp"

#include "spongebob_render_config.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>

namespace spongebob {
namespace {

std::atomic<int> g_axis_x{0};
std::atomic<int> g_axis_y{0};
std::atomic<bool> g_host_accelerate{false};
std::atomic<bool> g_host_brake{false};
std::atomic<std::int64_t> g_last_vehicle_read_ms{0};
std::atomic<std::int64_t> g_last_camera_read_ms{0};
std::atomic<std::int64_t> g_last_mouse_camera_ms{0};
std::atomic<std::int32_t> g_mouse_camera_dx{0};
std::atomic<std::int32_t> g_mouse_camera_dy{0};
std::atomic<std::int64_t> g_last_mouse_camera_take_ms{0};

constexpr std::uint32_t kPadCross = 42u;
constexpr std::uint32_t kPadSquare = 38u;
constexpr std::uint32_t kPadRightShoulder = 14u;
constexpr std::uint32_t kPadLeftShoulder = 10u;

std::int64_t now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool modern_scheme() noexcept {
    static const bool value = [] {
        const SpongebobConfiguration &config = spongebob_render_configuration();
        return config.initialized && config.controls.modern_control_scheme;
    }();
    return value;
}

}  // namespace

bool spongebob_frame_limiter_unlocked() noexcept {
    static const bool value = [] {
        const SpongebobConfiguration &config = spongebob_render_configuration();
        return config.initialized && config.timing.frame_rate > 30u;
    }();
    return value;
}

static bool mouse_camera_active() noexcept {
    if (!spongebob_camera_hook_enabled()) return false;
    const std::int64_t last = g_last_mouse_camera_ms.load(std::memory_order_relaxed);
    return last != 0 && now_ms() - last <= 500;
}

void spongebob_add_mouse_camera_delta(int dx, int dy) noexcept {
    if (dx == 0 && dy == 0) return;
    const std::int64_t now = now_ms();
    if (now - g_last_mouse_camera_take_ms.load(std::memory_order_relaxed) > 200) {
        g_mouse_camera_dx.store(0, std::memory_order_relaxed);
        g_mouse_camera_dy.store(0, std::memory_order_relaxed);
    }
    g_mouse_camera_dx.fetch_add(dx, std::memory_order_relaxed);
    g_mouse_camera_dy.fetch_add(dy, std::memory_order_relaxed);
    g_last_mouse_camera_ms.store(now, std::memory_order_relaxed);
}

static float take_mouse_angle(std::atomic<std::int32_t> &counts, float sign) noexcept {
    static const float radians_per_count = [] {
        const SpongebobConfiguration &config = spongebob_render_configuration();
        const std::uint32_t sensitivity = config.initialized ? config.controls.mouse_sensitivity : 20u;
        return static_cast<float>(sensitivity) * 0.0001f;
    }();
    g_last_mouse_camera_take_ms.store(now_ms(), std::memory_order_relaxed);
    const int delta = counts.exchange(0, std::memory_order_relaxed);
    return sign * static_cast<float>(delta) * radians_per_count;
}

static float pitch_sign() noexcept {
    static const float sign = [] {
        const SpongebobConfiguration &config = spongebob_render_configuration();
        return config.initialized && config.controls.invert_camera_y ? 1.0f : -1.0f;
    }();
    return sign;
}

static float mouse_camera_speed(std::atomic<std::int32_t> &counts, float sign,
                                float game_speed, float timestep) noexcept {
    spongebob_camera_tick();
    if (!mouse_camera_active()) return game_speed;
    if (!(timestep > 0.0f)) return 0.0f;
    return take_mouse_angle(counts, sign) / timestep;
}

static float mouse_camera_angle(std::atomic<std::int32_t> &counts, float sign,
                                float game_angle) noexcept {
    spongebob_camera_tick();
    if (!mouse_camera_active()) return game_angle;
    return take_mouse_angle(counts, sign);
}

float spongebob_mouse_camera_turn_speed(float game_speed, float timestep) noexcept {
    return mouse_camera_speed(g_mouse_camera_dx, -1.0f, game_speed, timestep);
}

float spongebob_mouse_camera_pitch_speed(float game_speed, float timestep) noexcept {
    return mouse_camera_speed(g_mouse_camera_dy, pitch_sign(), game_speed, timestep);
}

float spongebob_mouse_vehicle_camera_axis_x(float game_axis) noexcept {
    return mouse_camera_angle(g_mouse_camera_dx, 1.0f, game_axis * 0.0003f) / 0.0003f;
}

float spongebob_mouse_vehicle_camera_axis_y(float game_axis) noexcept {
    return mouse_camera_angle(g_mouse_camera_dy, pitch_sign(), game_axis * 0.0003f) / 0.0003f;
}

float spongebob_camera_smoothing(float keep) noexcept {
    static const float smoothing = [] {
        const SpongebobConfiguration &config = spongebob_render_configuration();
        return config.initialized ? static_cast<float>(config.controls.camera_smoothing) / 100.0f : 1.0f;
    }();
    if (smoothing >= 1.0f || !mouse_camera_active()) return keep;
    return keep * smoothing;
}

float spongebob_mouse_camera_turn_angle(float game_angle) noexcept {
    return mouse_camera_angle(g_mouse_camera_dx, -1.0f, game_angle);
}

float spongebob_mouse_camera_pitch_angle(float game_angle) noexcept {
    return mouse_camera_angle(g_mouse_camera_dy, pitch_sign(), game_angle);
}

bool spongebob_camera_hook_enabled() noexcept {
    static const bool value = [] {
        const SpongebobConfiguration &config = spongebob_render_configuration();
        return config.initialized && config.controls.camera_stick;
    }();
    return value;
}

int spongebob_camera_axis_x() noexcept {
    return g_axis_x.load(std::memory_order_relaxed);
}

int spongebob_camera_axis_y() noexcept {
    return g_axis_y.load(std::memory_order_relaxed);
}

void spongebob_camera_tick() noexcept {
    g_last_camera_read_ms.store(now_ms(), std::memory_order_relaxed);
}

bool spongebob_camera_in_use() noexcept {
    const std::int64_t last = g_last_camera_read_ms.load(std::memory_order_relaxed);
    return (last != 0 && now_ms() - last < 250) || spongebob_player_in_vehicle();
}

void spongebob_camera_set_axes(int x, int y) noexcept {
    g_axis_x.store(std::clamp(x, -127, 127), std::memory_order_relaxed);
    g_axis_y.store(std::clamp(y, -127, 127), std::memory_order_relaxed);
}

std::uint32_t spongebob_accelerate_pad_offset() noexcept {
    return modern_scheme() ? kPadRightShoulder : kPadCross;
}

std::uint32_t spongebob_brake_pad_offset() noexcept {
    return modern_scheme() ? kPadLeftShoulder : kPadSquare;
}

bool spongebob_host_accelerate() noexcept { return g_host_accelerate.load(std::memory_order_relaxed); }
bool spongebob_host_brake() noexcept { return g_host_brake.load(std::memory_order_relaxed); }

void spongebob_set_host_drive_inputs(bool accelerate, bool brake) noexcept {
    g_host_accelerate.store(accelerate, std::memory_order_relaxed);
    g_host_brake.store(brake, std::memory_order_relaxed);
}

void spongebob_note_vehicle_control_read() noexcept {
    g_last_vehicle_read_ms.store(now_ms(), std::memory_order_relaxed);
}

bool spongebob_player_in_vehicle() noexcept {
    const std::int64_t last = g_last_vehicle_read_ms.load(std::memory_order_relaxed);
    return last != 0 && now_ms() - last < 250;
}

}
