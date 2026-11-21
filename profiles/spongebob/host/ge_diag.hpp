#pragma once

#include <cstddef>
#include <cstdint>

namespace spongebob {

// PSPRECOMP_GE_UNSUPPORTED_DIAG=1: count GE work the renderer drops or cannot
// express (primitive types, texture functions) and print a summary about once
// per second of presented frames. Silent when nothing was dropped.
[[nodiscard]] bool ge_unsupported_diag_enabled() noexcept;
void ge_diag_count_dropped_primitive(std::uint32_t primitive, bool through) noexcept;
void ge_diag_count_gpu_lost_primitive(std::uint32_t primitive, bool through, std::size_t vertices) noexcept;
void ge_diag_count_unsupported_texture_function(std::uint32_t texture_function) noexcept;
void ge_diag_frame_tick() noexcept;

}  // namespace spongebob
