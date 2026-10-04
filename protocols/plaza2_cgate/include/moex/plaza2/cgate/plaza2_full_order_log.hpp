#pragma once

#include "moex/plaza2/cgate/plaza2_runtime.hpp"
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>

namespace moex::plaza2::cgate {

struct FullOrderLogLevelChange {
    std::int32_t isin_id{};
    bool bid{};
    std::int64_t price{}, quantity{}; // signed d16.5 price units, contracts
    std::uint64_t exchange_moment_ns{};
};

struct FullOrderLogMetrics {
    std::uint64_t rows_total{}, rows_filtered{}, excluded_adds{}, excluded_ids{}, excluded_evictions{};
    std::uint64_t ignored_executions{}, excluded_executions{}, retired_rows_ignored{};
    std::uint64_t life_events{}, crossed_transitions{}, lag_samples{}, lag_last_ns{}, lag_max_ns{}, lag_sum_ns{};
    std::uint64_t exchange_ahead_of_clock{};
};
struct FullOrderLogLifeState {
    std::uint64_t last_lifenum{}, info_trades_lifenum{};
    bool info_available{}; // Native composite LifeNum controls have no source/table identity.
};

// Single-owner composite consumer. Orders and level nodes use a bounded,
// reusable arena; published levels change only at an online TN_COMMIT.
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
    void reset();
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool crossed(std::int32_t isin) const noexcept;
    [[nodiscard]] std::uint64_t exchange_moment_ns(std::int32_t isin) const noexcept;
    [[nodiscard]] FullOrderLogMetrics metrics() const noexcept;
    [[nodiscard]] FullOrderLogLifeState life_state() const noexcept;
    [[nodiscard]] std::uint64_t epoch() const noexcept;
    [[nodiscard]] std::uint64_t revision(std::int32_t isin) const noexcept;
    [[nodiscard]] Clock::time_point committed_at() const noexcept;
    [[nodiscard]] std::span<const std::int32_t> instruments() const noexcept;
    [[nodiscard]] const LevelMap& levels(std::int32_t isin, bool bid) const;
    [[nodiscard]] std::span<const FullOrderLogLevelChange> changes() const noexcept;
    [[nodiscard]] std::size_t memory_bytes() const noexcept;
    [[nodiscard]] std::size_t order_count() const noexcept;
    std::function<void(const Plaza2FullOrderLog&)> on_commit;
    std::function<void()> on_invalidate;
    std::function<void(std::int32_t, bool)> on_crossed;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace moex::plaza2::cgate
