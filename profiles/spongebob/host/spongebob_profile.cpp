#include "spongebob_sas.hpp"
#include "ge_renderer.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>
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
    const auto it = ge_list_table.lists.find(list_id);
    if (it != ge_list_table.lists.end()) it->second.state = GeListState::Completed;
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
    rt.register_hle("sceGe_user", 0xB287BD61u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });          // sceGeDrawSync
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
    {"SysMemUserForUser", 0x13A5ABEFu}, {"SysMemUserForUser", 0x237DBD4Fu}, {"SysMemUserForUser", 0x91DE343Cu},
    {"SysMemUserForUser", 0x9D9A5BA1u}, {"SysMemUserForUser", 0xA291F107u}, {"SysMemUserForUser", 0xB6D61D02u},
    {"SysMemUserForUser", 0xF77D77CBu}, {"SysMemUserForUser", 0xF919F628u},
    {"ThreadManForUser", 0x0DDCD2C9u}, {"ThreadManForUser", 0x1FB15A32u}, {"ThreadManForUser", 0x278C0DF5u},
    {"ThreadManForUser", 0x293B45B8u}, {"ThreadManForUser", 0x369ED59Du}, {"ThreadManForUser", 0x402FCF22u},
    {"ThreadManForUser", 0x446D8DE6u}, {"ThreadManForUser", 0x55C20A00u}, {"ThreadManForUser", 0x623AE665u},
    {"ThreadManForUser", 0x68DA9E36u}, {"ThreadManForUser", 0x6B30100Fu}, {"ThreadManForUser", 0x809CE29Bu},
    {"ThreadManForUser", 0x82BC5777u}, {"ThreadManForUser", 0x9FA03CD3u}, {"ThreadManForUser", 0xAA73C935u},
    {"ThreadManForUser", 0xB011B11Fu}, {"ThreadManForUser", 0xB7D098C6u}, {"ThreadManForUser", 0xC07BB470u},
    {"ThreadManForUser", 0xCEADEB47u}, {"ThreadManForUser", 0xE81CAF8Fu}, {"ThreadManForUser", 0xEDBA5844u},
    {"ThreadManForUser", 0xEF9E4C70u}, {"ThreadManForUser", 0xF475845Du}, {"ThreadManForUser", 0xF8170FBEu},
    {"UtilsForUser", 0x27CC57F0u}, {"UtilsForUser", 0x3EE30821u}, {"UtilsForUser", 0x71EC4271u},
    {"UtilsForUser", 0x79D1C3FAu}, {"UtilsForUser", 0x91E4F6A7u},
};

void register_other_stubs(psprecomp::Runtime &rt) {
    for (const ImportStub &stub : kOtherStubs) {
        register_generic_stub(rt, stub.library, stub.nid);
    }
}

} // namespace

void install_spongebob_profile(psprecomp::Runtime &rt) {
    register_sas_hle(rt);        // sceSasCore (27) — full software implementation.
    register_ge_hle(rt);         // sceGe_user (14).
    register_atrac3_stubs(rt);   // sceAtrac3plus (8) — stubs.
    register_other_stubs(rt);    // everything else (IoFileMgrForUser, ThreadManForUser, ...).
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

