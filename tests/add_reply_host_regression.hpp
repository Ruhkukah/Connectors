#pragma once

#include "late_move_host_regression.hpp"

namespace moex::connector_host {
namespace add_reply_host_detail {
namespace fake = plaza2::test::fake;
namespace gen = plaza2::generated;
using plaza2::test::require;

inline fake::Event candidate(std::int64_t id, std::int32_t ext, std::int32_t isin, const std::string& account) {
    using enum gen::FieldCode;
    auto row = private_delta_host_detail::own_order(id, isin, account);
    for (auto& field : row.fields) {
        if (field.field_code == kFortsTradeReplOrdersLogPublicAmount ||
            field.field_code == kFortsTradeReplOrdersLogPrivateAmount)
            field.signed_value = 3;
        if (field.field_code == kFortsTradeReplOrdersLogPublicAmountRest ||
            field.field_code == kFortsTradeReplOrdersLogPrivateAmountRest)
            field.signed_value = 2;
    }
    row.fields.push_back(private_delta_host_detail::integer(kFortsTradeReplOrdersLogExtId, ext));
    return row;
}
inline fake::Event own_fill(std::int64_t id, std::int32_t isin, const std::string& account) {
    using enum gen::FieldCode;
    auto row = private_delta_host_detail::own_trade(70001, isin, account);
    for (auto& field : row.fields)
        if (field.field_code == kFortsTradeReplUserDealPublicOrderIdBuy ||
            field.field_code == kFortsTradeReplUserDealPrivateOrderIdBuy)
            field.signed_value = id;
    return row;
}
template <class T> inline T wire(const fake::PostedCommand& command) {
    require(command.result == 0 && command.payload.size() == sizeof(T), "native Add authority command layout");
    T value{};
    std::memcpy(&value, command.payload.data(), sizeof(value));
    return value;
}
inline OrderRequest add(std::string key, std::int32_t isin, std::int32_t quantity = 1) {
    return {.client_order_id = std::move(key), .isin_id = isin, .price = "103000", .quantity = quantity};
}
inline void assert_cancels(const fake::Control& control, std::size_t begin, std::int64_t official) {
    bool correct{};
    std::size_t adds{};
    const auto commands = control.commands();
    for (const auto& command : std::span(commands).subspan(begin)) {
        adds += command.name == "AddOrder";
        if (command.name == "DelOrder") {
            const auto cancel = wire<official_cgate99::DelOrder>(command);
            require(cancel.order_id != 7777, "client cancellation used the provisional wrong exchange ID");
            correct |= cancel.order_id == official;
        }
    }
    require(correct, "late official179 did not preserve the client cancellation intent");
    require(adds == 1, "late official179 caused a second native Add submission");
}
} // namespace add_reply_host_detail

inline void add_reply_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                      const std::filesystem::path& root) {
    using namespace add_reply_host_detail;
    require(!config.isin_ids.empty(), "native Add authority fixture needs an instrument");
    const auto isin = config.isin_ids.front();
    const auto account = config.orders.broker_code + config.orders.client_code;
    auto now = OrderManager::Clock::time_point{} + std::chrono::hours(1);
    config.session.recovery_now = [&] { return now; };
    config.session.process_timeout_ms = 0;
    config.session.reply_timeout_ms = 10;
    config.orders.reply_timeout = std::chrono::milliseconds(10);
    config.orders.risk.max_open_orders = 100;
    config.utc_now = [] { return std::int64_t{1700000005}; };
    const fake::Scenario scenario{.suppress_auto_replies = true,
                                  .suppress_initial_orders = true,
                                  .zero_position = true,
                                  .client_code = account,
                                  .session_id = 321};
    control.configure(scenario);
    {
        auto collision = config;
        collision.orders.risk.max_notional_scaled = 60'000'000'000LL;
        collision.journal_path = root / "native-add-collision.ndjson";
        collision.identity_state_path = root / "native-add-collision.state";
        CgateTradingHost host(collision);
        private_delta_host_detail::bootstrap(host);
        const auto poll = [&] {
            const auto error = host.poll();
            require(!error, "native Add authority collision poll: " + error.message);
        };
        const auto begin = control.commands().size();
        require(host.place(add("official-add", isin, 3)).empty(), "native Add authority seed refused");
        poll();
        const auto posted = control.commands().back();
        require(posted.name == "AddOrder", "native Add authority seed not published");
        const auto request = wire<official_cgate99::AddOrder>(posted);
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
        control.enqueue(candidate(7777, request.ext_id, isin, account));
        control.enqueue(own_fill(7777, isin, account));
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
        poll();
        const auto unresolved = late_move_host_detail::logical_order(host, "official-add");
        require(unresolved.find("\"order_id\":0") != std::string::npos &&
                    unresolved.find("\"executed\":0") != std::string::npos,
                "native ext_id row or its fill became client authority before179");
        require(!host.place(add("must-reserve-candidate-risk", isin)).empty(),
                "held own candidate exposure was omitted from native risk");
        require(host.cancel("official-add").empty(), "native provisional Add cancellation refused");
        poll();
        const auto pending_cancel = control.commands().back();
        require(pending_cancel.name == "DelUserOrders", "provisional Add cancellation chose an exchange ID");
        const auto scoped = wire<official_cgate99::DelUserOrders>(pending_cancel);
        require(scoped.ext_id == request.ext_id && scoped.isin_id == isin,
                "provisional Add cancellation lost its exact ext_id/instrument scope");
        now += std::chrono::milliseconds(2);
        official_cgate99::FORTS_MSG179 reply{};
        reply.order_id = 63011;
        late_move_host_detail::reply(control, posted.user_id, 179, reply);
        poll();
        const auto official = late_move_host_detail::logical_order(host, "official-add");
        require(official.find("\"order_id\":63011") != std::string::npos &&
                    official.find("\"executed\":0") != std::string::npos &&
                    official.find("\"remaining\":3") != std::string::npos,
                "late native179 did not replace provisional authority without crediting unrelated fills");
        const auto unrelated = late_move_host_detail::logical_order(host, "recovered:321:7777");
        require(unrelated.find("\"executed\":1") != std::string::npos &&
                    unrelated.find("\"remaining\":2") != std::string::npos,
                "official179 discarded the separate own order or its deferred fill");
        require(!host.place(add("must-keep-recovered-risk", isin)).empty(),
                "official179 transfer omitted one of the two working account exposures");
        assert_cancels(control, begin, 63011);
        late_move_host_detail::order(control, 63011, 80001, 3, 0, isin, account);
        poll();
        require(!host.place(add("must-keep-unrelated-risk", isin, 4)).empty(),
                "official Add termination released the separate recovered order exposure");
        require(host.place(add("after-official-terminal", isin)).empty(),
                "official Add termination did not preserve the separate remaining account exposure");
        require(!host.stop(), "native Add authority collision stop");
    }
    control.configure(scenario);
    {
        auto absent = config;
        absent.orders.risk.max_notional_scaled = 31'000'000'000LL;
        absent.journal_path = root / "native-add-late-absence.ndjson";
        absent.identity_state_path = root / "native-add-late-absence.state";
        CgateTradingHost host(absent);
        private_delta_host_detail::bootstrap(host);
        const auto poll = [&] {
            const auto error = host.poll();
            require(!error, "native Add authority absence poll: " + error.message);
        };
        const auto begin = control.commands().size();
        require(host.place(add("late-after-absence", isin, 3)).empty(), "native late Add seed refused");
        poll();
        const auto posted = control.commands().back();
        now += std::chrono::milliseconds(20);
        poll();
        const auto recovery = control.commands().back();
        require(recovery.name == "DelUserOrders", "native timed-out Add did not start absence resolution");
        official_cgate99::FORTS_MSG186 empty{};
        late_move_host_detail::reply(control, recovery.user_id, 186, empty);
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
        control.enqueue({.stream_code = gen::StreamCode::kFortsTradeRepl,
                         .table_code = gen::TableCode::kFortsTradeReplHeartbeat,
                         .revision = 90001,
                         .fields = {{.field_code = gen::FieldCode::kFortsTradeReplHeartbeatServerTime,
                                     .kind = fake::FieldKind::Timestamp,
                                     .unsigned_value = 1700020005}}});
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
        poll();
        require(late_move_host_detail::logical_order(host, "late-after-absence").find("\"state\":\"Cancelled\"") !=
                    std::string::npos,
                "native late Add fixture did not establish the earlier absence result");
        official_cgate99::FORTS_MSG179 reply{};
        reply.order_id = 63012;
        late_move_host_detail::reply(control, posted.user_id, 179, reply);
        poll();
        const auto official = late_move_host_detail::logical_order(host, "late-after-absence");
        require(official.find("\"order_id\":63012") != std::string::npos &&
                    official.find("\"remaining\":3") != std::string::npos,
                "late native179 lost its exact identity or risk after an earlier absence result");
        require(!host.place(add("must-restore-late-risk", isin)).empty(),
                "late native179 left accepted Add exposure released after absence");
        assert_cancels(control, begin, 63012);
        late_move_host_detail::order(control, 63012, 90002, 3, 0, isin, account);
        poll();
        require(host.place(add("after-late-official-terminal", isin)).empty(),
                "native late official terminal did not release accepted Add exposure");
        require(!host.stop(), "native late Add authority absence stop");
    }
    control.configure(scenario);
    {
        auto recovered = config;
        recovered.orders.risk.max_notional_scaled = 60'000'000'000LL;
        recovered.journal_path = root / "native-add-existing-identity.ndjson";
        recovered.identity_state_path = root / "native-add-existing-identity.state";
        CgateTradingHost host(recovered);
        private_delta_host_detail::bootstrap(host);
        const auto poll = [&] {
            const auto error = host.poll();
            require(!error, "native Add recovered-identity poll: " + error.message);
        };
        require(host.place(add("exact-recovered-add", isin, 3)).empty(), "native existing-ID Add refused");
        poll();
        const auto posted = control.commands().back();
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
        control.enqueue(candidate(63013, 0, isin, account));
        control.enqueue(own_fill(63013, isin, account));
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
        poll();
        require(late_move_host_detail::logical_order(host, "recovered:321:63013").find("\"executed\":1") !=
                    std::string::npos,
                "native existing-ID fixture did not reconstruct its pre179 fill");
        official_cgate99::FORTS_MSG179 reply{};
        reply.order_id = 63013;
        late_move_host_detail::reply(control, posted.user_id, 179, reply);
        poll();
        const auto official = late_move_host_detail::logical_order(host, "exact-recovered-add");
        require(official.find("\"order_id\":63013") != std::string::npos &&
                    official.find("\"executed\":1") != std::string::npos &&
                    official.find("\"remaining\":2") != std::string::npos &&
                    host.status().find("\"client_order_id\":\"recovered:321:63013\"") == std::string::npos,
                "official native179 duplicated an already reconstructed identity or lost its fill");
        // Replaying the original native evidence must preserve the transferred
        // fill deduplication and the remaining quantity under the client key.
        auto repeated = candidate(63013, 0, isin, account);
        repeated.revision = 95001;
        auto repeated_fill = own_fill(63013, isin, account);
        repeated_fill.revision = 95002;
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
        control.enqueue(repeated);
        control.enqueue(repeated_fill);
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
        poll();
        require(late_move_host_detail::logical_order(host, "exact-recovered-add").find("\"executed\":1") !=
                    std::string::npos,
                "native identity merge counted a replayed pre179 fill twice");
        require(host.place(add("fits-after-exact-identity-merge", isin, 3)).empty(),
                "native exact-ID merge retained duplicate reconstructed risk");
        require(!host.stop(), "native existing-ID Add authority stop");
    }
}
} // namespace moex::connector_host
