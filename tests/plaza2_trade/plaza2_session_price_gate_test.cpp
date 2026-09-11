#include "moex/plaza2_trade/plaza2_session_price_gate.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace moex::plaza2_trade;
namespace ps = moex::plaza2::private_state;
namespace gen = moex::plaza2::generated;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
ps::FutureSessionTerms terms(const char* settlement, const char* spread) {
    ps::FutureSessionTerms t;
    t.isin_id = 4519447;
    t.sess_id = 11704;
    t.repl_id = 123;
    t.settlement_price = ps::parse_session_decimal(settlement);
    t.raw_limit_up = t.raw_limit_down = ps::parse_session_decimal(spread);
    t.min_step = ps::parse_session_decimal("1");
    t.source = {gen::StreamCode::kFortsRefdataRepl, gen::TableCode::kFortsRefdataReplFutSessContents, 9, 7, true};
    t.bounds = ps::evaluate_future_price_bounds(t);
    return t;
}
int main() {
    try {
        const auto gate = [](const auto& t, std::string_view price) {
            return inspect_session_price(t, 4519447, 11704, 7, true, price);
        };
        for (const auto& t : {terms("2077", "184"), terms("2045", "180")}) {
            const bool historical = t.settlement_price->units == 207700000;
            const auto lower = historical ? "1893" : "1865";
            const auto upper = historical ? "2261" : "2225";
            require(gate(t, lower).allowed() && gate(t, upper).allowed(), "inclusive exact boundaries");
            require(!gate(t, historical ? "1892" : "1864").allowed(), "below boundary");
            require(!gate(t, historical ? "2262" : "2226").allowed(), "above boundary");
            require(gate(t, "2042").allowed(), "inside aligned");
            const auto misaligned = gate(t, "2042.5");
            require(misaligned.order_price_within_exchange_bounds && !misaligned.order_price_tick_aligned,
                    "bounds and tick facts independent");
            require(!gate(t, "").allowed(), "absent order intent is not send authority");
            require(!gate(t, "1e3").allowed() && !gate(t, "9223372036854775807").allowed(),
                    "exact parse rejects overflow/exponent");
            require(!inspect_session_price(t, 1, 11704, 7, true, "2042").allowed(), "wrong instrument");
            require(!inspect_session_price(t, 4519447, 1, 7, true, "2042").allowed(), "wrong session");
            require(!inspect_session_price(t, 4519447, 11704, 8, true, "2042").allowed(), "stale LifeNum");
            require(!inspect_session_price(t, 4519447, 11704, 7, false, "2042").allowed(),
                    "effective health mandatory");
            auto changed = t;
            changed.source.present = false;
            require(!gate(changed, "2042").allowed(), "invalidated provenance");
            changed = t;
            changed.settlement_price.reset();
            require(!gate(changed, "2042").allowed(), "missing field and stale cached bounds");
            changed = t;
            changed.min_step = ps::SessionDecimal{0};
            require(!gate(changed, "2042").allowed(), "zero step");
            changed = t;
            changed.raw_limit_up = ps::SessionDecimal{std::numeric_limits<std::int64_t>::max()};
            changed.bounds = ps::evaluate_future_price_bounds(changed);
            require(!gate(changed, "2042").allowed(), "checked addition overflow");
            changed = t;
            changed.raw_limit_down = ps::SessionDecimal{std::numeric_limits<std::int64_t>::min()};
            changed.bounds = ps::evaluate_future_price_bounds(changed);
            require(!gate(changed, "2042").allowed(), "checked subtraction overflow");
            changed = t;
            changed.raw_limit_down = ps::SessionDecimal{-999999999};
            changed.bounds = ps::evaluate_future_price_bounds(changed);
            require(!gate(changed, "2042").allowed(), "inverted interval is not repaired with abs");
            const SessionPriceBinding original{1, t};
            auto rebound = original;
            ++rebound.terms.source.repl_rev;
            require(original != rebound && session_price_binding_json(original) != session_price_binding_json(rebound),
                    "revision is authorization identity even with unchanged prices");
            rebound = original;
            ++rebound.transport_generation;
            require(original != rebound && session_price_binding_json(original) != session_price_binding_json(rebound),
                    "transport generation is authorization identity");
        }
        require(!gate(std::optional<ps::FutureSessionTerms>{}, "2042").allowed(), "deleted or missing row");
        std::cout << "session price gate regressions passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
