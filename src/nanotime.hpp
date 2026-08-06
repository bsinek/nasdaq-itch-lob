#pragma once
#include <cstdint>
#include <time.h>

namespace itch {

// Monotonic nanoseconds. On Apple Silicon this is the mach counter (24 MHz →
// ~41.7 ns granularity); the latency benchmark measures and reports the
// actual tick size rather than assuming it.
inline uint64_t now_ns() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

}  // namespace itch
