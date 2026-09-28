#pragma once

namespace spongebob {

[[nodiscard]] float spongebob_camera_smoothing(float keep) noexcept;

[[nodiscard]] float spongebob_mouse_camera_turn_speed(float game_speed, float timestep) noexcept;

[[nodiscard]] float spongebob_mouse_camera_pitch_speed(float game_speed, float timestep) noexcept;

[[nodiscard]] float spongebob_mouse_camera_turn_angle(float game_angle) noexcept;

[[nodiscard]] float spongebob_mouse_camera_pitch_angle(float game_angle) noexcept;

[[nodiscard]] float spongebob_mouse_vehicle_camera_axis_x(float game_axis) noexcept;

[[nodiscard]] float spongebob_mouse_vehicle_camera_axis_y(float game_axis) noexcept;

void spongebob_add_mouse_camera_delta(int dx, int dy) noexcept;

void spongebob_camera_tick() noexcept;

}
