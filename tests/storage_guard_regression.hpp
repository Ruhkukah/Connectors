#pragma once

#include "private_delta_host_regression.hpp"

#include <thread>

namespace moex::connector_host::regression {
inline void storage_capacity_guard(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                   const std::filesystem::path& root, bool periodic) {
    using namespace private_delta_host_detail;
    control.configure({.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"});
    auto now = OrderManager::Clock::time_point(std::chrono::seconds(100));
    config.session.recovery_now = [&] { return now; };
    const auto directory = root / (periodic ? "periodic-space" : "startup-space");
    config.journal_path = directory / "events.ndjson";
    config.identity_state_path = directory / "identity.state";
    CgateTradingHost host(config);
    if (periodic) {
        bootstrap(host);
        std::this_thread::sleep_for(std::chrono::milliseconds(270));
        require(!host.poll(), "flush before capacity check fixture");
    }
    // Open descriptors remain valid, but the configured storage filesystem can
    // no longer be inspected. This must fail closed even without another write.
    std::filesystem::rename(directory, directory.string() + ".moved");
    if (!periodic) {
        const auto processes = control.process_count();
        const auto error = host.start();
        require(error.code == cg::Plaza2ErrorCode::InvalidConfiguration && control.process_count() == processes,
                "startup failed to reject unavailable storage before native processing");
    } else {
        now += std::chrono::seconds(61);
        require(!host.poll() && host.status().find("\"cancel_only\":true") != std::string::npos &&
                    host.status().find("\"order_entry_ready\":false") != std::string::npos,
                "minute storage check did not protect entry while retaining the native owner");
    }
    require(host.stop().code == cg::Plaza2ErrorCode::RuntimeCallFailed, "storage capacity failure was concealed");
}

inline void storage_durable_cancel_guard(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                         const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    using enum gen::FieldCode;
    control.configure({.suppress_auto_replies = true,
                       .suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = "BRK1C01"});
    auto now = OrderManager::Clock::time_point(std::chrono::seconds(100));
    config.session.recovery_now = [&] { return now; };
    config.session.reply_timeout_ms = 10;
    config.orders.reply_timeout = std::chrono::milliseconds(10);
    config.orders.next_user_id = 1000; // Last ID in the journal's first durable block.
    config.journal_path = root / "durable-cancel.ndjson";
    config.identity_state_path = root / "durable-cancel.state";
    CgateTradingHost host(config);
    bootstrap(host);
    const auto account = config.orders.broker_code + config.orders.client_code;
    auto row = own_order(72001, config.isin_ids.front(), account);
    const auto transaction = [&](const auto& event) {
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = event.stream_code});
        control.enqueue(event);
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = event.stream_code});
        require(!host.poll(), "storage guard native transaction");
    };
    transaction(row);
    std::filesystem::remove(config.identity_state_path);
    std::filesystem::create_directory(config.identity_state_path);
    const auto writer_boundary = config.identity_state_path.string() + ".journal";
    std::filesystem::remove(writer_boundary);
    std::filesystem::create_directory(writer_boundary);
    std::this_thread::sleep_for(std::chrono::milliseconds(270));
    require(!host.poll() && host.status().find("\"cancel_only\":true") != std::string::npos,
            "durable cancellation fixture did not enter storage protection");
    bool kill_refused{};
    try {
        host.set_kill_switch(false);
    } catch (const std::invalid_argument&) {
        kill_refused = true;
    }
    require(kill_refused, "storage protection permitted kill off");
    const auto bytes = std::filesystem::file_size(config.journal_path);
    for (int i = 0; i < 5000; ++i)
        host.record_operator_input(std::string(256, 'x'));
    require(std::filesystem::file_size(config.journal_path) == bytes,
            "storage-failed callbacks kept buffering/writing ordinary interaction events");
    const auto posted = control.commands().size();
    require(host.cancel("recovered:321:72001").empty(), "durable cancellation was refused");
    auto commands = control.commands();
    require(commands.size() == posted + 1 && commands.back().name == "DelOrder" && commands.back().user_id == 1000,
            "last durably reserved cancellation ID did not reach the publisher");
    now += std::chrono::milliseconds(20);
    require(!host.poll(), "durable cancellation timeout poll");
    now += std::chrono::seconds(2);
    require(!host.poll() && control.commands().size() == posted + 1 && host.has_pending_cancellations(),
            "storage protection posted an unreserved retry ID beyond the durable ceiling");
    std::filesystem::remove(config.identity_state_path);
    std::filesystem::remove(writer_boundary);
    recover_storage(host);
    now += std::chrono::seconds(2);
    require(!host.poll() && control.commands().size() == posted + 2 && control.commands().back().user_id == 1001,
            "storage recovery failed to durably reserve and dispatch the queued cancellation retry");
    auto fill = own_trade(72002, config.isin_ids.front(), account, 72001);
    transaction(fill);
    for (auto& field : row.fields) {
        if (field.field_code == kFortsTradeReplOrdersLogPublicAction ||
            field.field_code == kFortsTradeReplOrdersLogPrivateAction)
            field.signed_value = 2;
        if (field.field_code == kFortsTradeReplOrdersLogPublicAmountRest ||
            field.field_code == kFortsTradeReplOrdersLogPrivateAmountRest)
            field.signed_value = 0;
    }
    row.revision = 72003;
    transaction(row);
    transaction(fill); // A replay during storage failure remains deduplicated.
    transaction(fake::Event{
        .stream_code = gen::StreamCode::kFortsPosRepl,
        .table_code = gen::TableCode::kFortsPosReplPosition,
        .revision = 72004,
        .fields = {integer(kFortsPosReplPositionReplId, 72004), text(kFortsPosReplPositionClientCode, account),
                   integer(kFortsPosReplPositionIsinId, config.isin_ids.front()),
                   integer(kFortsPosReplPositionAccountType, 2), integer(kFortsPosReplPositionXpos, 1)}});
    now += std::chrono::seconds(2);
    require(!host.poll(), "remove terminal cancellation after its retry delay");
    const auto status = host.status();
    require(status.find("\"state\":\"Filled\"") != std::string::npos &&
                status.find("\"executed\":1") != std::string::npos && status.find("\"xpos\":1") != std::string::npos &&
                !host.has_pending_cancellations() && control.commands().size() == posted + 2,
            "storage protection stopped native terminal/fill reconciliation or retried an Add: " + status +
                "; pending=" + std::to_string(host.has_pending_cancellations()) +
                "; posts=" + std::to_string(control.commands().size() - posted));
    require(!host.stop(), "recovered durable cancel guard stop failed");
}
} // namespace moex::connector_host::regression
