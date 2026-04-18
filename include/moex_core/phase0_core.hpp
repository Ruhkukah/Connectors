#pragma once

#include <cstdint>
#include <string>

namespace moex::phase0 {

struct BuildInfo {
    std::string project_version;
    std::string source_root;
};

struct BackpressureCounters {
    std::uint64_t produced = 0;
    std::uint64_t polled = 0;
    std::uint64_t dropped = 0;
    std::uint64_t high_watermark = 0;
    bool overflowed = false;
};

enum class EventLossPolicy {
    Lossless,
    DropOldest
};

BuildInfo build_info();

bool prod_requires_arm(std::string_view environment, bool armed);

BackpressureCounters make_seed_counters(EventLossPolicy policy);

}  // namespace moex::phase0
