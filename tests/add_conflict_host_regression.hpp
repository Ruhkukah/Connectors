#pragma once

#include "add_reply_host_regression.hpp"

#include <fstream>

namespace moex::connector_host {
inline void add_conflict_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                         const std::filesystem::path& root) {
    using namespace add_reply_host_detail;
    const auto isin = config.isin_ids.front();
    const auto account = config.orders.broker_code + config.orders.client_code;
    auto now = OrderManager::Clock::time_point{} + std::chrono::hours(1);
    config.session.recovery_now = [&] { return now; };
    config.session.process_timeout_ms = 0;
    config.orders.risk.max_notional_scaled = 65'000'000'000LL;
    config.utc_now = [] { return std::int64_t{1700000005}; };
    for (const bool submitted_owner : {true, false}) {
        control.configure({.suppress_auto_replies = true,
                           .suppress_initial_orders = true,
                           .zero_position = true,
                           .client_code = account,
                           .session_id = 321});
        const auto label = submitted_owner ? "submitted" : "recovered";
        config.journal_path = root / (std::string("native-add-conflict-") + label + ".ndjson");
        config.identity_state_path = root / (std::string("native-add-conflict-") + label + ".state");
        CgateTradingHost host(config);
        private_delta_host_detail::bootstrap(host);
        const auto poll = [&] {
            const auto error = host.poll();
            require(!error, "conflicting native179 killed host poll: " + error.message);
        };
        constexpr std::int64_t existing_id = 64001;
        if (submitted_owner) {
            require(host.place(add("existing-owner", isin, 3)).empty(), "native conflict owner Add refused");
            poll();
            official_cgate99::FORTS_MSG179 accepted{};
            accepted.order_id = existing_id;
            late_move_host_detail::reply(control, control.commands().back().user_id, 179, accepted);
            poll();
        } else {
            auto recovered = candidate(existing_id, 0, isin, account);
            for (auto& field : recovered.fields)
                if (field.field_code == gen::FieldCode::kFortsTradeReplOrdersLogDir)
                    field.signed_value = 2;
            control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
            control.enqueue(recovered);
            control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
            poll();
        }
        require(host.place(add("conflicting-add", isin, 3)).empty(), "native conflict pending Add refused");
        poll();
        const auto posted = control.commands().back();
        require(posted.name == "AddOrder", "native conflict Add was not published");
        official_cgate99::FORTS_MSG179 conflict{};
        conflict.order_id = existing_id;
        late_move_host_detail::reply(control, posted.user_id, 179, conflict);
        poll();
        const auto unresolved = late_move_host_detail::logical_order(host, "conflicting-add");
        require(unresolved.find("\"state\":\"Unknown\"") != std::string::npos &&
                    unresolved.find("\"operator_action_required\":true") != std::string::npos &&
                    unresolved.find("\"order_id\":0") != std::string::npos &&
                    unresolved.find("\"remaining\":3") != std::string::npos,
                "conflicting native179 did not preserve unresolved identity, exposure and operator alert");
        const auto owner =
            late_move_host_detail::logical_order(host, submitted_owner ? "existing-owner" : "recovered:321:64001");
        require(owner.find("\"order_id\":64001") != std::string::npos &&
                    owner.find("\"state\":\"Unknown\"") == std::string::npos,
                "conflicting native179 hijacked the existing native identity");
        require(!host.place(add("blocked-after-conflict", isin, 3)).empty(),
                "conflicting native179 released unresolved exposure");
        const auto commands = control.commands().size();
        for (int i = 0; i < 3; ++i)
            poll();
        require(control.commands().size() == commands, "conflicting native179 caused an unsafe automatic send");
        require(!host.stop(), "native Add conflict shutdown failed");
        std::ifstream journal(config.journal_path);
        const std::string records{std::istreambuf_iterator<char>(journal), std::istreambuf_iterator<char>()};
        require(records.find("\"event\":\"add_identity_conflict\"") != std::string::npos &&
                    records.find("\"official_order_id\":64001") != std::string::npos,
                "conflicting native179 was not recorded in the application journal");
    }
}
} // namespace moex::connector_host
