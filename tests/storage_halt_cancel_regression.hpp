#pragma once

#include "late_move_host_regression.hpp"

namespace moex::connector_host::regression {
inline void storage_halt_cancel_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                           const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    using enum gen::FieldCode;
    const auto isin = config.isin_ids.front();
    const auto account = config.orders.broker_code + config.orders.client_code;
    const auto directory = root / "storage-halt-cancel";
    config.journal_path = directory / "events.ndjson";
    config.identity_state_path = directory / "identity.state";
    config.orders.max_cancel_attempts = 1;
    auto now = OrderManager::Clock::time_point(std::chrono::hours(1));
    config.session.recovery_now = [&] { return now; };
    config.session.process_timeout_ms = 0;
    control.configure({.suppress_auto_replies = true,
                       .suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = account,
                       .session_id = 321});
    CgateTradingHost host(config);
    bootstrap(host);
    require(
        host.place({.client_order_id = "storage-clearing", .isin_id = isin, .price = "103000", .quantity = 2}).empty(),
        "storage/clearing seed Add refused");
    official_cgate99::FORTS_MSG179 accepted{};
    accepted.order_id = 82001;
    late_move_host_detail::reply(control, control.commands().back().user_id, 179, accepted);
    late_move_host_detail::order(control, accepted.order_id, 82001, 2, 2, isin, account);
    require(!host.poll(), "storage/clearing seed native reply/row");
    const auto session_state = [&](std::int64_t revision, std::int64_t state) {
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsSessionstateRepl});
        control.enqueue({.stream_code = gen::StreamCode::kFortsSessionstateRepl,
                         .table_code = gen::TableCode::kFortsSessionstateReplSessionState,
                         .revision = revision,
                         .fields = {integer(kFortsSessionstateReplSessionStateReplId, 9),
                                    integer(kFortsSessionstateReplSessionStateReplRev, revision),
                                    integer(kFortsSessionstateReplSessionStateSessId, 321),
                                    integer(kFortsSessionstateReplSessionStatePublicState, state)}});
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsSessionstateRepl});
        require(!host.poll(), "storage/clearing session-state transaction");
    };
    session_state(83001, 0);
    const auto posts = control.commands().size();
    require(host.cancel("storage-clearing").empty() && control.commands().size() == posts + 1,
            "halted session blocked the first known-ID cancel");
    const auto first_cancel = control.commands().back();
    official_cgate99::FORTS_MSG177 halted{};
    halted.code = 3;
    late_move_host_detail::reply(control, first_cancel.user_id, 177, halted);
    require(!host.poll(), "clearing cancellation rejection");
    now += std::chrono::seconds(2);
    require(!host.poll() && control.commands().size() == posts + 1 && host.has_pending_cancellations(),
            "transient clearing rejection was retried before session recovery");
    // Trigger the real periodic filesystem-capacity guard. Existing descriptors
    // and the durable startup ID block remain available for risk reduction.
    std::filesystem::rename(directory, directory.string() + ".moved");
    now += std::chrono::seconds(61);
    require(!host.poll() && host.status().find("\"cancel_only\":true") != std::string::npos,
            "filesystem failure did not activate storage cancel-only mode");
    session_state(83002, 1);
    const auto commands = control.commands();
    require(commands.size() == posts + 2 && commands.back().name == "DelOrder" &&
                commands.back().user_id != first_cancel.user_id &&
                host.status().find("\"operator_action_required\":true") == std::string::npos,
            "storage cancel-only mode stranded the clearing-rejected durable-ID cancel after trading resumed");
    require(!host.place({.client_order_id = "storage-blocked", .isin_id = isin, .price = "103000", .quantity = 1})
                    .empty() &&
                !host.move("storage-clearing", "103250", 3).empty() && control.commands().size() == commands.size(),
            "storage recovery reopened Add/Move entry");
    const auto closes = control.successful_closes();
    require(host.stop().code == cg::Plaza2ErrorCode::RuntimeCallFailed, "storage cancellation stop hid its error");
    const auto closed = control.successful_closes();
    for (std::size_t i = 0; i < closes.size(); ++i)
        require(closed[i] > closes[i], "storage cancellation stop left a native handle open");
}
} // namespace moex::connector_host::regression
