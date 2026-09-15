#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace moex::plaza2::cgate {

inline constexpr std::int64_t kPlaza2MaxLogClockSkewNs = 1'000'000'000;

struct Plaza2ClockSample {
    std::int64_t local_wall_ns{0};
    std::uint64_t local_monotonic_ns{0};
    std::int64_t exchange_wall_ns{0};
};

struct Plaza2ClockEvidence {
    std::string sync_source;
    bool sync_status_ok{false};
    std::optional<std::int64_t> wall_offset_ns;
    std::string monotonic_clock_id;
    std::vector<Plaza2ClockSample> paired_samples;
};

[[nodiscard]] inline bool plaza2_clock_evidence_passes(const Plaza2ClockEvidence& evidence) noexcept {
    if (evidence.sync_source.empty() || !evidence.sync_status_ok || !evidence.wall_offset_ns.has_value() ||
        evidence.monotonic_clock_id.empty() || evidence.paired_samples.empty()) {
        return false;
    }
    const auto within_limit = [](std::int64_t lhs, std::int64_t rhs) {
        if (lhs >= rhs)
            return lhs - rhs <= kPlaza2MaxLogClockSkewNs;
        return rhs - lhs <= kPlaza2MaxLogClockSkewNs;
    };
    if (!within_limit(*evidence.wall_offset_ns, 0))
        return false;
    for (const auto& sample : evidence.paired_samples) {
        if (!within_limit(sample.local_wall_ns, sample.exchange_wall_ns))
            return false;
    }
    return true;
}

} // namespace moex::plaza2::cgate
