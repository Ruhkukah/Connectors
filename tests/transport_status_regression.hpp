#pragma once

#include "private_delta_host_regression.hpp"
#include "scope_exit.hpp"

#include <iostream>
#include <sstream>

namespace moex::connector_host::regression {
inline void transport_status_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                        const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    std::ostringstream diagnostics;
    const auto previous = std::cerr.rdbuf(diagnostics.rdbuf());
    ScopeExit restore([&] { std::cerr.rdbuf(previous); });
    auto now = OrderManager::Clock::time_point{} + std::chrono::hours(1);
    config.session.recovery_now = [&] { return now; };
    std::string failures;
    for (const bool incompatible : {false, true}) {
        try {
            fake::Scenario scenario{.suppress_initial_orders = true, .zero_position = true};
            scenario.client_code = config.orders.broker_code + config.orders.client_code;
            if (incompatible) {
                scenario.mutated_schema_field = gen::FieldCode::kFortsUserorderbookReplOrdersClientCode;
                scenario.omit_schema_field = true;
            }
            control.configure(scenario);
            const auto prefix = incompatible ? "transport-incompatible" : "transport-status";
            config.journal_path = root / (std::string(prefix) + ".ndjson");
            config.identity_state_path = root / (std::string(prefix) + ".state");
            CgateTradingHost host(config);
            if (incompatible) {
                require(!host.start(), "incompatible descriptor setup");
                for (int i = 0; i < 30; ++i)
                    require(!host.poll(), "incompatible descriptor observation");
                const auto status = host.status();
                require(status.find("\"incompatible_scheme\":true") != std::string::npos &&
                            status.find("orders.client_code") != std::string::npos &&
                            status.find("\"order_entry_ready\":false") != std::string::npos,
                        "status omitted the pinned incompatible-scheme diagnostic");
                now += std::chrono::seconds(2);
                require(!host.poll() && host.status().find("orders.client_code") != std::string::npos,
                        "incompatible-scheme diagnostic disappeared on a later poll");
                require(diagnostics.str().find("listener_recovery:") != std::string::npos &&
                            diagnostics.str().find("orders.client_code") != std::string::npos,
                        "incompatible scheme was invisible on stderr");
            } else {
                bootstrap(host);
                const auto status = host.status();
                require(status.find("\"connection\":{\"state\":3,\"name\":\"ACTIVE\"}") != std::string::npos &&
                            status.find("\"publisher\":{\"state\":3,\"name\":\"ACTIVE\"}") != std::string::npos &&
                            status.find("\"operation\":\"Running\"") != std::string::npos &&
                            status.find("\"stream\":\"FORTS_TRADE_REPL\"") != std::string::npos &&
                            status.find("\"snapshot_complete\":true") != std::string::npos,
                        "status omitted connection/listener/publisher/recovery state");
                require(status.find("\"allow_orders\":true") != std::string::npos &&
                            status.find("\"rate\":" + std::to_string(config.orders.max_commands_per_second)) !=
                                std::string::npos &&
                            status.find("\"max_quantity\":" + std::to_string(config.orders.risk.max_quantity)) !=
                                std::string::npos &&
                            status.find("\"max_quote_notional_scaled\":" +
                                        std::to_string(config.orders.risk.max_notional_by_isin.at(1001))) !=
                                std::string::npos &&
                            status.find("\"max_position\":" + std::to_string(config.orders.risk.max_position_by_isin.at(
                                                                  1001))) != std::string::npos,
                        "status omitted effective configured order/rate/risk limits");
                require(diagnostics.str().find("cgate_state:") != std::string::npos &&
                            diagnostics.str().find("\"stream\":\"FORTS_TRADE_REPL\"") != std::string::npos &&
                            diagnostics.str().find("recovery:") != std::string::npos,
                        "transport state transitions were invisible on stderr");
                const auto unchanged = diagnostics.str();
                for (int i = 0; i < 10; ++i)
                    require(!host.poll(), "unchanged transport observation");
                require(diagnostics.str() == unchanged, "unchanged transport states flooded stderr");
                control.set(fake::Option::PublisherClosed);
                require(!host.poll() &&
                            host.status().find("\"publisher\":{\"state\":0,\"name\":\"CLOSED\"}") != std::string::npos,
                        "publisher-only closure was invisible in status");
                control.clear(fake::Option::PublisherClosed);
                require(!host.poll(), "publisher recovery observation");
                control.enqueue({.kind = fake::EventKind::ConnectionError});
                require(!host.poll() &&
                            host.status().find("\"wait_state\":\"WaitingForRouter\"") != std::string::npos &&
                            host.status().find("\"operation\":\"Recovering\"") != std::string::npos,
                        "router loss omitted current recovery wait state");
                now += std::chrono::seconds(2);
                for (int i = 0; i < 30; ++i)
                    require(!host.poll(), "router recovery observation");
                require(host.status().find("\"operation\":\"Running\"") != std::string::npos,
                        "restored link retained stale recovery operation");
            }
            require(!host.stop(), "transport diagnostic stop");
        } catch (const std::exception& error) {
            failures += std::string(incompatible ? "scheme: " : "transport: ") + error.what() + "; ";
        }
    }
    require(failures.empty(), failures);
    std::cout << "CRT8 transport-status regression: PASS\n";
}
} // namespace moex::connector_host::regression
