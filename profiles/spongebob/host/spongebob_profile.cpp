#include "spongebob_profile.hpp"
#include "spongebob_sas.hpp"
#include "spongebob_audio_output.hpp"
#include "display_window.hpp"
#include "ge_gpu_backend.hpp"
#include "ge_renderer.hpp"
#include "ge_diag.hpp"
#include "spongebob_frame_limit.hpp"
#include "spongebob_symbols.hpp"
#include "spongebob_render_config.hpp"
#include "game/spongebob_player.hpp"
#include "game/spongebob_enemy.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spongebob {
namespace {

// GE display-list execution state (mirrors the VCS GeListRecord/GeListTable).
enum class GeListState : std::uint32_t {
    None = 0u, Queued = 1u, Running = 2u, Completed = 3u, Stalled = 4u, Error = 5u,
};

struct GeState {
    std::array<std::uint32_t, 256> commands{};
    GeTransformState transform{};
    std::uint32_t offset_address{};
    std::uint32_t vertex_address{};
    std::uint32_t index_address{};
    bool bounding_box_result{true};
};

struct GeListRecord {
    std::uint32_t guest_id{};
    std::uint32_t list_address{};
    std::uint32_t stall_address{};
    std::uint32_t pc{};
    std::int32_t callback_id{-1};
    GeListState state{GeListState::None};
    bool has_saved_context{};
    std::array<std::uint32_t, 256> saved_commands{};
    GeTransformState saved_transform{};
};

struct GeListTable {
    std::uint32_t next_raw_id{1u};
    std::unordered_map<std::uint32_t, GeListRecord> lists;
    std::vector<std::uint32_t> queue;
};

// Host-backed file handles for the minimal IoFileMgr (Fase C). Each fd maps to a
// std::fstream open against the extracted game assets.
struct FileTable {
    std::int32_t next_fd{3};
    std::unordered_map<std::int32_t, std::fstream> files;
};

GeState ge_state{};
GeListTable ge_list_table{};
FileTable file_table{};

// fd -> native path, so the read tracer can flag .bik streaming chunks.
std::unordered_map<std::int32_t, std::string> file_table_paths;

// Minimal sceAudio state: one hardware channel per slot (VCS AudioChannelState).
struct AudioChannelState {
    bool reserved{};
    std::uint32_t sample_count{};
    std::uint32_t format{};      // 0 = stereo, 0x10 = mono
    std::uint32_t frequency{44100u};
    std::uint32_t left_volume{};
    std::uint32_t right_volume{};
    std::uint64_t busy_until_us{};
    std::uint64_t queued_frames{};
};
std::array<AudioChannelState, 8> audio_channels{};
// sceAudioOutput2 ("SRC") channel, always stereo s16. Mixed as host channel 8.
AudioChannelState audio_output2_channel{};
// PSPRECOMP_AUDIO_DIAG: blocks/peak submitted per Output2 reservation.
std::uint64_t output2_blocks{};
std::int32_t output2_peak{};
constexpr std::uint32_t kOutput2MixerChannel = 8u;

constexpr std::uint32_t kEdramBase = 0x04000000u;  // PSP VRAM base.
constexpr std::uint32_t kEdramSize = 0x00200000u;  // 2 MB.

// GE display-list command opcodes (the high byte of each 32-bit list word).
constexpr std::uint32_t kGeCommandNop = 0x00u;
constexpr std::uint32_t kGeCommandVertexAddress = 0x01u;
constexpr std::uint32_t kGeCommandIndexAddress = 0x02u;
constexpr std::uint32_t kGeCommandPrimitive = 0x04u;
constexpr std::uint32_t kGeCommandBoundingBox = 0x07u;
constexpr std::uint32_t kGeCommandJump = 0x08u;
constexpr std::uint32_t kGeCommandBoundingBoxJump = 0x09u;
constexpr std::uint32_t kGeCommandCall = 0x0Au;
constexpr std::uint32_t kGeCommandReturn = 0x0Bu;
constexpr std::uint32_t kGeCommandEnd = 0x0Cu;
constexpr std::uint32_t kGeCommandSignal = 0x0Eu;
constexpr std::uint32_t kGeCommandFinish = 0x0Fu;
constexpr std::uint32_t kGeCommandBase = 0x10u;
constexpr std::uint32_t kGeCommandOffsetAddress = 0x13u;
constexpr std::uint32_t kGeCommandOrigin = 0x14u;

// PSP relative address: (base << 20) | data24, added to the current offset.
std::uint32_t ge_relative_address(std::uint32_t data) {
    const std::uint32_t base_extended = ((ge_state.commands[kGeCommandBase] & 0x000F0000u) << 8u) |
                                        (data & 0x00FFFFFFu);
    return (ge_state.offset_address + base_extended) & 0x0FFFFFFFu;
}

// Instrumentation counters (diagnostics): how often the guest drives the GE and
// display, and whether it is actually presenting frames or spinning in place.
std::uint64_t ge_enqueue_count{};
std::uint64_t ge_list_sync_count{};
std::uint64_t ge_draw_sync_count{};
std::uint64_t frame_present_count{};
// PSPRECOMP_FRAME_PROFILE=1: where each frame's wall time goes. Every 120
// presented frames prints the average ms per frame for GE list translation,
// Vulkan finish + readback, window present (SDL scaling + menu), frame-pacing
// sleep, and the rest ("game": translated code, HLE, audio).
struct FrameProfile {
    bool enabled = std::getenv("PSPRECOMP_FRAME_PROFILE") != nullptr;
    std::uint64_t ge_ns{}, finish_ns{}, present_ns{}, sleep_ns{}, frames{};
    std::uint64_t ge_commands{}, ge_runs{}, ge_prim_ns{};
    std::chrono::steady_clock::time_point window_start{std::chrono::steady_clock::now()};
};
FrameProfile frame_profile;
struct ProfileScope {
    std::uint64_t *sink;
    std::chrono::steady_clock::time_point start;
    explicit ProfileScope(std::uint64_t &target)
        : sink(frame_profile.enabled ? &target : nullptr), start(std::chrono::steady_clock::now()) {}
    ~ProfileScope() {
        if (sink != nullptr)
            *sink += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
    }
};
void frame_profile_tick() {
    if (!frame_profile.enabled || ++frame_profile.frames < 120u) return;
    const auto now = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double, std::milli>(now - frame_profile.window_start).count();
    const double n = static_cast<double>(frame_profile.frames);
    const auto ms = [&](std::uint64_t ns) { return static_cast<double>(ns) / 1e6 / n; };
    const double frame = wall / n;
    const double game = frame - ms(frame_profile.ge_ns) - ms(frame_profile.finish_ns) -
                        ms(frame_profile.present_ns) - ms(frame_profile.sleep_ns);
    char line[400];
    std::snprintf(line, sizeof line,
                  "[frame-profile] %.2f ms/frame (%.0f fps): game %.2f | ge-lists %.2f | vulkan+readback %.2f | "
                  "present %.2f | pacing-sleep %.2f || ge: %.0f cmds %.1f runs, prims %.2f ms /frame",
                  frame, 1000.0 / frame, game, ms(frame_profile.ge_ns), ms(frame_profile.finish_ns),
                  ms(frame_profile.present_ns), ms(frame_profile.sleep_ns),
                  static_cast<double>(frame_profile.ge_commands) / n, static_cast<double>(frame_profile.ge_runs) / n,
                  ms(frame_profile.ge_prim_ns));
    std::cerr << line << "\n";
    static const bool ge_phase_diag = std::getenv("PSPRECOMP_GE_PHASE_DIAG") != nullptr;
    if (ge_phase_diag) {  // PSPRECOMP_GE_PHASE_DIAG: split of the ge-lists time
        const GePhaseTotals t = ge_phase_totals();
        std::snprintf(line, sizeof line,
                      "[frame-profile]   ge: draw-setup %.2f | texture-upload %.2f | vertex-decode %.2f | "
                      "gpu-stage %.2f | tri-prep %.2f | gpu-accumulate %.2f | %.0f prims %.0f verts /frame",
                      ms(t.draw_setup_ns), ms(t.texture_upload_ns), ms(t.vertex_decode_ns), ms(t.gpu_stage_ns),
                      ms(t.triangle_prep_ns), ms(t.gpu_accumulate_ns), static_cast<double>(t.primitives) / n,
                      static_cast<double>(t.vertices) / n);
        std::cerr << line << "\n";
        reset_ge_phase_totals();
    }
    frame_profile = FrameProfile{};
    frame_profile.window_start = now;
}

// Last perspective projection used by a 3D draw (PSPRECOMP_PLAYER_LOG).
std::array<float, 16> last_perspective_projection{};

// Last pad state handed to the game (for PSPRECOMP_PLAYER_RECORD).
std::uint32_t last_pad_buttons{};
std::uint8_t last_pad_lx{128u}, last_pad_ly{128u};
std::uint64_t vblank_wait_count{};
std::uint32_t last_frame_buffer{0u};
bool has_last_frame_buffer{false};
std::uint64_t starvation_tick_total{};
std::uint32_t busywait_tracked_pc{0u};
std::uint64_t busywait_same_pc_count{0u};
std::uint64_t frame_report_ticks{0u};

// Generic HLE stub: returns 0 in v0.
void register_generic_stub(psprecomp::Runtime &rt, const char *library, std::uint32_t nid) {
    rt.register_hle(library, nid, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        ctx.set_gpr(2, 0u);
    });
}

void save_ge_list_context(GeListRecord &record) {
    record.has_saved_context = true;
    record.saved_commands = ge_state.commands;
    record.saved_transform = ge_state.transform;
}

void restore_ge_list_context(const GeListRecord &record) {
    if (!record.has_saved_context) return;
    ge_state.commands = record.saved_commands;
    ge_state.transform = record.saved_transform;
}

bool execute_ge_list(psprecomp::Runtime &rt, GeListRecord &list) {
    constexpr std::uint32_t kMaximumCommandsPerRun = 4'000'000u;
    const auto read_command = [&](std::uint32_t address, std::uint32_t &value) -> bool {
        if (rt.memory().raw_pointer(address, 4u) == nullptr) return false;
        value = rt.memory().load32(address);
        return true;
    };
    struct CallFrame { std::uint32_t return_pc{}; std::uint32_t offset_address{}; std::uint32_t base{}; };
    std::vector<CallFrame> call_stack;

    list.state = GeListState::Running;
    ++frame_profile.ge_runs;
    std::uint32_t executed = 0u;
    while (executed < kMaximumCommandsPerRun) {
        const std::uint32_t pc = list.pc;
        if ((pc & 3u) != 0u) { list.state = GeListState::Error; return false; }
        std::uint32_t op{};
        if (!read_command(pc, op)) { list.state = GeListState::Error; return false; }

        const std::uint32_t command = op >> 24u;
        const std::uint32_t data = op & 0x00FFFFFFu;
        ++frame_profile.ge_commands;
        // PSPRECOMP_GE_LIST_DIAG: opcode and PC-page histogram, printed every 2M commands.
        static const bool list_diag = std::getenv("PSPRECOMP_GE_LIST_DIAG") != nullptr;
        if (list_diag) {
            static std::array<std::uint64_t, 256> opcode_hist{};
            static std::unordered_map<std::uint32_t, std::uint64_t> page_hist;
            static std::uint64_t seen = 0u;
            ++opcode_hist[command];
            ++page_hist[pc & ~0xFFFu];
            if (++seen % 2000000u == 0u) {
                std::cerr << "[ge-list] top opcodes:";
                for (int k = 0; k < 6; ++k) {
                    const auto top = std::max_element(opcode_hist.begin(), opcode_hist.end());
                    std::cerr << " 0x" << std::hex << (top - opcode_hist.begin()) << std::dec << "=" << *top;
                    *top = 0u;
                }
                std::cerr << " | top pages:";
                for (int k = 0; k < 4 && !page_hist.empty(); ++k) {
                    auto top = std::max_element(page_hist.begin(), page_hist.end(),
                                                [](const auto &a, const auto &b) { return a.second < b.second; });
                    std::cerr << " " << psprecomp::hex32(top->first) << "=" << top->second;
                    page_hist.erase(top);
                }
                std::cerr << "\n";
                opcode_hist.fill(0u);
                page_hist.clear();
            }
        }

        ge_state.commands[command] = op;
        // 0x2A-0x41: bone, world, view, projection and texture (0x40 TMS / 0x41
        // TMD) matrices. Stopping at 0x3F dropped the texture matrix, leaving
        // it all zeros: uvgen=1 surfaces (water, stomach acid) sampled one texel.
        if (command >= 0x2Au && command <= 0x41u)
            update_ge_transform_state(ge_state.transform, command, data);

        ++executed;
        std::uint32_t next_pc = (pc + 4u) & 0x0FFFFFFFu;

        switch (command) {
        case kGeCommandNop:
            break;
        case kGeCommandVertexAddress:
            ge_state.vertex_address = ge_relative_address(data);
            break;
        case kGeCommandIndexAddress:
            ge_state.index_address = ge_relative_address(data);
            break;
        case kGeCommandPrimitive: {
            ProfileScope prim_scope(frame_profile.ge_prim_ns);
            if (ge_state.transform.projection[11] != 0.0f)  // perspective (w = -z)
                last_perspective_projection = ge_state.transform.projection;
            GeRenderStats stats{};
            std::string error;
            // Draw distance (Rendering.DrawDistance / F1 menu): push the far plane of
            // perspective projections out by an integer factor, for rendering only.
            // The projection is OpenGL-style: m22 = -(f+n)/(f-n), m32 = -2fn/(f-n).
            const std::uint32_t far_scale = spongebob_render_configuration().rendering.draw_distance_scale;
            const auto &projection = ge_state.transform.projection;
            const bool extend_far = far_scale > 1u && projection[11] != 0.0f && projection[10] != -1.0f &&
                                    projection[10] != 1.0f;
            GeTransformState extended;
            if (extend_far) {
                extended = ge_state.transform;
                const float a = projection[10], b = projection[14];
                const float near_plane = b / (a - 1.0f), far_plane = b / (a + 1.0f);
                if (near_plane > 0.0f && far_plane > near_plane && std::isfinite(far_plane)) {
                    const float new_far = far_plane * static_cast<float>(far_scale);
                    extended.projection[10] = -(new_far + near_plane) / (new_far - near_plane);
                    extended.projection[14] = -2.0f * new_far * near_plane / (new_far - near_plane);
                }
            }
            if (!render_ge_primitive(rt.memory(), ge_state.commands, extend_far ? extended : ge_state.transform,
                                     ge_state.vertex_address, ge_state.index_address,
                                     data, stats, error)) {
                list.state = GeListState::Error;
                std::cerr << "[ge] rasterizer failed pc=" << psprecomp::hex32(pc)
                          << ": " << error << "\n";
                return false;
            }
            ge_state.vertex_address = stats.next_vertex_address;
            ge_state.index_address = stats.next_index_address;
            break;
        }
        case kGeCommandBoundingBox: {
            GeBoundingBoxResult result{};
            std::string error;
            if (!test_ge_bounding_box(rt.memory(), ge_state.commands, ge_state.transform,
                                      ge_state.vertex_address, ge_state.index_address,
                                      data & 0xFFFFu, result, error)) {
                list.state = GeListState::Error;
                return false;
            }
            ge_state.bounding_box_result = result.visible;
            ge_state.vertex_address = result.next_vertex_address;
            ge_state.index_address = result.next_index_address;
            break;
        }
        case kGeCommandJump:
            next_pc = ge_relative_address(data & 0x00FFFFFCu);
            break;
        case kGeCommandBoundingBoxJump:
            if (!ge_state.bounding_box_result)
                next_pc = ge_relative_address(data & 0x00FFFFFCu);
            break;
        case kGeCommandCall:
            call_stack.push_back(CallFrame{next_pc, ge_state.offset_address,
                                           ge_state.commands[kGeCommandBase]});
            next_pc = ge_relative_address(data & 0x00FFFFFCu);
            break;
        case kGeCommandReturn:
            if (call_stack.empty()) { list.state = GeListState::Error; return false; }
            {
                const CallFrame frame = call_stack.back();
                call_stack.pop_back();
                ge_state.offset_address = frame.offset_address;
                ge_state.commands[kGeCommandBase] = frame.base;
                next_pc = frame.return_pc & 0x0FFFFFFFu;
            }
            break;
        case kGeCommandOffsetAddress:
            ge_state.offset_address = op << 8u;
            break;
        case kGeCommandOrigin:
            ge_state.offset_address = pc;
            break;
        case kGeCommandEnd: {
            std::uint32_t previous{};
            if (pc >= 4u && read_command(pc - 4u, previous) &&
                (previous >> 24u) == kGeCommandFinish) {
                static const bool diag = std::getenv("PSPRECOMP_GE_LIST_DIAG") != nullptr;
                static int shown = 0;
                if (diag && shown < 8) {
                    ++shown;
                    std::cerr << "[ge-list] FINISH+END list=" << psprecomp::hex32(list.list_address)
                              << " end_pc=" << psprecomp::hex32(pc) << " executed=" << executed
                              << " bytes_from_start=" << (pc - list.list_address) << "\n";
                }
                list.state = GeListState::Completed;
                list.pc = next_pc;
                return true;
            }
            const std::uint32_t link = ge_relative_address(data);
            {
                static const bool diag = std::getenv("PSPRECOMP_GE_LIST_DIAG") != nullptr;
                static int shown = 0;
                if (diag && link != 0u && link != pc && shown < 12) {
                    ++shown;
                    std::cerr << "[ge-list] END links: pc=" << psprecomp::hex32(pc) << " end_data="
                              << psprecomp::hex32(data) << " prev=" << psprecomp::hex32(previous)
                              << " -> " << psprecomp::hex32(link) << " list=" << psprecomp::hex32(list.list_address)
                              << " stall=" << psprecomp::hex32(list.stall_address) << "\n";
                }
            }
            if (link == 0u || link == pc) {
                static const bool diag = std::getenv("PSPRECOMP_GE_LIST_DIAG") != nullptr;
                if (diag)
                    std::cerr << "[ge-list] list=" << psprecomp::hex32(list.list_address)
                              << " stall=" << psprecomp::hex32(list.stall_address) << " end_pc=" << psprecomp::hex32(pc)
                              << " executed=" << executed << "\n";
                list.state = GeListState::Completed;
                return true;
            }
            next_pc = link;
            break;
        }
        case kGeCommandSignal:
        case kGeCommandFinish:
            break;
        default:
            break;
        }
        {
            // PSPRECOMP_GE_LIST_DIAG: trace control flow of one list run (the 300th).
            static const bool diag = std::getenv("PSPRECOMP_GE_LIST_DIAG") != nullptr;
            static std::uint64_t run_id = 0u;
            constexpr std::uint64_t traced_run = 300u;
            if (diag && executed == 1u) ++run_id;
            if (diag && run_id == traced_run && next_pc != ((pc + 4u) & 0x0FFFFFFFu) && executed < 200000u)
                std::cerr << "[ge-flow] pc=" << psprecomp::hex32(pc) << " op=" << psprecomp::hex32(op)
                          << " -> " << psprecomp::hex32(next_pc) << " base=" << psprecomp::hex32(ge_state.commands[kGeCommandBase])
                          << " offset=" << psprecomp::hex32(ge_state.offset_address) << " depth=" << call_stack.size() << "\n";
        }
        list.pc = next_pc;
    }

    list.state = GeListState::Error;
    std::cerr << "[ge] display list exceeded command safety limit pc="
              << psprecomp::hex32(list.pc) << "\n";
    return false;
}

void ge_list_enqueue(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t list_address = ctx.gpr[4];
    const std::uint32_t stall_address = ctx.gpr[5];
    const std::uint32_t callback_id = ctx.gpr[6];
    ++ge_enqueue_count;
    std::cerr << "[ge] enqueue list=" << psprecomp::hex32(list_address)
              << " pc=" << psprecomp::hex32(ctx.gpr[31]) << "\n";
    GeListRecord record{};
    record.guest_id = ge_list_table.next_raw_id++;
    record.list_address = list_address;
    record.stall_address = stall_address;
    record.pc = list_address;
    record.callback_id = static_cast<std::int32_t>(callback_id);
    record.state = GeListState::Queued;
    const std::uint32_t guest_id = record.guest_id;
    ge_list_table.lists[guest_id] = std::move(record);
    ge_list_table.queue.push_back(guest_id);
    ctx.set_gpr(2, guest_id);
    {
        ProfileScope scope(frame_profile.ge_ns);
        (void)execute_ge_list(rt, ge_list_table.lists[guest_id]);
    }
}

void ge_list_sync(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t list_id = ctx.gpr[4];
    ++ge_list_sync_count;
    std::cerr << "[ge] sync list=" << psprecomp::hex32(list_id)
              << " count=" << ge_list_sync_count
              << " pc=" << psprecomp::hex32(ctx.gpr[31]) << "\n";
    const auto it = ge_list_table.lists.find(list_id);
    if (it != ge_list_table.lists.end()) it->second.state = GeListState::Completed;
    ctx.set_gpr(2, 0u);
}

void ge_draw_sync(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    ++ge_draw_sync_count;
    std::cerr << "[ge] draw_sync count=" << ge_draw_sync_count
              << " pc=" << psprecomp::hex32(ctx.gpr[31]) << "\n";
    ctx.set_gpr(2, 0u);
}

void register_ge_hle(psprecomp::Runtime &rt) {
    rt.register_hle("sceGe_user", 0xE47E40E4u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, kEdramBase); });  // sceGeEdramGetAddr
    rt.register_hle("sceGe_user", 0x1F6752ADu, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, kEdramSize); });  // sceGeEdramGetSize
    rt.register_hle("sceGe_user", 0xB77905EAu, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });          // sceGeEdramSetAddrTranslation
    rt.register_hle("sceGe_user", 0xAB49E76Au, ge_list_enqueue);  // sceGeListEnQueue
    rt.register_hle("sceGe_user", 0x1C0D95A6u, ge_list_enqueue);  // sceGeListEnQueueHead
    rt.register_hle("sceGe_user", 0xE0D68148u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });          // sceGeListUpdateStallAddr
    rt.register_hle("sceGe_user", 0x03444EB4u, ge_list_sync);     // sceGeListSync
    rt.register_hle("sceGe_user", 0xB287BD61u, ge_draw_sync);     // sceGeDrawSync
    rt.register_hle("sceGe_user", 0x4C06E472u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });          // sceGeContinue
    rt.register_hle("sceGe_user", 0xA4FC06A4u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });          // sceGeSetCallback
    rt.register_hle("sceGe_user", 0x05DB22CEu, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });          // sceGeUnsetCallback
    rt.register_hle("sceGe_user", 0xDC93CFEFu, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });          // sceGeGetCmd
    rt.register_hle("sceGe_user", 0x438A385Au, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });          // sceGeSaveContext
    rt.register_hle("sceGe_user", 0x0BF608FBu, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });          // sceGeRestoreContext
}

// sceAtrac3plus (8) — stubs (Opción A: sin FFmpeg).
void register_atrac3_stubs(psprecomp::Runtime &rt) {
    register_generic_stub(rt, "sceAtrac3plus", 0x5D268707u);
    register_generic_stub(rt, "sceAtrac3plus", 0x61EB33F5u);
    register_generic_stub(rt, "sceAtrac3plus", 0x6A8C3CD5u);
    register_generic_stub(rt, "sceAtrac3plus", 0x7A20E7AFu);
    register_generic_stub(rt, "sceAtrac3plus", 0x7DB31251u);
    register_generic_stub(rt, "sceAtrac3plus", 0x868120B5u);
    register_generic_stub(rt, "sceAtrac3plus", 0x9AE849A7u);
    register_generic_stub(rt, "sceAtrac3plus", 0xE88F759Bu);
}

struct ImportStub { const char *library; std::uint32_t nid; };
static constexpr ImportStub kOtherStubs[] = {
    {"IoFileMgrForUser", 0x06A70004u}, {"IoFileMgrForUser", 0x109F50BCu}, {"IoFileMgrForUser", 0x1117C65Fu},
    {"IoFileMgrForUser", 0x27EB27B8u}, {"IoFileMgrForUser", 0x3251EA56u}, {"IoFileMgrForUser", 0x42EC03ACu},
    {"IoFileMgrForUser", 0x54F5FB11u}, {"IoFileMgrForUser", 0x6A638D83u}, {"IoFileMgrForUser", 0x779103A0u},
    {"IoFileMgrForUser", 0x810C4BC3u}, {"IoFileMgrForUser", 0xA0B5A7C2u}, {"IoFileMgrForUser", 0xACE946E8u},
    {"IoFileMgrForUser", 0xB29DDF9Cu}, {"IoFileMgrForUser", 0xE3EB004Cu}, {"IoFileMgrForUser", 0xEB092469u},
    {"IoFileMgrForUser", 0xF27A9C51u},
    {"Kernel_Library", 0x092968F4u}, {"Kernel_Library", 0x3B84732Du}, {"Kernel_Library", 0x5F10D406u},
    {"LoadExecForUser", 0x05572A5Fu}, {"LoadExecForUser", 0x4AC57943u},
    {"ModuleMgrForUser", 0x2E0911AAu}, {"ModuleMgrForUser", 0x8F2DF740u}, {"ModuleMgrForUser", 0xD1FF982Au},
    {"ModuleMgrForUser", 0xD8B73127u}, {"ModuleMgrForUser", 0xF0A26395u},
    {"sceAudio", 0x01562BA3u}, {"sceAudio", 0x136CAF51u}, {"sceAudio", 0x13F592BCu}, {"sceAudio", 0x2D53F36Eu},
    {"sceAudio", 0x43196845u}, {"sceAudio", 0x5EC81C55u}, {"sceAudio", 0x647CEF33u}, {"sceAudio", 0x6FC46853u},
    {"sceAudio", 0x95FD0C2Du}, {"sceAudio", 0xB011922Fu}, {"sceAudio", 0xB7E1D8E7u}, {"sceAudio", 0xCB2E439Eu},
    {"sceAudio", 0xE2D56B2Du},
    {"sceCtrl", 0x1F4011E6u}, {"sceCtrl", 0x1F803938u}, {"sceCtrl", 0x3A622550u},
    {"sceImpose", 0x36AA6E91u},
    {"sceNet", 0x0BF0A3AEu}, {"sceNet", 0x281928A9u}, {"sceNet", 0x39AF39A6u},
    {"sceNetAdhoc", 0x6F92741Bu}, {"sceNetAdhoc", 0x7F27BB5Eu}, {"sceNetAdhoc", 0xA62C6F57u},
    {"sceNetAdhoc", 0xABED3790u}, {"sceNetAdhoc", 0xDFE53E03u}, {"sceNetAdhoc", 0xE1D621D7u},
    {"sceNetAdhocctl", 0x08FFF7A0u}, {"sceNetAdhocctl", 0x20B317A0u}, {"sceNetAdhocctl", 0x34401D65u},
    {"sceNetAdhocctl", 0x5E7F79C9u}, {"sceNetAdhocctl", 0x6402490Bu}, {"sceNetAdhocctl", 0x81AEE1BEu},
    {"sceNetAdhocctl", 0x9D689E13u}, {"sceNetAdhocctl", 0xE162CB14u}, {"sceNetAdhocctl", 0xE26F226Eu},
    {"sceNetAdhocctl", 0xEC0635C1u},
    {"scePower", 0x04B7766Eu}, {"scePower", 0xDFA8BAF8u},
    {"sceSuspendForUser", 0x090CCB3Fu},
    {"sceUmdUser", 0x46EBB729u}, {"sceUmdUser", 0x8EF08FCEu}, {"sceUmdUser", 0xC6183D47u},
    {"sceUtility", 0x2A2B3DE0u}, {"sceUtility", 0x2AD8E239u}, {"sceUtility", 0x34B78343u}, {"sceUtility", 0x50C4CD57u},
    {"sceUtility", 0x67AF3428u}, {"sceUtility", 0x8874DBE0u}, {"sceUtility", 0x95FC253Bu}, {"sceUtility", 0x9790B33Cu},
    {"sceUtility", 0x9A1C91D7u}, {"sceUtility", 0xA5DA2406u}, {"sceUtility", 0xD4B95FFBu}, {"sceUtility", 0xE49BFE92u},
    {"sceWlanDrv", 0xD7763699u},
    {"StdioForUser", 0x172D316Eu}, {"StdioForUser", 0xA6BAB2E9u}, {"StdioForUser", 0xF78BA90Au},
    {"UtilsForUser", 0x27CC57F0u}, {"UtilsForUser", 0x3EE30821u}, {"UtilsForUser", 0x71EC4271u},
    {"UtilsForUser", 0x79D1C3FAu}, {"UtilsForUser", 0x91E4F6A7u},
};

void register_other_stubs(psprecomp::Runtime &rt) {
    for (const ImportStub &stub : kOtherStubs) {
        register_generic_stub(rt, stub.library, stub.nid);
    }

    // sceUmdGetDriveStat — the boot polls this until it reports "disc inserted"
    // (state 2) with a 100 ms delay in between. Returning 0 (the generic stub)
    // made that poll spin forever. Report the disc as inserted so the boot can
    // advance past the UMD wait.
    rt.register_hle("sceUmdUser", 0x6B4A146Cu,  // sceUmdGetDriveStat
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            static std::uint32_t calls = 0u;
            ++calls;
            if (calls <= 40u)
                std::cerr << "[umd] GetDriveStat #" << calls << " uid="
                          << psprecomp::runtime_thread_uid() << " -> 0x32\n";
            ctx.set_gpr(2, 0x32u);
        });
    rt.register_hle("sceUmdUser", 0x46EBB729u,  // sceUmdCheckMedium
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            static std::uint32_t calls = 0u;
            ++calls;
            if (calls <= 40u)
                std::cerr << "[umd] CheckMedium #" << calls << " uid="
                          << psprecomp::runtime_thread_uid() << " -> 1\n";
            ctx.set_gpr(2, 1u);
        });
    rt.register_hle("sceUmdUser", 0x8EF08FCEu,  // sceUmdWaitDriveStat
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            static std::uint32_t calls = 0u;
            ++calls;
            if (calls <= 40u)
                std::cerr << "[umd] WaitDriveStat #" << calls << " uid="
                          << psprecomp::runtime_thread_uid() << " a0="
                          << psprecomp::hex32(ctx.gpr[4]) << " -> 0\n";
            ctx.set_gpr(2, 0u);
        });
    rt.register_hle("sceUmdUser", 0xC6183D47u,  // sceUmdActivate
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            static std::uint32_t calls = 0u;
            ++calls;
            if (calls <= 40u)
                std::cerr << "[umd] Activate #" << calls << " uid="
                          << psprecomp::runtime_thread_uid() << " a0="
                          << psprecomp::hex32(ctx.gpr[4]) << " -> 0\n";
            ctx.set_gpr(2, 0u);
        });

    // Minimal sceCtrl (input): expose the host keyboard/gamepad as the PSP pad.
    rt.register_hle("sceCtrl", 0x1F4011E6u,  // sceCtrlSetSamplingMode
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    const auto write_pad = [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        const std::uint32_t destination = ctx.gpr[4];
        const std::uint32_t count = ctx.gpr[5];
        if (count == 0u) { ctx.set_gpr(2, 0u); return; }
        constexpr std::uint32_t kSampleSize = 16u;
        if (count > 64u ||
            rt.memory().raw_pointer(destination, static_cast<std::size_t>(count) * kSampleSize) == nullptr) {
            ctx.set_gpr(2, 0x80000103u);
            return;
        }
        const HostInputState input = display_window_input();
        // Headless testing: OR in a synthetic button so the game can advance
        // past "press any button" without a real keyboard.
        std::uint32_t buttons = input.buttons;
        if (const char *sim = std::getenv("PSPRECOMP_SIMULATE_BUTTON"))
            buttons |= static_cast<std::uint32_t>(std::strtoul(sim, nullptr, 0));
        // PSPRECOMP_INPUT_SCRIPT="first-last=mask[@lx:ly],..." holds `mask` pressed
        // (and optionally the left stick at lx,ly; 128 = centre) during presented
        // frames [first, last], so menus see press/release edges.
        struct ScriptedPress {
            std::uint64_t first, last;
            std::uint32_t mask;
            int lx = -1, ly = -1;
        };
        static const std::vector<ScriptedPress> script = [] {
            std::vector<ScriptedPress> list;
            const char *text = std::getenv("PSPRECOMP_INPUT_SCRIPT");
            for (const char *p = text; p != nullptr && *p != '\0';) {
                char *end = nullptr;
                ScriptedPress press{};
                press.first = std::strtoull(p, &end, 0);
                if (end == p || *end != '-') break;
                p = end + 1;
                press.last = std::strtoull(p, &end, 0);
                if (end == p || *end != '=') break;
                p = end + 1;
                press.mask = static_cast<std::uint32_t>(std::strtoul(p, &end, 0));
                if (end == p) break;
                if (*end == '@') {
                    press.lx = static_cast<int>(std::strtol(end + 1, &end, 0));
                    if (*end == ':') press.ly = static_cast<int>(std::strtol(end + 1, &end, 0));
                }
                list.push_back(press);
                p = (*end == ',') ? end + 1 : end;
            }
            return list;
        }();
        std::uint8_t analog_x = input.analog_x, analog_y = input.analog_y;
        for (const ScriptedPress &press : script) {
            if (frame_present_count >= press.first && frame_present_count <= press.last) {
                buttons |= press.mask;
                if (press.lx >= 0) analog_x = static_cast<std::uint8_t>(std::clamp(press.lx, 0, 255));
                if (press.ly >= 0) analog_y = static_cast<std::uint8_t>(std::clamp(press.ly, 0, 255));
            }
        }
        for (std::uint32_t i = 0u; i < count; ++i) {
            const std::uint32_t sample = destination + i * kSampleSize;
            rt.memory().store32(sample, 0u);              // TimeStamp
            rt.memory().store32(sample + 4u, buttons);    // Buttons (PSP bitmask)
            rt.memory().store8(sample + 8u, analog_x);  // Lx
            rt.memory().store8(sample + 9u, analog_y);  // Ly
            rt.memory().store8(sample + 10u, 128u);           // rx
            rt.memory().store8(sample + 11u, 128u);           // ry
            rt.memory().zero(sample + 12u, 4u);               // Rsrv
        }
        last_pad_buttons = buttons;
        last_pad_lx = analog_x;
        last_pad_ly = analog_y;
        ctx.set_gpr(2, count);
    };
    rt.register_hle("sceCtrl", 0x3A622550u,  // sceCtrlPeekBufferPositive
        [write_pad](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { write_pad(rt, ctx); });
    rt.register_hle("sceCtrl", 0x1F803938u,  // sceCtrlReadBufferPositive
        [write_pad](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { write_pad(rt, ctx); });

    // sceIoGetstat: the boot polls this 444k times while the asset loader waits
    // for files. The generic stub returned 0 with an uninitialised SceIoStat, so
    // the guest read garbage and re-polled forever. Fill a real SceIoStat and
    // return ENOENT for files that are not present on the host.
    rt.register_hle("IoFileMgrForUser", 0xACE946E8u,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            static std::size_t traced = 0u;
            const std::string path = rt.memory().read_c_string(ctx.gpr[4]);
            if (traced < 30u) {
                ++traced;
                std::cerr << "[io] getstat uid=" << psprecomp::runtime_thread_uid()
                          << " path=\"" << path << "\" ra=" << psprecomp::hex32(ctx.gpr[31]) << "\n";
            }
            const std::uint32_t stat_address = ctx.gpr[5];
            if (stat_address == 0u || rt.memory().raw_pointer(stat_address, 0x58u) == nullptr) {
                ctx.set_gpr(2, 0x80010016u);  // EINVAL
                return;
            }
            try {
                std::error_code error;
                const std::filesystem::path native = rt.translate_path(path);
                const bool directory = std::filesystem::is_directory(native, error);
                const bool regular = std::filesystem::is_regular_file(native, error);
                if (!directory && !regular) { ctx.set_gpr(2, 0x80010002u); return; }  // ENOENT
                rt.memory().zero(stat_address, 0x58u);
                rt.memory().store32(stat_address + 0x00u, (directory ? 0x1000u : 0x2000u) | 0x01FFu);
                rt.memory().store32(stat_address + 0x04u, directory ? 0x0010u : 0x0020u);
                const std::uint64_t size = regular
                    ? static_cast<std::uint64_t>(std::filesystem::file_size(native, error)) : 0u;
                rt.memory().store32(stat_address + 0x08u, static_cast<std::uint32_t>(size));
                rt.memory().store32(stat_address + 0x0Cu, static_cast<std::uint32_t>(size >> 32u));
                ctx.set_gpr(2, 0u);
            } catch (...) {
                ctx.set_gpr(2, 0x80010002u);  // ENOENT
            }
        });

    // sceIoOpen: open a host file for the guest and hand back a real fd.
    rt.register_hle("IoFileMgrForUser", 0x109F50BCu,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::string path = rt.memory().read_c_string(ctx.gpr[4]);
            const std::uint32_t flags = ctx.gpr[5];
            std::ios::openmode mode = std::ios::binary;
            const bool read = (flags & 0x0001u) != 0u || (flags & 0x0004u) != 0u;
            const bool write = (flags & 0x0002u) != 0u || (flags & 0x0004u) != 0u ||
                               (flags & 0x0008u) != 0u;
            if (read) mode |= std::ios::in;
            if (write) mode |= std::ios::out;

            try {
                static std::size_t traced = 0u;
                const std::filesystem::path native = rt.translate_path(path);
                std::fstream stream(native, mode);
                const bool ok = static_cast<bool>(stream);
                if (traced < 30u) {
                    ++traced;
                    std::cerr << "[io] open \"" << path << "\" -> \"" << native.string()
                              << "\" " << (ok ? "OK" : "FAIL") << "\n";
                }
                if (!ok) { ctx.set_gpr(2, 0x80010002u); return; }  // ENOENT
                const std::int32_t fd = file_table.next_fd++;
                file_table.files.emplace(fd, std::move(stream));
                file_table_paths[fd] = native.string();
                ctx.set_gpr(2, static_cast<std::uint32_t>(fd));
            } catch (...) {
                ctx.set_gpr(2, 0x80010002u);  // ENOENT
            }
        });

    // sceIoRead: copy bytes from a host file into guest memory.
    rt.register_hle("IoFileMgrForUser", 0x6A638D83u,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            static std::size_t traced = 0u;
            const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
            const std::uint32_t dst = ctx.gpr[5];
            const std::uint32_t size = ctx.gpr[6];
            const auto it = file_table.files.find(fd);
            if (it == file_table.files.end()) { ctx.set_gpr(2, 0x80010009u); return; }  // EBADF
            const auto path_it = file_table_paths.find(fd);
            const bool is_bik = path_it != file_table_paths.end() &&
                path_it->second.size() >= 4u &&
                (path_it->second.compare(path_it->second.size() - 4u, 4u, ".bik") == 0 ||
                 path_it->second.compare(path_it->second.size() - 4u, 4u, ".BIK") == 0);

            std::uint8_t *guest = rt.memory().raw_pointer(dst, size);
            if (guest == nullptr) { ctx.set_gpr(2, 0x80010009u); return; }  // EBADF

            it->second.read(reinterpret_cast<char *>(guest), static_cast<std::streamsize>(size));
            const std::streamsize nread = it->second.gcount();
            const std::uint32_t result = nread >= 0 ? static_cast<std::uint32_t>(nread) : 0u;
            if (traced < 30u) {
                ++traced;
                std::cerr << "[io] read fd=" << fd << " size=" << size << " -> " << result
                          << (is_bik ? " [bik chunk]" : "") << "\n";
            }
            ctx.set_gpr(2, result);
        });

    // sceIoLseek (64-bit offset): the Bink decoder seeks through .bik files.
    rt.register_hle("IoFileMgrForUser", 0x27EB27B8u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            static std::size_t traced = 0u;
            const auto fd = static_cast<std::int32_t>(ctx.gpr[4]);
            const std::uint64_t raw_offset = static_cast<std::uint64_t>(ctx.gpr[6]) |
                (static_cast<std::uint64_t>(ctx.gpr[7]) << 32u);
            const auto offset = static_cast<std::int64_t>(raw_offset);
            const auto whence = static_cast<std::int32_t>(ctx.gpr[8]);
            const auto it = file_table.files.find(fd);
            if (it == file_table.files.end() || whence < 0 || whence > 2) {
                if (traced < 30u) { ++traced; std::cerr << "[io] lseek reject fd=" << fd << " whence=" << whence << "\n"; }
                ctx.set_gpr(2, 0x80010009u);  // EBADF
                ctx.set_gpr(3, 0xFFFFFFFFu);
                return;
            }
            std::ios_base::seekdir direction = std::ios::beg;
            if (whence == 1) direction = std::ios::cur;
            if (whence == 2) direction = std::ios::end;
            it->second.clear();
            it->second.seekg(static_cast<std::streamoff>(offset), direction);
            if (!it->second) {
                ctx.set_gpr(2, 0x80010016u);  // EINVAL
                ctx.set_gpr(3, 0xFFFFFFFFu);
                return;
            }
            const auto position = static_cast<std::int64_t>(it->second.tellg());
            if (position < 0) {
                ctx.set_gpr(2, 0x80010016u);  // EINVAL
                ctx.set_gpr(3, 0xFFFFFFFFu);
                return;
            }
            const auto result = static_cast<std::uint64_t>(position);
            ctx.set_gpr(2, static_cast<std::uint32_t>(result));
            ctx.set_gpr(3, static_cast<std::uint32_t>(result >> 32u));
            if (traced < 30u) {
                ++traced;
                std::cerr << "[io] lseek fd=" << fd << " offset=" << offset
                          << " whence=" << whence << " -> " << position << "\n";
            }
        });

    // sceIoClose: close and drop the host file handle.
    rt.register_hle("IoFileMgrForUser", 0x810C4BC3u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
            const bool closed = file_table.files.erase(fd) == 1u;
            ctx.set_gpr(2, closed ? 0u : 0x80010009u);  // EBADF
        });

    // sceIoWrite: copy bytes from guest memory into a host file.
    rt.register_hle("IoFileMgrForUser", 0x42EC03ACu,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
            const std::uint32_t src = ctx.gpr[5];
            const std::uint32_t size = ctx.gpr[6];
            const auto it = file_table.files.find(fd);
            if (it == file_table.files.end()) { ctx.set_gpr(2, 0x80010009u); return; }
            const std::uint8_t *guest = rt.memory().raw_pointer(src, size);
            if (guest == nullptr) { ctx.set_gpr(2, 0x80010009u); return; }
            it->second.write(reinterpret_cast<const char *>(guest), static_cast<std::streamsize>(size));
            ctx.set_gpr(2, size);
        });
}

// ---------------------------------------------------------------------------
// Fase A: minimal SysMem + ThreadMan HLE.
// ---------------------------------------------------------------------------

enum class ThreadState : std::uint8_t { Ready, Running, Waiting, Completed };

struct ThreadRecord {
    std::uint32_t uid{};
    std::string name;
    std::uint32_t entry{};
    std::uint32_t stack_base{};
    std::uint32_t stack_size{};
    std::uint32_t kernel_context{};  // k0 (gpr[26]): thread control block, stack_top - 0x100.
    psprecomp::AllegrexContext context{};
    ThreadState state{ThreadState::Ready};
    std::uint32_t exit_status{};
    std::uint64_t wake_at_us{};  // 0 == not waiting; else virtual-time wake deadline.
    std::uint32_t waiting_on_thread{0u};  // 0 == not waiting on a thread; else uid being waited on.
    std::uint32_t waiting_on_mutex{0u};   // 0 == not blocked in LockMutex; else mutex uid.
    std::uint32_t mutex_lock_count{0u};   // count requested by the blocked LockMutex.
    std::uint32_t timeout_ptr{0u};        // guest SceUInt* of a timed wait (0 == none).
};

struct ThreadTable {
    std::uint32_t current_uid{1u};              // uid 1 == module_start thread.
    std::uint32_t next_uid{2u};
    std::uint32_t next_stack_top{0x0A000000u};  // PSP user RAM top; stacks grow down.
    // Stacks of deleted threads, reused before carving new ones: Bink creates
    // two short-lived 32 KiB threads per movie.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> free_stacks;  // {base, size}
    std::map<std::uint32_t, ThreadRecord> threads;
    std::vector<std::uint32_t> ready_queue;
};

ThreadTable thread_table{};

// Kernel mutexes (sceKernelCreateMutex & co). Bink's audio helper thread uses
// one as its stop signal: it polls LockMutex with a 5 ms timeout and keeps
// decoding while it gets WAIT_TIMEOUT; acquiring it means "movie over, exit".
// A stub that always succeeds made the thread exit at once (silent movies).
struct KernelMutex {
    std::string name;
    std::uint32_t attr{};
    std::uint32_t owner{0u};  // thread uid, 0 == unlocked.
    std::uint32_t count{0u};
    std::vector<std::uint32_t> waiters;  // FIFO of blocked thread uids.
};
std::map<std::uint32_t, KernelMutex> kernel_mutexes;
std::uint32_t next_mutex_uid = 0x100u;

constexpr std::uint32_t kErrorWaitTimeout = 0x800201A8u;
constexpr std::uint32_t kErrorWaitDelete = 0x800201B5u;
constexpr std::uint32_t kErrorUnknownMutex = 0x800201C3u;
constexpr std::uint32_t kErrorMutexLocked = 0x800201C4u;
constexpr std::uint32_t kErrorMutexUnlockUnderflow = 0x800201C7u;
constexpr std::uint32_t kErrorMutexRecursive = 0x800201C8u;
constexpr std::uint32_t kMutexAttrRecursive = 0x200u;
// Guest memory for writing back *timeout when a timed wait expires inside the
// scheduler (no Runtime there). Set by register_mutex_hle.
psprecomp::GuestMemory *g_mutex_memory = nullptr;

struct PartitionBlock {
    std::string name;
    std::uint32_t address{};
    std::uint32_t size{};
};

struct PartitionTable {
    std::uint32_t next_uid{1u};
    // User partition arena: starts just past the loaded module and grows up
    // toward the thread stacks. ULUS-10478's image (text+data+bss) ends at
    // 0x08B60DE4; [0x08B61000, 0x08B70000) is reserved for host-owned guest
    // objects (currently unused).
    std::uint32_t next_address{0x08B70000u};
    std::map<std::uint32_t, PartitionBlock> blocks;
};

PartitionTable partition_table{};

struct FplPool {
    std::string name;
    std::uint32_t block_size{};
    std::uint32_t num_blocks{};
    std::uint32_t base_address{};
    std::vector<std::uint32_t> free_list;  // block addresses, LIFO.
};
std::map<std::uint32_t, FplPool> fpl_pools;
std::uint32_t next_fpl_uid = 1u;

// Execution-driven virtual time (mirrors VCS's virtual_time_us). Advanced by the
// runtime starvation hook and jumped directly to the next wakeup deadline when
// the scheduler would otherwise idle.
std::uint64_t virtual_time_us{};

constexpr std::uint64_t kVblankPeriodUs = 16683u;       // ~59.94 Hz PSP refresh.
constexpr std::uint64_t kTickMicroseconds = 64u;        // virtual time per starvation tick.
constexpr std::uint64_t kTickDispatchInterval = 256u;   // dispatches between ticks.

// Consecutive same-PC starvation ticks before the spin detector forces a
// cooperative yield (default 4 ticks ~= 1024 dispatches). Overridable at runtime
// via PSPRECOMP_SPIN_YIELD_TICKS so it can be tuned without rebuilding.
const std::uint64_t kSpinYieldTicks = []() {
    const char *env = std::getenv("PSPRECOMP_SPIN_YIELD_TICKS");
    if (env == nullptr || *env == '\0') return std::uint64_t{4};
    char *end = nullptr;
    const unsigned long long value = std::strtoull(env, &end, 0);
    return (end != nullptr && *end == '\0') ? static_cast<std::uint64_t>(value)
                                            : std::uint64_t{4};
}();

struct DisplayState {
    std::uint32_t mode{0u};
    std::uint32_t width{480u};
    std::uint32_t height{272u};
    std::uint32_t frame_buffer{0u};
    std::uint32_t buffer_width{512u};
    std::uint32_t pixel_format{3u};
};

DisplayState display_state{};

[[nodiscard]] std::uint64_t guest_time_us() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return static_cast<std::uint64_t>(
        duration_cast<microseconds>(steady_clock::now() - start).count());
}

// Guest system time is virtual time. Each poll nudges it forward by 1 us, so a
// spinner on sceKernelGetSystemTime* always sees the clock move. It used to add
// the wall-clock delta on top of virtual time, which counted real time twice.
std::uint64_t advance_system_time() {
    virtual_time_us += 1u;
    return virtual_time_us;
}

// Frame pacing: keep virtual time from running ahead of the wall clock.
// Virtual time jumps straight to the next VBlank when every thread is waiting,
// so without this the game runs as fast as the host executes it. Called at each
// VBlank wait. If the host falls behind by more than kMaxLagUs, re-anchor
// instead of racing to catch up. PSPRECOMP_NO_FRAME_PACING=1 disables it, for
// fast headless runs; PSPRECOMP_PACE_DIAG=1 prints the virtual/wall ratio.
void pace_virtual_time_to_wall_clock() {
    static const bool disabled = std::getenv("PSPRECOMP_NO_FRAME_PACING") != nullptr;
    static const bool diag = std::getenv("PSPRECOMP_PACE_DIAG") != nullptr;
    constexpr std::int64_t kMaxLagUs = 100000;
    static std::int64_t offset_us = 0;  // wall + offset == virtual when on time.
    static std::uint64_t calls = 0u, diag_virtual = 0u, diag_wall = 0u, slept_us = 0u;
    const auto wall = static_cast<std::int64_t>(guest_time_us());
    const auto virt = static_cast<std::int64_t>(virtual_time_us);
    if (calls++ == 0u) offset_us = virt - wall;
    const std::int64_t ahead = virt - (wall + offset_us);
    if (disabled) {
        // keep the anchor current so enabling later would not stall.
        offset_us = virt - wall;
    } else if (ahead > 0) {
        ProfileScope scope(frame_profile.sleep_ns);
        precise_sleep_until(std::chrono::steady_clock::now() + std::chrono::microseconds(ahead));
        slept_us += static_cast<std::uint64_t>(ahead);
    } else if (-ahead > kMaxLagUs) {
        offset_us = virt - wall;  // host too slow: drop the debt, do not sprint.
    }
    if (diag && calls % 300u == 0u) {
        const std::uint64_t now_wall = guest_time_us();
        const double dv = static_cast<double>(virtual_time_us - diag_virtual) / 1e6;
        const double dw = static_cast<double>(now_wall - diag_wall) / 1e6;
        std::cerr << "[pace] virtual=" << dv << "s wall=" << dw << "s speed=" << (dw > 0.0 ? dv / dw : 0.0)
                  << "x slept=" << static_cast<double>(slept_us) / 1e6 << "s\n";
        diag_virtual = virtual_time_us;
        diag_wall = now_wall;
        slept_us = 0u;
    }
}

std::uint32_t allocate_stack(std::uint32_t size) {
    std::uint32_t aligned = (size + 0xFFu) & ~0xFFu;
    if (aligned < 0x1000u) aligned = 0x1000u;
    for (auto it = thread_table.free_stacks.begin(); it != thread_table.free_stacks.end(); ++it) {
        if (it->second == aligned) {
            const std::uint32_t base = it->first;
            thread_table.free_stacks.erase(it);
            return base;
        }
    }
    if (thread_table.next_stack_top - aligned < partition_table.next_address) {
        std::cerr << "[thread] WARN: stack of " << aligned << " bytes collides with the user arena\n";
    }
    thread_table.next_stack_top -= aligned;
    return thread_table.next_stack_top;  // stack base (bottom).
}

void release_stack(std::uint32_t base, std::uint32_t size) {
    if (size == 0u) return;
    std::uint32_t aligned = (size + 0xFFu) & ~0xFFu;
    if (aligned < 0x1000u) aligned = 0x1000u;
    thread_table.free_stacks.emplace_back(base, aligned);
}

// Free user memory as the guest sees it: the gap between the partition arena
// (growing up) and the thread stacks (growing down). The engine sizes its main
// heap as sceKernelMaxFreeMemSize() - 1 MiB, so a fixed value starves it.
std::uint32_t user_free_memory() {
    return thread_table.next_stack_top > partition_table.next_address
        ? thread_table.next_stack_top - partition_table.next_address : 0u;
}

// Wake any Waiting thread whose virtual-time deadline has passed.
void promote_expired_delays() {
    for (auto &[uid, record] : thread_table.threads) {
        if (record.state == ThreadState::Waiting && record.wake_at_us != 0u &&
            record.wake_at_us <= virtual_time_us) {
            if (record.waiting_on_mutex != 0u) {
                // Timed LockMutex expired: leave the wait queue, report timeout.
                if (auto m = kernel_mutexes.find(record.waiting_on_mutex); m != kernel_mutexes.end())
                    std::erase(m->second.waiters, uid);
                record.waiting_on_mutex = 0u;
                record.context.gpr[2] = kErrorWaitTimeout;
            }
            if (record.timeout_ptr != 0u) {
                if (g_mutex_memory != nullptr) g_mutex_memory->store32(record.timeout_ptr, 0u);
                record.timeout_ptr = 0u;
            }
            record.state = ThreadState::Ready;
            record.wake_at_us = 0u;
            thread_table.ready_queue.push_back(uid);
        }
    }
}

const char *thread_state_name(ThreadState state) {
    switch (state) {
    case ThreadState::Ready: return "Ready";
    case ThreadState::Running: return "Running";
    case ThreadState::Waiting: return "Waiting";
    case ThreadState::Completed: return "Completed";
    }
    return "?";
}

void dump_thread_state() {
    std::cerr << "[thread] === FINAL STATE ===\n";
    for (auto &[uid, rec] : thread_table.threads) {
        std::cerr << "[thread] uid=" << uid
                  << " state=" << thread_state_name(rec.state)
                  << " entry=" << psprecomp::hex32(rec.entry)
                  << " exit=" << rec.exit_status << "\n";
    }
    std::cerr << "[thread] current_uid=" << thread_table.current_uid << "\n";
}

bool activate_next_thread(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx);

// Cooperative yield: park the current thread back on the Ready queue (resuming
// at the exact guest PC) and switch to the next runnable thread. This is the
// Wii "blr yield" analogue — without it, a spinner that never blocks would drain
// the Ready queue and leave every thread stranded in "Running" state.
void yield_current_thread(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    if (auto it = thread_table.threads.find(thread_table.current_uid);
        it != thread_table.threads.end()) {
        // Called from the starvation hook, i.e. at an arbitrary guest point
        // (dispatch/chain boundary), not from an HLE import: ctx.pc is the exact
        // resume point. Resuming at $ra instead forced a premature return from
        // the current function (e.g. Bink's decoder loop, ending in a jump to 0).
        it->second.context = ctx;
        it->second.state = ThreadState::Ready;
        thread_table.ready_queue.push_back(it->second.uid);
    }
    (void)activate_next_thread(rt, ctx);
}

// Restore the next Ready thread's full context into `ctx` and switch the
// runtime thread identity. Stops the runtime when no thread is left runnable.
bool activate_next_thread(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    promote_expired_delays();
    // Nothing runnable: jump virtual time straight to the earliest wakeup
    // deadline so a VBlank/delay waiter comes due (mirrors VCS's idle path).
    while (thread_table.ready_queue.empty()) {
        std::uint64_t earliest = ~std::uint64_t{0};
        for (const auto &[uid, record] : thread_table.threads) {
            (void)uid;
            if (record.state == ThreadState::Waiting && record.wake_at_us != 0u &&
                record.wake_at_us < earliest)
                earliest = record.wake_at_us;
        }
        if (earliest == ~std::uint64_t{0}) {
            dump_thread_state();
            std::cerr << "[thread] NO READY THREADS, stopping\n";
            rt.stop("all PSP threads completed");
            return false;
        }
        if (earliest > virtual_time_us) virtual_time_us = earliest;
        promote_expired_delays();
    }
    while (!thread_table.ready_queue.empty()) {
        const std::uint32_t uid = thread_table.ready_queue.front();
        thread_table.ready_queue.erase(thread_table.ready_queue.begin());
        const auto it = thread_table.threads.find(uid);
        if (it == thread_table.threads.end() || it->second.state != ThreadState::Ready) continue;
        ThreadRecord &record = it->second;
        record.state = ThreadState::Running;
        thread_table.current_uid = uid;
        ctx = record.context;
        psprecomp::set_runtime_thread_identity(static_cast<std::int32_t>(uid), record.name);
        std::cerr << "[thread] switch to uid=" << uid
                  << " state=" << thread_state_name(record.state) << "\n";
        std::cerr << "[thread] now running uid=" << thread_table.current_uid
                  << " pc=" << psprecomp::hex32(ctx.pc) << "\n";
        return true;
    }
    dump_thread_state();
    std::cerr << "[thread] NO READY THREADS, stopping\n";
    rt.stop("all PSP threads completed");
    return false;
}

// Wake every thread that was blocked in sceKernelWaitThreadEnd on the just-
// completed thread. These waiters have no wake_at_us deadline, so the idle path
// never promotes them — this is the only thing that can unblock them.
// sceKernelWaitThreadEnd returns the target's exit status in v0.
void wake_thread_end_waiters(psprecomp::Runtime &rt, std::uint32_t completed_uid,
                             std::uint32_t exit_status) {
    for (auto &[uid, rec] : thread_table.threads) {
        if (rec.state == ThreadState::Waiting && rec.waiting_on_thread == completed_uid) {
            rec.state = ThreadState::Ready;
            rec.waiting_on_thread = 0u;
            rec.context.gpr[2] = exit_status;
            thread_table.ready_queue.push_back(uid);
            std::cerr << "[thread] wake uid=" << uid
                      << " (was waiting on " << completed_uid << ")\n";
        }
    }
    (void)rt;
}

void complete_current_thread(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t completed_uid = thread_table.current_uid;
    std::cerr << "[jal0] called from ra=" << psprecomp::hex32(ctx.gpr[31])
              << " current_uid=" << thread_table.current_uid
              << " a0=" << psprecomp::hex32(ctx.gpr[4]) << "\n";
    if (const auto it = thread_table.threads.find(thread_table.current_uid);
        it != thread_table.threads.end()) {
        it->second.state = ThreadState::Completed;
        it->second.exit_status = ctx.gpr[4];  // a0 == exit status.
        std::cerr << "[thread] complete uid=" << thread_table.current_uid
                  << " exit_status=" << ctx.gpr[4] << "\n";
    } else {
        std::cerr << "[thread] WARN: complete for untracked uid=" << thread_table.current_uid << "\n";
    }
    wake_thread_end_waiters(rt, completed_uid, ctx.gpr[4]);
    (void)activate_next_thread(rt, ctx);
}

// Park the current thread until `wake_at_us` of virtual time. Saves the
// suspended context (resuming at $ra, like VCS's make_wait_context) so the
// thread continues after the HLE call once its deadline expires.
void suspend_for_wakeup(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx,
                        std::uint64_t wake_at_us) {
    if (const auto it = thread_table.threads.find(thread_table.current_uid);
        it != thread_table.threads.end()) {
        ThreadRecord &record = it->second;
        record.context = ctx;
        record.context.pc = ctx.gpr[31];  // resume after the HLE call.
        record.context.set_gpr(2, 0u);    // return value.
        record.state = ThreadState::Waiting;
        record.wake_at_us = wake_at_us;
    }
    (void)activate_next_thread(rt, ctx);
}

void register_sysmem_hle(psprecomp::Runtime &rt) {
    // 0x91DE343C (unknown) — no-op.
    rt.register_hle("SysMemUserForUser", 0x91DE343Cu,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    // sceKernelSetCompilerVersion — no-op.
    rt.register_hle("SysMemUserForUser", 0xF77D77CBu,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    // sceKernelMaxFreeMemSize / sceKernelTotalFreeMemSize: the real gap
    // between the user arena and the thread stacks (one contiguous block).
    rt.register_hle("SysMemUserForUser", 0xA291F107u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, user_free_memory());
            std::cerr << "[sysmem] MaxFreeMemSize -> " << psprecomp::hex32(user_free_memory()) << "\n";
        });
    rt.register_hle("SysMemUserForUser", 0xF919F628u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, user_free_memory()); });
    // sceKernelAllocPartitionMemory — simple upward arena.
    rt.register_hle("SysMemUserForUser", 0x237DBD4Fu,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t size = ctx.gpr[7];  // a3.
            const std::uint32_t aligned = (size + 0xFFu) & ~0xFFu;
            if (aligned == 0u) { ctx.set_gpr(2, 0x800200D9u); return; }
            PartitionBlock block{};
            block.name = ctx.gpr[5] != 0u ? rt.memory().read_c_string(ctx.gpr[5], 128u) : "partition";
            block.address = partition_table.next_address;
            block.size = aligned;
            partition_table.next_address += aligned;
            const std::uint32_t uid = partition_table.next_uid++;
            std::cerr << "[sysmem] alloc \"" << block.name << "\" size=" << psprecomp::hex32(aligned)
                      << " -> " << psprecomp::hex32(block.address) << ".." << psprecomp::hex32(block.address + aligned)
                      << " uid=" << uid << "\n";
            partition_table.blocks[uid] = std::move(block);
            ctx.set_gpr(2, uid);
        });
    // sceKernelGetBlockHeadAddr.
    rt.register_hle("SysMemUserForUser", 0x9D9A5BA1u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto it = partition_table.blocks.find(ctx.gpr[4]);
            ctx.set_gpr(2, it != partition_table.blocks.end() ? it->second.address : 0u);
        });
    // sceKernelFreePartitionMemory.
    rt.register_hle("SysMemUserForUser", 0xB6D61D02u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            partition_table.blocks.erase(ctx.gpr[4]);
            ctx.set_gpr(2, 0u);
        });
    // sceKernelPrintf — minimal: print the format string, ignore varargs.
    rt.register_hle("SysMemUserForUser", 0x13A5ABEFu,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            std::cout << rt.memory().read_c_string(ctx.gpr[4], 512u);
            ctx.set_gpr(2, 0u);
        });
}

// sceKernel*Mutex. Semantics follow the PSP kernel (and PPSSPP): FIFO waiters,
// ownership handed to the first waiter on the final unlock, recursion only with
// attr 0x200, and a timed lock returns WAIT_TIMEOUT with *timeout set to 0.
bool try_take_mutex(KernelMutex &m, std::uint32_t uid, std::uint32_t count, std::uint32_t &error) {
    if (m.owner == 0u) {
        m.owner = uid;
        m.count = count;
        return true;
    }
    if (m.owner == uid) {
        if ((m.attr & kMutexAttrRecursive) == 0u) { error = kErrorMutexRecursive; return false; }
        m.count += count;
        return true;
    }
    error = kErrorMutexLocked;
    return false;
}

void hand_off_mutex(KernelMutex &m) {
    m.owner = 0u;
    m.count = 0u;
    while (!m.waiters.empty()) {
        const std::uint32_t uid = m.waiters.front();
        m.waiters.erase(m.waiters.begin());
        const auto it = thread_table.threads.find(uid);
        if (it == thread_table.threads.end() || it->second.state != ThreadState::Waiting) continue;
        ThreadRecord &rec = it->second;
        m.owner = uid;
        m.count = rec.mutex_lock_count;
        rec.waiting_on_mutex = 0u;
        rec.wake_at_us = 0u;
        rec.timeout_ptr = 0u;  // *timeout keeps its value; good enough (time left is unused).
        rec.context.gpr[2] = 0u;
        rec.state = ThreadState::Ready;
        thread_table.ready_queue.push_back(uid);
        return;
    }
}

void register_mutex_hle(psprecomp::Runtime &rt) {
    g_mutex_memory = &rt.memory();
    static const bool diag = std::getenv("PSPRECOMP_MUTEX_DIAG") != nullptr;
    // sceKernelCreateMutex(name, attr, initCount, option).
    rt.register_hle("ThreadManForUser", 0xB7D098C6u,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            KernelMutex m{};
            m.name = ctx.gpr[4] != 0u ? rt.memory().read_c_string(ctx.gpr[4], 32u) : "mutex";
            m.attr = ctx.gpr[5];
            const auto init = static_cast<std::int32_t>(ctx.gpr[6]);
            if (init > 0) {
                m.owner = thread_table.current_uid;
                m.count = static_cast<std::uint32_t>(init);
            }
            const std::uint32_t uid = next_mutex_uid++;
            if (diag)
                std::cerr << "[mutex] create \"" << m.name << "\" uid=" << psprecomp::hex32(uid)
                          << " attr=" << psprecomp::hex32(m.attr) << " init=" << init
                          << " thread=" << thread_table.current_uid << "\n";
            kernel_mutexes[uid] = std::move(m);
            ctx.set_gpr(2, uid);
        });
    // sceKernelDeleteMutex: waiters wake with WAIT_DELETE.
    rt.register_hle("ThreadManForUser", 0xF8170FBEu,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto it = kernel_mutexes.find(ctx.gpr[4]);
            if (it == kernel_mutexes.end()) { ctx.set_gpr(2, kErrorUnknownMutex); return; }
            for (const std::uint32_t uid : it->second.waiters) {
                if (auto t = thread_table.threads.find(uid);
                    t != thread_table.threads.end() && t->second.state == ThreadState::Waiting) {
                    t->second.waiting_on_mutex = 0u;
                    t->second.wake_at_us = 0u;
                    t->second.timeout_ptr = 0u;
                    t->second.context.gpr[2] = kErrorWaitDelete;
                    t->second.state = ThreadState::Ready;
                    thread_table.ready_queue.push_back(uid);
                }
            }
            kernel_mutexes.erase(it);
            ctx.set_gpr(2, 0u);
        });
    // sceKernelLockMutex(uid, count, timeout*).
    rt.register_hle("ThreadManForUser", 0xB011B11Fu,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto it = kernel_mutexes.find(ctx.gpr[4]);
            if (it == kernel_mutexes.end()) {
                if (diag)
                    std::cerr << "[mutex] lock unknown uid=" << psprecomp::hex32(ctx.gpr[4]) << " thread="
                              << thread_table.current_uid << " ra=" << psprecomp::hex32(ctx.gpr[31]) << "\n";
                ctx.set_gpr(2, kErrorUnknownMutex);
                return;
            }
            const std::uint32_t count = ctx.gpr[5] != 0u ? ctx.gpr[5] : 1u;
            const std::uint32_t timeout_ptr = ctx.gpr[6];
            std::uint32_t error = 0u;
            if (try_take_mutex(it->second, thread_table.current_uid, count, error)) {
                if (diag)
                    std::cerr << "[mutex] lock uid=" << psprecomp::hex32(ctx.gpr[4]) << " thread="
                              << thread_table.current_uid << " ok ra=" << psprecomp::hex32(ctx.gpr[31]) << "\n";
                ctx.set_gpr(2, 0u);
                return;
            }
            if (error != kErrorMutexLocked) { ctx.set_gpr(2, error); return; }
            const std::uint32_t timeout_us = timeout_ptr != 0u ? rt.memory().load32(timeout_ptr) : 0u;
            if (diag)
                std::cerr << "[mutex] lock uid=" << psprecomp::hex32(ctx.gpr[4]) << " thread="
                          << thread_table.current_uid << " blocks (owner=" << it->second.owner
                          << " timeout=" << (timeout_ptr != 0u ? std::to_string(timeout_us) : "inf")
                          << ")\n";
            const auto cur = thread_table.threads.find(thread_table.current_uid);
            if (cur == thread_table.threads.end()) { ctx.set_gpr(2, kErrorMutexLocked); return; }
            ThreadRecord &rec = cur->second;
            rec.context = ctx;
            rec.context.pc = ctx.gpr[31];
            rec.state = ThreadState::Waiting;
            rec.waiting_on_mutex = ctx.gpr[4];
            rec.mutex_lock_count = count;
            rec.timeout_ptr = timeout_ptr;
            // Timed wait: the scheduler's deadline path reports WAIT_TIMEOUT.
            // A zero timeout still yields once (deadline == now).
            rec.wake_at_us = timeout_ptr != 0u ? virtual_time_us + std::max<std::uint32_t>(timeout_us, 1u) : 0u;
            it->second.waiters.push_back(rec.uid);
            (void)activate_next_thread(rt, ctx);
        });
    // sceKernelTryLockMutex(uid, count).
    rt.register_hle("ThreadManForUser", 0x0DDCD2C9u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto it = kernel_mutexes.find(ctx.gpr[4]);
            if (it == kernel_mutexes.end()) { ctx.set_gpr(2, kErrorUnknownMutex); return; }
            std::uint32_t error = 0u;
            const std::uint32_t count = ctx.gpr[5] != 0u ? ctx.gpr[5] : 1u;
            ctx.set_gpr(2, try_take_mutex(it->second, thread_table.current_uid, count, error) ? 0u : error);
        });
    // sceKernelUnlockMutex(uid, count).
    rt.register_hle("ThreadManForUser", 0x6B30100Fu,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto it = kernel_mutexes.find(ctx.gpr[4]);
            if (it == kernel_mutexes.end()) { ctx.set_gpr(2, kErrorUnknownMutex); return; }
            KernelMutex &m = it->second;
            const std::uint32_t count = ctx.gpr[5] != 0u ? ctx.gpr[5] : 1u;
            if (m.owner != thread_table.current_uid || m.count < count) {
                ctx.set_gpr(2, kErrorMutexUnlockUnderflow);
                return;
            }
            m.count -= count;
            if (diag)
                std::cerr << "[mutex] unlock uid=" << psprecomp::hex32(ctx.gpr[4]) << " thread="
                          << thread_table.current_uid << " ra=" << psprecomp::hex32(ctx.gpr[31]) << "\n";
            if (m.count == 0u) {
                hand_off_mutex(m);
                if (diag && m.owner != 0u)
                    std::cerr << "[mutex] unlock uid=" << psprecomp::hex32(ctx.gpr[4])
                              << " hands off to thread " << m.owner << "\n";
            }
            // No preemption: the woken waiter runs at the next blocking point.
            ctx.set_gpr(2, 0u);
        });
}

void register_threadman_hle(psprecomp::Runtime &rt) {
    // sceKernelGetThreadId.
    rt.register_hle("ThreadManForUser", 0x293B45B8u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, thread_table.current_uid);
        });
    // sceKernelCreateThread.
    rt.register_hle("ThreadManForUser", 0x446D8DE6u,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t uid = thread_table.next_uid++;
            std::cerr << "[thread] create uid=" << uid
                      << " entry=" << psprecomp::hex32(ctx.gpr[5])
                      << " prio=" << ctx.gpr[6]
                      << " stack=" << ctx.gpr[7] << "\n";
            const std::uint32_t stack_size = ctx.gpr[7];  // a3.
            ThreadRecord record{};
            record.uid = uid;
            record.name = ctx.gpr[4] != 0u ? rt.memory().read_c_string(ctx.gpr[4], 128u) : "unnamed";
            record.entry = ctx.gpr[5];        // a1.
            record.stack_size = stack_size;
            record.stack_base = allocate_stack(stack_size);
            record.context = psprecomp::AllegrexContext{};
            record.context.gpr[29] = record.stack_base + stack_size;  // sp == top.
            record.context.gpr[28] = ctx.gpr[28];                     // inherit gp.
            // k0 (gpr[26]) is the PSP thread control block. The guest stores its
            // context pointer at [k0 + 4] and reads it back after the first
            // cross-unit call, so give each thread its own 0x100-byte block just
            // below the stack top (mirrors VCS: stack_top - 0x100).
            record.kernel_context = record.stack_base + stack_size - 0x100u;
            record.context.gpr[26] = record.kernel_context;
            rt.memory().store32(record.kernel_context + 0xC0u, uid);
            rt.memory().store32(record.kernel_context + 0xC8u, record.stack_base);
            rt.memory().store32(record.kernel_context + 0xF8u, 0xFFFFFFFFu);
            rt.memory().store32(record.kernel_context + 0xFCu, 0xFFFFFFFFu);
            record.context.pc = record.entry;
            record.state = ThreadState::Ready;
            thread_table.threads[uid] = std::move(record);
            ctx.set_gpr(2, uid);
        });
    // sceKernelStartThread — mark Ready and enqueue (the scheduler switches at
    // the next blocking point, e.g. module_start's ExitThread).
    rt.register_hle("ThreadManForUser", 0xF475845Du,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto it = thread_table.threads.find(ctx.gpr[4]);
            if (it == thread_table.threads.end()) { ctx.set_gpr(2, 0x80020198u); return; }
            std::cerr << "[thread] start uid=" << ctx.gpr[4] << "\n";
            it->second.state = ThreadState::Ready;
            const std::uint32_t arglen = ctx.gpr[5];
            std::uint32_t argp = ctx.gpr[6];
            // Like the real kernel, copy the argument block onto the new
            // thread's stack (below the k0 block) and point a1 at the copy.
            // The caller may pass a pointer into its own stack frame (Bink's
            // sound helper does: StartThread(uid, 4, sp)), which is gone by the
            // time the thread first runs.
            if (argp != 0u && arglen != 0u && arglen <= 0x1000u) {
                const std::uint32_t dst = (it->second.kernel_context - arglen) & ~0xFu;
                for (std::uint32_t i = 0u; i < arglen; ++i)
                    rt.memory().store8(dst + i, rt.memory().load8(argp + i));
                argp = dst;
                it->second.context.gpr[29] = dst;
            }
            it->second.context.gpr[4] = arglen;  // arglen -> thread a0.
            it->second.context.gpr[5] = argp;    // argp   -> thread a1.
            thread_table.ready_queue.push_back(it->second.uid);
            ctx.set_gpr(2, 0u);
        });
    // sceKernelGetSystemTimeLow.
    rt.register_hle("ThreadManForUser", 0x369ED59Du,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, static_cast<std::uint32_t>(advance_system_time()));
        });
    // sceKernelGetSystemTimeWide.
    rt.register_hle("ThreadManForUser", 0x82BC5777u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const std::uint64_t us = advance_system_time();
            ctx.set_gpr(2, static_cast<std::uint32_t>(us & 0xFFFFFFFFu));          // low.
            ctx.set_gpr(3, static_cast<std::uint32_t>((us >> 32u) & 0xFFFFFFFFu)); // high.
        });
    // sceKernelDelayThread / sceKernelDelayThreadCB — mark Waiting and switch.
    const auto delay_thread = [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        // a0 == delay in microseconds.
        suspend_for_wakeup(rt, ctx,
                           virtual_time_us + static_cast<std::uint64_t>(ctx.gpr[4]));
    };
    rt.register_hle("ThreadManForUser", 0xCEADEB47u, delay_thread);
    rt.register_hle("ThreadManForUser", 0x68DA9E36u, delay_thread);
    // sceKernelWaitThreadEnd.
    rt.register_hle("ThreadManForUser", 0x278C0DF5u,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t target = ctx.gpr[4];   // uid del thread a esperar.
            if (target == thread_table.current_uid) {
                ctx.set_gpr(2, 0x800201A7u);  // SCE_KERNEL_ERROR_ILLEGAL_THID.
                return;
            }
            const auto target_rec = thread_table.threads.find(target);
            if (target_rec == thread_table.threads.end()) {
                ctx.set_gpr(2, 0x800201A7u);
                return;
            }
            if (target_rec->second.state == ThreadState::Completed) {
                ctx.set_gpr(2, 0u);
                return;
            }
            // Bloqueo real: guardar contexto + uid del target. Sin wake_at_us:
            // es un wait por evento, no por tiempo.
            if (auto current = thread_table.threads.find(thread_table.current_uid);
                current != thread_table.threads.end()) {
                current->second.context = ctx;
                current->second.context.pc = ctx.gpr[31];
                current->second.state = ThreadState::Waiting;
                current->second.waiting_on_thread = target;
            }
            (void)activate_next_thread(rt, ctx);
        });
    // sceKernelExitThread (0xAA73C935) — complete and switch; does not return.
    rt.register_hle("ThreadManForUser", 0xAA73C935u,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            std::cerr << "[thread] ExitThread called, uid=" << thread_table.current_uid
                      << " status=" << ctx.gpr[4] << "\n";
            complete_current_thread(rt, ctx);
        });
    // sceKernelExitDeleteThread — complete, remove, and switch.
    rt.register_hle("ThreadManForUser", 0x809CE29Bu,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            std::cerr << "[thread] ExitDeleteThread called, uid=" << thread_table.current_uid
                      << " status=" << ctx.gpr[4]
                      << " ra=" << psprecomp::hex32(ctx.gpr[31]) << "\n";
            if (const auto it = thread_table.threads.find(thread_table.current_uid);
                it != thread_table.threads.end()) {
                it->second.state = ThreadState::Completed;
                it->second.exit_status = ctx.gpr[4];
                std::cerr << "[thread] delete uid=" << thread_table.current_uid << "\n";
                release_stack(it->second.stack_base, it->second.stack_size);
                thread_table.threads.erase(it);
            }
            // Deleting must not strand threads blocked in sceKernelWaitThreadEnd
            // on this one (Bink's per-movie helper thread exits this way).
            wake_thread_end_waiters(rt, thread_table.current_uid, ctx.gpr[4]);
            (void)activate_next_thread(rt, ctx);
        });

    // Remaining ThreadManForUser imports reached during boot. UID-producing
    // creators get an incrementing handle the guest can store and later delete;
    // everything else is a no-op success.
    static std::uint32_t next_callback_uid = 1u;
    const auto return_uid = [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        ctx.set_gpr(2, next_callback_uid++);
    };
    rt.register_hle("ThreadManForUser", 0xE81CAF8Fu, return_uid);  // sceKernelCreateCallback
    rt.register_hle("ThreadManForUser", 0x55C20A00u, return_uid);  // sceKernelCreateEventFlag
    for (const std::uint32_t nid : {0xEDBA5844u, 0x1FB15A32u, 0xEF9E4C70u, 0x402FCF22u,
                                    0x9FA03CD3u}) {
        rt.register_hle("ThreadManForUser", nid,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    }
    register_mutex_hle(rt);

    // FPL (Fixed Pool Library) — Fase B.2. Blocks are 64-byte aligned and carved
    // out of the same upward arena as SysMem (0x09000000+). TryAllocateFpl MUST
    // write the block address into *data (a1): the guest stores it on the stack
    // and dereferences it right after, so leaving it NULL crashes the boot.
    rt.register_hle("ThreadManForUser", 0xC07BB470u,  // sceKernelCreateFpl
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t block_size = (ctx.gpr[7] + 63u) & ~63u;  // a3, 64-byte aligned.
            const std::uint32_t num_blocks = ctx.gpr[8];                  // a4 (t0).
            if (block_size == 0u || num_blocks == 0u) { ctx.set_gpr(2, 0x800200D9u); return; }
            const std::uint32_t base = partition_table.next_address;
            partition_table.next_address += block_size * num_blocks;
            FplPool pool{};
            pool.name = ctx.gpr[4] != 0u ? rt.memory().read_c_string(ctx.gpr[4], 128u) : "fpl";
            pool.block_size = block_size;
            pool.num_blocks = num_blocks;
            pool.base_address = base;
            for (std::uint32_t i = 0u; i < num_blocks; ++i)
                pool.free_list.push_back(base + i * block_size);
            const std::uint32_t uid = next_fpl_uid++;
            fpl_pools[uid] = std::move(pool);
            ctx.set_gpr(2, uid);
        });
    const auto allocate_fpl = [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        const auto it = fpl_pools.find(ctx.gpr[4]);  // a0 = uid.
        if (it == fpl_pools.end()) { ctx.set_gpr(2, 0x80020198u); return; }
        if (it->second.free_list.empty()) { ctx.set_gpr(2, 0x80020190u); return; }
        const std::uint32_t addr = it->second.free_list.back();
        it->second.free_list.pop_back();
        if (ctx.gpr[5] != 0u) rt.memory().store32(ctx.gpr[5], addr);  // *data = addr (a1).
        ctx.set_gpr(2, 0u);
    };
    rt.register_hle("ThreadManForUser", 0x623AE665u, allocate_fpl);  // sceKernelTryAllocateFpl
    // sceKernelAllocateFpl is the blocking variant. The boot only imports the
    // non-blocking TryAllocateFpl, so a plain non-blocking allocation suffices;
    // a proper FPL wait queue (wake on FreeFpl) is deferred.
    rt.register_hle("ThreadManForUser", 0xD979E9BFu, allocate_fpl);
    rt.register_hle("ThreadManForUser", 0xF2574E14u,  // sceKernelFreeFpl
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto it = fpl_pools.find(ctx.gpr[4]);
            if (it == fpl_pools.end()) { ctx.set_gpr(2, 0x80020198u); return; }
            if (ctx.gpr[5] != 0u) it->second.free_list.push_back(ctx.gpr[5]);  // return block.
            ctx.set_gpr(2, 0u);
        });
    rt.register_hle("ThreadManForUser", 0xED1410E0u,  // sceKernelDeleteFpl
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            fpl_pools.erase(ctx.gpr[4]);
            ctx.set_gpr(2, 0u);
        });
}

// Diagnostic: write one presented RGBA frame to a PPM when PSPRECOMP_DUMP_FRAME
// is set, so a headless run can be inspected without a display.
void dump_frame_if_requested(std::span<const std::byte> rgba, std::uint32_t width, std::uint32_t height) {
    const char *dir = std::getenv("PSPRECOMP_DUMP_FRAME");
    if (dir == nullptr || dir[0] == '\0') return;
    // PSPRECOMP_DUMP_FRAME_AT=<n,n,...> overrides the default frame numbers.
    static const std::vector<std::uint64_t> targets = [] {
        std::vector<std::uint64_t> list;
        if (const char *at = std::getenv("PSPRECOMP_DUMP_FRAME_AT"); at != nullptr) {
            for (const char *p = at; *p != '\0';) {
                char *end = nullptr;
                const unsigned long long value = std::strtoull(p, &end, 10);
                if (end == p) { ++p; continue; }
                list.push_back(value);
                p = end;
            }
        }
        if (list.empty()) list = {1u, 40u, 80u, 120u, 160u, 190u, 400u, 800u, 1200u, 1600u};
        return list;
    }();
    bool dump = false;
    for (const std::uint64_t target : targets) {
        if (frame_present_count == target) { dump = true; break; }
    }
    if (!dump || rgba.empty() || width == 0u || height == 0u) return;
    const std::string path = std::string(dir) + "/frame_" + std::to_string(frame_present_count) + ".ppm";
    std::ofstream out(path, std::ios::binary);
    if (!out) return;
    out << "P6\n" << width << " " << height << "\n255\n";
    for (std::size_t i = 0u; i < static_cast<std::size_t>(width) * height; ++i) {
        out.put(static_cast<char>(rgba[i * 4u + 0u]));
        out.put(static_cast<char>(rgba[i * 4u + 1u]));
        out.put(static_cast<char>(rgba[i * 4u + 2u]));
    }
    std::cerr << "[dump] frame " << frame_present_count << " -> " << path
              << " (" << width << "x" << height << ")\n";
}

// Present the current display framebuffer to the host window. Prefers the GPU
// backend's readback when it produced a frame; otherwise blits guest VRAM via
// the software path.
void present_display_frame(psprecomp::Runtime &rt) {
    display_window_pump();
    ge_diag_frame_tick();
    if (display_state.frame_buffer == 0u) return;
    frame_profile_tick();
    if (ge_gpu_backend_active()) {
        const std::uint64_t vblank = virtual_time_us / kVblankPeriodUs;
        {
            ProfileScope scope(frame_profile.finish_ns);
            (void)ge_gpu_backend_finish_color_frame(vblank);
        }
        const GeGpuBackendReport report = ge_gpu_backend_report();
        const std::span<const std::byte> rgba = ge_gpu_backend_game_frame_rgba();
        if (!rgba.empty() && report.offscreen_width != 0u && report.offscreen_height != 0u) {
            dump_frame_if_requested(rgba, report.offscreen_width, report.offscreen_height);
            ProfileScope scope(frame_profile.present_ns);
            display_window_present_rgba(rgba, report.offscreen_width, report.offscreen_height);
            ge_gpu_backend_mark_window_presented();
            return;
        }
    }
    display_window_present(rt, display_state.frame_buffer, display_state.buffer_width,
                           display_state.pixel_format, display_state.width, display_state.height);
}

// Schedule one audio buffer on the channel's virtual-time grid and return the
// instant the hardware would start playing it (VCS audio_queue_buffer).
std::uint64_t audio_queue_buffer(AudioChannelState &channel, std::uint32_t frames) {
    const std::uint64_t rate = channel.frequency == 0u ? 44100u : channel.frequency;
    const auto elapsed_us = [&](std::uint64_t sample_frames) {
        return (sample_frames * 1000000ull) / rate;
    };
    std::uint64_t start = channel.busy_until_us;
    if (start < virtual_time_us) start = virtual_time_us;
    channel.busy_until_us = start + elapsed_us(frames);
    channel.queued_frames += frames;
    return start;
}

// Minimal sceAudio (Fase E): reserve + output so the audio worker paces instead
// of spinning, and PCM reaches the host device when PSPRECOMP_AUDIO is set.
void register_audio_hle(psprecomp::Runtime &rt) {
    const auto reserve = [](psprecomp::AllegrexContext &ctx) {
        std::int32_t channel = static_cast<std::int32_t>(ctx.gpr[4]);
        const std::uint32_t sample_count = ctx.gpr[5];
        const std::uint32_t format = ctx.gpr[6];
        if (channel < 0) {
            channel = -1;
            for (std::int32_t candidate = 7; candidate >= 1; --candidate) {
                if (!audio_channels[static_cast<std::size_t>(candidate)].reserved) {
                    channel = candidate; break;
                }
            }
            if (channel < 0) { ctx.set_gpr(2, 0x80260005u); return; }
        }
        if (channel >= 8) { ctx.set_gpr(2, 0x80260003u); return; }
        if (sample_count == 0u || (sample_count & 63u) != 0u || sample_count > 65472u) {
            ctx.set_gpr(2, 0x80260006u); return;
        }
        if (format != 0u && format != 0x10u) { ctx.set_gpr(2, 0x80260007u); return; }
        auto &state = audio_channels[static_cast<std::size_t>(channel)];
        if (state.reserved) { ctx.set_gpr(2, 0x80268002u); return; }
        state = AudioChannelState{};
        state.reserved = true;
        state.sample_count = sample_count;
        state.format = format;
        ctx.set_gpr(2, static_cast<std::uint32_t>(channel));
    };
    rt.register_hle("sceAudio", 0x5EC81C55u,  // sceAudioChReserve
        [reserve](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { reserve(ctx); });

    const auto output = [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx, bool blocking) {
        const std::uint32_t channel = ctx.gpr[4];
        const std::uint32_t left = ctx.gpr[5];
        const std::uint32_t right = ctx.gpr[6];
        const std::uint32_t samples = ctx.gpr[7];
        if (channel >= 8u) { ctx.set_gpr(2, 0x80260003u); return; }
        auto &state = audio_channels[channel];
        if (!state.reserved) { ctx.set_gpr(2, 0x80260001u); return; }
        if (left > 0xFFFFu || right > 0xFFFFu) { ctx.set_gpr(2, 0x8026000Bu); return; }
        const std::uint32_t channels = state.format == 0x10u ? 1u : 2u;
        const std::size_t bytes = static_cast<std::size_t>(state.sample_count) * channels * 2u;
        if (samples != 0u && rt.memory().raw_pointer(samples, bytes) == nullptr) {
            ctx.set_gpr(2, 0x800200D3u); return;
        }
        state.left_volume = left;
        state.right_volume = right;
        const std::uint64_t start_us = audio_queue_buffer(state, state.sample_count);
        if (samples != 0u && audio_output_enabled()) {
            std::vector<std::int16_t> pcm(bytes / sizeof(std::int16_t));
            const std::uint8_t *base = rt.memory().raw_pointer(samples, bytes);
            for (std::size_t i = 0u; i < pcm.size(); ++i)
                pcm[i] = static_cast<std::int16_t>(base[i * 2u] | (base[i * 2u + 1u] << 8));
            audio_output_submit(pcm, state.sample_count, channels == 2u, left, right,
                                state.frequency, channel, start_us, virtual_time_us);
        }
        const std::uint64_t wait_us = start_us > virtual_time_us ? start_us - virtual_time_us : 0u;
        if (blocking)
            suspend_for_wakeup(rt, ctx, virtual_time_us + wait_us);
        else
            ctx.set_gpr(2, state.sample_count);
    };
    rt.register_hle("sceAudio", 0x13F592BCu,  // sceAudioOutputPannedBlocking
        [output](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { output(rt, ctx, true); });
    rt.register_hle("sceAudio", 0x136CAF51u,  // sceAudioOutputBlocking (volume, buffer)
        [output](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t volume = ctx.gpr[5];
            const std::uint32_t buffer = ctx.gpr[6];
            ctx.set_gpr(6, volume);
            ctx.set_gpr(7, buffer);
            output(rt, ctx, true);
        });
    rt.register_hle("sceAudio", 0xE2D56B2Du,  // sceAudioOutput (non-blocking)
        [output](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { output(rt, ctx, false); });

    // sceAudioOutput2*: the single stereo 44.1 kHz "SRC" output that Bink's
    // audio thread plays movie soundtracks through. They used to be generic
    // return-0 stubs, so movies were silent.
    rt.register_hle("sceAudio", 0x01562BA3u,  // sceAudioOutput2Reserve(sample_count)
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t samples = ctx.gpr[4];
            if (samples < 17u || samples > 4111u) { ctx.set_gpr(2, 0x80260006u); return; }
            if (audio_output2_channel.reserved) { ctx.set_gpr(2, 0x80268002u); return; }
            audio_output2_channel = AudioChannelState{};
            audio_output2_channel.reserved = true;
            audio_output2_channel.sample_count = samples;
            audio_output_reset_channel(kOutput2MixerChannel);
            output2_blocks = 0u;
            output2_peak = 0;
            if (std::getenv("PSPRECOMP_AUDIO_DIAG") != nullptr)
                std::cerr << "[audio] output2 reserve samples=" << samples << " t=" << virtual_time_us << "us\n";
            ctx.set_gpr(2, 0u);
        });
    rt.register_hle("sceAudio", 0x43196845u,  // sceAudioOutput2Release
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!audio_output2_channel.reserved) { ctx.set_gpr(2, 0x80268001u); return; }
            audio_output2_channel.reserved = false;
            if (std::getenv("PSPRECOMP_AUDIO_DIAG") != nullptr)
                std::cerr << "[audio] output2 release: blocks=" << output2_blocks
                          << " peak=" << output2_peak << " t=" << virtual_time_us << "us\n";
            ctx.set_gpr(2, 0u);
        });
    rt.register_hle("sceAudio", 0x647CEF33u,  // sceAudioOutput2GetRestSample
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto &state = audio_output2_channel;
            if (!state.reserved) { ctx.set_gpr(2, 0x80268001u); return; }
            const std::uint64_t left_us =
                state.busy_until_us > virtual_time_us ? state.busy_until_us - virtual_time_us : 0u;
            ctx.set_gpr(2, static_cast<std::uint32_t>(left_us * state.frequency / 1000000ull));
        });
    rt.register_hle("sceAudio", 0x2D53F36Eu,  // sceAudioOutput2OutputBlocking(volume, buffer)
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            auto &state = audio_output2_channel;
            const std::uint32_t volume = ctx.gpr[4];
            const std::uint32_t buffer = ctx.gpr[5];
            if (!state.reserved) { ctx.set_gpr(2, 0x80268001u); return; }
            if (volume > 0xFFFFu) { ctx.set_gpr(2, 0x8026000Bu); return; }
            const std::size_t bytes = static_cast<std::size_t>(state.sample_count) * 4u;
            const std::uint8_t *base = buffer != 0u ? rt.memory().raw_pointer(buffer, bytes) : nullptr;
            if (buffer != 0u && base == nullptr) { ctx.set_gpr(2, 0x800200D3u); return; }
            state.left_volume = state.right_volume = volume;
            const std::uint64_t start_us = audio_queue_buffer(state, state.sample_count);
            if (base != nullptr && audio_output_enabled()) {
                std::vector<std::int16_t> pcm(bytes / sizeof(std::int16_t));
                for (std::size_t i = 0u; i < pcm.size(); ++i) {
                    pcm[i] = static_cast<std::int16_t>(base[i * 2u] | (base[i * 2u + 1u] << 8));
                    output2_peak = std::max<std::int32_t>(output2_peak, std::abs(static_cast<std::int32_t>(pcm[i])));
                }
                ++output2_blocks;
                audio_output_submit(pcm, state.sample_count, true, volume, volume, state.frequency,
                                    kOutput2MixerChannel, start_us, virtual_time_us);
            }
            const std::uint64_t wait_us = start_us > virtual_time_us ? start_us - virtual_time_us : 0u;
            suspend_for_wakeup(rt, ctx, virtual_time_us + wait_us);
        });
}

// PSPRECOMP_OBJECT_DUMP=<vtable>:<size>:<every N frames>[:dir] — phase-2 aid.
// Every N presented frames (the PSPRECOMP_INPUT_SCRIPT clock), find live objects whose first word is <vtable> (heap and
// stacks, [0x08B70000, 0x0A000000)) and write <size> bytes of each to
// <dir>/obj_<addr>_<frame>.bin. Diffing dumps across frames reveals fields
// (position, velocity, state) of an engine class.
// PSPRECOMP_PLAYER_LOG=1: once per second, follow the game-session pointer to
// the player and print its position, yaw and velocity (validates the phase-2
// layout in game/spongebob_player.hpp).
void player_log_tick(psprecomp::Runtime &rt) {
    static const bool enabled = std::getenv("PSPRECOMP_PLAYER_LOG") != nullptr;
    static std::uint64_t last_frame = 0u;
    if (!enabled || frame_present_count < last_frame + 60u) return;
    last_frame = frame_present_count;
    namespace g = spongebob::game;
    const std::uint32_t entity = g::find_player(rt.memory());
    if (entity == 0u) return;
    const auto f = [&](std::uint32_t offset) {
        const std::uint32_t bits = rt.memory().load32(entity + offset);
        float value;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    };
    const std::uint32_t controller = entity + g::player::kController;
    const std::uint32_t progress = rt.memory().load32(g::kPlayerProgressPtr);
    char line[256];
    std::snprintf(line, sizeof line,
                  "[player] frame=%llu ent=0x%08X pos=(%.2f, %.2f, %.2f) yaw=%.2f vel=(%.2f, %.2f, %.2f) "
                  "stick=%.2f state=%u health=%u lives=%u score=%u",
                  static_cast<unsigned long long>(frame_present_count), entity,
                  f(g::player::kPosition), f(g::player::kPosition + 4u), f(g::player::kPosition + 8u),
                  f(g::player::kYaw), f(g::player::kVelocity), f(g::player::kVelocity + 4u),
                  f(g::player::kVelocity + 8u),
                  f(g::player::kController + g::controller::kStickMagnitude),
                  rt.memory().load32(controller + g::controller::kMoveState),
                  rt.memory().load32(controller + g::controller::kHealth),
                  progress != 0u ? rt.memory().load32(progress + g::kProgressLives) : 0u,
                  progress != 0u ? rt.memory().load32(progress + g::kProgressScore) : 0u);
    std::cerr << line << "\n";
    // Projection in use (GE state): near/far can be derived from it.
    const auto &p = last_perspective_projection;
    std::snprintf(line, sizeof line, "[player]   projection m00=%.3f m11=%.3f m22=%.4f m23=%.4f m32=%.4f m33=%.4f",
                  p[0], p[5], p[10], p[11], p[14], p[15]);
    std::cerr << line << "\n";
}

// PSPRECOMP_PLAYER_RECORD=<file>: append one record per presented frame while a
// player exists: "SBR1" header once, then {u32 frame, u32 buttons, u8 lx, u8 ly,
// u16 0, u32 entity, u8 object[0x928]}. tools/symbolize/player_record.py
// correlates button presses with the fields that change after them.
void player_record_tick(psprecomp::Runtime &rt) {
    static std::FILE *file = [] {
        const char *path = std::getenv("PSPRECOMP_PLAYER_RECORD");
        std::FILE *f = path != nullptr ? std::fopen(path, "wb") : nullptr;
        if (f != nullptr) {
            std::fwrite("SBR1", 1u, 4u, f);
            std::cerr << "[player-record] writing " << path << "\n";
        }
        return f;
    }();
    static std::uint64_t last_frame = ~std::uint64_t{0};
    if (file == nullptr || frame_present_count == last_frame) return;
    last_frame = frame_present_count;
    namespace g = spongebob::game;
    const std::uint32_t entity = g::find_player(rt.memory());
    if (entity == 0u) return;
    const std::uint8_t *object = rt.memory().raw_pointer(entity, g::kSpongeBobEntSize);
    if (object == nullptr) return;
    const std::uint32_t header[4] = {
        static_cast<std::uint32_t>(frame_present_count), last_pad_buttons,
        static_cast<std::uint32_t>(last_pad_lx) | (static_cast<std::uint32_t>(last_pad_ly) << 8u), entity};
    std::fwrite(header, sizeof header, 1u, file);
    std::fwrite(object, 1u, g::kSpongeBobEntSize, file);
    if ((frame_present_count % 60u) == 0u) std::fflush(file);  // Ctrl+C loses at most ~1 s
}

// PSPRECOMP_OBJECT_RECORD=<file>:<vtable>:<size>[:<vtable>:<size>...] records,
// every presented frame, each live object of the given classes:
// "SBO1" once, then {u32 frame, u32 buttons, u32 stick (lx | ly << 8),
// u32 address, u32 vtable, u32 size, u8 object[size]}. Objects are located by
// a RAM scan once per second and re-validated (vtable still there) every frame.
// tools/symbolize/player_record.py reads it (one report per object).
void object_record_tick(psprecomp::Runtime &rt) {
    struct Spec { std::uint32_t vtable, size; };
    static std::vector<Spec> specs;
    static std::FILE *file = [] {
        const char *env = std::getenv("PSPRECOMP_OBJECT_RECORD");
        if (env == nullptr) return static_cast<std::FILE *>(nullptr);
        std::string text(env);
        const std::size_t colon = text.find(':');
        if (colon == std::string::npos) return static_cast<std::FILE *>(nullptr);
        const std::string path = text.substr(0u, colon);
        const char *p = text.c_str() + colon + 1;
        while (*p != '\0') {
            char *end = nullptr;
            Spec spec{static_cast<std::uint32_t>(std::strtoul(p, &end, 0)), 0x400u};
            if (end == p) break;
            if (*end == ':') {
                p = end + 1;
                spec.size = static_cast<std::uint32_t>(std::strtoul(p, &end, 0));
            }
            specs.push_back(spec);
            p = (*end == ':') ? end + 1 : end;
        }
        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (f != nullptr) {
            std::fwrite("SBO1", 1u, 4u, f);
            std::cerr << "[object-record] writing " << path << " (" << specs.size() << " classes)\n";
        }
        return f;
    }();
    static std::uint64_t last_frame = ~std::uint64_t{0};
    static std::vector<std::pair<std::uint32_t, Spec>> live;  // address, class
    if (file == nullptr || frame_present_count == last_frame) return;
    last_frame = frame_present_count;
    constexpr std::uint32_t kScanBegin = 0x08B70000u, kScanEnd = 0x0A000000u;
    if (frame_present_count % 60u == 0u) {
        live.clear();
        const std::uint8_t *ram = rt.memory().raw_pointer(kScanBegin, kScanEnd - kScanBegin);
        for (std::uint32_t off = 0u; ram != nullptr && off + 4u <= kScanEnd - kScanBegin; off += 4u) {
            std::uint32_t word;
            std::memcpy(&word, ram + off, 4u);
            for (const Spec &spec : specs)
                if (word == spec.vtable && off + spec.size <= kScanEnd - kScanBegin)
                    live.emplace_back(kScanBegin + off, spec);
        }
    }
    for (const auto &[address, spec] : live) {
        if (rt.memory().load32(address) != spec.vtable) continue;
        const std::uint8_t *object = rt.memory().raw_pointer(address, spec.size);
        if (object == nullptr) continue;
        const std::uint32_t header[6] = {
            static_cast<std::uint32_t>(frame_present_count), last_pad_buttons,
            static_cast<std::uint32_t>(last_pad_lx) | (static_cast<std::uint32_t>(last_pad_ly) << 8u),
            address, spec.vtable, spec.size};
        std::fwrite(header, sizeof header, 1u, file);
        std::fwrite(object, 1u, spec.size, file);
    }
    if (frame_present_count % 60u == 0u) std::fflush(file);
}

// PSPRECOMP_PUPPET_TEST=1 — co-op groundwork. Takes one hub townsfolk (a
// CStdEnemyEnt driven by CDummyControl, i.e. a character with no brain) and,
// every frame, writes the player's world transform onto it, 30 frames late and
// 1.5 units to the side (+z). If it follows SpongeBob on screen, a remote player can
// be shown the same way. Animation is still the NPC's own.
void puppet_test_tick(psprecomp::Runtime &rt) {
    static const bool enabled = std::getenv("PSPRECOMP_PUPPET_TEST") != nullptr;
    if (!enabled) return;
    namespace g = spongebob::game;
    auto &mem = rt.memory();
    const std::uint32_t player = g::find_player(mem);
    static std::uint32_t puppet = 0u;
    static std::deque<std::array<std::uint32_t, 16>> history;
    if (player == 0u) { puppet = 0u; history.clear(); return; }
    const auto valid_puppet = [&](std::uint32_t ent) {
        if (ent == 0u || mem.load32(ent) != g::enemy::kEntVtable) return false;
        const std::uint32_t control = mem.load32(ent + g::enemy::ent::kControl);
        return control != 0u && mem.raw_pointer(control, 4u) != nullptr &&
               mem.load32(control) == g::enemy::kDummyControlVtable &&
               mem.load32(control + g::enemy::control::kEntity) == ent;  // live, linked both ways
    };
    if (!valid_puppet(puppet)) {
        puppet = 0u;
        if (frame_present_count % 60u != 0u) return;
        constexpr std::uint32_t kBegin = 0x08B70000u, kEnd = 0x0A000000u;
        const std::uint8_t *ram = mem.raw_pointer(kBegin, kEnd - kBegin);
        // Pick the live townsfolk closest to the player (likely on screen).
        const auto as_f = [](std::uint32_t bits) { float f; std::memcpy(&f, &bits, 4u); return f; };
        const auto pos = [&](std::uint32_t ent, std::uint32_t i) {
            return as_f(mem.load32(ent + g::player::kPosition + i * 4u));
        };
        float best = 1e30f;
        for (std::uint32_t off = 0u; ram != nullptr && off + 4u <= kEnd - kBegin; off += 4u) {
            std::uint32_t word;
            std::memcpy(&word, ram + off, 4u);
            if (word != g::enemy::kEntVtable || !valid_puppet(kBegin + off)) continue;
            float d2 = 0.0f;
            for (std::uint32_t i = 0u; i < 3u; ++i) {
                const float d = pos(kBegin + off, i) - pos(player, i);
                d2 += d * d;
            }
            std::cerr << "[puppet] candidate 0x" << std::hex << kBegin + off << std::dec
                      << " dist=" << std::sqrt(d2) << "\n";
            if (d2 < best) { best = d2; puppet = kBegin + off; }
        }
        if (puppet == 0u) return;
        std::cerr << "[puppet] using townsfolk 0x" << std::hex << puppet << std::dec
                  << " (dist " << std::sqrt(best) << ") as puppet\n";
    }
    std::array<std::uint32_t, 16> matrix{};
    for (std::uint32_t i = 0u; i < 16u; ++i) matrix[i] = mem.load32(player + g::player::kWorldMatrix + i * 4u);
    history.push_back(matrix);
    if (history.size() < 30u) return;
    std::array<std::uint32_t, 16> m = history.front();
    history.pop_front();
    const auto as_float = [](std::uint32_t bits) { float f; std::memcpy(&f, &bits, 4u); return f; };
    const auto as_bits = [](float f) { std::uint32_t b; std::memcpy(&b, &f, 4u); return b; };
    m[14] = as_bits(as_float(m[14]) + 1.5f);  // translation z (row 3): beside, not in front
    for (std::uint32_t i = 0u; i < 16u; ++i) {
        mem.store32(puppet + g::player::kWorldMatrix + i * 4u, m[i]);
        mem.store32(puppet + g::player::kRenderMatrix + i * 4u, m[i]);
    }
    for (std::uint32_t i = 0u; i < 3u; ++i) {  // position copy and previous position
        mem.store32(puppet + g::player::kPositionCopy + i * 4u, m[12u + i]);
        mem.store32(puppet + g::player::kVelocity + i * 4u, 0u);
    }
    if (frame_present_count % 120u == 0u)
        std::cerr << "[puppet] frame=" << frame_present_count << " puppet at (" << as_float(m[12]) << ", "
                  << as_float(m[13]) << ", " << as_float(m[14]) << ")\n";
}

void object_dump_tick(psprecomp::Runtime &rt) {
    static const struct Config {
        std::uint32_t vtable{}, size{}, every{};
        std::string dir;
    } config = [] {
        Config c;
        const char *env = std::getenv("PSPRECOMP_OBJECT_DUMP");
        if (env == nullptr) return c;
        char *end = nullptr;
        c.vtable = static_cast<std::uint32_t>(std::strtoul(env, &end, 0));
        if (end != nullptr && *end == ':') c.size = static_cast<std::uint32_t>(std::strtoul(end + 1, &end, 0));
        if (end != nullptr && *end == ':') c.every = static_cast<std::uint32_t>(std::strtoul(end + 1, &end, 0));
        c.dir = (end != nullptr && *end == ':') ? std::string(end + 1) : std::string(".");
        if (c.size == 0u) c.size = 0x400u;
        if (c.every == 0u) c.every = 60u;
        return c;
    }();
    static std::uint64_t last_frame = ~std::uint64_t{0};
    if (config.vtable == 0u || frame_present_count % config.every != 0u ||
        frame_present_count == last_frame) return;
    last_frame = frame_present_count;
    constexpr std::uint32_t kScanBegin = 0x08B70000u, kScanEnd = 0x0A000000u;
    const std::uint8_t *ram = rt.memory().raw_pointer(kScanBegin, kScanEnd - kScanBegin);
    if (ram == nullptr) return;
    for (std::uint32_t off = 0u; off + config.size <= kScanEnd - kScanBegin; off += 4u) {
        std::uint32_t word;
        std::memcpy(&word, ram + off, 4u);
        if (word != config.vtable) continue;
        const std::string path = config.dir + "/obj_" + psprecomp::hex32(kScanBegin + off) + "_" +
                                 std::to_string(frame_present_count) + ".bin";
        if (std::FILE *f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(ram + off, 1u, config.size, f);
            std::fclose(f);
        }
    }
}

void register_display_hle(psprecomp::Runtime &rt) {
    // sceDisplayWaitVblank* — park the caller until the next virtual VBlank.
    const auto wait_vblank = [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        ++vblank_wait_count;
        // Closing the window ends the run cleanly (files flushed, audio closed).
        if (display_window_closed()) {
            rt.stop("window closed");
            return;
        }
        object_dump_tick(rt);
        player_log_tick(rt);
        player_record_tick(rt);
        object_record_tick(rt);
        puppet_test_tick(rt);
        if (vblank_wait_count % 100u == 0u) {
            std::cerr << "[display] wait_vblank count=" << vblank_wait_count
                      << " pc=" << psprecomp::hex32(ctx.gpr[31]) << "\n";
        }
        // Advance GE and present the current frame before parking.
        spongebob_profile_tick(rt);
        pace_virtual_time_to_wall_clock();
        const std::uint64_t phase = virtual_time_us % kVblankPeriodUs;
        suspend_for_wakeup(rt, ctx, virtual_time_us + (kVblankPeriodUs - phase));
    };

    rt.register_hle("sceDisplay", 0x0E20F177u,  // sceDisplaySetMode
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            display_state.mode = ctx.gpr[4];
            display_state.width = ctx.gpr[5];
            display_state.height = ctx.gpr[6];
            ctx.set_gpr(2, 0u);
        });
    rt.register_hle("sceDisplay", 0xDEA197D4u,  // sceDisplayGetMode
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (ctx.gpr[4] != 0u) rt.memory().store32(ctx.gpr[4], display_state.mode);
            if (ctx.gpr[5] != 0u) rt.memory().store32(ctx.gpr[5], display_state.width);
            if (ctx.gpr[6] != 0u) rt.memory().store32(ctx.gpr[6], display_state.height);
            ctx.set_gpr(2, 0u);
        });
    rt.register_hle("sceDisplay", 0x289D82FEu,  // sceDisplaySetFrameBuf
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t framebuf = ctx.gpr[4];
            display_state.frame_buffer = framebuf;
            display_state.buffer_width = ctx.gpr[5];
            display_state.pixel_format = ctx.gpr[6];
            if (!has_last_frame_buffer || framebuf != last_frame_buffer) {
                if (has_last_frame_buffer) ++frame_present_count;  // a new buffer was presented
                last_frame_buffer = framebuf;
                has_last_frame_buffer = true;
            }
            ge_gpu_backend_set_display_framebuffer(framebuf, display_state.width, display_state.height);
            std::cerr << "[display] framebuf=" << psprecomp::hex32(framebuf)
                      << " width=" << ctx.gpr[5]
                      << " height=" << display_state.height << "\n";
            ctx.set_gpr(2, 0u);
        });
    rt.register_hle("sceDisplay", 0xEEDA2E54u,  // sceDisplayGetFrameBuf
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (ctx.gpr[4] != 0u) rt.memory().store32(ctx.gpr[4], display_state.frame_buffer);
            if (ctx.gpr[5] != 0u) rt.memory().store32(ctx.gpr[5], display_state.buffer_width);
            if (ctx.gpr[6] != 0u) rt.memory().store32(ctx.gpr[6], display_state.pixel_format);
            ctx.set_gpr(2, 0u);
        });
    rt.register_hle("sceDisplay", 0x984C27E7u, wait_vblank);  // sceDisplayWaitVblankStart
    rt.register_hle("sceDisplay", 0x46F186C3u, wait_vblank);  // sceDisplayWaitVblankStartCB
    rt.register_hle("sceDisplay", 0x36CDFADEu, wait_vblank);  // sceDisplayWaitVblank
    rt.register_hle("sceDisplay", 0x8EB9EC49u, wait_vblank);  // sceDisplayWaitVblankCB
    rt.register_hle("sceDisplay", 0x9C6EAAD7u,  // sceDisplayGetVcount
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, static_cast<std::uint32_t>(virtual_time_us / kVblankPeriodUs));
        });
}

// TAREA 4 diagnostics: log the first entry to the three hot main-loop PCs so we
// can see which generated unit (or import stub) each one belongs to.
void log_main_loop_entries(psprecomp::Runtime &, psprecomp::AllegrexContext &,
                           std::uint32_t dispatch_pc, std::int32_t) {
    struct Watch { std::uint32_t pc; const char *name; };
    static const Watch kWatch[] = {
        {0x08971930u, "recomp_unit_0011"},
        {0x08A2C448u, "import_29 (sceSasCore::0xA3589D81)"},
        {0x08A08EBCu, "recomp_unit_0016"},
    };
    static bool reported[3] = {false, false, false};
    for (std::size_t i = 0u; i < 3u; ++i) {
        if (dispatch_pc == kWatch[i].pc && !reported[i]) {
            reported[i] = true;
            std::cerr << "[mainloop] enter pc=" << psprecomp::hex32(dispatch_pc)
                      << " unit=" << kWatch[i].name << "\n";
            break;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Engine debug log (opt-in with PSPRECOMP_GAME_LOG=1).
//
// The Whiptail engine's debug printf is sub_089C9E34(fmt, ...): it vsprintf's
// into a stack buffer and hands it to sub_089C9D14(channel, message), which
// filters by channel (off in retail) and forwards to libc printf. The messages
// name engine classes (CPSPSystem, CWFileMgr, ...) and report failures such as
// missing resources. The sink cannot be hooked (its caller reaches it through
// an intra-unit goto), so replace the printf entry and format it here. Callers
// inside the same generated unit still bypass the hook.
// ---------------------------------------------------------------------------
std::string format_guest_printf(psprecomp::Runtime &rt, const psprecomp::AllegrexContext &ctx) {
    // Allegrex EABI-style varargs: fmt in a0, integer args in a1-a3, t0-t3.
    static constexpr int kArgRegs[] = {5, 6, 7, 8, 9, 10, 11};
    std::size_t next_arg = 0u;
    const auto arg = [&]() -> std::uint32_t {
        return next_arg < std::size(kArgRegs) ? ctx.gpr[kArgRegs[next_arg++]] : 0u;
    };
    std::string fmt;
    try { fmt = rt.memory().read_c_string(ctx.gpr[4], 1024u); } catch (...) { return {}; }
    std::string out;
    for (std::size_t i = 0u; i < fmt.size(); ++i) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        std::size_t j = i + 1u;
        while (j < fmt.size() && std::strchr("-+ #0123456789.lhz", fmt[j]) != nullptr) ++j;
        if (j >= fmt.size()) break;
        const std::string spec = fmt.substr(i, j - i);  // flags/width without length mods.
        std::string clean;
        for (char c : spec) if (c != 'l' && c != 'h' && c != 'z') clean += c;
        char buffer[512];
        switch (fmt[j]) {
        case '%': out += '%'; break;
        case 's': {
            std::string text;
            const std::uint32_t address = arg();
            try { text = address != 0u ? rt.memory().read_c_string(address, 256u) : "(null)"; } catch (...) { text = "(bad)"; }
            std::snprintf(buffer, sizeof buffer, (clean + 's').c_str(), text.c_str());
            out += buffer;
            break;
        }
        case 'd': case 'i':
            std::snprintf(buffer, sizeof buffer, (clean + 'd').c_str(), static_cast<int>(arg()));
            out += buffer;
            break;
        case 'u': case 'x': case 'X': case 'o': case 'c':
            std::snprintf(buffer, sizeof buffer, (clean + fmt[j]).c_str(), static_cast<unsigned>(arg()));
            out += buffer;
            break;
        case 'p':
            out += psprecomp::hex32(arg());
            break;
        default:  // floats etc. are not decoded; show the raw word.
            out += "<" + spec + fmt[j] + ":" + psprecomp::hex32(arg()) + ">";
            break;
        }
        i = j;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

void register_game_log(psprecomp::Runtime &rt) {
    if (std::getenv("PSPRECOMP_GAME_LOG") == nullptr) return;
    rt.register_function(0x089C9E34u,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::string caller = symbol_for(ctx.gpr[31]);
            std::cerr << "[game] " << format_guest_printf(rt, ctx);
            if (!caller.empty()) std::cerr << "  (" << caller << ")";
            std::cerr << "\n";
            ctx.pc = ctx.gpr[31];
        },
        "game_debug_printf");
}

void install_spongebob_profile(psprecomp::Runtime &rt) {
    install_symbols(rt);  // phase-2 names in crash dumps, traces and the game log.
    // Registrar module_start como uid 1 (por si acaso).  En el PSP real el
    // module_start es un thread del kernel; aquí no estaba en la tabla, así que
    // su `jal 0x00000000` no encontraba el uid y hacía un switch sin completar.
    {
        ThreadRecord main;
        main.uid = 1u;
        main.entry = 0x08804124u;
        main.stack_base = 0x09FFFF00u;   // mismo que k0/sp del main.
        main.stack_size = 0u;
        main.state = ThreadState::Running;
        main.name = "module_start";
        thread_table.threads[1u] = main;
    }

    register_sas_hle(rt);        // sceSasCore (27) — full software implementation.
    register_ge_hle(rt);         // sceGe_user (14).
    register_atrac3_stubs(rt);   // sceAtrac3plus (8) — stubs.
    register_sysmem_hle(rt);     // SysMemUserForUser (8) — Fase A.
    register_threadman_hle(rt);  // ThreadManForUser (9) — Fase A.
    register_display_hle(rt);    // sceDisplay (8) — Fase B (VBlank + framebuffer).
    register_other_stubs(rt);    // everything else (IoFileMgrForUser, ...).
    register_audio_hle(rt);      // sceAudio (reserve + output + pacing) — Fase E.

    // Fake Memory Stick: make the save-data (checkpoint) dialog finish
    // instantly so the game initializes its save-data heap instead of treating
    // the stick as "not inserted" (which left the heap pointer 0x08A8268C null
    // and crashed "new" at 0x0898CF60). Overrides the generic return-0 stubs
    // registered above (register_hle overwrites).
    rt.register_hle("sceUtility", 0x8874DBE0u,  // sceUtilitySavedataGetStatus
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            std::cerr << "[savedata] GetStatus -> 4 (FINISHED, fake)\n";
            ctx.set_gpr(2, 4u);  // PSP_UTILITY_DIALOG_FINISHED
        });
    rt.register_hle("sceUtility", 0x50C4CD57u,  // sceUtilitySavedataInitStart
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            std::cerr << "[savedata] InitStart -> 0 (fake)\n";
            ctx.set_gpr(2, 0u);
        });
    rt.register_hle("sceUtility", 0x9790B33Cu,  // sceUtilitySavedataShutdownStart
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            std::cerr << "[savedata] ShutdownStart -> 0 (fake)\n";
            ctx.set_gpr(2, 0u);
        });
    rt.register_hle("sceUtility", 0xD4B95FFBu,  // sceUtilitySavedataUpdate
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2, 0u);
        });

    register_game_log(rt);      // PSPRECOMP_GAME_LOG: print the engine's debug messages.

    // PSP kernel syscall dispatch: a `jal 0x00000000` (the thread-return
    // syscall, syscall 0) jumps to guest address 0. Map it to the thread
    // completion handler so the module_start thread can hand off to the worker
    // thread it created via sceKernelStartThread (mirrors the VCS profile's
    // `vcs_module_thread_return` registered at the same address).
    rt.register_function(0x00000000u, &complete_current_thread, "psp_thread_return");

    // Fase B: execution-driven virtual time. The starvation hook advances the
    // clock every N dispatches and promotes expired VBlank/delay waiters; the
    // scheduler idle path (activate_next_thread) jumps straight to the next
    // wakeup deadline when nothing is runnable.
    psprecomp::set_runtime_starvation_hook(
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            virtual_time_us += kTickMicroseconds;
            promote_expired_delays();

            // Spin detector: a PC that never advances across consecutive ticks is
            // a cooperative busy-wait.  Each tick covers kTickDispatchInterval
            // dispatches, so kSpinYieldTicks ticks ~= kSpinYieldTicks*256
            // dispatches stuck on one PC.  Forcing a yield here (the analogue of
            // the Wii "blr yield") lets the ready worker thread run and flip the
            // flag the spinner is waiting on.
            ++starvation_tick_total;
            if (ctx.pc == busywait_tracked_pc) {
                ++busywait_same_pc_count;
            } else {
                busywait_tracked_pc = ctx.pc;
                busywait_same_pc_count = 1u;
            }
            if (busywait_same_pc_count == kSpinYieldTicks) {
                busywait_same_pc_count = 0u;
                static std::uint64_t spin_yield_count = 0u;
                ++spin_yield_count;
                if (spin_yield_count <= 10u || (spin_yield_count % 1000u) == 0u) {
                    std::cerr << "[spin-yield] #" << spin_yield_count
                              << " pc=" << psprecomp::hex32(ctx.pc)
                              << " from_uid=" << thread_table.current_uid
                              << " queue_size=" << thread_table.ready_queue.size()
                              << " dispatch=" << (starvation_tick_total * kTickDispatchInterval) << "\n";
                }
                yield_current_thread(rt, ctx);
            }

            // Frame summary every ~100k dispatches (~390 ticks).
            if (++frame_report_ticks % 390u == 0u) {
                std::cerr << "[frame] frames=" << frame_present_count
                          << " enqueues=" << ge_enqueue_count
                          << " draws=" << ge_draw_sync_count
                          << " fb=" << psprecomp::hex32(last_frame_buffer) << "\n";
            }
        },
        kTickDispatchInterval);

    // TAREA 4 diagnostics: log the first entry to each hot main-loop PC.
    psprecomp::set_runtime_post_dispatch_hook(&log_main_loop_entries);
}

void spongebob_profile_tick(psprecomp::Runtime &rt) {
    // Advance any still-queued GE display lists (they are normally executed
    // synchronously on enqueue; this drains leftovers defensively).
    for (std::uint32_t id : ge_list_table.queue) {
        auto it = ge_list_table.lists.find(id);
        if (it != ge_list_table.lists.end() && it->second.state == GeListState::Queued) {
            ProfileScope scope(frame_profile.ge_ns);
            (void)execute_ge_list(rt, it->second);
        }
    }
    ge_list_table.queue.clear();

    // Present the current display framebuffer to the host window.
    present_display_frame(rt);
}

} // namespace spongebob

