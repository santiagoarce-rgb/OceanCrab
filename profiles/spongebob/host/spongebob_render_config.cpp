#include "spongebob_render_config.hpp"
#include "spongebob_widescreen.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace spongebob {
namespace {

std::string trim_copy(std::string value) {
    const auto not_space = [](unsigned char ch) { return std::isspace(ch) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string strip_inline_comment(std::string value) {
    bool quoted = false;
    char quote = '\0';
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if ((ch == '\'' || ch == '"')) {
            if (!quoted) {
                quoted = true;
                quote = ch;
            } else if (quote == ch) {
                quoted = false;
            }
            continue;
        }
        if (!quoted && (ch == ';' || ch == '#')) {
            value.resize(index);
            break;
        }
    }
    return trim_copy(std::move(value));
}

bool parse_bool(std::string value, bool &out) {
    value = lowercase_copy(trim_copy(std::move(value)));
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parse_u32(std::string value, std::uint32_t minimum, std::uint32_t maximum,
               std::uint32_t &out) {
    value = trim_copy(std::move(value));
    if (value.empty()) return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (errno == ERANGE || end == value.c_str() || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        return false;
    }
    out = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parse_u64(std::string value, std::uint64_t minimum, std::uint64_t maximum,
               std::uint64_t &out) {
    value = trim_copy(std::move(value));
    if (value.empty()) return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno == ERANGE || end == value.c_str() || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        return false;
    }
    out = static_cast<std::uint64_t>(parsed);
    return true;
}

bool parse_float(std::string value, float minimum, float maximum, float &out) {
    value = trim_copy(std::move(value));
    if (value.empty()) return false;
    errno = 0;
    char *end = nullptr;
    const float parsed = std::strtof(value.c_str(), &end);
    if (errno == ERANGE || end == value.c_str() || *end != '\0' ||
        !std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
        return false;
    }
    out = parsed;
    return true;
}

void warning(SpongebobConfiguration &config, std::size_t line, const std::string &message) {
    std::ostringstream stream;
    stream << "line " << line << ": " << message;
    config.warnings.push_back(stream.str());
}

void apply_display_key(SpongebobConfiguration &config, const std::string &key,
                       const std::string &value, std::size_t line) {
    if (key == "enabled") {
        if (!parse_bool(value, config.display.enabled))
            warning(config, line, "Display.Enabled expects true/false");
        return;
    }
    if (key == "resolutionmode" || key == "resolution") {
        const std::string mode = lowercase_copy(trim_copy(value));
        if (mode == "psp" || mode == "pspnative" || mode == "nativepsp" || mode == "480x272")
            config.display.resolution_mode = DisplayResolutionMode::PspNative;
        else if (mode == "custom")
            config.display.resolution_mode = DisplayResolutionMode::Custom;
        else if (mode == "desktop" || mode == "native" || mode == "monitor")
            config.display.resolution_mode = DisplayResolutionMode::Desktop;
        else
            warning(config, line, "Display.ResolutionMode expects PSP, Custom or Desktop");
        return;
    }
    if (key == "width") {
        if (!parse_u32(value, 320u, 16384u, config.display.custom_width))
            warning(config, line, "Display.Width must be between 320 and 16384");
        return;
    }
    if (key == "height") {
        if (!parse_u32(value, 180u, 16384u, config.display.custom_height))
            warning(config, line, "Display.Height must be between 180 and 16384");
        return;
    }
    if (key == "fullscreen" || key == "borderlessfullscreen") {
        if (!parse_bool(value, config.display.fullscreen))
            warning(config, line, "Display.Fullscreen expects true/false");
        return;
    }
    if (key == "aspectratio" || key == "aspectmode") {
        const std::string aspect = lowercase_copy(trim_copy(value));
        if (aspect == "preserve" || aspect == "keep" || aspect == "letterbox")
            config.display.aspect_mode = DisplayAspectMode::Preserve;
        else if (aspect == "stretch" || aspect == "fill")
            config.display.aspect_mode = DisplayAspectMode::Stretch;
        else
            warning(config, line, "Display.AspectRatio expects Preserve or Stretch");
        return;
    }
    if (key == "upscalefilter" || key == "filter") {
        const std::string filter = lowercase_copy(trim_copy(value));
        if (filter == "nearest" || filter == "point" || filter == "pixel")
            config.display.upscale_filter = DisplayUpscaleFilter::Nearest;
        else if (filter == "bilinear" || filter == "linear" || filter == "smooth")
            config.display.upscale_filter = DisplayUpscaleFilter::Bilinear;
        else
            warning(config, line, "Display.UpscaleFilter expects Nearest or Bilinear");
        return;
    }
    if (key == "integerscale") {
        if (!parse_bool(value, config.display.integer_scale))
            warning(config, line, "Display.IntegerScale expects true/false");
        return;
    }
    if (key == "showfps" || key == "fpscounter") {
        if (!parse_bool(value, config.display.show_fps))
            warning(config, line, "Display.ShowFPS expects true/false");
        return;
    }
    if (key == "vsync" || key == "verticalsync") {
        if (!parse_bool(value, config.display.vsync))
            warning(config, line, "Display.VSync expects true/false");
        return;
    }
    if (key == "hudscale") {
        if (!parse_float(value, 0.25f, 1.0f, config.display.hud_scale))
            warning(config, line, "Display.HudScale must be between 0.25 and 1.0");
        return;
    }
    warning(config, line, "unknown [Display] key '" + key + "'");
}

void apply_rendering_key(SpongebobConfiguration &config, const std::string &key,
                         const std::string &value, std::size_t line) {
    if (key == "backend" || key == "renderingbackend") {
        const std::string backend = lowercase_copy(trim_copy(value));
        if (backend == "directx12" || backend == "dx12" || backend == "d3d12" || backend == "gpu")
            config.rendering.backend = RenderingBackend::DirectX12;
        else if (backend == "vulkan" || backend == "vk")
            config.rendering.backend = RenderingBackend::Vulkan;
        else
            warning(config, line, "Rendering.Backend supports DirectX12 and Vulkan");
        return;
    }
    if (key == "internalresolutionmode" || key == "internalmode") {
        const std::string mode = lowercase_copy(trim_copy(value));
        if (mode == "psp" || mode == "pspnative" || mode == "480x272")
            config.rendering.internal_resolution_mode = InternalResolutionMode::PspNative;
        else if (mode == "scale" || mode == "scaled")
            config.rendering.internal_resolution_mode = InternalResolutionMode::Scale;
        else if (mode == "custom")
            config.rendering.internal_resolution_mode = InternalResolutionMode::Custom;
        else if (mode == "desktop" || mode == "native" || mode == "monitor")
            config.rendering.internal_resolution_mode = InternalResolutionMode::Desktop;
        else
            warning(config, line,
                    "Rendering.InternalResolutionMode expects PSP, Scale, Custom or Desktop");
        return;
    }
    if (key == "internalscale" || key == "scale") {
        if (!parse_u32(value, 1u, 8u, config.rendering.internal_scale))
            warning(config, line, "Rendering.InternalScale must be between 1 and 8");
        return;
    }
    if (key == "msaa" || key == "multisampling") {
        std::uint32_t samples = 0u;
        if (!parse_u32(value, 1u, 16u, samples)) {
            warning(config, line, "Rendering.MSAA expects 1, 2, 4, 8 or 16");
            return;
        }
        if (samples != 1u && samples != 2u && samples != 4u &&
            samples != 8u && samples != 16u) {
            warning(config, line, "Rendering.MSAA expects 1, 2, 4, 8 or 16");
            return;
        }
        config.rendering.msaa = samples;
        return;
    }
    if (key == "hardwaretransform") {
        if (!parse_bool(value, config.rendering.hardware_transform))
            warning(config, line, "Rendering.HardwareTransform expects true/false");
        return;
    }
    if (key == "dx12gecolor" || key == "directx12gecolor" || key == "nativege") {
        if (!parse_bool(value, config.rendering.dx12_ge_color))
            warning(config, line, "Rendering.NativeGE expects true/false");
        return;
    }
    if (key == "texturecacheentries" || key == "texturecachelimit") {
        if (!parse_u32(value, 256u, 32768u, config.rendering.texture_cache_entries))
            warning(config, line, "Rendering.TextureCacheEntries must be between 256 and 32768");
        return;
    }
    if (key == "texturecachemb" || key == "texturecachememorymb") {
        if (!parse_u32(value, 32u, 2048u, config.rendering.texture_cache_mb))
            warning(config, line, "Rendering.TextureCacheMB must be between 32 and 2048");
        return;
    }
    if (key == "depthprecision" || key == "depthbits") {
        std::uint32_t bits = 0u;
        if (!parse_u32(value, 16u, 32u, bits) || (bits != 16u && bits != 24u && bits != 32u)) {
            warning(config, line, "Rendering.DepthPrecision expects 16, 24 or 32");
            return;
        }
        config.rendering.depth_precision = bits;
        return;
    }
    if (key == "smaa" || key == "antialiasing") {
        if (!parse_bool(value, config.rendering.smaa))
            warning(config, line, "Rendering.SMAA expects true/false");
        return;
    }
    if (key == "anisotropicfiltering" || key == "anisotropy") {
        if (!parse_u32(value, 1u, 16u, config.rendering.anisotropic_filtering))
            warning(config, line, "Rendering.AnisotropicFiltering must be between 1 and 16");
        return;
    }
    if (key == "internalwidth") {
        if (!parse_u32(value, 480u, 16384u, config.rendering.internal_width))
            warning(config, line, "Rendering.InternalWidth must be between 480 and 16384");
        return;
    }
    if (key == "internalheight") {
        if (!parse_u32(value, 272u, 16384u, config.rendering.internal_height))
            warning(config, line, "Rendering.InternalHeight must be between 272 and 16384");
        return;
    }
    if (key == "experimentalgpucolorpreview" || key == "gpucolorpreview") {
        if (!parse_bool(value, config.rendering.experimental_gpu_color_preview))
            warning(config, line, "Rendering.ExperimentalGpuColorPreview expects true/false");
        return;
    }
    if (key == "geometrydebugcolors" || key == "gpugeometrydebugcolors") {
        if (!parse_bool(value, config.rendering.gpu_geometry_debug_colors))
            warning(config, line, "Rendering.GeometryDebugColors expects true/false");
        return;
    }
    if (key == "dumpgpuframevblank" || key == "dumpinternalframevblank") {
        if (!parse_u64(value, 0u, 1000000000u, config.rendering.dump_gpu_frame_vblank))
            warning(config, line, "Rendering.DumpGpuFrameVblank must be between 0 and 1000000000");
        return;
    }
    warning(config, line, "unknown [Rendering] key '" + key + "'");
}

void apply_audio_key(SpongebobConfiguration &config, const std::string &key,
                     const std::string &value, std::size_t line) {
    if (key == "enabled") {
        if (!parse_bool(value, config.audio.enabled))
            warning(config, line, "Audio.Enabled expects true/false");
        return;
    }
    if (key == "volume") {
        if (!parse_u32(value, 0u, 400u, config.audio.volume))
            warning(config, line, "Audio.Volume must be between 0 and 400 (percent)");
        return;
    }
    if (key == "diagnostics" || key == "performancelog") {
        if (!parse_bool(value, config.audio.diagnostics))
            warning(config, line, "Audio.Diagnostics expects true/false");
        return;
    }
    if (key == "prebufferblocks") {
        if (!parse_u32(value, 2u, 22u, config.audio.prebuffer_blocks))
            warning(config, line, "Audio.PrebufferBlocks must be between 2 and 22");
        return;
    }
    if (key == "recoveryprebufferblocks") {
        if (!parse_u32(value, 2u, 22u, config.audio.recovery_prebuffer_blocks))
            warning(config, line, "Audio.RecoveryPrebufferBlocks must be between 2 and 22");
        return;
    }
    warning(config, line, "unknown [Audio] key '" + key + "'");
}

bool parse_aspect_ratio(const std::string &value, std::uint32_t &x, std::uint32_t &y) {
    if (lowercase_copy(trim_copy(value)) == "auto") { x = 0u; y = 0u; return true; }
    const std::size_t separator = value.find(':');
    if (separator == std::string::npos) return false;
    std::uint32_t parsed_x = 0u;
    std::uint32_t parsed_y = 0u;
    if (!parse_u32(value.substr(0u, separator), 1u, 1000u, parsed_x)) return false;
    if (!parse_u32(value.substr(separator + 1u), 1u, 1000u, parsed_y)) return false;
    x = parsed_x;
    y = parsed_y;
    return true;
}

void apply_controls_key(SpongebobConfiguration &config, const std::string &key,
                        const std::string &value, std::size_t line) {
    if (key == "camerastick" || key == "mousecamera") {
        if (!parse_bool(value, config.controls.camera_stick))
            warning(config, line, "Controls.CameraStick expects true/false");
        return;
    }
    if (key == "mousesensitivity") {
        if (!parse_u32(value, 1u, 100u, config.controls.mouse_sensitivity))
            warning(config, line, "Controls.MouseSensitivity must be between 1 and 100");
        return;
    }
    if (key == "camerasmoothing") {
        if (!parse_u32(value, 0u, 100u, config.controls.camera_smoothing))
            warning(config, line, "Controls.CameraSmoothing must be between 0 and 100");
        return;
    }
    if (key == "invertcameray") {
        if (!parse_bool(value, config.controls.invert_camera_y))
            warning(config, line, "Controls.InvertCameraY expects true/false");
        return;
    }
    if (key == "pedcamerauplimitdegrees" || key == "pedcamerauplimit") {
        if (!parse_u32(value, 10u, 85u, config.controls.ped_camera_up_limit_degrees))
            warning(config, line,
                    "Controls.PedCameraUpLimitDegrees must be between 10 and 85");
        return;
    }
    if (key == "moderncontrolscheme") {
        if (!parse_bool(value, config.controls.modern_control_scheme))
            warning(config, line, "Controls.ModernControlScheme expects true/false");
        return;
    }
    warning(config, line, "unknown [Controls] key '" + key + "'");
}

void apply_widescreen_key(SpongebobConfiguration &config, const std::string &key,
                          const std::string &value, std::size_t line) {
    if (key == "enabled") {
        if (!parse_bool(value, config.widescreen.enabled))
            warning(config, line, "Widescreen.Enabled expects true/false");
        return;
    }
    if (key == "aspectratio" || key == "forceaspectratio") {
        if (!parse_aspect_ratio(value, config.widescreen.aspect_x, config.widescreen.aspect_y))
            warning(config, line, "Widescreen.AspectRatio expects 'auto' or 'x:y' (e.g. 21:9)");
        return;
    }
    warning(config, line, "unknown [Widescreen] key '" + key + "'");
}

void apply_timing_key(SpongebobConfiguration &config, const std::string &key,
                      const std::string &value, std::size_t line) {
    if (key == "framerate" || key == "fps" || key == "targetfps") {
        std::uint32_t frame_rate = 0u;
        if (!parse_u32(value, 30u, 240u, frame_rate) ||
            (frame_rate != 30u && frame_rate != 60u && frame_rate != 120u &&
             frame_rate != 200u && frame_rate != 240u)) {
            warning(config, line, "Timing.FrameRate expects 30, 60, 120, 200 or 240");
            return;
        }
        config.timing.frame_rate = frame_rate;
        return;
    }
    if (key == "realtimespeeddiagnostics" || key == "showspeeddiagnostics") {
        if (!parse_bool(value, config.timing.realtime_speed_diagnostics))
            warning(config, line, "Timing.RealtimeSpeedDiagnostics expects true/false");
        return;
    }
    if (key == "realtimespeedintervalvblanks" || key == "speedintervalvblanks") {
        if (!parse_u64(value, 1u, 36000u, config.timing.realtime_speed_interval_vblanks))
            warning(config, line, "Timing.RealtimeSpeedIntervalVblanks must be between 1 and 36000");
        return;
    }
    warning(config, line, "unknown [Timing] key '" + key + "'");
}

void apply_diagnostics_key(SpongebobConfiguration &config, const std::string &key,
                           const std::string &value, std::size_t line) {
    if (key == "logtofile" || key == "enablelog" || key == "enabled") {
        if (!parse_bool(value, config.diagnostics.log_to_file))
            warning(config, line, "Diagnostics.LogToFile expects true/false");
        return;
    }
    if (key == "logfile" || key == "filename") {
        const std::string trimmed = trim_copy(value);
        if (trimmed.empty())
            warning(config, line, "Diagnostics.LogFile must not be empty");
        else
            config.diagnostics.log_file = trimmed;
        return;
    }
    if (key == "flusheveryline" || key == "autoflush") {
        if (!parse_bool(value, config.diagnostics.flush_every_line))
            warning(config, line, "Diagnostics.FlushEveryLine expects true/false");
        return;
    }
    warning(config, line, "unknown [Diagnostics] key '" + key + "'");
}

SpongebobConfiguration &global_configuration() {

    static SpongebobConfiguration config;
    return config;
}

std::mutex &global_configuration_mutex() {
    static std::mutex mutex;
    return mutex;
}

void set_environment_value(const char *name, const std::string &value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

}

DisplaySurfaceDimensions resolve_display_surface_dimensions(
    const DisplayConfiguration &configuration) noexcept {
    switch (configuration.resolution_mode) {
    case DisplayResolutionMode::PspNative:
        return {480u, 272u};
    case DisplayResolutionMode::Custom:
        return {std::clamp(configuration.custom_width, 320u, 16384u),
                std::clamp(configuration.custom_height, 180u, 16384u)};
    case DisplayResolutionMode::Desktop:
#if defined(_WIN32)
        return {static_cast<std::uint32_t>(std::max(320, GetSystemMetrics(SM_CXSCREEN))),
                static_cast<std::uint32_t>(std::max(180, GetSystemMetrics(SM_CYSCREEN)))};
#else
        return {std::clamp(configuration.custom_width, 320u, 16384u),
                std::clamp(configuration.custom_height, 180u, 16384u)};
#endif
    }
    return {480u, 272u};
}

PresentationRectangle calculate_presentation_rectangle(
    std::uint32_t client_width, std::uint32_t client_height,
    std::uint32_t source_width, std::uint32_t source_height,
    DisplayAspectMode aspect_mode, bool integer_scale) noexcept {
    if (client_width == 0u || client_height == 0u) return {};
    if (aspect_mode == DisplayAspectMode::Stretch || source_width == 0u || source_height == 0u) {
        return PresentationRectangle{0, 0, static_cast<std::int32_t>(client_width),
                                     static_cast<std::int32_t>(client_height)};
    }
    const double horizontal = static_cast<double>(client_width) /
                              static_cast<double>(source_width);
    const double vertical = static_cast<double>(client_height) /
                            static_cast<double>(source_height);
    double scale = std::min(horizontal, vertical);
    if (integer_scale && scale >= 1.0) scale = std::max(1.0, std::floor(scale));
    const std::uint32_t width = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::llround(static_cast<double>(source_width) * scale)),
        1u, client_width);
    const std::uint32_t height = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::llround(static_cast<double>(source_height) * scale)),
        1u, client_height);
    return PresentationRectangle{
        static_cast<std::int32_t>((client_width - width) / 2u),
        static_cast<std::int32_t>((client_height - height) / 2u),
        static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
}

[[nodiscard]] static InternalResolutionDimensions fit_psp_aspect(
    std::uint32_t monitor_width, std::uint32_t monitor_height) noexcept {
    const std::uint32_t width_from_height = std::max<std::uint32_t>(
        480u, static_cast<std::uint32_t>(
                  (static_cast<std::uint64_t>(monitor_height) * 480ull + 136ull) / 272ull));
    const std::uint32_t width = std::clamp(
        std::min(std::max(monitor_width, 480u), width_from_height), 480u, 16384u);
    const std::uint32_t height = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(width) * 272ull + 240ull) / 480ull),
        272u, 16384u);
    return {width, height};
}

InternalResolutionDimensions resolve_internal_resolution(
    const RenderingConfiguration &configuration) noexcept {
    switch (configuration.internal_resolution_mode) {
    case InternalResolutionMode::PspNative:
        return {480u, 272u};
    case InternalResolutionMode::Scale: {
        const std::uint32_t scale = std::clamp(configuration.internal_scale, 1u, 8u);
        return {480u * scale, 272u * scale};
    }
    case InternalResolutionMode::Custom:
        return {std::clamp(configuration.internal_width, 480u, 16384u),
                std::clamp(configuration.internal_height, 272u, 16384u)};
    case InternalResolutionMode::Desktop:
#if defined(_WIN32)
        return {static_cast<std::uint32_t>(std::max(480, GetSystemMetrics(SM_CXSCREEN))),
                static_cast<std::uint32_t>(std::max(272, GetSystemMetrics(SM_CYSCREEN)))};
#else
        return {std::clamp(configuration.internal_width, 480u, 16384u),
                std::clamp(configuration.internal_height, 272u, 16384u)};
#endif
    }
    return {480u, 272u};
}

SpongebobConfiguration load_spongebob_render_configuration(const std::filesystem::path &path) {
    SpongebobConfiguration config;
    config.source_path = path;

    std::ifstream input(path);
    if (!input) return config;
    config.loaded_from_file = true;

    std::string section;
    std::string raw_line;
    std::size_t line_number = 0u;
    while (std::getline(input, raw_line)) {
        ++line_number;
        if (line_number == 1u && raw_line.size() >= 3u &&
            static_cast<unsigned char>(raw_line[0]) == 0xEFu &&
            static_cast<unsigned char>(raw_line[1]) == 0xBBu &&
            static_cast<unsigned char>(raw_line[2]) == 0xBFu) {
            raw_line.erase(0u, 3u);
        }
        std::string line = trim_copy(raw_line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lowercase_copy(trim_copy(line.substr(1u, line.size() - 2u)));
            continue;
        }
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            warning(config, line_number, "expected key=value");
            continue;
        }
        const std::string key = lowercase_copy(trim_copy(line.substr(0u, separator)));
        const std::string value = strip_inline_comment(line.substr(separator + 1u));
        if (section == "display")
            apply_display_key(config, key, value, line_number);
        else if (section == "rendering")
            apply_rendering_key(config, key, value, line_number);
        else if (section == "audio")
            apply_audio_key(config, key, value, line_number);
        else if (section == "timing")
            apply_timing_key(config, key, value, line_number);
        else if (section == "diagnostics" || section == "logging")
            apply_diagnostics_key(config, key, value, line_number);
        else if (section == "widescreen")
            apply_widescreen_key(config, key, value, line_number);
        else if (section == "controls")
            apply_controls_key(config, key, value, line_number);
        else if (section == "project2dfx" || section == "project 2dfx" ||
                 section == "lodlights" || section == "lod lights" ||
                 section == "trafficlights" || section == "traffic lights" ||
                 section == "blinkinglights" || section == "blinking lights" ||
                 section == "skygfx" || section == "heliheight" ||
                 section == "heli height" || section == "drawdistance" ||
                 section == "draw distance" || section == "simulatehdr" ||
                 section == "simulate hdr") {
        }
        else if (section.empty())
            warning(config, line_number, "key outside a section");
        else
            warning(config, line_number, "unknown section [" + section + "]");
    }
    return config;
}

void initialize_spongebob_render_configuration(const std::filesystem::path &executable_directory) {
    std::filesystem::path path;
    if (const char *override_path = std::getenv("PSPRECOMP_CONFIG");
        override_path != nullptr && *override_path != '\0') {
        path = override_path;
    } else {
        path = executable_directory / "SpongebobNative.ini";
    }

    SpongebobConfiguration loaded = load_spongebob_render_configuration(path);
    loaded.initialized = true;
    loaded.executable_directory = executable_directory;
    {
        std::lock_guard<std::mutex> guard(global_configuration_mutex());
        global_configuration() = std::move(loaded);
    }

    const SpongebobConfiguration &config = global_configuration();
    if (config.timing.realtime_speed_diagnostics &&
        std::getenv("PSPRECOMP_REALTIME_SPEED_DIAG") == nullptr) {
        set_environment_value("PSPRECOMP_REALTIME_SPEED_DIAG", "1");
    }
    if (std::getenv("PSPRECOMP_REALTIME_SPEED_INTERVAL") == nullptr) {
        set_environment_value("PSPRECOMP_REALTIME_SPEED_INTERVAL",
                              std::to_string(config.timing.realtime_speed_interval_vblanks));
    }
    if (std::getenv("PSPRECOMP_GE_BACKEND") == nullptr) {
        const char *backend_name = "software";
        if (config.rendering.backend == RenderingBackend::DirectX12) backend_name = "directx12";
        else if (config.rendering.backend == RenderingBackend::Vulkan) backend_name = "vulkan";
        set_environment_value("PSPRECOMP_GE_BACKEND", backend_name);
    }
    const InternalResolutionDimensions internal =
        resolve_internal_resolution(config.rendering);
    if (std::getenv("PSPRECOMP_INTERNAL_WIDTH") == nullptr)
        set_environment_value("PSPRECOMP_INTERNAL_WIDTH", std::to_string(internal.width));
    if (std::getenv("PSPRECOMP_INTERNAL_HEIGHT") == nullptr)
        set_environment_value("PSPRECOMP_INTERNAL_HEIGHT", std::to_string(internal.height));
    if (std::getenv("PSPRECOMP_GE_GPU_TEXTURE_DECODE_LIMIT") == nullptr)
        set_environment_value("PSPRECOMP_GE_GPU_TEXTURE_DECODE_LIMIT",
                              std::to_string(config.rendering.texture_cache_entries));
    if (std::getenv("PSPRECOMP_GE_GPU_TEXTURE_CACHE_MB") == nullptr)
        set_environment_value("PSPRECOMP_GE_GPU_TEXTURE_CACHE_MB",
                              std::to_string(config.rendering.texture_cache_mb));
    if (config.rendering.experimental_gpu_color_preview &&
        std::getenv("PSPRECOMP_GE_GPU_COLOR_PREVIEW") == nullptr) {
        set_environment_value("PSPRECOMP_GE_GPU_COLOR_PREVIEW", "1");
    }
    if (config.rendering.gpu_geometry_debug_colors &&
        std::getenv("PSPRECOMP_GE_GPU_GEOMETRY_DEBUG_COLORS") == nullptr) {
        set_environment_value("PSPRECOMP_GE_GPU_GEOMETRY_DEBUG_COLORS", "1");
    }
    if (config.rendering.dump_gpu_frame_vblank != 0u &&
        std::getenv("PSPRECOMP_GE_GPU_DUMP_VBLANK") == nullptr) {
        set_environment_value("PSPRECOMP_GE_GPU_DUMP_VBLANK",
                              std::to_string(config.rendering.dump_gpu_frame_vblank));
    }
}

const SpongebobConfiguration &spongebob_render_configuration() {
    return global_configuration();
}

const char *display_resolution_mode_name(DisplayResolutionMode mode) noexcept {
    switch (mode) {
    case DisplayResolutionMode::PspNative: return "PSP";
    case DisplayResolutionMode::Custom: return "Custom";
    case DisplayResolutionMode::Desktop: return "Desktop";
    }
    return "Unknown";
}

const char *display_aspect_mode_name(DisplayAspectMode mode) noexcept {
    switch (mode) {
    case DisplayAspectMode::Preserve: return "Preserve";
    case DisplayAspectMode::Stretch: return "Stretch";
    }
    return "Unknown";
}

const char *display_upscale_filter_name(DisplayUpscaleFilter filter) noexcept {
    switch (filter) {
    case DisplayUpscaleFilter::Nearest: return "Nearest";
    case DisplayUpscaleFilter::Bilinear: return "Bilinear";
    }
    return "Unknown";
}

const char *internal_resolution_mode_name(InternalResolutionMode mode) noexcept {
    switch (mode) {
    case InternalResolutionMode::PspNative: return "PSP";
    case InternalResolutionMode::Scale: return "Scale";
    case InternalResolutionMode::Custom: return "Custom";
    case InternalResolutionMode::Desktop: return "Desktop";
    }
    return "Unknown";
}

const char *rendering_backend_name(RenderingBackend backend) noexcept {
    switch (backend) {
    case RenderingBackend::Software: return "Software";
    case RenderingBackend::DirectX12: return "DirectX 12";
    case RenderingBackend::Vulkan: return "Vulkan";
    }
    return "Unknown";
}

float resolve_widescreen_aspect_ratio(const SpongebobConfiguration &configuration,
                                      std::uint32_t surface_width,
                                      std::uint32_t surface_height) noexcept {
    if (!configuration.widescreen.enabled) return 0.0f;
    if (configuration.widescreen.aspect_x != 0u && configuration.widescreen.aspect_y != 0u) {
        return static_cast<float>(configuration.widescreen.aspect_x) /
               static_cast<float>(configuration.widescreen.aspect_y);
    }
    if (surface_width == 0u || surface_height == 0u) return 0.0f;
    return static_cast<float>(surface_width) / static_cast<float>(surface_height);
}

float widescreen_stretch_factor(const SpongebobConfiguration &configuration,
                                std::uint32_t surface_width,
                                std::uint32_t surface_height) noexcept {
    const float aspect =
        resolve_widescreen_aspect_ratio(configuration, surface_width, surface_height);
    if (!(aspect > 0.0f)) return 1.0f;
    const float factor = aspect / kGameNativeAspectRatio;
    if (!std::isfinite(factor) || factor <= 0.0f || std::abs(factor - 1.0f) < 0.01f) return 1.0f;
    return std::clamp(factor, 0.5f, 4.0f);
}

float widescreen_render_stretch() noexcept {
    const SpongebobConfiguration &config = spongebob_render_configuration();
    if (!config.initialized || !config.widescreen.enabled) return 1.0f;
    const InternalResolutionDimensions target = resolve_internal_resolution(config.rendering);
    return widescreen_stretch_factor(config, target.width, target.height);
}

float spongebob_widescreen_aspect(float native_aspect) noexcept {
    const float stretch = widescreen_render_stretch();
    return stretch == 1.0f ? native_aspect : native_aspect * stretch;
}

float spongebob_widescreen_extent(float native_extent) noexcept {
    return native_extent * widescreen_render_stretch();
}

}
