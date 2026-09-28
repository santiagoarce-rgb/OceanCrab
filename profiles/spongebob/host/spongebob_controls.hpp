#pragma once

#include <cstdint>

namespace spongebob {

[[nodiscard]] bool spongebob_camera_hook_enabled() noexcept;
[[nodiscard]] int spongebob_camera_axis_x() noexcept;
[[nodiscard]] int spongebob_camera_axis_y() noexcept;
void spongebob_camera_set_axes(int x, int y) noexcept;
[[nodiscard]] bool spongebob_camera_in_use() noexcept;

[[nodiscard]] std::uint32_t spongebob_accelerate_pad_offset() noexcept;
[[nodiscard]] std::uint32_t spongebob_brake_pad_offset() noexcept;
[[nodiscard]] bool spongebob_host_accelerate() noexcept;
[[nodiscard]] bool spongebob_host_brake() noexcept;
void spongebob_set_host_drive_inputs(bool accelerate, bool brake) noexcept;
void spongebob_note_vehicle_control_read() noexcept;
[[nodiscard]] bool spongebob_player_in_vehicle() noexcept;

}
