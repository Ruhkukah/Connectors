#pragma once

#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include <sstream>

namespace moex::plaza2_trade {

struct SessionPriceBinding {
    std::uint64_t transport_generation{0};
    plaza2::private_state::FutureSessionTerms terms;
    bool operator==(const SessionPriceBinding&) const = default;
};

struct SessionPriceGate {
    bool session_terms_present{false};
    bool session_terms_current{false};
    bool session_price_bounds_valid{false};
    bool order_price_within_exchange_bounds{false};
    bool order_price_tick_aligned{false};
    [[nodiscard]] bool allowed() const noexcept {
        return session_terms_present && session_terms_current && session_price_bounds_valid &&
               order_price_within_exchange_bounds && order_price_tick_aligned;
    }
};

// Effective transport/REFDATA health is supplied by the single-owner host, never
// by an operator flag. Provenance and row identity are checked independently.
inline SessionPriceGate inspect_session_price(const std::optional<plaza2::private_state::FutureSessionTerms>& terms,
                                              std::int64_t isin, std::int64_t session, std::uint64_t lifenum,
                                              bool effective_refdata, std::string_view price) noexcept {
    namespace ps = plaza2::private_state;
    SessionPriceGate result;
    result.session_terms_present = terms.has_value();
    if (!terms)
        return result;
    const auto& t = *terms;
    result.session_terms_current =
        effective_refdata && t.isin_id == isin && t.sess_id == session && isin > 0 && session > 0 && t.repl_id > 0 &&
        t.source.present && t.source.lifenum == lifenum &&
        t.source.stream_code == plaza2::generated::StreamCode::kFortsRefdataRepl &&
        t.source.table_code == plaza2::generated::TableCode::kFortsRefdataReplFutSessContents;
    const auto computed = ps::evaluate_future_price_bounds(t);
    result.session_price_bounds_valid = t.bounds.interval_valid && t.bounds == computed;
    const auto value = ps::parse_session_decimal(price);
    result.order_price_within_exchange_bounds = value && result.session_price_bounds_valid &&
                                                value->units >= t.bounds.lower->units &&
                                                value->units <= t.bounds.upper->units;
    result.order_price_tick_aligned =
        value && t.min_step && t.min_step->units > 0 && value->units % t.min_step->units == 0;
    return result;
}

// Integer units are exact scale=100000. The raw fields are retained separately
// from their derived absolute bounds; no market observation substitutes for them.
inline std::string session_price_binding_json(const SessionPriceBinding& binding) {
    const auto& t = binding.terms;
    const auto decimal = [](const auto& value) { return value ? std::to_string(value->units) : "null"; };
    std::ostringstream out;
    out << "{\"scale\":100000,\"generation\":" << binding.transport_generation << ",\"isin_id\":" << t.isin_id
        << ",\"sess_id\":" << t.sess_id << ",\"stream\":" << static_cast<unsigned>(t.source.stream_code)
        << ",\"table\":" << static_cast<unsigned>(t.source.table_code) << ",\"repl_id\":" << t.repl_id
        << ",\"repl_rev\":" << t.source.repl_rev << ",\"lifenum\":" << t.source.lifenum
        << ",\"present\":" << t.source.present << ",\"settlement_price\":" << decimal(t.settlement_price)
        << ",\"raw_limit_up\":" << decimal(t.raw_limit_up) << ",\"raw_limit_down\":" << decimal(t.raw_limit_down)
        << ",\"lower\":" << decimal(t.bounds.lower) << ",\"upper\":" << decimal(t.bounds.upper)
        << ",\"min_step\":" << decimal(t.min_step) << '}';
    return out.str();
}

} // namespace moex::plaza2_trade
