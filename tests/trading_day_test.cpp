#include "moex/connector_host/operator_config.hpp"
#include "moex/connector_host/trading_host.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"
#include "fixtures/cgate99_messages.hpp"

#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace cg = moex::plaza2::cgate;
namespace test = moex::plaza2::test;
namespace fake = test::fake;
namespace gen = moex::plaza2::generated;
using namespace moex::connector_host;
using namespace std::chrono_literals;

fake::Field integer(gen::FieldCode code, std::int64_t value) {
    return {.field_code = code, .signed_value = value};
}
fake::Field text(gen::FieldCode code, std::string value) {
    return {.field_code = code, .kind = fake::FieldKind::Text, .text = std::move(value)};
}
fake::Event order(std::int64_t id, std::int32_t session, std::int32_t ext, std::int64_t remaining,
                  std::int64_t revision, std::int64_t previous = 0, std::int8_t action = 1) {
    using enum gen::FieldCode;
    return {
        .stream_code = gen::StreamCode::kFortsTradeRepl,
        .table_code = gen::TableCode::kFortsTradeReplOrdersLog,
        .revision = revision,
        .fields = {integer(kFortsTradeReplOrdersLogReplId, id), integer(kFortsTradeReplOrdersLogPublicOrderId, id),
                   integer(kFortsTradeReplOrdersLogPrivateOrderId, id),
                   integer(kFortsTradeReplOrdersLogSessId, session), integer(kFortsTradeReplOrdersLogIsinId, 1001),
                   integer(kFortsTradeReplOrdersLogExtId, ext), integer(kFortsTradeReplOrdersLogIdOrd1, previous),
                   integer(kFortsTradeReplOrdersLogDir, 1), integer(kFortsTradeReplOrdersLogPublicAction, action),
                   integer(kFortsTradeReplOrdersLogPrivateAction, action),
                   integer(kFortsTradeReplOrdersLogPublicAmount, 3), integer(kFortsTradeReplOrdersLogPrivateAmount, 3),
                   integer(kFortsTradeReplOrdersLogPublicAmountRest, remaining),
                   integer(kFortsTradeReplOrdersLogPrivateAmountRest, remaining),
                   text(kFortsTradeReplOrdersLogClientCode, "BRK1C01"), text(kFortsTradeReplOrdersLogPrice, "103000")}};
}
fake::Event deal(std::int64_t id, std::int64_t deal_id, std::int64_t amount, std::int64_t revision) {
    using enum gen::FieldCode;
    return {.stream_code = gen::StreamCode::kFortsTradeRepl,
            .table_code = gen::TableCode::kFortsTradeReplUserDeal,
            .revision = revision,
            .fields = {integer(kFortsTradeReplUserDealReplId, deal_id), integer(kFortsTradeReplUserDealIdDeal, deal_id),
                       integer(kFortsTradeReplUserDealSessId, 321), integer(kFortsTradeReplUserDealIsinId, 1001),
                       integer(kFortsTradeReplUserDealPublicOrderIdBuy, id),
                       integer(kFortsTradeReplUserDealPrivateOrderIdBuy, id),
                       integer(kFortsTradeReplUserDealXamount, amount), text(kFortsTradeReplUserDealCodeBuy, "BRK1C01"),
                       text(kFortsTradeReplUserDealCodeSell, "OTHER"), text(kFortsTradeReplUserDealPrice, "103000")}};
}
void transaction(const fake::Control& control, gen::StreamCode stream, std::vector<fake::Event> rows) {
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = stream});
    for (const auto& row : rows)
        control.enqueue(row);
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = stream});
}
template <class T> T wire(const fake::PostedCommand& command) {
    test::require(command.result == 0 && command.payload.size() == sizeof(T), "native command wire invalid");
    T value{};
    std::memcpy(&value, command.payload.data(), sizeof(value));
    return value;
}
template <class T> void reply(const fake::Control& control, const fake::PostedCommand& command, int msgid, T value) {
    std::vector<std::byte> bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, bytes.size());
    control.enqueue(
        {.kind = fake::EventKind::Reply, .message_id = msgid, .user_id = command.user_id, .payload = std::move(bytes)});
}
std::string logical_order(const CgateTradingHost& host, std::string_view key) {
    const auto status = host.status();
    const auto start = status.find("\"client_order_id\":\"" + std::string(key) + "\"");
    test::require(start != std::string::npos, "logical order missing from status");
    return status.substr(start, status.find('}', start) - start + 1);
}

int main(int argc, char** argv) {
    std::filesystem::path root;
    try {
        test::require(argc == 3, "fake runtime and output journal required");
        root = test::make_temp_directory("trading-day");
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cg::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA9.9.0", "9.9", "T1"));
        fake::Control control(fixture.library_path);
        fake::Scenario scenario{.continuous_input = false,
                                .suppress_auto_replies = true,
                                .suppress_initial_orders = true,
                                .zero_position = true,
                                .client_code = "BRK1C01",
                                .session_id = 321};
        control.configure(scenario);
        Plaza2HostConfigInputs input;
        input.runtime_root = fixture.root;
        input.library_path = fixture.library_path;
        input.scheme_dir = fixture.scheme_dir;
        input.config_dir = fixture.config_dir;
        input.env_open_settings = "ini=config/t1.ini;key=00000000";
        ::setenv("MOEX_FAKE_CREDENTIALS", "fake-only", 1);
        ::setenv("MOEX_FAKE_SOFTWARE_KEY", "00000000", 1);
        input.credentials_env_var = "MOEX_FAKE_CREDENTIALS";
        input.software_key_env_var = "MOEX_FAKE_SOFTWARE_KEY";
        input.broker_code = "BRK1";
        input.client_code = "C01";
        input.isin_ids = {1001};
        input.allow_orders = true;
        auto now = OrderManager::Clock::time_point{} + 1h;
        std::int64_t utc = 1700000005;
        TradingHostConfig config;
        config.session = build_plaza2_host_config(input).transport.host;
        config.session.mode = moex::plaza2_trade::CgateSessionMode::OfflineFake;
        config.session.process_timeout_ms = 0;
        config.session.recovery_now = [&] { return now; };
        config.orders.broker_code = input.broker_code;
        config.orders.client_code = input.client_code;
        config.orders.login_from = "owner-login";
        config.orders.ext_id_range_configured = true;
        config.orders.max_commands_per_second = 5;
        config.session.publisher_messages_per_second = 5;
        config.isin_ids = {1001};
        config.orders.risk.quantity_configured = true;
        config.orders.risk.open_orders_configured = true;
        config.orders.risk.max_notional_by_isin = {{1001, INT64_MAX}};
        config.orders.risk.max_position_by_isin = {{1001, 100}};
        config.journal_path = root / "events.ndjson";
        config.identity_state_path = root / "day.state";
        config.utc_now = [&] { return utc; };
        const auto poll = [&](CgateTradingHost& host, int count = 3) {
            for (int i = 0; i < count; ++i) {
                now += 200ms;
                test::require(!host.poll(), "native CGate day poll failed");
            }
        };
        const auto phase = [&](CgateTradingHost& host, int hours) {
            now += std::chrono::hours(hours);
            utc += hours * 3600;
            poll(host);
        };
        std::int32_t carry_ext{}, restart_ext{};
        std::uint32_t largest_user{};
        {
            CgateTradingHost host(config);
            const auto start_error = host.start();
            test::require(!start_error, "morning start failed: " + start_error.message);
            poll(host, 20);
            test::require(host.status().find("\"reconstructing\":false") != std::string::npos,
                          "initial exposure reconstruction never completed");
            scenario.continuous_input = true;
            control.configure(scenario);
            const auto before = control.commands().size();
            for (const auto key : {"partial", "cancelled", "moved"})
                test::require(
                    host.place({.client_order_id = key, .isin_id = 1001, .price = "103000", .quantity = 3}).empty(),
                    "morning concurrent Add refused");
            const auto calls = control.process_count();
            poll(host);
            test::require(control.process_count() - calls <= 310, "continuous input starved the command owner");
            auto commands = control.commands();
            test::require(commands.size() == before + 3, "three concurrent orders did not reach native publisher");
            std::vector<fake::Event> initial;
            for (int i = 0; i < 3; ++i) {
                const auto add = wire<official_cgate99::AddOrder>(commands[before + i]);
                test::require(commands[before + i].name == "AddOrder", "wrong command family");
                reply(control, commands[before + i], 179, official_cgate99::FORTS_MSG179{.order_id = 61001 + i});
                initial.push_back(order(61001 + i, 321, add.ext_id, 3, 100 + i));
            }
            transaction(control, gen::StreamCode::kFortsTradeRepl, initial);
            poll(host);
            const auto partial_ext = wire<official_cgate99::AddOrder>(commands[before]).ext_id;
            transaction(control, gen::StreamCode::kFortsTradeRepl,
                        {order(61001, 321, partial_ext, 2, 110, 0, 2), deal(61001, 70001, 1, 111)});
            poll(host);
            test::require(logical_order(host, "partial").find("\"executed\":1") != std::string::npos,
                          "partial user_deal not accounted");
            test::require(host.cancel("cancelled").empty(), "day DelOrder refused");
            poll(host);
            auto cancel = control.commands().back();
            test::require(wire<official_cgate99::DelOrder>(cancel).order_id == 61002, "cancel targeted wrong order");
            reply(control, cancel, 177, official_cgate99::FORTS_MSG177{});
            transaction(
                control, gen::StreamCode::kFortsTradeRepl,
                {order(61002, 321, wire<official_cgate99::AddOrder>(commands[before + 1]).ext_id, 0, 112, 0, 0)});
            poll(host);
            test::require(host.move("moved", "103250", 2).empty(), "day MoveOrder refused");
            poll(host);
            const auto move = control.commands().back();
            test::require(wire<official_cgate99::MoveOrder>(move).regime == 3, "Move uses unsafe fill regime");
            // New-ID replication arrives before reply 176, while the old ID fills.
            const auto moved_ext = wire<official_cgate99::MoveOrder>(move).ext_id1;
            transaction(control, gen::StreamCode::kFortsTradeRepl,
                        {order(61003, 321, moved_ext, 2, 113, 0, 2), deal(61003, 70002, 1, 114),
                         order(62003, 321, moved_ext, 1, 115)});
            poll(host);
            reply(control, move, 176, official_cgate99::FORTS_MSG176{.order_id1 = 62003});
            poll(host);
            const auto moved = logical_order(host, "moved");
            test::require(moved.find("\"order_id\":62003") != std::string::npos &&
                              moved.find("\"executed\":1") != std::string::npos &&
                              moved.find("\"remaining\":1") != std::string::npos,
                          "Move race changed execution quantity or lost replacement ID");
            phase(host, 4); // Morning to day, with working orders still present.
            transaction(control, gen::StreamCode::kFortsSessionstateRepl,
                        {{.stream_code = gen::StreamCode::kFortsSessionstateRepl,
                          .table_code = gen::TableCode::kFortsSessionstateReplSessionState,
                          .revision = 500,
                          .fields = {integer(gen::FieldCode::kFortsSessionstateReplSessionStateSessId, 321),
                                     integer(gen::FieldCode::kFortsSessionstateReplSessionStatePublicState, 2)}}});
            transaction(control, gen::StreamCode::kFortsAggrRepl,
                        {{.stream_code = gen::StreamCode::kFortsAggrRepl,
                          .table_code = gen::TableCode::kFortsAggrReplSysEvents,
                          .revision = 500,
                          .fields = {integer(gen::FieldCode::kFortsAggrReplSysEventsEventId, 500),
                                     integer(gen::FieldCode::kFortsAggrReplSysEventsSessId, 321),
                                     integer(gen::FieldCode::kFortsAggrReplSysEventsEventType, 5)}}});
            poll(host);
            test::require(
                !host.place({.client_order_id = "during-clearing", .isin_id = 1001, .price = "103000", .quantity = 1})
                     .empty(),
                "Add allowed during clearing");
            host.set_kill_switch(true);
            test::require(host.cancel_all(1001).empty(), "emergency mass cancel refused under kill");
            poll(host);
            const auto mass = control.commands().back();
            const auto mass_wire = wire<official_cgate99::DelUserOrders>(mass);
            test::require(mass.name == "DelUserOrders" && mass_wire.buy_sell == 3 && mass_wire.ext_id == 0 &&
                              mass_wire.non_system == 0 && mass_wire.instrument_mask == 1,
                          "emergency mass cancel malformed");
            reply(control, mass, 186, official_cgate99::FORTS_MSG186{.num_orders = 2});
            transaction(control, gen::StreamCode::kFortsTradeRepl,
                        {order(61001, 321, partial_ext, 0, 600, 0, 0), order(62003, 321, moved_ext, 0, 601, 0, 0)});
            poll(host);
            test::require(logical_order(host, "partial").find("Cancelled") != std::string::npos &&
                              logical_order(host, "moved").find("Cancelled") != std::string::npos,
                          "mass cancel did not reconcile terminal replication: " + logical_order(host, "partial") +
                              " / " + logical_order(host, "moved"));
            transaction(control, gen::StreamCode::kFortsSessionstateRepl,
                        {{.stream_code = gen::StreamCode::kFortsSessionstateRepl,
                          .table_code = gen::TableCode::kFortsSessionstateReplSessionState,
                          .revision = 602,
                          .fields = {integer(gen::FieldCode::kFortsSessionstateReplSessionStateSessId, 321),
                                     integer(gen::FieldCode::kFortsSessionstateReplSessionStatePublicState, 1)}}});
            poll(host);
            host.set_kill_switch(false);
            for (const auto key : {"carry", "restart-working"}) {
                test::require(
                    host.place({.client_order_id = key, .isin_id = 1001, .price = "103000", .quantity = 3}).empty(),
                    "post-clearing Add refused");
                poll(host);
                const auto add = control.commands().back();
                const bool carry = std::string_view(key) == "carry";
                const auto id = carry ? 63001 : 63002;
                const auto ext = wire<official_cgate99::AddOrder>(add).ext_id;
                (carry ? carry_ext : restart_ext) = ext;
                reply(control, add, 179, official_cgate99::FORTS_MSG179{.order_id = id});
                transaction(control, gen::StreamCode::kFortsTradeRepl, {order(id, 321, ext, 3, id)});
                poll(host);
            }
            control.set(fake::Option::PrepublishNextSession);
            poll(host);
            control.clear(fake::Option::PrepublishNextSession);
            control.set(fake::Option::LiveSessionSwitch);
            phase(host, 6); // Main clearing and evening session selection.
            control.clear(fake::Option::LiveSessionSwitch);
            test::require(host.status().find("\"sess_id\":322") != std::string::npos,
                          "evening session not selected from native status/refdata");
            transaction(control, gen::StreamCode::kFortsTradeRepl,
                        {order(63001, 321, carry_ext, 0, 64000, 0, 0), order(64001, 322, 0, 3, 64001, 63001)});
            poll(host);
            const auto carried = logical_order(host, "carry");
            test::require(carried.find("\"order_id\":64001") != std::string::npos &&
                              carried.find("\"state\":\"Working\"") != std::string::npos,
                          "documented multi-day relist lost logical order");
            for (const auto& posted : control.commands())
                largest_user = std::max(largest_user, posted.user_id);
            test::require(!host.stop(), "evening shutdown failed");
        }
        scenario.continuous_input = false;
        scenario.session_id = 322;
        control.configure(scenario);
        {
            CgateTradingHost restarted(config);
            test::require(!restarted.start(), "restart failed");
            transaction(control, gen::StreamCode::kFortsTradeRepl,
                        {order(64001, 322, 0, 3, 65001), order(63002, 322, restart_ext, 3, 65002)});
            poll(restarted, 20);
            test::require(restarted.status().find("recovered:322:64001") != std::string::npos &&
                              restarted.status().find("recovered:322:63002") != std::string::npos,
                          "restart omitted one of two working exchange orders");
            test::require(restarted.cancel("recovered:322:64001").empty(), "recovered cancel refused");
            poll(restarted);
            const auto cancelled = control.commands().back();
            test::require(wire<official_cgate99::DelOrder>(cancelled).order_id == 64001 &&
                              cancelled.user_id > largest_user,
                          "restart reused correlation ID or cancelled old exchange ID");
            reply(control, cancelled, 177, official_cgate99::FORTS_MSG177{});
            transaction(control, gen::StreamCode::kFortsTradeRepl, {order(64001, 322, 0, 0, 65003, 0, 0)});
            poll(restarted);
            test::require(logical_order(restarted, "recovered:322:64001").find("Cancelled") != std::string::npos,
                          "restart cancel not confirmed by TRADE");
            test::require(logical_order(restarted, "recovered:322:63002").find("Working") != std::string::npos,
                          "restart cancel affected the other working order");
            test::require(
                restarted.place({.client_order_id = "evening", .isin_id = 1001, .price = "103000", .quantity = 1})
                    .empty(),
                "evening Add refused");
            poll(restarted);
            const auto evening = control.commands().back();
            test::require(wire<official_cgate99::AddOrder>(evening).ext_id > restart_ext &&
                              evening.user_id > largest_user,
                          "restart reused external/correlation IDs");
            reply(control, evening, 179, official_cgate99::FORTS_MSG179{.order_id = 65001});
            transaction(control, gen::StreamCode::kFortsTradeRepl,
                        {order(65001, 322, wire<official_cgate99::AddOrder>(evening).ext_id, 1, 65004)});
            poll(restarted);
            // Lose an Add reply and reject recovery cancels through the native reply listener.
            test::require(
                restarted.place({.client_order_id = "lost-reply", .isin_id = 1001, .price = "103000", .quantity = 1})
                    .empty(),
                "uncertain Add refused");
            poll(restarted);
            const auto uncertain = control.commands().back();
            const auto uncertain_ext = wire<official_cgate99::AddOrder>(uncertain).ext_id;
            now += 61s;
            utc += 61;
            poll(restarted, 1);
            for (int attempt = 0; attempt < 3; ++attempt) {
                const auto recovery = control.commands().back();
                const auto recovery_wire = wire<official_cgate99::DelUserOrders>(recovery);
                test::require(recovery.name == "DelUserOrders" && recovery_wire.ext_id == uncertain_ext &&
                                  recovery_wire.buy_sell == 3,
                              "native unknown-order recovery malformed");
                reply(control, recovery, 186, official_cgate99::FORTS_MSG186{.code = 1});
                poll(restarted, 1);
                test::require(logical_order(restarted, "lost-reply").find("\"state\":\"Unknown\"") != std::string::npos,
                              "failed recovery manufactured a working order");
                if (attempt != 2) {
                    now += 2s;
                    utc += 2;
                    poll(restarted, 1);
                }
            }
            test::require(logical_order(restarted, "lost-reply").find("\"operator_action_required\":true") !=
                              std::string::npos,
                          "native recovery exhaustion hid required operator action");
            const auto bounded = control.commands().size();
            now += 130s;
            utc += 130;
            poll(restarted, 10);
            test::require(control.commands().size() == bounded, "native failed recovery loop resumed indefinitely");
            test::require(!restarted.stop(), "restarted shutdown failed");
        }
        std::filesystem::copy_file(config.journal_path, argv[2], std::filesystem::copy_options::overwrite_existing);
        test::remove_tree(root);
        std::cout << "compressed native CGate day: morning/day/clearing/evening/restart passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (!root.empty())
            test::remove_tree(root);
        return 1;
    }
}
