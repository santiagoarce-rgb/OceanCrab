#pragma once
// CSpongeBobEnt: the player entity (phase 2 reverse engineering).
//
// Class chain (GCC RTTI): CSpongeBobEnt -> CCharEntBase -> CTrackedAnimEnt ->
// ... -> CWRenderable -> CWEntity. Allocated by the factory at 0x08A195A4
// (operator new 0x089C4998, size 0x928), constructed at 0x0882A14C.
//
// Offsets were found by diffing live objects while walking (see NOTES.md,
// PSPRECOMP_OBJECT_DUMP). Confidence: "confirmed" = seen changing as expected
// in-game; "likely" = consistent but not tested in isolation.

#include "psprecomp/guest_memory.hpp"

#include <cstdint>

namespace spongebob::game {

// Game-session manager (316 bytes, no RTTI; created at 0x08805680 with ctor
// 0x0881CCA0, deleted at 0x08804704). The player constructor registers itself here.
inline constexpr std::uint32_t kGameSessionPtr = 0x08A82488u;  // CGameSession* (guest global)
inline constexpr std::uint32_t kSessionPlayer = 0x124u;       // CSpongeBobEnt*       (confirmed)
inline constexpr std::uint32_t kSessionEntityLoader = 0x11Cu; // loads .ent effects   (likely)

inline constexpr std::uint32_t kSpongeBobEntVtable = 0x08A74978u;
inline constexpr std::uint32_t kSpongeBobEntSize = 0x928u;

namespace player {
inline constexpr std::uint32_t kVtable = 0x000u;
inline constexpr std::uint32_t kYaw = 0x04Cu;              // float radians          (confirmed)
inline constexpr std::uint32_t kPositionCopy = 0x054u;     // float x,y,z            (confirmed)
inline constexpr std::uint32_t kWorldMatrix = 0x0B8u;      // float[16], row-major 4x4 (confirmed)
inline constexpr std::uint32_t kPosition = 0x0E8u;         // float x,y,z = matrix row 3 (confirmed)
inline constexpr std::uint32_t kVelocity = 0x160u;         // float x,y,z units/s    (confirmed)
inline constexpr std::uint32_t kAnimTime = 0x1BCu;         // float seconds          (likely)
inline constexpr std::uint32_t kAnimFrame = 0x1E0u;        // u32 current, +4 next   (likely)
inline constexpr std::uint32_t kRenderMatrix = 0x404u;     // float[16] copy         (likely)
inline constexpr std::uint32_t kInterfaceVtables = 0x450u; // 2 secondary vtables (multiple inheritance)
inline constexpr std::uint32_t kBoundsMin = 0x458u;        // float x,y,z            (likely)
inline constexpr std::uint32_t kBoundsMax = 0x464u;        // float x,y,z            (likely)
inline constexpr std::uint32_t kController = 0x470u;       // embedded CSpongeBobController
inline constexpr std::uint32_t kMorph = 0x7C0u;            // embedded CWMorphController
inline constexpr std::uint32_t kPreviousPosition = 0x764u; // float x,y,z, one frame behind (likely)
inline constexpr std::uint32_t kEffects = 0x8ECu;          // embedded CEffectHandler
}  // namespace player

// CSpongeBobController (ctor 0x08854E44), offsets relative to the controller.
namespace controller {
inline constexpr std::uint32_t kWantedVelocity = 0x088u;   // float x,y,z            (likely)
inline constexpr std::uint32_t kStickMagnitude = 0x094u;   // float 0..1             (confirmed)
inline constexpr std::uint32_t kWantedYaw = 0x098u;        // float radians          (likely)
inline constexpr std::uint32_t kYaw = 0x09Cu;              // float radians          (likely)
}  // namespace controller

// Returns the live player entity, or 0 when no level is loaded.
[[nodiscard]] inline std::uint32_t find_player(const psprecomp::GuestMemory &memory) {
    const std::uint32_t session = memory.load32(kGameSessionPtr);
    if (session == 0u) return 0u;
    const std::uint32_t entity = memory.load32(session + kSessionPlayer);
    if (entity == 0u || memory.load32(entity + player::kVtable) != kSpongeBobEntVtable) return 0u;
    return entity;
}

}  // namespace spongebob::game
