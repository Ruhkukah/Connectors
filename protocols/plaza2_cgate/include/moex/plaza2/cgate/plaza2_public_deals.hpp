#pragma once

#include "moex/plaza2/cgate/plaza2_fixed_point.hpp"
#include "moex/plaza2/cgate/plaza2_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace moex::plaza2::cgate {

struct Plaza2PublicDeal {
    std::uint64_t sequence{0};
    std::uint64_t stream_epoch{0};
    std::uint64_t lifenum{0};
    std::uint64_t repl_id{0};
    std::int64_t repl_rev{0};
    std::int64_t deal_id{0};
    std::int64_t isin_id{0};
    std::int32_t session_id{0};
    std::int64_t price_scaled{0};
    std::int64_t quantity{0};
    std::string price;
    std::uint64_t moment{0};
    std::uint64_t moment_ns{0};
    std::uint64_t received_at_unix_ns{0};
    std::uint64_t xstatus_buy{0};
    std::uint64_t xstatus_sell{0};
    std::int32_t at_bid_or_ask{0};
};

struct Plaza2PublicDealsSnapshot {
    bool online{false};
    bool valid{false};
    std::uint64_t stream_epoch{0};
    std::uint64_t lifenum{0};
    std::uint64_t last_sequence{0};
    std::uint64_t first_sequence{0};
    std::string error;
    std::vector<Plaza2PublicDeal> trades;
};

class Plaza2PublicDealsBridge final : public Plaza2ListenerEventHandler {
  public:
    explicit Plaza2PublicDealsBridge(std::int64_t isin_id, std::int32_t sess_id, std::size_t max_trades = 8192);

    [[nodiscard]] Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override;
    [[nodiscard]] Plaza2Error on_message(const Plaza2ListenerEvent& event);
    void on_plaza2_listener_error(const Plaza2Error& error) noexcept override;

    void reset() noexcept;
    [[nodiscard]] bool online() const noexcept {
        return online_;
    }
    [[nodiscard]] bool valid() const noexcept {
        return valid_;
    }
    [[nodiscard]] const std::string& error() const noexcept {
        return error_;
    }
    [[nodiscard]] Plaza2PublicDealsSnapshot snapshot(std::uint64_t after_sequence = 0) const;

  private:
    struct Identity {
        std::uint64_t repl_id{0};
        std::int64_t repl_rev{0};
        std::int64_t repl_act{0};
        std::int64_t deal_id{0};
        std::int64_t isin_id{0};
        std::int32_t session_id{0};
        std::int64_t price_scaled{0};
        std::int64_t quantity{0};
        std::uint64_t moment{0};
        std::uint64_t moment_ns{0};
        std::uint64_t xstatus_buy{0};
        std::uint64_t xstatus_sell{0};
    };
    struct PendingDeal {
        Identity identity;
        Plaza2PublicDeal deal;
        bool emit_candidate{false};
    };
    struct SeenEntry {
        std::uint64_t repl_id{0};
        std::int64_t deal_id{0};
    };

    [[nodiscard]] Plaza2Error fail(Plaza2ErrorCode code, const char* message);
    [[nodiscard]] Plaza2Error begin_transaction();
    [[nodiscard]] Plaza2Error commit_transaction();
    [[nodiscard]] Plaza2Error stage_deal(const Plaza2ListenerEvent& event);
    [[nodiscard]] bool push_committed(Plaza2PublicDeal&& deal) noexcept;
    void remember(const Identity& identity);
    [[nodiscard]] static bool same_identity(const Identity& a, const Identity& b) noexcept;
    [[nodiscard]] static bool same_economic_trade(const Identity& a, const Identity& b) noexcept;
    void clear_generation() noexcept;

    const std::int64_t target_isin_id_;
    const std::int32_t target_session_id_;
    const std::size_t capacity_;
    std::vector<Plaza2PublicDeal> ring_;
    std::size_t ring_begin_{0};
    std::size_t ring_size_{0};
    std::vector<SeenEntry> dedup_history_;
    std::size_t dedup_begin_{0};
    std::size_t dedup_size_{0};
    std::unordered_map<std::uint64_t, Identity> seen_by_repl_id_;
    std::unordered_map<std::int64_t, std::vector<std::uint64_t>> seen_by_deal_id_;
    std::vector<PendingDeal> pending_;
    std::unordered_map<std::uint64_t, std::size_t> pending_by_repl_id_;
    std::unordered_map<std::int64_t, Identity> pending_by_deal_id_;
    std::uint64_t stream_epoch_{0};
    std::uint64_t lifenum_{0};
    std::uint64_t last_sequence_{0};
    std::int64_t last_repl_rev_{-1};
    bool has_lifenum_{false};
    bool opened_{false};
    bool transaction_open_{false};
    bool online_{false};
    bool valid_{false};
    std::string error_;
};

} // namespace moex::plaza2::cgate
