#pragma once
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_refdata.hpp"

namespace moex::plaza2::cgate {
enum class FullOrderLogCaptureEndStatus : std::uint64_t { Clean = 0, HostNonzero = 1, CaptureFailed = 2 };
struct FullOrderLogRecoveryCursor {
    std::uint64_t original_identity{}, reopen_sequence{}, life_number{};
    std::int64_t revision{};
    std::int32_t matching_id{};
    bool operator==(const FullOrderLogRecoveryCursor&) const = default;
};
struct FullOrderLogCaptureOptions {
    std::filesystem::path directory;
    std::uint64_t max_bytes{4ULL << 30}, min_free_bytes{10ULL << 30};
    std::size_t block_bytes{1 << 20}, queue_blocks{32};
    std::chrono::milliseconds flush_interval{200}, sync_interval{1000};
    std::uint64_t segment_bytes{32ULL << 20};
    std::optional<FullOrderLogRecoveryCursor> recovery_cursor;
};
struct FullOrderLogCaptureMetrics {
    std::uint64_t records{}, filtered{}, logical_bytes{}, stored_bytes{}, syncs{}, sync_max_ns{}, queue_peak{};
    bool failed{};
};
// Owner-thread wrapper: copies complete selected raw rows before book admission.
// Only the worker compresses/writes/syncs; poll() also flushes quiet partial blocks.
class FullOrderLogCapture final : public Plaza2ListenerEventHandler {
  public:
    FullOrderLogCapture(FullOrderLogCaptureOptions options, std::span<const std::int32_t> isins,
                        Plaza2ListenerEventHandler& downstream);
    ~FullOrderLogCapture() noexcept;
    bool wants_raw_replication(std::string_view table) const noexcept override;
    bool should_log_listener_event(const Plaza2ListenerEvent& event) const noexcept override;
    bool needs_fresh_snapshot() const noexcept override;
    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override;
    void on_plaza2_listener_error(const Plaza2Error& error) noexcept override;
    // Same owner thread, additional restricted public DEALS recorder tap.
    Plaza2Error record(const Plaza2ListenerEvent& event);
    void record_error(generated::StreamCode stream, const Plaza2Error& error) noexcept;
    void record_refdata(const FullOrderLogRefdataValue& value);
    void poll();
    // Call only after the host has stopped. A missing end record is an incomplete capture.
    void finish(FullOrderLogCaptureEndStatus status = FullOrderLogCaptureEndStatus::Clean, int host_status = 0);
    FullOrderLogCaptureMetrics metrics() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct FullOrderLogCaptureRecord {
    std::uint64_t sequence{}, source_sequence{}, utc_ns{}, monotonic_ns{};
    Plaza2ListenerEvent event;
    bool listener_error{};
};
struct FullOrderLogArchiveStreamCoverage {
    std::uint64_t opens{}, onlines{}, closes{}, listener_errors{};
    std::uint64_t life_notifications{}, clear_deleted{};
};
struct FullOrderLogArchiveSummary {
    std::vector<std::int32_t> isins;
    std::string source_sha;
    std::string refdata_sha256; // Empty for captures without a verified REFDATA sidecar.
    std::string dbn_sha256;     // Set when the export has a pinned REFDATA companion.
    std::uint64_t capture_identity{};
    std::uint64_t records{}, rows{}, commits{}, valid_commits{}, last_valid_order_count{}, stored_bytes{},
        open_interest_rows{}, open_interest_updates{}, listener_errors{}, last_good_sequence{};
    std::uint64_t trade_prints{};             // DBN export only.
    std::uint64_t refdata_unjoined_records{}; // DBN records before or between current REFDATA observations.
    FullOrderLogArchiveStreamCoverage fol_coverage, deals_coverage;
    std::optional<FullOrderLogRecoveryCursor> recovery_cursor;
    bool truncated{}, end_record_present{};
    FullOrderLogCaptureEndStatus end_status{FullOrderLogCaptureEndStatus::Clean};
    int host_status{};
};
// Offline only. Refuses missing/duplicate/reordered segments, damaged frames and unclean EOF.
// Visitor borrows all spans for the duration of the call only; schema lifetime ends at next OPEN.
FullOrderLogArchiveSummary
read_full_order_log_capture(const std::filesystem::path& directory,
                            const std::function<void(const FullOrderLogCaptureRecord&)>& visitor,
                            bool allow_incomplete = false);
FullOrderLogArchiveSummary replay_full_order_log_capture(const std::filesystem::path& directory,
                                                         bool allow_incomplete = false);
// Derived DBN v3 MBO book changes and committed public DEALS prints, plus OI Statistics.
// Creates a new file exclusively, publishes it only after complete archive validation.
FullOrderLogArchiveSummary export_full_order_log_dbn(const std::filesystem::path& directory,
                                                     const std::filesystem::path& output,
                                                     bool allow_incomplete = false);
} // namespace moex::plaza2::cgate
