#pragma once
#include "private_delta_host_regression.hpp"

namespace moex::connector_host::regression {
inline void closed_session_cancel_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                                  const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    control.configure({.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"});
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
    for (int i = 0; i < 30; ++i)
        require(!host.poll(), "closed-empty drain");
    require(control.commands().size() == before && !host.has_pending_cancellations(),
            "verified empty scope posted a closed-session mass cancel or left a drain pending");
    require(!host.stop(), "closed-empty stop");

    // Signal during a lost link must not turn cached absence into either a
    // redundant closed-session request or a successful unproved drain.
    for (const bool reconnect : {false, true}) {
        control.configure({.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"});
        control.set(fake::Option::PubReplyCode, "3");
        config.journal_path = root / (reconnect ? "shutdown-reconnect.ndjson" : "shutdown-bootstrap.ndjson");
        config.identity_state_path = root / (reconnect ? "shutdown-reconnect.state" : "shutdown-bootstrap.state");
        CgateTradingHost recovering(config);
        if (reconnect) {
            bootstrap(recovering);
            control.set(fake::Option::ConnectionClosed, "1");
            require(!recovering.poll(), "shutdown loss poll");
            control.clear(fake::Option::ConnectionClosed);
        } else
            require(!recovering.start(), "shutdown bootstrap start");
        control.set(fake::Option::DelayUserorderbook, "1");
        const auto posts = control.commands().size();
        recovering.set_kill_switch(true);
        recovering.request_shutdown_cancellation(1001);
        require(recovering.shutdown_reconciliation_pending(), "unknown snapshot falsely completed shutdown");
        for (int i = 0; i < 30; ++i)
            require(!recovering.poll(), "shutdown incomplete reconstruction poll");
        require(recovering.shutdown_reconciliation_pending() && control.commands().size() == posts &&
                    !recovering.has_pending_cancellations(),
                "incomplete reconstruction posted redundant cancellation or passed shutdown");
        control.clear(fake::Option::DelayUserorderbook);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (recovering.shutdown_reconciliation_pending() && std::chrono::steady_clock::now() < deadline) {
            require(!recovering.poll(), "shutdown recovery poll");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(!recovering.shutdown_reconciliation_pending() && !recovering.has_pending_cancellations() &&
                    control.commands().size() == posts,
                "fresh empty reconstruction failed to drain without closed-session posts");
        // A second loss after a proved scope invalidates the shutdown proof.
        control.set(fake::Option::ConnectionClosed, "1");
        require(!recovering.poll() && recovering.shutdown_reconciliation_pending(),
                "second loss retained stale shutdown proof");
        control.clear(fake::Option::ConnectionClosed);
        require(!recovering.stop(), "shutdown recovery stop");
    }

    // A cached working order must be cancelled even while reconstruction is
    // unavailable. Closed-session rejection remains pending, never an ack.
    control.configure({.zero_position = true, .client_code = "BRK1C01"});
    control.set(fake::Option::PubReplyCode, "3");
    config.journal_path = root / "shutdown-working.ndjson";
    config.identity_state_path = root / "shutdown-working.state";
    CgateTradingHost working(config);
    bootstrap(working);
    require(working.has_working_orders(), "shutdown working fixture lacks exposure");
    control.set(fake::Option::ConnectionClosed, "1");
    require(!working.poll(), "shutdown working loss");
    working.set_kill_switch(true);
    working.request_shutdown_cancellation(1001);
    require(working.has_pending_cancellations() && working.shutdown_reconciliation_pending(),
            "shutdown deferred known exposure or accepted stale reconstruction");
    control.clear(fake::Option::ConnectionClosed);
    const auto recovery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (working.shutdown_reconciliation_pending() && std::chrono::steady_clock::now() < recovery_deadline) {
        require(!working.poll(), "shutdown working recovery");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (int i = 0; i < 30; ++i)
        require(!working.poll(), "shutdown working rejection poll");
    require(!control.commands().empty() && working.has_pending_cancellations(),
            "closed-session shutdown cancellation was discarded or treated as acknowledged");
    require(!working.stop(), "shutdown working stop");

    // No snapshot is not proof of absence. Retain cancellation/retry safety during bootstrap.
    control.configure({.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"});
    control.set(fake::Option::PubReplyCode, "3");
    config.journal_path = root / "closed-unproved.ndjson";
    config.identity_state_path = root / "closed-unproved.state";
    CgateTradingHost unknown(config);
    require(!unknown.start(), "closed-unproved start");
    require(unknown.cancel_all(1001).empty() && unknown.has_pending_cancellations(),
            "unreconstructed scope incorrectly skipped cancellation");
    for (int i = 0; i < 30; ++i)
        require(!unknown.poll(), "closed-unproved poll");
    require(!control.commands().empty() && unknown.has_pending_cancellations(),
            "closed-session rejection discarded an unproved cancellation");
    require(!unknown.stop(), "closed-unproved stop");
    control.configure({.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"});
}
} // namespace moex::connector_host::regression
