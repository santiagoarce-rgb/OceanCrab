#pragma once

#include "psprecomp/guest_memory.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace spongebob {

struct GeTransformState {
    std::array<float, 96> bones{};
    std::array<float, 12> world{};
    std::array<float, 12> view{};
    std::array<float, 16> projection{};
    std::array<float, 12> texture{};
    std::array<float, 8> morph_weights{};
    std::uint32_t bone_cursor{};
    std::uint32_t world_cursor{};
    std::uint32_t view_cursor{};
    std::uint32_t projection_cursor{};
    std::uint32_t texture_cursor{};
};

void reset_ge_transform_state(GeTransformState &state) noexcept;
void update_ge_transform_state(GeTransformState &state, std::uint32_t command,
                               std::uint32_t data) noexcept;

struct GeBoundingBoxResult {
    bool visible{};
    std::uint32_t next_vertex_address{};
    std::uint32_t next_index_address{};
};

struct GeRenderStats {
    std::uint64_t primitives{};
    std::uint64_t points{};
    std::uint64_t lines{};
    std::uint64_t triangles{};
    std::uint64_t rectangles{};
    std::uint64_t skinned_vertices{};
    std::uint64_t morphed_vertices{};
    std::uint64_t lit_vertices{};
    std::uint64_t generated_uv_vertices{};
    std::uint64_t culled_triangles{};
    std::uint64_t flat_shaded_primitives{};
    std::uint64_t pixels_tested{};
    std::uint64_t pixels_written{};
    std::uint64_t unsupported_primitives{};
    std::uint64_t decoded_vertices{};
    std::uint64_t nonfinite_clip_vertices{};
    std::uint64_t screen_vertices{};
    bool has_clip_bounds{};
    bool has_screen_bounds{};
    float clip_min_x{};
    float clip_min_y{};
    float clip_min_z{};
    float clip_min_w{};
    float clip_max_x{};
    float clip_max_y{};
    float clip_max_z{};
    float clip_max_w{};
    float screen_min_x{};
    float screen_min_y{};
    float screen_max_x{};
    float screen_max_y{};
    float min_abs_w{};
    float max_abs_screen_coordinate{};
    std::uint32_t next_vertex_address{};
    std::uint32_t next_index_address{};
};

bool test_ge_bounding_box(const psprecomp::GuestMemory &memory,
                          const std::array<std::uint32_t, 256> &commands,
                          const GeTransformState &transform,
                          std::uint32_t vertex_address,
                          std::uint32_t index_address,
                          std::uint32_t count,
                          GeBoundingBoxResult &result,
                          std::string &error);

bool render_ge_primitive(psprecomp::GuestMemory &memory,
                         const std::array<std::uint32_t, 256> &commands,
                         const GeTransformState &transform,
                         std::uint32_t vertex_address,
                         std::uint32_t index_address,
                         std::uint32_t primitive_data,
                         GeRenderStats &stats,
                         std::string &error,
                         std::uint32_t logical_primitive_count = 1u,
                         std::uint64_t draw_state_revision = 0u,
                         std::uint64_t camera_state_revision = 0u,
                         std::uint64_t lighting_state_revision = 0u,
                         bool collect_diagnostic_stats = true);

struct GePhaseTotals {
    std::uint64_t pixel_loop_ns{};
    std::uint64_t triangles{};
    std::uint64_t draw_setup_ns{};
    std::uint64_t texture_upload_ns{};
    std::uint64_t vertex_decode_ns{};
    std::uint64_t gpu_stage_ns{};
    std::uint64_t triangle_prep_ns{};
    std::uint64_t gpu_accumulate_ns{};
    std::uint64_t primitives{};
    std::uint64_t vertices{};
};
void flush_ge_deferred_rasterization(psprecomp::GuestMemory &memory);

[[nodiscard]] GePhaseTotals ge_phase_totals() noexcept;
void reset_ge_phase_totals() noexcept;

}
