#include "spongebob_profile.hpp"
#include "spongebob_sas.hpp"
#include "ge_renderer.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
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

GeState ge_state{};
GeListTable ge_list_table{};

constexpr std::uint32_t kEdramBase = 0x04000000u;  // PSP VRAM base.
constexpr std::uint32_t kEdramSize = 0x00200000u;  // 2 MB.

// Instrumentation counters (diagnostics): how often the guest drives the GE and
// display, and whether it is actually presenting frames or spinning in place.
std::uint64_t ge_enqueue_count{};
std::uint64_t ge_list_sync_count{};
std::uint64_t ge_draw_sync_count{};
std::uint64_t frame_present_count{};
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

void ge_list_enqueue(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
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
    ge_list_table.lists[record.guest_id] = record;
    ge_list_table.queue.push_back(record.guest_id);
    ctx.set_gpr(2, record.guest_id);  // return list id.
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
    {"sceDisplay", 0x0E20F177u}, {"sceDisplay", 0x289D82FEu}, {"sceDisplay", 0x46F186C3u},
    {"sceDisplay", 0x984C27E7u}, {"sceDisplay", 0x9C6EAAD7u},
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
    {"sceUmdUser", 0x46EBB729u}, {"sceUmdUser", 0x6B4A146Cu}, {"sceUmdUser", 0x8EF08FCEu}, {"sceUmdUser", 0xC6183D47u},
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
};

struct ThreadTable {
    std::uint32_t current_uid{1u};              // uid 1 == module_start thread.
    std::uint32_t next_uid{2u};
    std::uint32_t next_stack_top{0x0A000000u};  // PSP user RAM top; stacks grow down.
    std::map<std::uint32_t, ThreadRecord> threads;
    std::vector<std::uint32_t> ready_queue;
};

ThreadTable thread_table{};

struct PartitionBlock {
    std::string name;
    std::uint32_t address{};
    std::uint32_t size{};
};

struct PartitionTable {
    std::uint32_t next_uid{1u};
    std::uint32_t next_address{0x09000000u};  // below the thread stacks; grows up.
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

// Advance virtual time by the real elapsed microseconds since the last poll and
// return the new value. Guest code that spins on sceKernelGetSystemTime* without
// dispatching enough to trip the starvation hook still sees the clock move.
std::uint64_t advance_system_time() {
    static std::uint64_t last_wall_us = 0u;
    const std::uint64_t now = guest_time_us();
    if (now > last_wall_us) virtual_time_us += now - last_wall_us;
    last_wall_us = now;
    return virtual_time_us;
}

std::uint32_t allocate_stack(std::uint32_t size) {
    std::uint32_t aligned = (size + 0xFFu) & ~0xFFu;
    if (aligned < 0x1000u) aligned = 0x1000u;
    thread_table.next_stack_top -= aligned;
    return thread_table.next_stack_top;  // stack base (bottom).
}

// Wake any Waiting thread whose virtual-time deadline has passed.
void promote_expired_delays() {
    for (auto &[uid, record] : thread_table.threads) {
        if (record.state == ThreadState::Waiting && record.wake_at_us != 0u &&
            record.wake_at_us <= virtual_time_us) {
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
// after the HLE call at $ra) and switch to the next runnable thread. This is the
// Wii "blr yield" analogue — without it, a spinner that never blocks would drain
// the Ready queue and leave every thread stranded in "Running" state.
void yield_current_thread(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    if (auto it = thread_table.threads.find(thread_table.current_uid);
        it != thread_table.threads.end()) {
        it->second.context = ctx;
        it->second.context.pc = ctx.gpr[31];   // resume after the HLE call.
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

void complete_current_thread(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
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
    // sceKernelMaxFreeMemSize.
    rt.register_hle("SysMemUserForUser", 0xA291F107u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 8u * 1024u * 1024u); });
    // sceKernelTotalFreeMemSize.
    rt.register_hle("SysMemUserForUser", 0xF919F628u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 8u * 1024u * 1024u); });
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
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto it = thread_table.threads.find(ctx.gpr[4]);
            if (it == thread_table.threads.end()) { ctx.set_gpr(2, 0x80020198u); return; }
            std::cerr << "[thread] start uid=" << ctx.gpr[4] << "\n";
            it->second.state = ThreadState::Ready;
            it->second.context.gpr[4] = ctx.gpr[5];  // arglen -> thread a0.
            it->second.context.gpr[5] = ctx.gpr[6];  // argp   -> thread a1.
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
            const auto target = thread_table.threads.find(ctx.gpr[4]);
            if (target == thread_table.threads.end()) { ctx.set_gpr(2, 0x80020198u); return; }
            if (target->second.state == ThreadState::Completed) {
                ctx.set_gpr(2, 0u);
                return;
            }
            if (auto current = thread_table.threads.find(thread_table.current_uid);
                current != thread_table.threads.end()) {
                current->second.state = ThreadState::Waiting;
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
                thread_table.threads.erase(it);
            }
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
                                    0xF8170FBEu, 0x0DDCD2C9u, 0x6B30100Fu,
                                    0x9FA03CD3u, 0xB011B11Fu, 0xB7D098C6u}) {
        rt.register_hle("ThreadManForUser", nid,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    }

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

void register_display_hle(psprecomp::Runtime &rt) {
    // sceDisplayWaitVblank* — park the caller until the next virtual VBlank.
    const auto wait_vblank = [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        ++vblank_wait_count;
        if (vblank_wait_count % 100u == 0u) {
            std::cerr << "[display] wait_vblank count=" << vblank_wait_count
                      << " pc=" << psprecomp::hex32(ctx.gpr[31]) << "\n";
        }
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

void install_spongebob_profile(psprecomp::Runtime &rt) {
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
    // Minimal GE advance: mark every queued display list completed and clear the
    // queue. Full command walking + render_ge_primitive() comes in a later phase.
    for (std::uint32_t id : ge_list_table.queue) {
        auto it = ge_list_table.lists.find(id);
        if (it != ge_list_table.lists.end()) it->second.state = GeListState::Completed;
    }
    ge_list_table.queue.clear();
    (void)rt;
}

} // namespace spongebob

