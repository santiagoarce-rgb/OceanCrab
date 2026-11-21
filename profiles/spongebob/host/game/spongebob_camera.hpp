#pragma once
// CCameraStory: the gameplay camera in story mode (phase 2 reverse engineering).
//
// Class chain (GCC RTTI): CCameraStory -> CCameraBase. Ctor 0x0880B848. Alive
// while a level/hub is playable; CWStandardCamera (vtable 0x08A73EE8) exists
// from boot and is the platform view, not the gameplay rig.
//
// Mapped with PSPRECOMP_OBJECT_RECORD + tools/symbolize/player_record.py in the
// hub and level 1: the eye orbits the target at a fixed distance (+0x1D0,
// ~3.11), the target is the player position + 1.3 in y, and L recentres the
// eye behind the player. Holding R turns the stick into a free orbit: x spins
// the azimuth (~4.5 deg/frame at full tilt), y the elevation (clamped near
// -4.4 deg at the bottom), and the player stops walking (speed 0.20 vs 3.57).
// No angle is stored: the orbit state is the eye position itself, rotated
// about the target each frame. Released, the elevation drifts back to ~7 deg.

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
inline constexpr std::uint32_t kOrbitDistance = 0x1D0u;  // float ~3.11  (confirmed)

}  // namespace spongebob::game::camera
