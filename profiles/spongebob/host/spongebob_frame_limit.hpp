#pragma once

#include <chrono>

namespace spongebob {

[[nodiscard]] bool spongebob_frame_limiter_unlocked() noexcept;

void precise_sleep_until(std::chrono::steady_clock::time_point deadline) noexcept;

}
