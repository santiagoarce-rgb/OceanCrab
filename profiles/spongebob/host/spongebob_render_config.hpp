#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace spongebob {

enum class DisplayResolutionMode : std::uint8_t {
    PspNative,
    Custom,
    Desktop,
};

enum class DisplayAspectMode : std::uint8_t {
    Preserve,
    Stretch,
};

enum class DisplayUpscaleFilter : std::uint8_t {
    Nearest,
    Bilinear,
};

enum class InternalResolutionMode : std::uint8_t {
    PspNative,
    Scale,
    Custom,
    Desktop,
};

enum class RenderingBackend : std::uint8_t {
    Software,
    DirectX12,
    Vulkan,
};

struct DisplayConfiguration {
    bool enabled{true};
    DisplayResolutionMode resolution_mode{DisplayResolutionMode::PspNative};
    std::uint32_t custom_width{480u};
    std::uint32_t custom_height{272u};
    bool fullscreen{false};
    DisplayAspectMode aspect_mode{DisplayAspectMode::Preserve};
    DisplayUpscaleFilter upscale_filter{DisplayUpscaleFilter::Nearest};
    bool integer_scale{false};
    bool show_fps{false};
    bool vsync{false};
    float hud_scale{1.0f};
};

struct PresentationRectangle {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t width{};
    std::int32_t height{};
};

struct DisplaySurfaceDimensions {
    std::uint32_t width{480u};
    std::uint32_t height{272u};
};

[[nodiscard]] DisplaySurfaceDimensions resolve_display_surface_dimensions(
    const DisplayConfiguration &configuration) noexcept;

[[nodiscard]] PresentationRectangle calculate_presentation_rectangle(
    std::uint32_t client_width, std::uint32_t client_height,
    std::uint32_t source_width, std::uint32_t source_height,
    DisplayAspectMode aspect_mode, bool integer_scale) noexcept;

struct InternalResolutionDimensions {
    std::uint32_t width{480u};
    std::uint32_t height{272u};
};

struct RenderingConfiguration {
    RenderingBackend backend{RenderingBackend::DirectX12};
    InternalResolutionMode internal_resolution_mode{InternalResolutionMode::PspNative};
    std::uint32_t internal_scale{2u};
    std::uint32_t internal_width{960u};
    std::uint32_t internal_height{544u};
    std::uint32_t anisotropic_filtering{1u};
    std::uint32_t msaa{4u};
    std::uint32_t depth_precision{24u};
    std::uint32_t texture_cache_entries{8192u};
    std::uint32_t texture_cache_mb{256u};
    bool hardware_transform{true};
    bool dx12_ge_color{true};
    bool smaa{false};
    bool experimental_gpu_color_preview{false};
    bool gpu_geometry_debug_colors{false};
    std::uint64_t dump_gpu_frame_vblank{0u};
};

[[nodiscard]] InternalResolutionDimensions resolve_internal_resolution(
    const RenderingConfiguration &configuration) noexcept;

struct AudioConfiguration {
    bool enabled{true};
    std::uint32_t volume{100u};
    bool diagnostics{true};
    std::uint32_t prebuffer_blocks{6u};
    std::uint32_t recovery_prebuffer_blocks{12u};
};

struct TimingConfiguration {
    std::uint32_t frame_rate{240u};
    bool realtime_speed_diagnostics{false};
    std::uint64_t realtime_speed_interval_vblanks{120u};
};

struct DiagnosticsConfiguration {
    bool log_to_file{false};
    std::string log_file{"SpongebobNative.log"};
    bool flush_every_line{true};
};

struct WidescreenConfiguration {
    bool enabled{false};
    std::uint32_t aspect_x{0u};
    std::uint32_t aspect_y{0u};
};

inline constexpr float kGameNativeAspectRatio = 16.0f / 9.0f;

struct ControlsConfiguration {
    bool camera_stick{true};
    std::uint32_t mouse_sensitivity{12u};
    std::uint32_t camera_smoothing{50u};
    bool invert_camera_y{false};
    std::uint32_t ped_camera_up_limit_degrees{45u};
    bool modern_control_scheme{false};
};

struct SpongebobConfiguration {
    ControlsConfiguration controls{};
    DisplayConfiguration display{};
    RenderingConfiguration rendering{};
    AudioConfiguration audio{};
    TimingConfiguration timing{};
    DiagnosticsConfiguration diagnostics{};
    WidescreenConfiguration widescreen{};
    std::filesystem::path source_path{};
    std::filesystem::path executable_directory{};
    bool initialized{true};
    bool loaded_from_file{false};
    std::vector<std::string> warnings{};
};

[[nodiscard]] SpongebobConfiguration load_spongebob_render_configuration(const std::filesystem::path &path);

void initialize_spongebob_render_configuration(const std::filesystem::path &executable_directory);

[[nodiscard]] const SpongebobConfiguration &spongebob_render_configuration();
[[nodiscard]] const char *display_resolution_mode_name(DisplayResolutionMode mode) noexcept;
[[nodiscard]] const char *display_aspect_mode_name(DisplayAspectMode mode) noexcept;
[[nodiscard]] const char *display_upscale_filter_name(DisplayUpscaleFilter filter) noexcept;
[[nodiscard]] const char *internal_resolution_mode_name(InternalResolutionMode mode) noexcept;
[[nodiscard]] const char *rendering_backend_name(RenderingBackend backend) noexcept;

[[nodiscard]] float resolve_widescreen_aspect_ratio(
    const SpongebobConfiguration &configuration,
    std::uint32_t surface_width, std::uint32_t surface_height) noexcept;

[[nodiscard]] float widescreen_render_stretch() noexcept;

[[nodiscard]] float widescreen_stretch_factor(
    const SpongebobConfiguration &configuration,
    std::uint32_t surface_width, std::uint32_t surface_height) noexcept;

}
