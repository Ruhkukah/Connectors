#include "moex/plaza2_trade/plaza2_deep_passive.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace moex::plaza2_trade;
namespace ps = moex::plaza2::private_state;
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
ps::FutureSessionTerms terms(const char* settlement, const char* offset, const char* tick = "1") {
    ps::FutureSessionTerms t;
    t.settlement_price = ps::parse_session_decimal(settlement);
    t.raw_limit_up = t.raw_limit_down = ps::parse_session_decimal(offset);
    t.min_step = ps::parse_session_decimal(tick);
    t.bounds = ps::evaluate_future_price_bounds(t);
    return t;
}
int main() {
    try {
        const auto t = terms("2045", "180");
        auto p = propose_deep_passive(t, 204200000, 204500000, 1000);
        require(p.buy.price_units == 186700000 && p.sell.price_units == 222300000, "exact September 11 candidates");
        require(p.buy.gap_ticks == 175 && p.sell.gap_ticks == 178 && p.preferred_side == 2,
                "prefer greater valid cushion");
        const auto old = propose_deep_passive(terms("2077", "184"), 204200000, 204500000, 0);
        require(old.buy.price_units == 189500000 && old.sell.price_units == 225900000,
                "historical regression candidates");
        const auto rounded = propose_deep_passive(terms("2045.5", "180", "2"), 204200000, 204600000, 0);
        require(rounded.buy.price_units == 187000000 && rounded.sell.price_units == 222000000,
                "round bounds inward then inset");
        require(propose_deep_passive(t, 204200000, 204500000, 1001).preferred_side == 0, "stale BBO");
        require(propose_deep_passive(t, 204200000, 204500000, -1).preferred_side == 0, "future clock invalid");
        require(propose_deep_passive(t, 204500000, 204500000, 0).preferred_side == 0, "locked BBO");
        require(propose_deep_passive(t, 204600000, 204500000, 0).preferred_side == 0, "crossed BBO");
        require(deep_passive_price_allowed(t, 186700000, 1, 188700000, 188800000, 0),
                "exact 20-tick cushion inclusive");
        require(!deep_passive_price_allowed(t, 186700000, 1, 188600000, 188700000, 0), "19 ticks rejected");
        require(!deep_passive_price_allowed(t, 186800000, 1, 204200000, 204500000, 0), "no silent alternative price");
        require(!deep_passive_price_allowed(t, 222300000, 2, 221000000, 221100000, 0),
                "BBO movement invalidates sell cushion");
        require(propose_deep_passive(terms("100", "10"), 9900000, 10100000, 0).preferred_side == 0,
                "neither side DO_NOT_RUN");
        auto invalid = t;
        invalid.min_step.reset();
        require(propose_deep_passive(invalid, 204200000, 204500000, 0).preferred_side == 0, "missing step");
        invalid = t;
        invalid.raw_limit_up = ps::SessionDecimal{std::numeric_limits<std::int64_t>::max()};
        invalid.bounds = ps::evaluate_future_price_bounds(invalid);
        require(propose_deep_passive(invalid, 204200000, 204500000, 0).preferred_side == 0, "overflow fails closed");
        require(first_order_deep_passive_sha256().size() == 64, "named exact policy hash");
        std::cout << "deep-passive exact calculation and revalidation passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
