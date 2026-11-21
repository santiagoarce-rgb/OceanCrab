#pragma once
// CStdEnemyEnt + CStdEnemyControl: standard enemies (phase 2 reverse engineering).
//
// Class chains (GCC RTTI): CStdEnemyEnt -> CCharEntBase (same base as the
// player, so the CWEntity/CCharEntBase fields match: position at +0xE8, world
// matrix at +0xB8). Control: CStdEnemyControl -> CCharControlBase, with
// per-enemy subclasses (Jose, Keith, Lyle, Gopher, Flyer, TimerFlyer, Spring,
// Turret, Bomb, PathSpinner). AI: CStdEnemyAI -> CCharInputBase.
// The hub's townsfolk are CStdEnemyEnt too, driven by CDummyControl.
//
// Mapped from a recorded session in level 1 (PSPRECOMP_OBJECT_RECORD +
// tools/symbolize/player_record.py): three enemies fought and killed with the
// spatula.

#include <cstdint>

namespace spongebob::game::enemy {

inline constexpr std::uint32_t kEntVtable = 0x08A74A70u;      // CStdEnemyEnt
inline constexpr std::uint32_t kEntSize = 0x484u;             // ctor 0x0882D210, factory 0x08A19570
inline constexpr std::uint32_t kControlVtable = 0x08A76DF8u;  // CStdEnemyControl
inline constexpr std::uint32_t kDummyControlVtable = 0x08A76D38u;  // hub NPCs
inline constexpr std::uint32_t kAiVtable = 0x08A773D8u;       // CStdEnemyAI

namespace ent {
inline constexpr std::uint32_t kPosition = 0x0E8u;   // float x,y,z (confirmed)
inline constexpr std::uint32_t kControl = 0x470u;    // CStdEnemyControl* (pointer, not embedded) (confirmed)
}  // namespace ent

namespace control {
inline constexpr std::uint32_t kEntity = 0x010u;       // CStdEnemyEnt* back pointer (confirmed)
inline constexpr std::uint32_t kAttackFlags = 0x034u;  // u32, bit 0 set while attacking (likely)
inline constexpr std::uint32_t kStriking = 0x040u;     // u32 1 during the strike (likely)
inline constexpr std::uint32_t kHealth = 0x054u;       // u32, 2 = full, 0 = defeated (confirmed)
inline constexpr std::uint32_t kAttackPhase = 0x0B8u;  // u32 0 idle, 1 wind-up, 2 strike (confirmed)
}  // namespace control

}  // namespace spongebob::game::enemy
