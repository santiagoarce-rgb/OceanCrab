#pragma once

#include "ge_gpu_backend.hpp"

#include <cstdint>

namespace spongebob {

void fps_overlay_observe_draw(const GeGpuDrawDescriptor &draw,
                              std::uint32_t vertex_weight) noexcept;

void fps_overlay_render_frame(std::uint32_t selected_framebuffer,
                              std::uint32_t logical_width,
                              std::uint32_t logical_height) noexcept;

}
