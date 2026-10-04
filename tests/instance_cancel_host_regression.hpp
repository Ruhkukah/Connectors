#pragma once

#include "fake_cgate_control.hpp"
#include "fixtures/cgate99_messages.hpp"
#include "moex/connector_host/trading_host.hpp"
#include "plaza2_runtime_test_support.hpp"

#include <cstring>
#include <sstream>

namespace moex::connector_host::regression {
inline void instance_cancel_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                            const std::filesystem::path& root) {
    namespace test = plaza2::test;
    namespace fake = test::fake;
    namespace gen = plaza2::generated;
    using enum gen::FieldCode;
    const auto isin = config.isin_ids.front();
    const auto account = config.orders.broker_code + config.orders.client_code;
    config.orders.login_from = "owner-login";
    config.orders.ext_id_begin = 1000;
    config.orders.ext_id_end = 1999;
    config.journal_path = root / "instance-cancel.ndjson";
    config.identity_state_path = root / "instance-cancel.state";
    control.configure({.suppress_auto_replies = true,
                       .suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = account,
                       .session_id = 321});
    CgateTradingHost host(config);
    test::require(!host.start(), "instance cancel native startup");
    for (int i = 0; i < 30; ++i)
        test::require(!host.poll(), "instance cancel native bootstrap");
    const auto integer = [](gen::FieldCode field, std::int64_t value) {
        return fake::Field{.field_code = field, .signed_value = value};
    };
    const auto order = [&](std::int64_t id, std::int64_t ext, const char* login, std::int64_t rest,
                           std::int64_t revision) {
        return fake::Event{
            .stream_code = gen::StreamCode::kFortsTradeRepl,
            .table_code = gen::TableCode::kFortsTradeReplOrdersLog,
            .revision = revision,
            .fields = {
                integer(kFortsTradeReplOrdersLogReplId, id),
                integer(kFortsTradeReplOrdersLogPrivateOrderId, id),
                integer(kFortsTradeReplOrdersLogExtId, ext),
                integer(kFortsTradeReplOrdersLogSessId, 321),
                integer(kFortsTradeReplOrdersLogIsinId, isin),
                integer(kFortsTradeReplOrdersLogDir, 1),
                integer(kFortsTradeReplOrdersLogPublicAction, rest ? 1 : 0),
                integer(kFortsTradeReplOrdersLogPrivateAction, rest ? 1 : 0),
                integer(kFortsTradeReplOrdersLogPublicAmount, 1),
                integer(kFortsTradeReplOrdersLogPrivateAmount, 1),
                integer(kFortsTradeReplOrdersLogPublicAmountRest, rest),
                integer(kFortsTradeReplOrdersLogPrivateAmountRest, rest),
                {.field_code = kFortsTradeReplOrdersLogClientCode, .kind = fake::FieldKind::Text, .text = account},
                {.field_code = kFortsTradeReplOrdersLogLoginFrom, .kind = fake::FieldKind::Text, .text = login},
                {.field_code = kFortsTradeReplOrdersLogPrice, .kind = fake::FieldKind::Text, .text = "103000"}}};
    };
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
    control.enqueue(order(81001, 1001, "owner-login", 1, 81001));
    control.enqueue(order(81002, 1002, "other-login", 1, 81002));
    control.enqueue(order(81003, 3000, "owner-login", 1, 81003));
    control.enqueue(order(81004, 0, "owner-login", 1, 81004));
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
    test::require(!host.poll() && host.has_working_orders(), "instance own order not visible to shutdown");
    const auto before = control.commands().size();
    test::require(host.cancel_all(isin).empty(), "instance cancel-all refused");
    const auto commands = control.commands();
    test::require(commands.size() == before + 1 && commands.back().name == "DelOrder",
                  "instance cancel-all sent a broad or foreign cancellation");
    official_cgate99::DelOrder cancel{};
    test::require(commands.back().payload.size() == sizeof(cancel), "instance DelOrder layout");
    std::memcpy(&cancel, commands.back().payload.data(), sizeof(cancel));
    test::require(cancel.order_id == 81001, "instance cancel-all targeted another login/range/manual order");
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsTradeRepl});
    control.enqueue(order(81001, 1001, "owner-login", 0, 81010));
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsTradeRepl});
    test::require(!host.poll(), "instance cancellation terminal proof");
    test::require(!host.has_working_orders(), "other instances/manual orders prevented this instance shutdown");
    std::ostringstream report;
    host.report_outstanding_orders(report);
    test::require(report.str().empty(), "instance shutdown reported untouched other-instance orders as its own");
    test::require(!host.stop(), "instance cancel native stop");
}
} // namespace moex::connector_host::regression
