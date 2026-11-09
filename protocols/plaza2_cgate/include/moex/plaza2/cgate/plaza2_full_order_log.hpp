#pragma once

#include "moex/plaza2/cgate/plaza2_runtime.hpp"
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>

namespace moex::plaza2::cgate {

// Fixed client schemas restrict each leg independently; CGate p2ordbook does
// not support the p2repl tables= selector. Keep forts_scheme.ini beside this
// subdirectory for the shared runtime compatibility probe.
inline std::string full_order_log_listener_settings(const std::filesystem::path& scheme_dir,
                                                    std::optional<std::int8_t> matching_id = {}) {
    const auto directory = scheme_dir.string();
    if (!scheme_dir.is_absolute() || directory.find_first_of(";|\r\n") != std::string::npos ||
        directory.find('\0') != std::string::npos)
        throw std::invalid_argument("FullOrderLog requires an absolute scheme directory without CGate delimiters");
    if (matching_id && *matching_id <= 0)
        throw std::invalid_argument("FullOrderLog requires a positive matching ID");
    const auto suffix = matching_id ? "_MATCH" + std::to_string(*matching_id) : std::string{};
    return "p2ordbook://FORTS_ORDLOG_REPL" + suffix + ";snapshot=FORTS_ORDBOOK_REPL" + suffix +
           ";online.scheme=|FILE|" + (scheme_dir.lexically_normal() / "full_order_log/online.ini").string() +
           "|ORDLOG;snapshot.scheme=|FILE|" + (scheme_dir.lexically_normal() / "full_order_log/snapshot.ini").string() +
           "|OrdBook";
}

struct FullOrderLogMetrics {
    std::uint64_t rows_total{}, rows_filtered{}, excluded_adds{}, excluded_ids{}, excluded_evictions{};
    std::uint64_t ignored_executions{}, excluded_executions{}, retired_rows_ignored{};
    std::uint64_t life_events{}, crossed_transitions{}, lag_samples{}, lag_last_ns{}, lag_max_ns{}, lag_sum_ns{};
    std::uint64_t exchange_ahead_of_clock{}, clear_deleted_events{}, clear_deleted_erased{};
};
struct FullOrderLogOrder {
    std::int64_t id{}, price{}, remaining{};
    std::int32_t isin{}, session{};
    bool bid{};
};
// Single-owner composite consumer. Orders and level nodes use a bounded,
// reusable arena; unframed snapshots publish at complete ONLINE, then rows
// require transactions and published levels change only at TN_COMMIT.
class Plaza2FullOrderLog final : public Plaza2ListenerEventHandler {
  public:
    using LevelMap = std::pmr::map<std::int64_t, std::int64_t>;
    using Clock = std::chrono::steady_clock;
    explicit Plaza2FullOrderLog(std::span<const std::int32_t> isins, std::size_t order_capacity = 200000,
                                std::size_t level_capacity = 20000);
    ~Plaza2FullOrderLog();
    Plaza2FullOrderLog(const Plaza2FullOrderLog&) = delete;
    Plaza2FullOrderLog& operator=(const Plaza2FullOrderLog&) = delete;
    [[nodiscard]] bool wants_raw_replication(std::string_view) const noexcept override {
        return true;
    }
    [[nodiscard]] bool should_log_listener_event(const Plaza2ListenerEvent&) const noexcept override;
    [[nodiscard]] Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent&) override;
    void on_plaza2_listener_error(const Plaza2Error&) noexcept override;
    void reset(bool require_snapshot = false);
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool needs_fresh_snapshot() const noexcept override;
    [[nodiscard]] bool crossed(std::int32_t isin) const noexcept;
    [[nodiscard]] std::uint64_t exchange_moment_ns(std::int32_t isin) const noexcept;
    // Zero for an invalid/empty book or mixed/unknown sessions among retained orders.
    [[nodiscard]] std::int32_t committed_session_id(std::int32_t isin) const noexcept;
    [[nodiscard]] FullOrderLogMetrics metrics() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> committed_log_lifenum() const noexcept;
    [[nodiscard]] std::uint64_t epoch() const noexcept;
    [[nodiscard]] std::uint64_t revision(std::int32_t isin) const noexcept;
    [[nodiscard]] Clock::time_point committed_at() const noexcept;
    [[nodiscard]] std::span<const std::int32_t> instruments() const noexcept;
    [[nodiscard]] const LevelMap& levels(std::int32_t isin, bool bid) const;
    [[nodiscard]] std::size_t memory_bytes() const noexcept;
    [[nodiscard]] std::size_t order_count() const noexcept;
    // Diagnostic, owner-thread only. Refuse inspection during an uncommitted frame.
    // No copy or iteration occurs on the ordinary publication path.
    bool inspect_committed_orders(const std::function<void(const FullOrderLogOrder&)>& visitor) const;
    // Offline exporter only: point inspection includes staged state and must never authorize trading.
    [[nodiscard]] std::optional<FullOrderLogOrder> inspect_staged_order_for_replay(std::int64_t id) const;
    void inspect_staged_orders_for_replay(const std::function<void(const FullOrderLogOrder&)>& visitor) const;
    std::function<void(const Plaza2FullOrderLog&)> on_commit;
    std::function<void()> on_invalidate;
    std::function<void(std::int32_t, bool)> on_crossed;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace moex::plaza2::cgate
