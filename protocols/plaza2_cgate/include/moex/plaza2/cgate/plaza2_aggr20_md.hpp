#pragma once

#include "moex/plaza2/cgate/plaza2_credential_provider.hpp"
#include "moex/plaza2/cgate/plaza2_runtime.hpp"
#include "moex/plaza2/cgate/plaza2_fixed_point.hpp"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace moex::plaza2::cgate {

struct Plaza2Aggr20Level {
    std::int64_t isin_id{0};
    // FORTS_AGGR##_REPL.orders_aggr.price is generated SPECTRA d16.5 and is
    // normalized to signed integer units at scale 1e5.
    std::int64_t price_scaled{0};
    std::int64_t volume{0};
    std::int32_t dir{0};
    std::uint64_t repl_id{0};
    std::int64_t repl_rev{0};
    std::uint64_t moment{0};
    std::uint64_t moment_ns{0};
    std::string price;
    std::string synth_volume;
};

struct Plaza2Aggr20Snapshot {
    std::size_t row_count{0};
    std::size_t instrument_count{0};
    std::size_t bid_depth_levels{0};
    std::size_t ask_depth_levels{0};
    std::uint64_t last_repl_id{0};
    std::int64_t last_repl_rev{0};
    // Cross-instrument values retained for diagnostics only. Trading must use
    // snapshot_for_isin(), never these global best prices.
    std::optional<Plaza2Aggr20Level> top_bid;
    std::optional<Plaza2Aggr20Level> top_ask;
    std::vector<Plaza2Aggr20Level> levels;
    std::chrono::steady_clock::time_point committed_at{};
    std::uint64_t exchange_moment{0};
    std::uint64_t exchange_moment_ns{0};
};

struct Plaza2Aggr20InstrumentSnapshot {
    std::int64_t isin_id{0};
    std::size_t row_count{0};
    std::size_t bid_depth_levels{0};
    std::size_t ask_depth_levels{0};
    std::uint64_t last_repl_id{0};
    std::int64_t last_repl_rev{0};
    std::optional<Plaza2Aggr20Level> top_bid;
    std::optional<Plaza2Aggr20Level> top_ask;
    // Positional depth in deterministic price order. Bids are descending and
    // asks are ascending; the vector is target-scoped and never represents a
    // replication-vector or repl_id order.
    std::vector<Plaza2Aggr20Level> levels;
    // Monotonic target-source version. It advances only when this target's
    // normalized state or source identity changes; unrelated ISIN commits do
    // not advance it.
    std::uint64_t source_snapshot_version{0};
    // This is captured from the local monotonic clock at commit time.
    std::chrono::steady_clock::time_point committed_at{};
    std::uint64_t exchange_moment{0};
    std::uint64_t exchange_moment_ns{0};
};

class Plaza2Aggr20QualificationObserver {
  public:
    virtual ~Plaza2Aggr20QualificationObserver() = default;
    virtual void committed(const Plaza2Aggr20Snapshot&) noexcept = 0;
};

class Plaza2Aggr20BookProjector {
  public:
    using Clock = std::chrono::steady_clock;
    using NowFn = std::function<Clock::time_point()>;

    explicit Plaza2Aggr20BookProjector(NowFn now = {});

    void set_qualification_observer(Plaza2Aggr20QualificationObserver* observer) noexcept {
        qualification_observer_ = observer;
    }
    void reset();
    void begin_transaction();
    [[nodiscard]] Plaza2Error on_row(std::span<const Plaza2DecodedFieldValue> fields);
    [[nodiscard]] Plaza2Error commit();
    void rollback();
    void clear_deleted(std::int64_t revision);

    // Global levels and diagnostics are reconstructed on demand; scoped
    // snapshots remain incrementally maintained at commit time.
    [[nodiscard]] const Plaza2Aggr20Snapshot& snapshot() const;
    [[nodiscard]] std::optional<Plaza2Aggr20InstrumentSnapshot> snapshot_for_isin(std::int64_t isin_id) const;
    [[nodiscard]] bool transaction_open() const noexcept;

  private:
    struct InstrumentBook {
        // Price and replID uniquely identify each visible level.
        std::map<std::pair<std::int64_t, std::uint64_t>, std::uint64_t> bids, asks;
        std::uint64_t version{}, last_repl_id{}, moment{}, moment_ns{};
        std::int64_t last_repl_rev{};
        Clock::time_point committed_at{};
    };
    struct Operation {
        bool clear{false};
        std::int64_t revision{};
        Plaza2Aggr20Level row;
    };
    void erase_row(std::uint64_t repl_id, std::unordered_set<std::int64_t>& affected);
    [[nodiscard]] Plaza2Aggr20InstrumentSnapshot make_snapshot(std::int64_t isin_id, const InstrumentBook& book) const;
    Plaza2Aggr20QualificationObserver* qualification_observer_{nullptr};
    std::vector<Operation> staged_;
    std::unordered_map<std::uint64_t, Plaza2Aggr20Level> rows_;
    std::unordered_map<std::int64_t, InstrumentBook> books_;
    mutable Plaza2Aggr20Snapshot committed_;
    std::uint64_t version_{};
    NowFn now_;
    mutable bool dirty_{true};
    bool transaction_open_{false};
};

struct Plaza2Aggr20SysEventSnapshot {
    std::uint64_t source_repl_id{0};
    std::int64_t source_repl_rev{0};
    std::int64_t source_repl_act{0};
    std::int32_t event_type{0};
    std::int64_t event_id{0};
    std::int32_t sess_id{0};
    std::string message;
    std::uint64_t server_time{0};
    bool seen_during_snapshot{false};
};

struct Plaza2Aggr20Status {
    bool transport_active{false};
    bool snapshot_complete{false};
    bool session_data_ready{false};
    bool aggr_online{false};
    bool valid{false};
    std::uint64_t stream_epoch{0};
    std::optional<Plaza2Aggr20SysEventSnapshot> ready_event;
    std::optional<Plaza2Aggr20SysEventSnapshot> last_sys_event;
};

// Projection owned by the shared CGate session.
class Plaza2Aggr20ListenerBridge final : public Plaza2ListenerEventHandler {
  public:
    explicit Plaza2Aggr20ListenerBridge(Plaza2Aggr20BookProjector& projector, std::int32_t expected_session_id = 0)
        : projector_(projector), expected_session_id_(expected_session_id) {}
    void reset() noexcept;
    [[nodiscard]] bool online() const noexcept {
        return online_;
    }
    [[nodiscard]] bool snapshot_complete() const noexcept {
        return snapshot_complete_;
    }
    [[nodiscard]] bool transport_active() const noexcept {
        return transport_active_;
    }
    [[nodiscard]] bool session_data_ready() const noexcept {
        return session_data_ready_;
    }
    [[nodiscard]] bool valid() const noexcept {
        return valid_ && !projector_.transaction_open();
    }
    [[nodiscard]] Plaza2Aggr20Status status() const;
    void set_session_id(std::int32_t session_id) noexcept;
    [[nodiscard]] bool has_lifenum() const noexcept {
        return has_lifenum_;
    }
    [[nodiscard]] std::uint64_t last_lifenum() const noexcept {
        return last_lifenum_;
    }
    [[nodiscard]] bool recovering() const noexcept {
        return reopen_required_;
    }
    [[nodiscard]] Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent&) override;
    void on_plaza2_listener_error(const Plaza2Error&) noexcept override {
        invalidate(true, false);
    }

  private:
    void invalidate(bool request_reopen, bool transport_active) noexcept;
    [[nodiscard]] bool accepts_session(std::int32_t sess_id) const noexcept;
    Plaza2Aggr20BookProjector& projector_;
    std::int32_t expected_session_id_{0};
    bool transport_active_{false};
    bool online_{false};
    bool snapshot_complete_{false};
    bool session_data_ready_{false};
    bool valid_{false};
    bool reopen_required_{false};
    bool has_lifenum_{false};
    std::uint64_t last_lifenum_{0};
    std::uint64_t stream_epoch_{0};
    std::optional<Plaza2Aggr20SysEventSnapshot> last_sys_event_;
    std::optional<Plaza2Aggr20SysEventSnapshot> ready_event_;
    bool clearing_started_{false};
    void refresh_ready() noexcept;
    // sys_events is part of the source transaction. Every row is retained in
    // source order and applied after TN_COMMIT.
    std::vector<Plaza2Aggr20SysEventSnapshot> pending_sys_events_;
};

} // namespace moex::plaza2::cgate
