#pragma once
#include "private_delta_host_regression.hpp"

namespace moex::connector_host::regression {
inline void closed_session_cancel_host_regression(TradingHostConfig config,
    const plaza2::test::fake::Control& control, const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    control.configure({.suppress_initial_orders = true, .zero_position = true,
                       .client_code = "BRK1C01"});
    control.set(fake::Option::PubReplyCode, "3"); // Session closed: a redundant mass cancel would never drain.
    config.orders.sole_instance = true;
    config.journal_path = root / "closed-empty.ndjson";
    config.identity_state_path = root / "closed-empty.state";
    CgateTradingHost host(config);
    bootstrap(host);
    const auto before = control.commands().size();
    require(!host.has_working_orders(), "closed-empty fixture has working orders");
    host.set_kill_switch(true);
    require(host.cancel_all(1001).empty(), "closed-empty cancel refused");
    for (int i = 0; i < 30; ++i) require(!host.poll(), "closed-empty drain");
    require(control.commands().size() == before && !host.has_pending_cancellations(),
            "verified empty scope posted a closed-session mass cancel or left a drain pending");
    require(!host.stop(), "closed-empty stop");

    // No snapshot is not proof of absence. Retain cancellation/retry safety during bootstrap.
    control.configure({.suppress_initial_orders = true, .zero_position = true,
                       .client_code = "BRK1C01"});
    control.set(fake::Option::PubReplyCode, "3");
    config.journal_path = root / "closed-unproved.ndjson";
    config.identity_state_path = root / "closed-unproved.state";
    CgateTradingHost unknown(config);
    require(!unknown.start(), "closed-unproved start");
    require(unknown.cancel_all(1001).empty() && unknown.has_pending_cancellations(),
            "unreconstructed scope incorrectly skipped cancellation");
    for (int i = 0; i < 30; ++i) require(!unknown.poll(), "closed-unproved poll");
    require(!control.commands().empty() && unknown.has_pending_cancellations(),
            "closed-session rejection discarded an unproved cancellation");
    require(!unknown.stop(), "closed-unproved stop");
    control.configure({.suppress_initial_orders = true, .zero_position = true,
                       .client_code = "BRK1C01"});
}
}
