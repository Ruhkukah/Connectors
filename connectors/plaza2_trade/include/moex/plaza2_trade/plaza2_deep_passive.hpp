#pragma once
#include "moex/plaza2_trade/plaza2_session_price_gate.hpp"
#include "moex/plaza2/cgate/plaza2_runtime.hpp"

namespace moex::plaza2_trade {
inline constexpr std::string_view kFirstOrderDeepPassiveVersion = "FIRST_ORDER_DEEP_PASSIVE_V1";
inline constexpr std::int64_t kFirstOrderPassiveGapTicks = 20;
inline constexpr std::int64_t kFirstOrderBoundaryInsetTicks = 2;
inline constexpr std::int64_t kFirstOrderBboMaxAgeMs = 1000;
inline constexpr std::string_view kFirstOrderDeepPassiveDefinition =
    "FIRST_ORDER_DEEP_PASSIVE_V1;quantity=1;boundary_inset_ticks=2;min_gap_ticks=20;bbo_max_age_ms=1000;"
    "buy=ceil(lower/tick)*tick+2*tick;sell=floor(upper/tick)*tick-2*tick;fresh_uncrossed_bbo;no_auto_send";
inline std::string first_order_deep_passive_sha256() {
    return plaza2::cgate::plaza2_sha256_hex(kFirstOrderDeepPassiveDefinition);
}
struct DeepPassiveBboBinding {
    std::int64_t bid_units{}, ask_units{}; // exact scale=100000, not AGGR's internal scale
    std::uint64_t repl_id{};
    std::int64_t repl_rev{}, committed_monotonic_ns{};
    std::uint64_t exchange_moment{}, exchange_moment_ns{};
};
struct DeepPassiveCandidate {
    std::int64_t price_units{}, gap_ticks{};
    bool eligible{false};
};
struct DeepPassiveProposal {
    DeepPassiveCandidate buy, sell;
    // 0 means DO_NOT_RUN, 1 buy, 2 sell. Proposal only, never send authority.
    int preferred_side{0};
};
inline DeepPassiveProposal propose_deep_passive(const plaza2::private_state::FutureSessionTerms& terms,
                                                std::int64_t bid, std::int64_t ask, std::int64_t age_ms) {
    DeepPassiveProposal result;
    if (!terms.bounds.interval_valid || terms.bounds != plaza2::private_state::evaluate_future_price_bounds(terms) ||
        !terms.min_step || terms.min_step->units <= 0 || bid <= 0 || ask <= bid || age_ms < 0 ||
        age_ms > kFirstOrderBboMaxAgeMs)
        return result;
    const auto step = terms.min_step->units;
    if (bid % step || ask % step)
        return result;
    const auto lower = terms.bounds.lower->units, upper = terms.bounds.upper->units;
    auto tick_lower = lower, tick_upper = upper;
    const auto lo_rem = lower % step, hi_rem = upper % step;
    if (__builtin_sub_overflow(lower, lo_rem, &tick_lower) ||
        (lo_rem > 0 && __builtin_add_overflow(tick_lower, step, &tick_lower)) ||
        __builtin_sub_overflow(upper, hi_rem, &tick_upper) ||
        (hi_rem < 0 && __builtin_sub_overflow(tick_upper, step, &tick_upper)))
        return result;
    std::int64_t inset{}, buy{}, sell{}, buy_gap{}, sell_gap{};
    if (__builtin_mul_overflow(step, kFirstOrderBoundaryInsetTicks, &inset))
        return result;
    if (!__builtin_add_overflow(tick_lower, inset, &buy) && buy > 0 && buy >= lower && buy <= upper &&
        !__builtin_sub_overflow(bid, buy, &buy_gap))
        result.buy = {buy, buy_gap / step, buy_gap / step >= kFirstOrderPassiveGapTicks};
    if (!__builtin_sub_overflow(tick_upper, inset, &sell) && sell > 0 && sell >= lower && sell <= upper &&
        !__builtin_sub_overflow(sell, ask, &sell_gap))
        result.sell = {sell, sell_gap / step, sell_gap / step >= kFirstOrderPassiveGapTicks};
    if (result.buy.eligible)
        result.preferred_side = 1;
    if (result.sell.eligible && (!result.buy.eligible || result.sell.gap_ticks > result.buy.gap_ticks))
        result.preferred_side = 2;
    return result;
}
inline bool deep_passive_price_allowed(const plaza2::private_state::FutureSessionTerms& terms, std::int64_t price,
                                       int side, std::int64_t bid, std::int64_t ask, std::int64_t age_ms) {
    const auto proposal = propose_deep_passive(terms, bid, ask, age_ms);
    if (side != 1 && side != 2)
        return false;
    const auto& candidate = side == 1 ? proposal.buy : proposal.sell;
    return candidate.eligible && candidate.price_units == price;
}
inline std::string deep_passive_binding_json(const DeepPassiveBboBinding& bbo) {
    std::ostringstream out;
    out << "{\"policy_version\":\"" << kFirstOrderDeepPassiveVersion << "\",\"policy_sha256\":\""
        << first_order_deep_passive_sha256() << "\",\"min_gap_ticks\":" << kFirstOrderPassiveGapTicks
        << ",\"boundary_inset_ticks\":" << kFirstOrderBoundaryInsetTicks
        << ",\"bbo_max_age_ms\":" << kFirstOrderBboMaxAgeMs
        << ",\"source\":\"FORTS_AGGR20_REPL\",\"scale\":100000,\"bid_units\":" << bbo.bid_units
        << ",\"ask_units\":" << bbo.ask_units << ",\"repl_id\":" << bbo.repl_id << ",\"repl_rev\":" << bbo.repl_rev
        << ",\"committed_monotonic_ns\":" << bbo.committed_monotonic_ns
        << ",\"exchange_moment\":" << bbo.exchange_moment << ",\"exchange_moment_ns\":" << bbo.exchange_moment_ns
        << '}';
    return out.str();
}
} // namespace moex::plaza2_trade
