#pragma once

#include "moex/connector_host/trading_host.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"

namespace moex::connector_host::regression {
inline void immediate_dispatch_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                               const std::filesystem::path& root) {
    const auto check = plaza2::test::require;
    const auto configure = [&] {
        control.configure({.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"});
    };
    configure();
    config.session.process_timeout_ms = 50;
    config.orders.sole_instance = true;
    auto now = OrderManager::Clock::time_point(std::chrono::seconds(100));
    config.session.recovery_now = [&] { return now; };
    config.orders.max_commands_per_second = 30;
    config.journal_path = root / "immediate-dispatch.ndjson";
    config.identity_state_path = root / "immediate-dispatch.state";
    {
        CgateTradingHost host(config);
        check(!host.start(), "immediate dispatch host start");
        for (int i = 0; i < 30; ++i)
            check(!host.poll(), "immediate dispatch bootstrap");
        const auto before = control.commands().size();
        const auto processes = control.process_count();
        control.set(plaza2::test::fake::Option::PubReplyOrderId, "66001");
        check(host.place({.client_order_id = "immediate", .isin_id = 1001, .price = "103000", .quantity = 2}).empty(),
              "immediate Add refused");
        check(control.commands().size() == before + 1 && control.process_count() == processes,
              "accepted Add waited for a CGate poll before reaching the publisher");
        check(!host.poll(), "immediate Add reply");
        const auto move_processes = control.process_count();
        check(host.move("immediate", "103250", 2).empty(), "immediate Move refused");
        check(control.commands().size() == before + 2 && control.process_count() == move_processes,
              "accepted Move waited for a CGate poll before reaching the publisher");
        check(!host.poll(), "immediate Move reply");
        const auto cancel_processes = control.process_count();
        check(host.cancel("immediate").empty(), "immediate cancel refused");
        check(control.commands().size() == before + 3 && control.process_count() == cancel_processes,
              "accepted cancel waited for a CGate poll before reaching the publisher");
        check(host.cancel_all(1001).empty(), "immediate mass cancel refused");
        check(control.commands().size() == before + 4 && control.process_count() == cancel_processes,
              "accepted mass cancel waited for a CGate poll before reaching the publisher");
        check(!host.stop(), "immediate dispatch host stop");
    }
    configure();
    config.orders.max_commands_per_second = 1;
    config.journal_path = root / "rate-held-dispatch.ndjson";
    config.identity_state_path = root / "rate-held-dispatch.state";
    {
        CgateTradingHost host(config);
        check(!host.start(), "rate-held dispatch host start");
        for (int i = 0; i < 30; ++i)
            check(!host.poll(), "rate-held dispatch bootstrap");
        check(control.last_process_timeout() == 50, "idle fixture did not use its configured native wait");
        const auto before = control.commands().size();
        control.set(plaza2::test::fake::Option::PubReplyOrderId, "66011");
        check(host.place({.client_order_id = "rate-first", .isin_id = 1001, .price = "103000", .quantity = 2}).empty(),
              "rate-first Add refused");
        control.set(plaza2::test::fake::Option::PubReplyOrderId, "66012");
        check(host.place({.client_order_id = "rate-held", .isin_id = 1001, .price = "103000", .quantity = 2}).empty(),
              "rate-held Add refused");
        check(control.commands().size() == before + 1, "immediate dispatch bypassed the one-command rate limit");
        check(!host.poll(), "rate-held queue poll");
        check(control.commands().size() == before + 1 && control.last_process_timeout() == 0 &&
                  host.status().find("\"queued\":1") != std::string::npos,
              "a queued command waited in a blocking CGate process call or escaped its rate limit");
        now += std::chrono::seconds(1);
        // The command must reach the publisher before the next CGate process
        // observes a disconnect. Its definitive identity is still pending;
        // there must be no blind Add resubmission after that observation.
        control.enqueue({.kind = plaza2::test::fake::EventKind::ConnectionError});
        check(!host.poll(), "pre-process rate-window dispatch poll");
        check(control.commands().size() == before + 2,
              "rate-released Add was dispatched only after processing the connection loss");
        check(!host.poll() && control.commands().size() == before + 2,
              "connection recovery blindly resubmitted the already posted Add");
        check(!host.stop(), "rate-held dispatch host stop");
    }
    configure();
}
} // namespace moex::connector_host::regression
