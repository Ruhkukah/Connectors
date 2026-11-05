#pragma once

#include "moex/plaza2/cgate/plaza2_full_order_log_capture.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace moex::plaza2::cgate {
struct FullOrderLogBackfillBoundary {
    std::int64_t revision{};
    std::int32_t session{};
    std::uint64_t commit_sequence{};
    std::string sha256;
};

struct FullOrderLogBackfillPlan {
    std::uint64_t capture_identity{}, reopen_sequence{}, ordlog_lifenum{};
    std::int32_t matching_id{};
    std::vector<std::int32_t> isins;
    FullOrderLogBackfillBoundary start, left, right;
    std::string listener_settings, open_settings;
};

struct FullOrderLogBackfillCheck {
    FullOrderLogBackfillPlan plan;
    std::uint64_t recovery_capture_identity{}, committed_rows_between{};
};

// Both primary generations must record the requested matching and the same
// committed ORDLOG LifeNum. A supplied LifeNum is an additional assertion.
// A zero reopen sequence selects the first recorded FOL reopen.
FullOrderLogBackfillPlan plan_full_order_log_backfill(const std::filesystem::path& capture, std::int32_t matching_id,
                                                      std::uint64_t ordlog_lifenum = 0,
                                                      std::uint64_t reopen_sequence = 0);

// Recovery must be a separate, clean, single-table p2repl evidence capture for
// the specified matching, using the same selected instrument set. This checks
// captured overlap, not server retention or a fully repaired DBN history.
FullOrderLogBackfillCheck check_full_order_log_backfill(const std::filesystem::path& capture,
                                                        const std::filesystem::path& recovery_capture,
                                                        std::int32_t matching_id, std::uint64_t ordlog_lifenum,
                                                        std::uint64_t reopen_sequence = 0);

// The caller holds the writer's existing identity lock. The collector creates
// one TEST-only read-only connection and a new recovery artifact.
FullOrderLogBackfillCheck
collect_full_order_log_backfill(const FullOrderLogBackfillPlan& plan, FullOrderLogCaptureOptions capture,
                                const Plaza2Settings& runtime, std::string_view connection_settings,
                                std::chrono::seconds timeout, std::chrono::sys_seconds not_after,
                                const std::function<bool()>& stopped);
} // namespace moex::plaza2::cgate
