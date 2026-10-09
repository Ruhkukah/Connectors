#pragma once

#include "private_delta_host_regression.hpp"

#include <iostream>

namespace moex::connector_host::regression {
inline void heartbeat_journal_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                         const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    constexpr std::uint32_t replay_rows = 10000;
    constexpr std::uint64_t base_time = 1700000000;
    auto now = OrderManager::Clock::time_point{} + std::chrono::hours(1);
    config.session.recovery_now = [&] { return now; };
    config.session.process_timeout_ms = 0;
    config.journal_path = root / "heartbeat-startup.ndjson";
    config.identity_state_path = root / "heartbeat-startup.state";
    control.configure({.suppress_initial_orders = true,
                       .zero_position = true,
                       .heartbeat_replay_rows = replay_rows,
                       .client_code = config.orders.broker_code + config.orders.client_code,
                       .session_id = 321});
    CgateTradingHost host(config);
    bootstrap(host);
    require(!host.stop(), "heartbeat-heavy startup stop");
    std::ifstream journal(config.journal_path);
    std::string line;
    std::size_t bytes{}, raw_rows{}, samples{}, sampled_bytes{};
    bool online_transition{}, final_time{};
    while (std::getline(journal, line)) {
        bytes += line.size() + 1;
        raw_rows += line.find("\"event\":\"stream_row\"") != std::string::npos &&
                    line.find("\"table\":\"heartbeat\"") != std::string::npos;
        if (line.find("\"event\":\"trade_server_time\"") != std::string::npos) {
            ++samples;
            sampled_bytes += line.size() + 1;
            online_transition |= line.find("\"reason\":\"listener_online\"") != std::string::npos;
            final_time |= line.find("\"server_time\":" + std::to_string(base_time + replay_rows - 1 - 10800)) !=
                          std::string::npos;
        }
    }
    std::cout << "SPD1 heartbeat startup: decoded_rows=" << replay_rows << " raw_journal_rows=" << raw_rows
              << " time_samples=" << samples << " sample_bytes=" << sampled_bytes << " total_bytes=" << bytes << '\n';
    require(raw_rows == 0, "TRADE heartbeat rows still enter the interaction journal");
    require(samples == 2 && online_transition && final_time,
            "heartbeat journal lost compact startup or ONLINE server-time evidence");

    config.journal_path = root / "heartbeat-minute.ndjson";
    config.identity_state_path = root / "heartbeat-minute.state";
    control.configure({.suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = config.orders.broker_code + config.orders.client_code,
                       .session_id = 321});
    CgateTradingHost sampled(config);
    bootstrap(sampled);
    const auto row = [&](std::int64_t revision, std::uint64_t timestamp) {
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
        control.enqueue({.stream_code = gen::StreamCode::kFortsTradeRepl,
                         .table_code = gen::TableCode::kFortsTradeReplHeartbeat,
                         .revision = revision,
                         .fields = {integer(gen::FieldCode::kFortsTradeReplHeartbeatReplId, revision),
                                    integer(gen::FieldCode::kFortsTradeReplHeartbeatReplRev, revision),
                                    {.field_code = gen::FieldCode::kFortsTradeReplHeartbeatServerTime,
                                     .kind = fake::FieldKind::Timestamp,
                                     .unsigned_value = timestamp}}});
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
        for (int i = 0; i < 5; ++i)
            require(!sampled.poll(), "heartbeat sampling transaction");
    };
    row(100000, base_time);
    now += std::chrono::seconds(59);
    row(100001, base_time + 3600); // Historical server-time advance is not a receipt-time minute.
    now += std::chrono::seconds(1);
    row(100002, base_time + 3601);
    row(100003, base_time + 3602);
    control.enqueue({.kind = fake::EventKind::LifeNum, .stream_code = gen::StreamCode::kFortsTradeRepl, .value = 8});
    require(!sampled.poll(), "heartbeat LifeNum transition");
    row(100004, base_time + 3603);
    control.enqueue({.kind = fake::EventKind::Close, .stream_code = gen::StreamCode::kFortsTradeRepl});
    require(!sampled.poll(), "heartbeat CLOSE transition");
    require(!sampled.stop(), "heartbeat sampling stop");
    std::ifstream sampled_journal(config.journal_path);
    std::size_t minute_samples{}, lifenum_samples{}, close_samples{};
    bool unsampled_row{}, transition_latest{}, close_latest{};
    while (std::getline(sampled_journal, line)) {
        raw_rows += line.find("\"event\":\"stream_row\"") != std::string::npos &&
                    line.find("\"table\":\"heartbeat\"") != std::string::npos;
        if (line.find("\"event\":\"trade_server_time\"") == std::string::npos)
            continue;
        minute_samples += line.find("\"reason\":\"minute\"") != std::string::npos;
        lifenum_samples += line.find("\"reason\":\"lifenum\"") != std::string::npos;
        close_samples += line.find("\"reason\":\"listener_close\"") != std::string::npos;
        unsampled_row |= line.find("\"repl_rev\":100001") != std::string::npos;
        transition_latest |=
            line.find("\"reason\":\"lifenum\"") != std::string::npos &&
            line.find("\"repl_rev\":100003") != std::string::npos &&
            line.find("\"server_time\":" + std::to_string(base_time + 3602 - 10800)) != std::string::npos;
        close_latest |= line.find("\"reason\":\"listener_close\"") != std::string::npos &&
                        line.find("\"repl_rev\":100004") != std::string::npos;
    }
    require(raw_rows == 0 && minute_samples == 3 && lifenum_samples == 1 && close_samples == 1 && !unsampled_row &&
                transition_latest && close_latest,
            "heartbeat sampling did not enforce one receipt-time minute plus the latest time at an epoch transition");
    require(control.commands().empty(), "heartbeat journal test posted an exchange command");
}
} // namespace moex::connector_host::regression
