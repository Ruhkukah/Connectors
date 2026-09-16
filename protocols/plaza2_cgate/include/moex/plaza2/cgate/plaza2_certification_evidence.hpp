#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace moex::plaza2::cgate {

inline constexpr std::int64_t kPlaza2MaxLogClockSkewNs = 1'000'000'000;
inline constexpr std::uint64_t kPlaza2MaxClockSampleAgeNs = 5'000'000'000;
inline constexpr std::uint64_t kPlaza2MaxClockTransportDelayNs = 1'000'000'000;

struct Plaza2ClockSample {
    std::int64_t local_wall_ns{0};
    std::uint64_t local_monotonic_ns{0};
    std::int64_t exchange_wall_ns{0};
    // A clock-reference sample must identify its source. Snapshot rows are
    // exchange-event evidence, not a current clock reference, unless the
    // acquisition path explicitly marks them as such.
    std::string provenance;
    bool current_reference{false};
    // These are reported separately from wall-clock offset. They must never
    // be silently folded into the offset or used to make an old row current.
    std::uint64_t transport_delay_ns{0};
    std::uint64_t exchange_event_age_ns{0};
};

struct Plaza2ClockEvidence {
    std::string sync_source;
    bool sync_status_ok{false};
    std::optional<std::int64_t> wall_offset_ns;
    // Uncertainty is an explicit bound around wall_offset_ns, not an excuse to
    // accept an unbounded or inconsistent offset series.
    std::optional<std::uint64_t> offset_uncertainty_ns;
    std::string monotonic_clock_id;
    std::vector<Plaza2ClockSample> paired_samples;
    std::optional<std::int64_t> current_local_wall_ns;
    std::optional<std::uint64_t> current_local_monotonic_ns;
    std::optional<std::uint64_t> sync_status_monotonic_ns;
};

namespace detail {

[[nodiscard]] inline std::optional<std::int64_t> checked_difference(std::int64_t lhs, std::int64_t rhs) noexcept {
    if ((rhs > 0 && lhs < std::numeric_limits<std::int64_t>::min() + rhs) ||
        (rhs < 0 && lhs > std::numeric_limits<std::int64_t>::max() + rhs)) {
        return std::nullopt;
    }
    return lhs - rhs;
}

[[nodiscard]] inline std::uint64_t unsigned_magnitude(std::int64_t value) noexcept {
    if (value >= 0) {
        return static_cast<std::uint64_t>(value);
    }
    return static_cast<std::uint64_t>(-(value + 1)) + 1U;
}

[[nodiscard]] inline bool absolute_difference_within(std::int64_t lhs, std::int64_t rhs, std::uint64_t limit) noexcept {
    const auto difference = checked_difference(lhs, rhs);
    if (!difference.has_value()) {
        return false;
    }
    return unsigned_magnitude(*difference) <= limit;
}

[[nodiscard]] inline bool nonnegative_age_within(std::int64_t current, std::int64_t observed,
                                                 std::uint64_t limit) noexcept {
    const auto age = checked_difference(current, observed);
    return age.has_value() && *age >= 0 && static_cast<std::uint64_t>(*age) <= limit;
}

} // namespace detail

[[nodiscard]] inline bool plaza2_clock_evidence_passes(const Plaza2ClockEvidence& evidence) noexcept {
    if (evidence.sync_source.empty() || !evidence.sync_status_ok || !evidence.wall_offset_ns.has_value() ||
        !evidence.offset_uncertainty_ns.has_value() || evidence.monotonic_clock_id.empty() ||
        evidence.paired_samples.size() < 2 || !evidence.current_local_wall_ns.has_value() ||
        !evidence.current_local_monotonic_ns.has_value() || !evidence.sync_status_monotonic_ns.has_value()) {
        return false;
    }
    if (*evidence.current_local_wall_ns == 0 || *evidence.current_local_monotonic_ns == 0 ||
        *evidence.sync_status_monotonic_ns == 0 ||
        *evidence.offset_uncertainty_ns > static_cast<std::uint64_t>(kPlaza2MaxLogClockSkewNs) ||
        !detail::absolute_difference_within(*evidence.wall_offset_ns, 0,
                                            static_cast<std::uint64_t>(kPlaza2MaxLogClockSkewNs)) ||
        *evidence.sync_status_monotonic_ns > *evidence.current_local_monotonic_ns ||
        *evidence.current_local_monotonic_ns - *evidence.sync_status_monotonic_ns > kPlaza2MaxClockSampleAgeNs) {
        return false;
    }

    std::optional<std::uint64_t> previous_monotonic;
    std::optional<std::int64_t> previous_offset;
    for (const auto& sample : evidence.paired_samples) {
        if (sample.local_wall_ns == 0 || sample.exchange_wall_ns == 0 || sample.local_monotonic_ns == 0 ||
            sample.provenance.empty() || !sample.current_reference ||
            sample.transport_delay_ns > kPlaza2MaxClockTransportDelayNs ||
            sample.exchange_event_age_ns > kPlaza2MaxClockSampleAgeNs ||
            sample.local_monotonic_ns > *evidence.current_local_monotonic_ns ||
            *evidence.current_local_monotonic_ns - sample.local_monotonic_ns > kPlaza2MaxClockSampleAgeNs ||
            !detail::nonnegative_age_within(*evidence.current_local_wall_ns, sample.local_wall_ns,
                                            kPlaza2MaxClockSampleAgeNs)) {
            return false;
        }
        if (previous_monotonic.has_value() && sample.local_monotonic_ns <= *previous_monotonic) {
            return false;
        }
        const auto observed_offset = detail::checked_difference(sample.local_wall_ns, sample.exchange_wall_ns);
        if (!observed_offset.has_value() ||
            !detail::absolute_difference_within(*observed_offset, *evidence.wall_offset_ns,
                                                *evidence.offset_uncertainty_ns) ||
            (previous_offset.has_value() && !detail::absolute_difference_within(*observed_offset, *previous_offset,
                                                                                *evidence.offset_uncertainty_ns))) {
            return false;
        }
        previous_monotonic = sample.local_monotonic_ns;
        previous_offset = *observed_offset;
    }
    return true;
}

} // namespace moex::plaza2::cgate
