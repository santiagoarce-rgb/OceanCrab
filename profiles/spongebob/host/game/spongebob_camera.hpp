#pragma once
// CCameraStory: the gameplay camera in story mode (phase 2 reverse engineering).
//
// Class chain (GCC RTTI): CCameraStory -> CCameraBase. Ctor 0x0880B848. Alive
// while a level/hub is playable; CWStandardCamera (vtable 0x08A73EE8) exists
// from boot and is the platform view, not the gameplay rig.
//
// Mapped with PSPRECOMP_OBJECT_RECORD + tools/symbolize/player_record.py in the
// hub: the eye orbits the target at a fixed distance (~3.11, eye 0.81 above
// the target), the target is the player position + 1.3 in y, and L recentres
// the eye behind the player.

#include <cstdint>

namespace spongebob::game::camera {

inline constexpr std::uint32_t kStoryCameraVtable = 0x08A741D0u;

// Each vector has three copies; during the L recentre the second one lags the
// others, so they look like current / smoothed / goal.
inline constexpr std::uint32_t kEye = 0x008u;            // float x,y,z  (confirmed)
inline constexpr std::uint32_t kEyeSmoothed = 0x014u;    // float x,y,z  (likely)
inline constexpr std::uint32_t kEyeGoal = 0x020u;        // float x,y,z  (likely)
inline constexpr std::uint32_t kTarget = 0x084u;         // float x,y,z = player + (0, 1.3, 0) (confirmed)
inline constexpr std::uint32_t kTargetSmoothed = 0x090u; // float x,y,z  (likely)
inline constexpr std::uint32_t kTargetGoal = 0x09Cu;     // float x,y,z  (likely)
inline constexpr std::uint32_t kEyeCopy = 0x190u;        // float x,y,z  (likely)
inline constexpr std::uint32_t kTargetCopy = 0x1A8u;     // float x,y,z  (likely)

}  // namespace spongebob::game::camera
