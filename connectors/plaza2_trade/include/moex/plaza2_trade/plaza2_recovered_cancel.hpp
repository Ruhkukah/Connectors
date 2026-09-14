#pragma once
#include "moex/plaza2_trade/plaza2_order_lifecycle.hpp"

namespace moex::plaza2_trade {

enum class RecoveredOrderOutcome {
    ExactlyOneWorkingMatch,
    NoMatch,
    MultipleMatches,
    IdentityConflict,
    TerminalAlready,
    Ambiguous,
    PrivateStreamNotCurrent,
    GenerationNotFresh
};
[[nodiscard]] std::string_view recovered_order_outcome_name(RecoveredOrderOutcome value) noexcept;

struct RecoveredOrderKey {
    std::string epoch;
    std::string account;
    std::int32_t isin_id{0}, sess_id{0}, ext_id{0};
    Plaza2TradeSide side{Plaza2TradeSide::Buy};
    std::string price;
    std::int64_t quantity{0};
    std::int64_t known_exchange_id{0};
};

struct RecoveredOrderReconciliation {
    RecoveredOrderOutcome outcome{RecoveredOrderOutcome::Ambiguous};
    std::uint64_t generation{0};
    RecoveredOrderKey key;
    std::optional<OrderObservation> observation;
    std::int64_t exchange_order_id{0};
    std::string evidence_json;
    std::string evidence_sha256;
};

// Explicit cold-path inspection. TRADE is authoritative for this lifecycle;
// USERORDERBOOK remains a separately fresh census, never merged into TRADE rows.
[[nodiscard]] RecoveredOrderReconciliation
reconcile_recovered_order(const RecoveredOrderKey& key, std::uint64_t generation, bool fresh,
                          std::span<const plaza2::private_state::OwnOrderSnapshot> orders,
                          std::span<const plaza2::private_state::OwnTradeSnapshot> trades);

struct RecoveredCancelPlan {
    RecoveredOrderReconciliation reconciliation;
    std::filesystem::path artifact;
    std::string canonical_json;
    std::string sha256;
    std::string error;
    [[nodiscard]] bool eligible() const noexcept {
        return error.empty() && reconciliation.outcome == RecoveredOrderOutcome::ExactlyOneWorkingMatch;
    }
};

// Private operator artifacts: exclusive create, 0600, no symlink following,
// file+directory fsync. Consumption leaves an exclusive durable marker.
[[nodiscard]] bool write_recovered_artifact(const std::filesystem::path& path, std::string_view bytes,
                                            std::string& error);
[[nodiscard]] bool consume_recovered_artifact(const std::filesystem::path& path, std::string_view bytes,
                                              std::string& error);

} // namespace moex::plaza2_trade
