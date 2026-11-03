#include "moex/connector_host/full_order_log_trading.hpp"
#include "moex/connector_host/operator_config.hpp"
#include "moex/connector_host/trading_host.hpp"
#include "fake_cgate_control.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "risk_limits_host_regression.hpp"
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>

using namespace moex::connector_host;
namespace cg = moex::plaza2::cgate;
namespace tr = moex::plaza2_trade;
namespace gen = moex::plaza2::generated;
namespace test = moex::plaza2::test;
namespace fake = test::fake;
using test::require;

int main(int argc, char** argv) {
    try {
        require(argc == 2, "fake runtime required");
        const auto root = test::make_temp_directory("full-order-log-trading");
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cg::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA9.9.0", "9.9", "T1"));
        fake::Control control(fixture.library_path);
        ::setenv("MOEX_FOL_WRITE_TEST_KEY", "00000000", 1);
        Plaza2HostConfigInputs inputs;
        inputs.runtime_root = fixture.root;
        inputs.library_path = fixture.library_path;
        inputs.scheme_dir = fixture.scheme_dir;
        inputs.config_dir = fixture.config_dir;
        inputs.env_open_settings = "ini=config/t1.ini;key=00000000";
        inputs.software_key_env_var = "MOEX_FOL_WRITE_TEST_KEY";
        inputs.broker_code = "BRK1";
        inputs.client_code = "C01";
        inputs.isin_ids = {1001};
        inputs.allow_orders = true;
        const std::array<std::int32_t, 1> ids{1001};
        cg::Plaza2FullOrderLog book(ids, 100, 100);
        TradingHostConfig base;
        base.session = build_plaza2_host_config(inputs).transport.host;
        base.session.mode = tr::CgateSessionMode::OfflineFake;
        base.session.process_timeout_ms = 0;
        base.session.collect_poll_metrics = true;
        base.session.aggr20_stream = {};
        base.session.full_order_log_stream = {
            cg::kFullOrderLogStreamCode, cg::full_order_log_listener_settings(fixture.scheme_dir), {}};
        base.session.full_order_log_handler = &book;
        std::chrono::milliseconds elapsed{};
        std::optional<std::chrono::steady_clock::time_point> frozen_now;
        base.session.recovery_now = [&] { return frozen_now.value_or(std::chrono::steady_clock::now() + elapsed); };
        const auto initial_utc = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
        auto utc = initial_utc;
        base.session.full_order_log_utc_now_ns = [&] { return utc; };
        std::function<void(tr::FullOrderLogTradingAuthority&)> alter;
        base.session.full_order_log_authority = [&](auto isin) {
            auto authority = make_full_order_log_trading_authority(book, isin);
            if (alter)
                alter(authority);
            return authority;
        };
        base.orders.broker_code = "BRK1";
        base.orders.client_code = "C01";
        base.isin_ids = {1001};
        regression::explicit_test_risk(base);
        const auto configure = [&] {
            fake::Scenario scenario{.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"};
            scenario.options[static_cast<std::size_t>(fake::Option::FullOrderLogRefdata)] = "1";
            control.configure(scenario);
            alter = {};
            elapsed = {};
            frozen_now.reset();
            utc = initial_utc;
        };
        const auto configuration = [&](std::string_view name) {
            auto config = base;
            config.journal_path = root / (std::string(name) + ".ndjson");
            config.identity_state_path = root / (std::string(name) + ".state");
            return config;
        };
        const auto pump = [&](CgateTradingHost& host, int n = 1) {
            for (int i = 0; i < n; ++i) {
                const auto error = host.poll();
                require(!error, "FOL trading poll: " + error.message);
            }
        };
        const auto warm = [&](CgateTradingHost& host) {
            require(!host.start(), "FOL trading start");
            pump(host, 30);
            // Genuine native bytes update the configured book's exchange moment.
            using enum gen::FieldCode;
            control.enqueue({.kind = fake::EventKind::Begin, .stream_code = cg::kFullOrderLogStreamCode});
            control.enqueue(
                {.stream_code = cg::kFullOrderLogStreamCode,
                 .table_code = gen::TableCode::kFortsTradeReplOrdersLog,
                 .revision = 30,
                 .fields = {
                     {.field_code = kFortsTradeReplOrdersLogReplId, .signed_value = 30},
                     {.field_code = kFortsTradeReplOrdersLogReplRev, .signed_value = 30},
                     {.field_code = kFortsTradeReplOrdersLogPublicOrderId, .signed_value = 9001},
                     {.field_code = kFortsTradeReplOrdersLogSessId, .signed_value = 321},
                     {.field_code = kFortsTradeReplOrdersLogIsinId, .signed_value = 1001},
                     {.field_code = kFortsTradeReplOrdersLogPublicAction, .signed_value = 1},
                     {.field_code = kFortsTradeReplOrdersLogPublicAmountRest, .signed_value = 7},
                     {.field_code = kFortsTradeReplOrdersLogDir, .signed_value = 1},
                     {.field_code = kFortsTradeReplOrdersLogXstatus, .signed_value = 1},
                     {.field_code = kFortsTradeReplOrdersLogPrice, .kind = fake::FieldKind::Text, .text = "102500"},
                     {.field_code = kFortsTradeReplOrdersLogMomentNs,
                      .kind = fake::FieldKind::UnsignedInteger,
                      .unsigned_value = utc}}});
            control.enqueue({.kind = fake::EventKind::Commit, .stream_code = cg::kFullOrderLogStreamCode});
            pump(host, 3);
            require(book.valid() && book.committed_session_id(1001) == 321, "current committed FOL session");
            require(host.last_poll_metrics().sequence > 0,
                    "explicit poll metrics must survive disabled journal timing");
            require(host.status().find("\"source\":\"full_order_log\",\"valid\":true") != std::string::npos &&
                        host.status().find("\"market_data_source\":\"full_order_log\"") != std::string::npos &&
                        host.status().find("\"order_entry_ready\":true") != std::string::npos,
                    "status must expose current FOL authority without an AGGR listener");
            require(control.opens(gen::StreamCode::kFortsAggrRepl) == 0, "FOL write must not open AGGR");
        };
        const auto place = [&](CgateTradingHost& host, std::string id) {
            return host.place({.client_order_id = std::move(id), .isin_id = 1001, .price = "102500", .quantity = 1});
        };
        for (const auto mode :
             {"invalid", "dead", "future-exchange", "crossed", "one-sided", "empty-quantity", "missing-epoch",
              "missing-revision", "session-mismatch", "exception", "MAX", "LifeNum", "channel-close"}) {
            configure();
            CgateTradingHost host(configuration(mode));
            warm(host);
            const auto before = control.commands().size();
            require(place(host, "owned").empty(), "valid FOL Add reaches existing order path");
            pump(host, 2);
            require(control.commands().size() == before + 1, "exactly one initial Add");
            const std::string_view name = mode;
            if (name == "dead") {
                frozen_now = std::chrono::steady_clock::now() + std::chrono::seconds(31);
                pump(host, 3);
            } else if (name == "MAX" || name == "LifeNum" || name == "channel-close") {
                control.enqueue({.kind = name == "MAX"       ? fake::EventKind::ClearDeleted
                                         : name == "LifeNum" ? fake::EventKind::LifeNum
                                                             : fake::EventKind::Close,
                                 .stream_code = cg::kFullOrderLogStreamCode,
                                 .table_code = gen::TableCode::kFortsTradeReplOrdersLog,
                                 .revision = std::numeric_limits<std::int64_t>::max(),
                                 .value = name == "channel-close" ? 0xA006u : 999u});
                pump(host);
                require(!book.valid(), "native MAX/LifeNum must invalidate public authority");
            } else {
                alter = [=](auto& value) {
                    if (name == "invalid")
                        value.valid = false;
                    if (name == "future-exchange")
                        value.exchange_moment_ns += 1000000001ULL;
                    if (name == "crossed")
                        value.crossed = true;
                    if (name == "one-sided")
                        value.best_ask = 0;
                    if (name == "empty-quantity")
                        value.best_bid_quantity = 0;
                    if (name == "missing-epoch")
                        value.epoch = 0;
                    if (name == "missing-revision")
                        value.revision = 0;
                    if (name == "session-mismatch")
                        ++value.session_id;
                    if (name == "exception")
                        throw std::runtime_error("offline provider failure");
                };
            }
            require(!place(host, "blocked").empty() && !host.move("owned", "102250", 1).empty(),
                    "invalid FOL authority must reject Add and Move at admission");
            require(control.commands().size() == before + 1 &&
                        host.status().find("\"order_entry_ready\":false") != std::string::npos,
                    "rejected entry must not post or claim readiness");
            require(host.cancel("owned").empty() && control.commands().size() == before + 2 &&
                        control.commands().back().name == "DelOrder",
                    "FOL invalidation must preserve the existing risk-reducing cancel path");
            require(!host.stop(), "FOL authority case stop");
            std::ifstream journal(root / (std::string(mode) + ".ndjson"));
            bool canonical_url{}, clear{}, close{};
            for (std::string line; std::getline(journal, line);) {
                canonical_url |=
                    line.find("p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL;online.scheme=|FILE|") !=
                    std::string::npos;
                if (line.find("\"stream\":\"FORTS_ORDLOG_REPL\"") == std::string::npos)
                    continue;
                require(line.find("\"event\":\"transaction_begin\"") == std::string::npos &&
                            line.find("\"event\":\"transaction_commit\"") == std::string::npos,
                        "FOL transaction traffic must not flood the interaction journal");
                if (line.find("\"event\":\"stream_row\"") != std::string::npos)
                    require(line.find("\"table\":\"sys_events\"") != std::string::npos,
                            "per-order public book rows must not enter the interaction journal");
                clear |= line.find("\"event\":\"clear_deleted\"") != std::string::npos &&
                         line.find("\"table\":\"orders_log\"") != std::string::npos &&
                         line.find("\"signed_value\":9223372036854775807") != std::string::npos;
                close |= line.find("REPL:CHANNEL_DISCONNECT_ON_TIMEOUT") != std::string::npos &&
                         line.find("0xA006") != std::string::npos;
            }
            require(canonical_url && (name != "MAX" || clear) && (name != "channel-close" || close),
                    "FOL URL and native clear/close lifecycle evidence must be retained");
        }
        for (const auto activity : {"commit", "filtered-row", "sys_events"}) {
            configure();
            CgateTradingHost host(configuration(std::string("quiet-") + activity));
            warm(host);
            frozen_now = std::chrono::steady_clock::now() + std::chrono::seconds(31);
            alter = [=](auto& value) {
                value.committed_at -= std::chrono::seconds(31);
                value.exchange_moment_ns = initial_utc - 31000000000ULL;
            };
            require(!place(host, "before-activity").empty(), "dead stream must not be revived by empty process calls");
            const auto revision = book.revision(1001);
            control.enqueue({.kind = fake::EventKind::Begin, .stream_code = cg::kFullOrderLogStreamCode});
            if (std::string_view(activity) == "commit")
                control.enqueue({.kind = fake::EventKind::Commit, .stream_code = cg::kFullOrderLogStreamCode});
            else
                control.enqueue({.stream_code = cg::kFullOrderLogStreamCode,
                                 .table_code = std::string_view(activity) == "filtered-row"
                                                   ? gen::TableCode::kFortsTradeReplOrdersLog
                                                   : gen::TableCode::kFortsTradeReplSysEvents,
                                 .fields = {{.field_code = gen::FieldCode::kFortsTradeReplOrdersLogIsinId,
                                             .signed_value = 999999}}});
            pump(host, 3);
            require(book.revision(1001) == revision && place(host, "quiet").empty(),
                    "stream activity elsewhere must admit an unchanged quiet instrument's committed book");
            pump(host, 2);
            require(host.move("quiet", "102250", 1).empty(), "active stream must also allow quiet-instrument Move");
            pump(host, 2);
            require(host.cancel("quiet").empty(), "quiet instrument cancel");
            if (std::string_view(activity) != "commit") {
                control.enqueue({.kind = fake::EventKind::Commit, .stream_code = cg::kFullOrderLogStreamCode});
                pump(host);
            }
            require(!host.stop(), "quiet FOL source stop");
        }
        for (const bool exchange : {false, true}) {
            configure();
            CgateTradingHost host(configuration(exchange ? "future-tolerance" : "stream-boundary"));
            warm(host);
            const auto boundary = std::chrono::steady_clock::now();
            frozen_now = boundary;
            alter = [=](auto& value) {
                value.committed_at = boundary - std::chrono::seconds(30);
                value.exchange_moment_ns = exchange ? initial_utc + 1000000000ULL : initial_utc - 31000000000ULL;
            };
            control.enqueue({.kind = fake::EventKind::Begin, .stream_code = cg::kFullOrderLogStreamCode});
            control.enqueue({.kind = fake::EventKind::Commit, .stream_code = cg::kFullOrderLogStreamCode});
            pump(host);
            if (!exchange)
                *frozen_now += std::chrono::seconds(30);
            const auto before = control.commands().size();
            require(place(host, "boundary").empty() && control.commands().size() == before + 1,
                    "exactly30s stream age or1s future exchange time must remain admitted");
            pump(host, 2);
            if (exchange)
                --utc;
            else
                *frozen_now += std::chrono::nanoseconds(1);
            require(!place(host, "expired").empty() && !host.move("boundary", "102250", 1).empty() &&
                        control.commands().size() == before + 1,
                    "one nanosecond beyond stream inactivity or future tolerance must refuse Add and Move");
            pump(host, 3);
            require(!place(host, "still-expired").empty(), "empty native process calls must not restore FOL liveness");
            require(host.cancel("boundary").empty(), "freshness boundary expiry must preserve cancel");
            require(!host.stop(), "FOL freshness boundary stop");
        }
        for (const bool move : {false, true}) {
            configure();
            auto config = configuration(move ? "queued-move" : "queued-add");
            config.orders.max_commands_per_second = 1;
            CgateTradingHost host(config);
            warm(host);
            const auto before = control.commands().size();
            require(place(host, "first").empty(), "rate-window first Add");
            pump(host, 2);
            require((move ? host.move("first", "102250", 1) : place(host, "second")).empty(),
                    "valid authority admits a rate-held command");
            require(control.commands().size() == before + 1, "command must be rate-held before authority loss");
            alter = [](auto& value) { value.valid = false; };
            elapsed += std::chrono::milliseconds(1100);
            pump(host);
            require(control.commands().size() == before + 1 && host.status().find("\"queued\":0") != std::string::npos,
                    "queued Add/Move must be definitively rejected by final prepost FOL check");
            require(host.cancel("first").empty(), "queued-command rejection must retain cancel authority");
            elapsed += std::chrono::milliseconds(1100);
            pump(host);
            require(control.commands().size() == before + 2 && control.commands().back().name == "DelOrder",
                    "cancel must drain after rate-held Add/Move is refused");
            require(!host.stop(), "queued FOL authority stop");
        }
        configure();
        {
            fake::Scenario scenario{.zero_position = true, .client_code = "BRK1C01"};
            control.configure(scenario);
            auto recovery = configuration("cancel-only");
            recovery.session.full_order_log_cancel_only = true;
            recovery.session.full_order_log_stream = {};
            recovery.session.full_order_log_handler = nullptr;
            recovery.session.full_order_log_authority = {};
            recovery.orders.login_from = "trader_a";
            const auto full_opens = control.opens(cg::kFullOrderLogStreamCode);
            const auto before = control.commands().size();
            CgateTradingHost host(recovery);
            require(!host.start(), "cancel-only recovery start");
            pump(host, 30);
            require(host.status().find("recovered:321:20003") != std::string::npos,
                    "cancel-only recovery must reconstruct existing owned orders");
            host.set_kill_switch(false);
            require(!place(host, "forbidden").empty() && !host.move("recovered:321:20003", "102250", 1).empty() &&
                        control.commands().size() == before,
                    "immutable cancel-only mode must block Add/Move even after kill off");
            require(host.cancel("recovered:321:20003").empty() && control.commands().size() == before + 1 &&
                        control.commands().back().name == "DelOrder" &&
                        control.opens(cg::kFullOrderLogStreamCode) == full_opens &&
                        control.opens(gen::StreamCode::kFortsAggrRepl) == 0,
                    "reconstructed cancel must remain available without FOL or AGGR");
            require(!host.stop(), "cancel-only recovery stop");
            tr::CgateSession native(recovery.session);
            require(!native.start(), "native cancel-only start");
            for (int i = 0; i < 20; ++i)
                require(!native.poll(false), "native cancel-only poll");
            const auto health = native.runtime_health();
            require(health.publisher == 3 && health.reply == 3 && health.private_active,
                    "cancel-only prepost regression requires otherwise ready native transports");
            const tr::Plaza2TradeCodec codec;
            const auto add = codec.encode(tr::AddOrderRequest{.broker_code = "BRK1",
                                                              .isin_id = 1001,
                                                              .client_code = "C01",
                                                              .dir = tr::Plaza2TradeSide::Buy,
                                                              .type = tr::Plaza2TradeOrderType::Limit,
                                                              .amount = 1,
                                                              .price = "102500",
                                                              .ext_id = 80});
            const auto move = codec.encode(tr::MoveOrderRequest{.broker_code = "BRK1",
                                                                .regime = 3,
                                                                .order_id1 = 20003,
                                                                .amount1 = 1,
                                                                .price1 = "102250",
                                                                .ext_id1 = 80,
                                                                .client_code = "C01",
                                                                .isin_id = 1001});
            require(add.validation.ok() && move.validation.ok() && !native.post_command(add, 1).post_invoked &&
                        !native.post_command(move, 2).post_invoked && control.commands().size() == before + 1,
                    "cancel-only native final guard must refuse otherwise valid encoded Add/Move");
            require(!native.stop(), "native cancel-only stop");
        }
        configure();
        auto missing_host = configuration("missing-provider");
        missing_host.session.full_order_log_authority = {};
        bool host_refused{};
        try {
            CgateTradingHost invalid_host(missing_host);
        } catch (const std::invalid_argument&) {
            host_refused = true;
        }
        require(host_refused && !std::filesystem::exists(missing_host.journal_path) &&
                    !std::filesystem::exists(missing_host.identity_state_path),
                "missing FOL authority must be refused before journal or durable-ID mutation");
        auto invalid = base.session;
        invalid.full_order_log_authority = {};
        tr::CgateSession missing(invalid);
        require(missing.start().code == cg::Plaza2ErrorCode::InvalidConfiguration,
                "writable FOL requires an authority provider before native startup");
        invalid = base.session;
        invalid.allow_orders = false;
        tr::CgateSession not_allowed(invalid);
        require(not_allowed.start().code == cg::Plaza2ErrorCode::InvalidConfiguration,
                "writable FOL requires explicit allow_orders before native startup");
        invalid = base.session;
        invalid.runtime.environment = cg::Plaza2Environment::Prod;
        tr::CgateSession production(invalid);
        require(production.start().code == cg::Plaza2ErrorCode::InvalidConfiguration,
                "writable FOL must reject production before native startup");
        invalid = base.session;
        invalid.read_only_market_data = true;
        invalid.allow_orders = false;
        invalid.publisher_settings.clear();
        invalid.p2mqreply_settings.clear();
        invalid.full_order_log_authority = {};
        control.set(fake::Option::DisablePublisher);
        control.set(fake::Option::DisableReplyListener);
        tr::CgateSession readonly(invalid);
        require(!readonly.start(), "default FOL read-only start");
        for (int i = 0; i < 20; ++i)
            require(!readonly.poll(false), "default FOL read-only poll");
        require(!readonly.publisher_open() && !readonly.p2mqreply_open(), "read-only FOL creates no order handles");
        require(!readonly.stop(), "read-only FOL stop");
        test::remove_tree(root);
        std::cout << "FOL write: admission, dispatch, invalidation and cancel isolation passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
