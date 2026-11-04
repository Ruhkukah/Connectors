#pragma once
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"

namespace moex::plaza2::cgate {
struct FullOrderLogCaptureOptions {
    std::filesystem::path directory;
    std::uint64_t max_bytes{4ULL << 30}, min_free_bytes{10ULL << 30};
    std::size_t block_bytes{1 << 20}, queue_blocks{32};
    std::chrono::milliseconds flush_interval{200}, sync_interval{1000};
    std::uint64_t segment_bytes{32ULL << 20};
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
    void poll();
    // Call only after the host has stopped. A missing end record is an incomplete capture.
    void finish();
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
struct FullOrderLogArchiveSummary {
    std::vector<std::int32_t> isins;
    std::string source_sha;
    std::uint64_t records{}, rows{}, commits{}, valid_commits{}, last_valid_order_count{}, stored_bytes{},
        open_interest_rows{}, open_interest_updates{}, listener_errors{};
};
// Offline only. Refuses missing/duplicate/reordered segments, damaged frames and unclean EOF.
// Visitor borrows all spans for the duration of the call only; schema lifetime ends at next OPEN.
FullOrderLogArchiveSummary
read_full_order_log_capture(const std::filesystem::path& directory,
                            const std::function<void(const FullOrderLogCaptureRecord&)>& visitor);
FullOrderLogArchiveSummary replay_full_order_log_capture(const std::filesystem::path& directory);
// Derived DBN v3 MBO book changes plus public OI Statistics (A/M/C/R), never fabricated trade prints.
// Creates a new file exclusively, publishes it only after complete archive validation.
FullOrderLogArchiveSummary export_full_order_log_dbn(const std::filesystem::path& directory,
                                                     const std::filesystem::path& output);
} // namespace moex::plaza2::cgate
