#pragma once

#include "private_delta_host_regression.hpp"
#include "scope_exit.hpp"

#include <sstream>
#include <iostream>

namespace moex::connector_host::regression {
inline void exchange_message_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                        const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    using enum gen::FieldCode;
    std::ostringstream diagnostics;
    const auto previous = std::cerr.rdbuf(diagnostics.rdbuf());
    ScopeExit restore([&] { std::cerr.rdbuf(previous); });
    auto now = OrderManager::Clock::time_point{} + std::chrono::hours(1);
    config.session.recovery_now = [&] { return now; };
    std::string failures;
    for (const bool missing : {false, true}) {
        try {
            fake::Scenario scenario{.suppress_initial_orders = true, .zero_position = true};
            scenario.client_code = config.orders.broker_code + config.orders.client_code;
            if (missing)
                scenario.omitted_schema_table = gen::TableCode::kFortsRefdataReplSysMessages;
            control.configure(scenario);
            const auto prefix = missing ? "exchange-missing" : "exchange-messages";
            config.journal_path = root / (std::string(prefix) + ".ndjson");
            config.identity_state_path = root / (std::string(prefix) + ".state");
            CgateTradingHost host(config);
            bootstrap(host);
            if (missing) {
                require(diagnostics.str().find("sys_messages table unavailable") != std::string::npos,
                        "negotiated REFDATA without sys_messages had no operator warning");
                require(host.status().find("\"order_entry_ready\":true") != std::string::npos,
                        "missing informational messages blocked trading readiness");
                require(!host.stop(), "missing informational table stop");
                std::ifstream journal(config.journal_path);
                const std::string logged{std::istreambuf_iterator<char>(journal), {}};
                require(logged.find("\"event\":\"exchange_messages_unavailable\"") != std::string::npos &&
                            logged.find("sys_messages table unavailable") != std::string::npos,
                        "missing exchange table warning was not journaled");
                continue;
            }
            require(host.status().find("Certification test service") != std::string::npos &&
                        diagnostics.str().find("exchange_message:") != std::string::npos,
                    "committed exchange message absent from status/stderr");
            const auto message = [&](std::int64_t id, const std::string& value) {
                return fake::Event{.stream_code = gen::StreamCode::kFortsRefdataRepl,
                                   .table_code = gen::TableCode::kFortsRefdataReplSysMessages,
                                   .revision = id,
                                   .fields = {integer(kFortsRefdataReplSysMessagesReplId, id),
                                              integer(kFortsRefdataReplSysMessagesReplRev, id),
                                              integer(kFortsRefdataReplSysMessagesMsgId, id),
                                              text(kFortsRefdataReplSysMessagesLangCode, "EN"),
                                              integer(kFortsRefdataReplSysMessagesTypeId, 7),
                                              integer(kFortsRefdataReplSysMessagesUrgency, 2),
                                              integer(kFortsRefdataReplSysMessagesStatus, 1),
                                              text(kFortsRefdataReplSysMessagesText, value),
                                              text(kFortsRefdataReplSysMessagesMessageBody, "NCC decoded body")}};
            };
            const auto row = message(100, "committed-only exchange text");
            control.enqueue({.kind = fake::EventKind::Begin, .stream_code = row.stream_code});
            control.enqueue(row);
            require(!host.poll(), "uncommitted message poll");
            require(host.status().find("committed-only exchange text") == std::string::npos &&
                        diagnostics.str().find("committed-only exchange text") == std::string::npos,
                    "uncommitted exchange message escaped to the operator");
            control.enqueue({.kind = fake::EventKind::Commit, .stream_code = row.stream_code});
            require(!host.poll(), "committed message poll");
            require(host.status().find("committed-only exchange text") != std::string::npos,
                    "committed exchange update was not surfaced");
            control.enqueue({.kind = fake::EventKind::Begin, .stream_code = row.stream_code});
            control.enqueue(row);
            control.enqueue({.kind = fake::EventKind::Commit, .stream_code = row.stream_code});
            require(!host.poll(), "replayed message poll");
            control.enqueue({.kind = fake::EventKind::Close, .stream_code = row.stream_code});
            require(!host.poll(), "exchange listener close");
            now += std::chrono::seconds(2);
            for (int i = 0; i < 30; ++i)
                require(!host.poll(), "exchange listener replay recovery");
            for (int id = 200; id < 222; ++id) {
                control.enqueue({.kind = fake::EventKind::Begin, .stream_code = row.stream_code});
                control.enqueue(message(id, "bounded-message-" + std::to_string(id)));
                control.enqueue({.kind = fake::EventKind::Commit, .stream_code = row.stream_code});
            }
            require(!host.poll(), "batched message commits poll");
            for (int i = 0; i < 30 && host.status().find("bounded-message-221") == std::string::npos; ++i)
                require(!host.poll(), "bounded message drain");
            const auto status = host.status();
            require(status.find("bounded-message-200") == std::string::npos &&
                        status.find("bounded-message-201") == std::string::npos &&
                        status.find("bounded-message-202") != std::string::npos &&
                        status.find("bounded-message-221") != std::string::npos,
                    "operator exchange-message history was not bounded to the last20: " + status);
            control.enqueue({.kind = fake::EventKind::LifeNum, .stream_code = row.stream_code, .value = 8});
            control.enqueue({.kind = fake::EventKind::Begin, .stream_code = row.stream_code});
            control.enqueue(message(221, "bounded-message-221"));
            control.enqueue({.kind = fake::EventKind::Commit, .stream_code = row.stream_code});
            control.enqueue({.kind = fake::EventKind::Online, .stream_code = row.stream_code});
            require(!host.poll() && host.status().find("\"lifenum\":8") != std::string::npos,
                    "new REF epoch suppressed a reused exchange-message revision");
            require(!host.stop(), "exchange message stop");
            std::ifstream journal(config.journal_path);
            std::string line;
            std::size_t named{}, replayed{};
            while (std::getline(journal, line)) {
                if (line.find("\"event\":\"exchange_message\"") == std::string::npos)
                    continue;
                ++named;
                replayed += line.find("committed-only exchange text") != std::string::npos;
                require(line.find("\"lang_code\":\"EN\"") != std::string::npos &&
                            line.find("\"message_body\":") != std::string::npos,
                        "exchange journal omitted decoded message fields");
            }
            require(named == 25 && replayed == 1, "exchange journal lost commits or duplicated a replayed message");
        } catch (const std::exception& error) {
            failures += std::string(missing ? "missing table: " : "messages: ") + error.what() + "; ";
        }
    }
    require(failures.empty(), failures);
    std::cout << "CRT3 exchange-message regression: PASS\n";
}
} // namespace moex::connector_host::regression
