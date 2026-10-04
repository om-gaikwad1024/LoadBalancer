#pragma once

#include <chrono>

namespace lb {

// Every interval and timeout in the engine uses the monotonic clock (plan II.8).
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;

}  // namespace lb
