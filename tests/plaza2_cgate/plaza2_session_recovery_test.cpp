#include "moex/plaza2_trade/cgate_session.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"
#include "plaza2_trade_test_support.hpp"
#include "private_pos_anchor_regression.hpp"
#include "publisher_prewarm_regression.hpp"
#include "fixtures/cgate99_messages.hpp"
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>
using namespace moex::plaza2_trade;
using namespace moex::plaza2;
using test::require;
namespace {
void instrument_session_post_regression(CgateSessionConfig config, const test::fake::Control& fake) {
    using enum generated::StreamCode;
    using enum generated::TableCode;
    using enum generated::FieldCode;
    config.publisher_rate_owner = PublisherRateOwner::External;
    config.event_log = {};
    config.listener_event_log = {};
    fake.configure({});
    CgateSession session(config);
    require(!session.start(), "instrument session post regression start");
    for (int i = 0; i < 10; ++i)
        require(!session.poll(false), "instrument session post regression bootstrap");
    auto add = test_support::make_add_order();
    add.isin_id = 1001;
    auto move = test_support::make_move_order();
    move.isin_id = 1001;
    const auto add_command = Plaza2TradeCodec{}.encode(add);
    const auto move_command = Plaza2TradeCodec{}.encode(move);
    require(add_command.validation.ok() && move_command.validation.ok(), "session post commands must be valid");
    const auto rejected = [&](std::uint32_t id, const char* message) {
        const auto before = session.publisher_call_counts();
        const auto result = session.post_command(add_command, id);
        require(result.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent && !result.post_invoked,
                message);
        const auto moved = session.post_command(move_command, id + 1);
        require(moved.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent && !moved.post_invoked,
                "direct Move ignored an unbound instrument session");
        const auto after = session.publisher_call_counts();
        require(after.msgnew == before.msgnew && after.post == before.post,
                "unbound direct session post reached native allocation or publication");
    };
    fake.enqueue({.kind = test::fake::EventKind::Begin, .stream_code = kFortsInstrumentstateRepl});
    fake.enqueue({.kind = test::fake::EventKind::ClearDeleted,
                  .stream_code = kFortsInstrumentstateRepl,
                  .table_code = kFortsInstrumentstateReplSysEvents,
                  .revision = std::numeric_limits<std::int64_t>::max()});
    fake.enqueue({.kind = test::fake::EventKind::Commit, .stream_code = kFortsInstrumentstateRepl});
    require(!session.poll(false), "missing instrument session proof pump");
    rejected(880101, "direct Add accepted status without committed instrument session proof");
    const auto publish = [&](std::int32_t day, std::int64_t revision) {
        fake.enqueue({.kind = test::fake::EventKind::Begin, .stream_code = kFortsInstrumentstateRepl});
        fake.enqueue({.stream_code = kFortsInstrumentstateRepl,
                      .table_code = kFortsInstrumentstateReplSysEvents,
                      .revision = revision,
                      .fields = {{.field_code = kFortsInstrumentstateReplSysEventsReplId, .signed_value = revision},
                                 {.field_code = kFortsInstrumentstateReplSysEventsSessId, .signed_value = day}}});
        fake.enqueue(
            {.stream_code = kFortsInstrumentstateRepl,
             .table_code = kFortsInstrumentstateReplInstrumentState,
             .revision = revision + 1,
             .fields = {{.field_code = kFortsInstrumentstateReplInstrumentStateIsinId, .signed_value = 1001},
                        {.field_code = kFortsInstrumentstateReplInstrumentStatePublicState, .signed_value = 1}}});
        fake.enqueue({.kind = test::fake::EventKind::Commit, .stream_code = kFortsInstrumentstateRepl});
        require(!session.poll(false), "instrument session proof publication pump");
    };
    publish(322, 100);
    rejected(880103, "direct Add accepted status from a different instrument stream session");
    publish(321, 102);
    require(session.post_command(add_command, 880105).certainty == cgate::Plaza2SubmissionCertainty::Posted,
            "matching committed session and fresh status did not restore direct Add");
    require(!session.stop(), "instrument session post regression stop");
    fake.configure({});
}
void poll_metrics_regression(CgateSessionConfig config, const test::fake::Control& fake) {
    using enum generated::StreamCode;
    using enum generated::TableCode;
    using enum generated::FieldCode;
    config.collect_poll_metrics = true;
    config.publisher_rate_owner = PublisherRateOwner::External;
    config.event_log = {};
    bool slow_observer = false;
    config.listener_event_log = [&](const auto& event) {
        if (slow_observer && event.stream_code == kFortsTradeRepl &&
            event.kind == cgate::Plaza2ListenerEventKind::TransactionCommit)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    fake.configure({});
    CgateSession session(config);
    require(!session.start(), "poll metrics regression startup");
    for (int i = 0; i < 10; ++i)
        require(!session.poll(false), "poll metrics regression bootstrap");
    slow_observer = true;
    fake.enqueue({.kind = test::fake::EventKind::Begin, .stream_code = kFortsTradeRepl});
    fake.enqueue({.stream_code = kFortsTradeRepl,
                  .table_code = kFortsTradeReplHeartbeat,
                  .revision = 100901,
                  .fields = {{.field_code = kFortsTradeReplHeartbeatReplId, .signed_value = 100901},
                             {.field_code = kFortsTradeReplHeartbeatReplRev, .signed_value = 100901},
                             {.field_code = kFortsTradeReplHeartbeatServerTime,
                              .kind = test::fake::FieldKind::Timestamp,
                              .unsigned_value = 1700000045}}});
    constexpr std::uint64_t moment_ns = 1699989345123456789ULL;
    fake.enqueue({.stream_code = kFortsTradeRepl,
                  .table_code = kFortsTradeReplOrdersLog,
                  .revision = 100902,
                  .fields = {{.field_code = kFortsTradeReplOrdersLogReplId, .signed_value = 100902},
                             {.field_code = kFortsTradeReplOrdersLogReplRev, .signed_value = 100902},
                             {.field_code = kFortsTradeReplOrdersLogMoment,
                              .kind = test::fake::FieldKind::Timestamp,
                              .unsigned_value = 1700000045},
                             {.field_code = kFortsTradeReplOrdersLogMomentNs,
                              .kind = test::fake::FieldKind::UnsignedInteger,
                              .unsigned_value = moment_ns}}});
    fake.enqueue({.kind = test::fake::EventKind::Commit, .stream_code = kFortsTradeRepl});
    const auto processes = fake.process_count();
    const auto sequence = session.last_poll_metrics().sequence;
    require(!session.poll(false), "measured callback poll");
    const auto& metrics = session.last_poll_metrics();
    require(metrics.sequence == sequence + 1 && metrics.process_calls == fake.process_count() - processes &&
                metrics.process_calls >= 3 && metrics.total_ns >= metrics.process_ns &&
                metrics.process_ns >= metrics.observer_ns && metrics.observer_ns >= 1000000 &&
                metrics.blocking_process_ns == 0 && metrics.started_utc_ns > 0 &&
                metrics.finished_utc_ns >= metrics.started_utc_ns,
            "slow observer was not attributed to the measured CGate drain");
    const auto event = [&](cgate::Plaza2ListenerEventKind kind, generated::TableCode table) {
        return std::find_if(metrics.events.begin(), metrics.events.end(), [&](const auto& row) {
            return row.stream_code == kFortsTradeRepl && row.table_code == table && row.kind == kind;
        });
    };
    const auto heartbeat = event(cgate::Plaza2ListenerEventKind::StreamData, kFortsTradeReplHeartbeat);
    const auto begin = event(cgate::Plaza2ListenerEventKind::TransactionBegin, cgate::kNoTableCode);
    const auto commit = event(cgate::Plaza2ListenerEventKind::TransactionCommit, cgate::kNoTableCode);
    require(heartbeat != metrics.events.end() && heartbeat->count == 1 && begin != metrics.events.end() &&
                begin->count == 1 && commit != metrics.events.end() && commit->count == 1 &&
                heartbeat->last_receipt_utc_ns >= metrics.started_utc_ns &&
                heartbeat->last_receipt_utc_ns <= metrics.finished_utc_ns &&
                heartbeat->timestamp_field == kFortsTradeReplHeartbeatServerTime &&
                heartbeat->exchange_timestamp_ns == 1699989245000000000ULL && metrics.unattributed_events == 0,
            "poll metrics lost stream/table/transaction attribution or nanosecond UTC clock evidence");
    const auto order = event(cgate::Plaza2ListenerEventKind::StreamData, kFortsTradeReplOrdersLog);
    require(order != metrics.events.end() && order->count == 1 &&
                order->timestamp_field == kFortsTradeReplOrdersLogMomentNs && order->exchange_timestamp_ns == moment_ns,
            "poll clock evidence preferred the rounded moment timestamp over native UTC moment_ns");
    auto request = test_support::make_add_order();
    request.isin_id = 1001;
    const auto result = session.post_command(Plaza2TradeCodec{}.encode(request), 880001);
    require(result.certainty == cgate::Plaza2SubmissionCertainty::Posted && result.post_invoked &&
                result.post_started_steady_ns > 0 && result.post_started_utc_ns > 0 &&
                result.post_finished_utc_ns >= result.post_started_utc_ns && result.post_duration_ns > 0,
            "measured publisher invocation lacks exact cg_pub_post boundary timestamps");
    slow_observer = false;
    require(!session.poll(false), "poll metrics reply pump");
    require(std::none_of(session.last_poll_metrics().events.begin(), session.last_poll_metrics().events.end(),
                         [](const auto& row) { return row.table_code == kFortsTradeReplHeartbeat; }),
            "poll metrics accumulated prior-poll rows");
    require(!session.stop(), "poll metrics regression stop");
    fake.configure({});
}
void private_trade_replay_regression(CgateSessionConfig config, const test::fake::Control& fake) {
    using enum generated::StreamCode;
    using enum generated::TableCode;
    using enum generated::FieldCode;
    auto now = std::chrono::steady_clock::time_point(std::chrono::hours(1));
    config.recovery_now = [&] { return now; };
    config.trade_replay_from_pos_anchor = true;
    config.read_only_market_data = true;
    config.allow_orders = false;
    config.publisher_settings.clear();
    config.p2mqreply_settings.clear();
    config.event_log = {};
    std::vector<std::int64_t> deal_revisions, heartbeat_revisions;
    bool retained_old_order = false;
    config.listener_event_log = [&](const auto& event) {
        if (event.kind != cgate::Plaza2ListenerEventKind::StreamData || event.stream_code != kFortsTradeRepl)
            return;
        if (event.table_code == kFortsTradeReplUserDeal)
            deal_revisions.push_back(event.signed_value);
        else if (event.table_code == kFortsTradeReplHeartbeat)
            heartbeat_revisions.push_back(event.signed_value);
        else if (event.table_code == kFortsTradeReplOrdersLog && event.signed_value == 9)
            retained_old_order = true;
    };
    fake.configure({});
    CgateSession session(config);
    require(!session.start(), "private TRADE replay regression start");
    const auto poll = [&] {
        now += std::chrono::seconds(1);
        require(!session.poll(), "private TRADE replay regression poll");
    };
    for (int i = 0; i < 20; ++i)
        poll();
    const auto anchor = session.trade_replay_anchor_used();
    require(anchor && anchor->trades_rev == 44 && anchor->orders_rev == 1001 && session.order_book_snapshot_ready(),
            "actual private-table revision keys did not open the anchored TRADE listener");
    require(deal_revisions.empty() && heartbeat_revisions.empty() && session.private_state().own_trades().empty(),
            "TRADE bootstrap delivered deal/heartbeat rows below the requested POS replay revision");
    const auto orders = session.private_state().own_orders();
    require(retained_old_order &&
                std::any_of(orders.begin(), orders.end(),
                            [](const auto& row) { return row.from_trade_repl && row.private_order_id == 20003; }),
            "order revision filter discarded current-LifeNum order history needed for reconstruction");
    fake.enqueue({.kind = test::fake::EventKind::Begin, .stream_code = kFortsTradeRepl});
    for (const std::int64_t revision : {43, 45}) {
        fake.enqueue(
            {.stream_code = kFortsTradeRepl,
             .table_code = kFortsTradeReplUserDeal,
             .revision = revision,
             .fields = {
                 {.field_code = kFortsTradeReplUserDealReplId, .signed_value = revision},
                 {.field_code = kFortsTradeReplUserDealReplRev, .signed_value = revision},
                 {.field_code = kFortsTradeReplUserDealIdDeal, .signed_value = 600000 + revision},
                 {.field_code = kFortsTradeReplUserDealSessId, .signed_value = 321},
                 {.field_code = kFortsTradeReplUserDealIsinId, .signed_value = 1001},
                 {.field_code = kFortsTradeReplUserDealPrice, .kind = test::fake::FieldKind::Text, .text = "102500"},
                 {.field_code = kFortsTradeReplUserDealXamount, .signed_value = 2},
                 {.field_code = kFortsTradeReplUserDealCodeBuy,
                  .kind = test::fake::FieldKind::Text,
                  .text = "BRK1C01"}}});
        fake.enqueue({.stream_code = kFortsTradeRepl,
                      .table_code = kFortsTradeReplHeartbeat,
                      .revision = revision,
                      .fields = {{.field_code = kFortsTradeReplHeartbeatReplId, .signed_value = revision},
                                 {.field_code = kFortsTradeReplHeartbeatReplRev, .signed_value = revision},
                                 {.field_code = kFortsTradeReplHeartbeatServerTime,
                                  .kind = test::fake::FieldKind::Timestamp,
                                  .unsigned_value = static_cast<std::uint64_t>(1700000000 + revision)}}});
    }
    fake.enqueue({.kind = test::fake::EventKind::Commit, .stream_code = kFortsTradeRepl});
    poll();
    require(deal_revisions == std::vector<std::int64_t>{45} && heartbeat_revisions == std::vector<std::int64_t>{45},
            "native callbacks did not enforce both actual private-table replay floors");
    const auto trades = session.private_state().own_trades();
    const auto health = session.private_state().stream_health();
    const auto trade_health =
        std::find_if(health.begin(), health.end(), [](const auto& row) { return row.stream_code == kFortsTradeRepl; });
    require(trades.size() == 1,
            "private projection did not retain exactly one above-floor fill: " + std::to_string(trades.size()));
    require(trades.front().id_deal == 600045 && trades.front().repl_rev == 45 && trades.front().trade_lifenum == 7,
            "above-floor fill lost native identity, revision or epoch provenance");
    require(trade_health != health.end(), "private projection omitted TRADE health");
    // The fake encodes this value as CGate Moscow wall time; the projector stores UTC.
    require(trade_health->last_server_time == 1700000045 - 3 * 60 * 60,
            "private projection lost above-floor heartbeat time: " + std::to_string(trade_health->last_server_time));
    const auto native = session.publisher_call_counts();
    require(native.msgnew == 0 && native.post == 0 && fake.commands().empty(),
            "read-only private TRADE replay regression invoked a publisher");
    require(!session.stop(), "private TRADE replay regression stop");
    fake.configure({});
}
void requested_trade_epoch_regression(CgateSessionConfig config, const test::fake::Control& fake) {
    using enum generated::StreamCode;
    using enum generated::TableCode;
    using enum generated::FieldCode;
    auto now = std::chrono::steady_clock::time_point(std::chrono::hours(1));
    config.recovery_now = [&] { return now; };
    config.trade_replay_from_pos_anchor = true;
    config.read_only_market_data = true;
    config.allow_orders = false;
    config.publisher_settings.clear();
    config.p2mqreply_settings.clear();
    config.event_log = {};
    std::vector<cgate::Plaza2ListenerEventKind> bootstrap_events;
    std::size_t native_lifenum_count = 0;
    config.listener_event_log = [&](const auto& event) {
        if (event.stream_code != kFortsTradeRepl)
            return;
        if (event.kind == cgate::Plaza2ListenerEventKind::LifeNum) {
            ++native_lifenum_count;
            bootstrap_events.push_back(event.kind);
        } else if (event.kind == cgate::Plaza2ListenerEventKind::Open) {
            bootstrap_events.push_back(event.kind);
        }
    };
    const auto poll = [&](CgateSession& session) {
        now += std::chrono::seconds(1);
        require(!session.poll(), "requested TRADE epoch regression poll");
    };
    const auto warm = [&](CgateSession& session) {
        for (int i = 0; i < 20; ++i)
            poll(session);
    };
    const auto deal = [&] {
        fake.enqueue({.kind = test::fake::EventKind::Begin, .stream_code = kFortsTradeRepl});
        fake.enqueue(
            {.stream_code = kFortsTradeRepl,
             .table_code = kFortsTradeReplUserDeal,
             .revision = 100901,
             .fields = {
                 {.field_code = kFortsTradeReplUserDealReplId, .signed_value = 100901},
                 {.field_code = kFortsTradeReplUserDealReplRev, .signed_value = 100901},
                 {.field_code = kFortsTradeReplUserDealIdDeal, .signed_value = 600901},
                 {.field_code = kFortsTradeReplUserDealSessId, .signed_value = 321},
                 {.field_code = kFortsTradeReplUserDealIsinId, .signed_value = 1001},
                 {.field_code = kFortsTradeReplUserDealPrice, .kind = test::fake::FieldKind::Text, .text = "102500"},
                 {.field_code = kFortsTradeReplUserDealXamount, .signed_value = 2},
                 {.field_code = kFortsTradeReplUserDealPrivateOrderIdBuy, .signed_value = 500901},
                 {.field_code = kFortsTradeReplUserDealCodeBuy,
                  .kind = test::fake::FieldKind::Text,
                  .text = "BRK1C01"}}});
        fake.enqueue({.kind = test::fake::EventKind::Commit, .stream_code = kFortsTradeRepl});
    };
    fake.configure({});
    {
        CgateSession session(config);
        require(!session.start(), "requested TRADE epoch regression start");
        warm(session);
        require(native_lifenum_count == 0, "equal requested/server TRADE epoch unexpectedly emitted LifeNum");
        require(session.private_state().stream_lifenum(kFortsTradeRepl) == 7 && session.order_book_snapshot_ready(),
                "OPEN without LifeNum did not adopt the supplied TRADE epoch");
        deal();
        poll(session);
        const auto trades = session.private_state().own_trades();
        const auto found =
            std::find_if(trades.begin(), trades.end(), [](const auto& row) { return row.id_deal == 600901; });
        require(found != trades.end() && found->repl_rev == 100901 && found->trade_lifenum == 7,
                "first committed own fill lacks the supplied TRADE epoch provenance");
        static_cast<void>(session.take_private_row_changes());
        const auto opens = fake.opens(kFortsTradeRepl);
        fake.enqueue({.kind = test::fake::EventKind::Close, .stream_code = kFortsTradeRepl});
        poll(session);
        require(!session.order_book_snapshot_ready(), "TRADE CLOSE retained reconstruction readiness");
        warm(session);
        require(fake.opens(kFortsTradeRepl) == opens + 1 && native_lifenum_count == 0 &&
                    session.private_state().stream_lifenum(kFortsTradeRepl) == 7 && session.order_book_snapshot_ready(),
                "same-epoch TRADE reopen required a LifeNum callback");
        const auto reopened = session.take_private_row_changes();
        require(!reopened.trade_history_truncated && !reopened.regular_trade_history_truncated &&
                    !reopened.regular_trade_history_reloaded,
                "same-epoch OPEN fallback manufactured a history loss");
        deal();
        poll(session);
        static_cast<void>(session.take_private_row_changes());
        fake.enqueue({.kind = test::fake::EventKind::LifeNum, .stream_code = kFortsTradeRepl, .value = 8});
        poll(session);
        const auto changed = session.take_private_row_changes();
        require(session.private_state().stream_lifenum(kFortsTradeRepl) == 8 &&
                    session.private_state().own_trades().empty() && !session.order_book_snapshot_ready() &&
                    changed.regular_trade_history_truncated && changed.regular_trade_history_reloaded,
                "explicit changed TRADE LifeNum did not supersede supplied epoch and clear history");
        require(!session.stop(), "requested TRADE epoch regression stop");
    }
    bootstrap_events.clear();
    native_lifenum_count = 0;
    fake.configure({.trade_server_lifenum = 8});
    {
        CgateSession changed(config);
        require(!changed.start(), "server TRADE epoch regression start");
        warm(changed);
        require(bootstrap_events.size() >= 2 && bootstrap_events[0] == cgate::Plaza2ListenerEventKind::LifeNum &&
                    bootstrap_events[1] == cgate::Plaza2ListenerEventKind::Open,
                "different server TRADE LifeNum must precede OPEN");
        require(changed.private_state().stream_lifenum(kFortsTradeRepl) == 8 && !changed.order_book_snapshot_ready(),
                "OPEN fallback overwrote explicit server TRADE epoch with stale requested epoch");
        require(!changed.stop(), "server TRADE epoch regression stop");
    }
    bootstrap_events.clear();
    fake.configure({.trade_server_lifenum = 6});
    {
        CgateSession newer_request(config);
        require(!newer_request.start(), "newer requested TRADE epoch regression start");
        warm(newer_request);
        require(bootstrap_events.empty() && !newer_request.private_state().stream_lifenum(kFortsTradeRepl) &&
                    !newer_request.order_book_snapshot_ready(),
                "rejected future TRADE epoch was adopted without OPEN");
        require(!newer_request.stop(), "newer requested TRADE epoch regression stop");
    }
    require(fake.commands().empty(), "read-only epoch regression posted a command");
    fake.configure({});
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 2, "fake runtime path required");
        const auto root = test::make_temp_directory("cgate-recovery");
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cgate::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA9.9.0", "9.9", "T1"));
        moex::plaza2::test::fake::Control fake(fixture.library_path);
        const auto flag = [&](moex::plaza2::test::fake::Option key, bool on) { fake.set(key, on ? "1" : ""); };
        void* dso = ::dlopen(fixture.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        require(dso, "load fixture counters");
        auto reset = reinterpret_cast<void (*)()>(::dlsym(dso, "moex_fake_reset_publisher_counts"));
        reset();
        auto envs = reinterpret_cast<std::uint64_t (*)()>(::dlsym(dso, "moex_fake_environment_open_count"));
        auto conns = reinterpret_cast<std::uint64_t (*)()>(::dlsym(dso, "moex_fake_connection_new_count"));
        auto now = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
        CgateSessionConfig config;
        config.runtime.runtime_root = fixture.root;
        config.runtime.env_open_settings = "ini=config/t1.ini;key=00000000";
        // Hash changes are logged and do not reject compatible runtimes.
        config.runtime.expected_runtime_library_sha256 = "wrong";
        config.runtime.expected_scheme_sha256 = "wrong";
        config.connection_settings = "p2tcp://127.0.0.1:4101;app_name=recovery";
        config.publisher_settings = "p2mq://FORTS_SRV;category=FORTS_MSG;name=PUB";
        config.allow_orders = true;
        config.process_timeout_ms = 0;
        config.recovery_now = [&] { return now; };
        std::size_t opening_transitions = 0;
        config.event_log = [&](std::string_view kind, std::string_view fields) {
            if (kind == "cgate_state" && fields.find("\"object\":\"connection\"") != std::string_view::npos &&
                fields.find("\"to\":2") != std::string_view::npos)
                ++opening_transitions;
            require(fields.find("key=") == std::string_view::npos, "CGate interaction log disclosed settings");
        };
        for (auto code : {generated::StreamCode::kFortsTradeRepl, generated::StreamCode::kFortsUserorderbookRepl,
                          generated::StreamCode::kFortsPosRepl, generated::StreamCode::kFortsPartRepl,
                          generated::StreamCode::kFortsRefdataRepl}) {
            auto descriptor = generated::FindStreamByCode(code);
            config.private_streams.push_back(
                {code, "p2repl://" + std::string(descriptor->stream_name), "mode=snapshot+online"});
        }
        config.aggr20_stream = {generated::StreamCode::kFortsAggrRepl, "p2repl://FORTS_AGGR20_REPL",
                                "mode=snapshot+online"};
        const auto absolute_logfile = "logfile=" + (root / "CGate-client.log").string() + "\n";
        for (const bool read_only : {false, true}) {
            auto live_config = config;
            live_config.mode = CgateSessionMode::Live;
            live_config.read_only_market_data = read_only;
            if (read_only) {
                live_config.allow_orders = false;
                live_config.publisher_settings.clear();
            }
            test::write_text_file(fixture.config_dir / "t1.ini",
                                  "[cgate]\n# log=p2:p2syslog\n[p2syslog]\n" + absolute_logfile);
            CgateSession invalid_logging(live_config);
            require(invalid_logging.start().code == cgate::Plaza2ErrorCode::InvalidConfiguration,
                    "Live session accepted disabled native logging");
            require(envs() == 0, "Live logging validation ran after opening native environment");
            test::write_text_file(fixture.config_dir / "t1.ini",
                                  "[cgate]\nlog=p2:p2syslog\n[p2syslog]\n" + absolute_logfile);
            CgateSession valid_logging(live_config);
            require(!valid_logging.start(), "Live session rejected valid native logging");
            require(!valid_logging.stop(), "Live logging session stop");
            reset();
        }
        {
            struct LoggingCase {
                std::string name, ini, env;
                bool valid{};
            };
            const std::vector<LoggingCase> cases{
                {"env_log_default_severity", "[p2syslog]\n" + absolute_logfile, ";log=p2:p2syslog", true},
                {"env_log_debug", "[p2syslog]\n" + absolute_logfile, ";log=p2:p2syslog;minloglevel=debug", true},
                {"unrelated_application_options",
                 "[p2syslog]\n" + absolute_logfile + "[application]\nlogfile=relative.log\nminloglevel=error\n",
                 ";log=p2:p2syslog", true},
                {"missing_logfile", "[cgate]\nlog=p2:p2syslog\n[p2syslog]\n", "", false},
                {"relative_logfile", "[cgate]\nlog=p2:p2syslog\n[p2syslog]\nlogfile=CGate-client.log\n", "", false},
                {"empty_logfile", "[cgate]\nlog=p2:p2syslog\n[p2syslog]\nlogfile=\n", "", false},
                {"nul_sink", "[p2syslog]\nlogfile=nul\n", ";log=p2:p2syslog", false},
                {"null_sink", "[p2syslog]\nlogfile=null\n", ";log=p2:p2syslog", false},
                {"dev_null_sink", "[p2syslog]\nlogfile=/dev/null\n", ";log=p2:p2syslog", false},
                {"commented_sink", ";[p2syslog]\n" + absolute_logfile, ";log=p2:p2syslog", false},
                {"disabled_env_log", "[p2syslog]\n" + absolute_logfile, ";log=null", false},
                {"reduced_env_severity", "[p2syslog]\n" + absolute_logfile, ";log=p2:p2syslog;minloglevel=error",
                 false},
            };
            std::vector<std::string> failures;
            for (const bool read_only : {false, true}) {
                for (const auto& example : cases) {
                    auto live_config = config;
                    live_config.mode = CgateSessionMode::Live;
                    live_config.read_only_market_data = read_only;
                    live_config.runtime.env_open_settings += example.env;
                    if (read_only) {
                        live_config.allow_orders = false;
                        live_config.publisher_settings.clear();
                    }
                    test::write_text_file(fixture.config_dir / "t1.ini", example.ini);
                    const auto opens_before = envs();
                    CgateSession logging(live_config);
                    const auto error = logging.start();
                    if (example.valid ? bool(error) : error.code != cgate::Plaza2ErrorCode::InvalidConfiguration)
                        failures.push_back(example.name);
                    if (!example.valid && envs() != opens_before)
                        failures.push_back(example.name + "_opened_native_environment");
                    require(!logging.stop(), "logging matrix session stop");
                    reset();
                }
            }
            for (const auto& failure : failures)
                std::cerr << "CGate logging validation mismatch: " << failure << '\n';
            require(failures.empty(), "Live startup must accept env logging and require an absolute enabled logfile");
            test::write_text_file(fixture.config_dir / "t1.ini",
                                  "[cgate]\nlog=p2:p2syslog\n[p2syslog]\n" + absolute_logfile);
        }
        {
            auto closed_config = config;
            closed_config.process_timeout_ms = 10;
            fake.set(test::fake::Option::ConnectionOpenResult, "131076");
            CgateSession closed_session(closed_config);
            require(!closed_session.start(), "closed retry session start");
            const auto waiting_started = std::chrono::steady_clock::now();
            for (int i = 0; i < 4; ++i)
                require(!closed_session.poll(), "CLOSED retry pump failed");
            require(std::chrono::steady_clock::now() - waiting_started >= std::chrono::milliseconds(7),
                    "CLOSED recovery polls busy-spin instead of waiting");
            require(closed_session.recovery_status().attempts == 0,
                    "CLOSED wait retried before the one-second recovery boundary");
            require(!closed_session.stop(), "closed retry session stop");
            fake.clear(test::fake::Option::ConnectionOpenResult);
            reset();
        }
        {
            // Even process_timeout_ms=0 must yield when no connection can process data.
            fake.set(test::fake::Option::ConnectionOpenResult, "131076");
            CgateSession nonblocking(config);
            require(!nonblocking.start(), "nonblocking outage session start");
            const auto waiting_started = std::chrono::steady_clock::now();
            require(!nonblocking.poll(), "nonblocking outage poll failed");
            require(std::chrono::steady_clock::now() - waiting_started >= std::chrono::milliseconds(1),
                    "zero process timeout busy-spins during an outage");
            require(!nonblocking.stop(), "nonblocking outage session stop");
            fake.clear(test::fake::Option::ConnectionOpenResult);
            reset();
        }
        {
            auto error_config = config;
            error_config.process_timeout_ms = 10;
            flag(test::fake::Option::ConnectionOpenFail, true);
            CgateSession errored(error_config);
            require(!errored.start(), "ERROR recovery session start");
            std::chrono::steady_clock::duration error_wait{};
            for (int i = 0; i < 4; ++i) {
                const auto waiting_started = std::chrono::steady_clock::now();
                require(!errored.poll(), "ERROR recovery poll became terminal");
                error_wait += std::chrono::steady_clock::now() - waiting_started;
                if (i < 3) {
                    now += std::chrono::seconds(1);
                    require(!errored.poll(), "scheduled ERROR reconnect became terminal");
                }
            }
            require(error_wait >= std::chrono::milliseconds(7), "ERROR recovery polls busy-spin instead of waiting");
            require(errored.recovery_status().attempts == 3, "ERROR pacing changed the one-second retry schedule");
            flag(test::fake::Option::ConnectionOpenFail, false);
            now += std::chrono::seconds(1);
            for (int i = 0; i < 10; ++i) {
                now += std::chrono::seconds(1);
                require(!errored.poll(), "ERROR recovery failed");
            }
            require(errored.runtime_health().private_active,
                    "ERROR did not recover to ONLINE: " + errored.last_callback_error());
            const auto retries = errored.recovery_status().attempts;
            flag(test::fake::Option::ConnGetstateInternal, true);
            const auto query_started = std::chrono::steady_clock::now();
            for (int i = 0; i < 4; ++i)
                require(!errored.poll(), "connection state-query error became terminal");
            require(std::chrono::steady_clock::now() - query_started >= std::chrono::milliseconds(7),
                    "failed connection state-query polls busy-spin instead of waiting");
            require(errored.recovery_status().attempts == retries, "state-query wait changed the retry schedule");
            flag(test::fake::Option::ConnGetstateInternal, false);
            require(!errored.poll(), "state-query recovery failed");
            require(errored.runtime_health().private_active, "state-query error did not recover");
            require(!errored.stop(), "ERROR recovery session stop");
            reset();
        }
        {
            struct StderrCapture {
                std::ostringstream text;
                std::streambuf* previous{std::cerr.rdbuf(text.rdbuf())};
                ~StderrCapture() {
                    std::cerr.rdbuf(previous);
                }
            } stderr_capture;
            std::size_t key_check_alerts = 0;
            auto key_config = config;
            key_config.event_log = [&](std::string_view kind, std::string_view fields) {
                if (kind == "cgate_key_check_failed") {
                    ++key_check_alerts;
                    require(fields.find("131074") != std::string_view::npos, "key-check alert lost native error code");
                }
                require(fields.find("00000000") == std::string_view::npos,
                        "key-check alert disclosed the configured software key");
            };
            fake.set(test::fake::Option::ConnectionOpenResult, "131074");
            CgateSession key_failure(key_config);
            require(!key_failure.start(), "key verification failure stopped C5 recovery");
            require(key_check_alerts == 1, "unsupported connection did not emit key-check alert immediately");
            require(stderr_capture.text.str().find("cgate_key_check_failed") != std::string::npos,
                    "key-check failure was silent on operator stderr");
            require(key_failure.recovery_status().key_check_failed, "key-check failure missing from recovery status");
            for (int i = 0; i < 59; ++i) {
                now += std::chrono::seconds(1);
                require(!key_failure.poll(), "key-check retry became terminal");
            }
            require(key_check_alerts == 1 && key_failure.recovery_status().attempts == 59,
                    "key-check warning throttle changed C5 retries or repeated before one minute");
            now += std::chrono::seconds(1);
            require(!key_failure.poll(), "one-minute key-check retry failed");
            require(key_check_alerts == 2, "persistent key-check failure did not repeat at one minute");
            for (int i = 0; i < 10; ++i)
                require(!key_failure.poll(), "same-time key-check poll failed");
            require(key_check_alerts == 2, "same-time key-check polls repeated the warning");
            fake.clear(test::fake::Option::ConnectionOpenResult);
            for (int i = 0; i < 10; ++i) {
                now += std::chrono::seconds(1);
                require(!key_failure.poll(), "key-check recovery pump failed");
            }
            require(key_failure.runtime_health().private_active && !key_failure.recovery_status().key_check_failed,
                    "successful ACTIVE recovery retained key-check failure");
            flag(test::fake::Option::ConnectionError, true);
            require(!key_failure.poll(), "new key-check episode close failed");
            flag(test::fake::Option::ConnectionError, false);
            fake.set(test::fake::Option::ConnectionOpenResult, "131074");
            now += std::chrono::seconds(1);
            require(!key_failure.poll(), "new key-check episode retry failed");
            require(key_check_alerts == 3 && key_failure.recovery_status().key_check_failed,
                    "new key-check failure after recovery did not warn immediately");
            const auto warnings = stderr_capture.text.str();
            std::size_t warning_count = 0, at = 0;
            while ((at = warnings.find("cgate_key_check_failed", at)) != std::string::npos) {
                ++warning_count;
                at += std::string_view("cgate_key_check_failed").size();
            }
            require(warning_count == key_check_alerts && warnings.find("00000000") == std::string::npos,
                    "operator key-check warnings were not throttled with the journal or leaked the key");
            require(!key_failure.stop(), "key failure stop");
            fake.clear(test::fake::Option::ConnectionOpenResult);
            fake.set(test::fake::Option::ConnectionOpenResult, "131075");
            CgateSession other_open_error(key_config);
            require(!other_open_error.start(), "other connection failure stopped recovery");
            require(key_check_alerts == 3 && !other_open_error.recovery_status().key_check_failed,
                    "ordinary connection timeout was misreported as a key-check failure");
            require(!other_open_error.stop(), "ordinary open failure stop");
            fake.clear(test::fake::Option::ConnectionOpenResult);
            reset();
        }
        private_trade_replay_regression(config, fake);
        requested_trade_epoch_regression(config, fake);
        poll_metrics_regression(config, fake);
        instrument_session_post_regression(config, fake);
        test::publisher_prewarm_regression(config, fake, fixture.library_path);
        test::private_pos_anchor_regression(config, fake);
        reset();
        flag(moex::plaza2::test::fake::Option::ConnHoldOpening, true);
        CgateSession session(config);
        const auto start_process_calls = fake.process_count();
        require(!session.start(), "nonblocking start");
        // Synchronous runtime identity hashing is unrelated to ACTIVE waiting.
        // The native connection deliberately stays OPENING until later polls.
        require(session.runtime_health().connection == 2 && fake.process_count() == start_process_calls,
                "start waited or processed CGate while ACTIVE was withheld");
        for (int i = 0; i < 600; ++i) {
            now += std::chrono::seconds(1);
            require(!session.poll(), "10 minute OPENING pump failed");
        }
        require(session.started() && session.recovery_status().operation != Plaza2SessionOperation::Failed,
                "OPENING became Failed");
        require(opening_transitions == 1, "unchanged OPENING state produced duplicate state transitions");
        flag(moex::plaza2::test::fake::Option::ConnHoldOpening, false);
        const auto pump = [&] {
            for (int i = 0; i < 10; ++i) {
                now += std::chrono::seconds(1);
                require(!session.poll(), "pump failed");
            }
        };
        pump();
        if (!session.runtime_health().private_active) {
            auto h = session.runtime_health();
            std::cerr << "callback: " << session.last_callback_error() << " conn=" << h.connection
                      << " private=" << h.private_count << "\n";
            for (std::size_t i = 0; i < h.private_count; ++i)
                std::cerr << static_cast<int>(h.private_streams[i]) << ":" << h.private_states[i] << "\n";
        }
        require(session.runtime_health().private_active, "listeners did not become ONLINE after delayed ACTIVE");
        auto request = test_support::make_add_order();
        request.isin_id = 1001;
        auto command = Plaza2TradeCodec{}.encode(request);
        require(session.post_command(command, 101).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "order blocked after delayed ACTIVE");
        flag(moex::plaza2::test::fake::Option::ConnToOpening, true);
        pump();
        for (int i = 0; i < 300; ++i) {
            now += std::chrono::seconds(1);
            require(!session.poll(), "5 minute outage failed");
        }
        flag(moex::plaza2::test::fake::Option::ConnToOpening, false);
        pump();
        if (!session.runtime_health().private_active) {
            std::cerr << "resume callback: " << session.last_callback_error() << "\n";
            for (auto& health : session.private_state().stream_health())
                std::cerr << health.stream_name << " " << health.online << " " << health.snapshot_complete << "\n";
        }
        require(session.runtime_health().private_active, "private streams did not resume after ACTIVE to OPENING");
        flag(moex::plaza2::test::fake::Option::ConnectionError, true);
        require(!session.poll(), "asynchronous connection ERROR terminal");
        flag(moex::plaza2::test::fake::Option::ConnectionError, false);
        pump();
        require(session.runtime_health().private_active, "asynchronous connection ERROR did not recover");
        flag(moex::plaza2::test::fake::Option::ConnDirectClosed, true);
        require(!session.poll(), "direct connection CLOSED terminal");
        require(!session.runtime_health().private_active, "direct connection CLOSED retained old private ONLINE");
        require(session.post_command(command, 102).certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent,
                "direct connection CLOSED allowed command");
        flag(moex::plaza2::test::fake::Option::ConnDirectClosed, false);
        pump();
        require(session.runtime_health().private_active, "direct connection CLOSED did not rebuild snapshots");
        require(envs() == 1 && conns() == 1, "recovery recreated environment or connection");
        flag(moex::plaza2::test::fake::Option::CallbackCorruption, true);
        require(!session.poll(), "malformed private row terminal");
        flag(moex::plaza2::test::fake::Option::CallbackCorruption, false);
        pump();
        require(session.runtime_health().private_active, "private callback anomaly did not recover");
        // Idle liveness has no age or connection bootstrap deadline.
        for (int i = 0; i < 300; ++i) {
            now += std::chrono::seconds(1);
            require(!session.poll(), "idle poll failed");
        }
        require(session.runtime_health().private_active, "quiet private streams became stale");
        // An always-readable runtime must yield to the owning command/kill path.
        fake.configure({.continuous_input = true});
        const auto calls = fake.process_count();
        const auto busy_start = std::chrono::steady_clock::now();
        require(!session.poll(), "continuous flow poll failed");
        require(std::chrono::steady_clock::now() - busy_start < std::chrono::milliseconds(100),
                "continuous flow starved owner loop");
        require(fake.process_count() - calls <= 100, "continuous flow exceeded drain call budget");
        auto cancel = Plaza2TradeCodec{}.encode(test_support::make_del_order());
        require(session.post_command(cancel, 103).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "continuous flow starved risk-reduction command");
        fake.configure({});

        require(!session.stop(), "stop");
        flag(moex::plaza2::test::fake::Option::SchemeExtraTable, true);
        flag(moex::plaza2::test::fake::Option::CgateClearDeletedInsideTransaction, true);
        flag(moex::plaza2::test::fake::Option::TimestampMilliseconds, true);
        std::size_t unknown_clears = 0;
        bool precise_time = false, moscow_time = false;
        config.listener_event_log = [&](const cgate::Plaza2ListenerEvent& event) {
            unknown_clears += event.kind == cgate::Plaza2ListenerEventKind::ClearDeleted;
            for (const auto& field : event.fields) {
                if (field.field_code == generated::FieldCode::kFortsRefdataReplSessionBegin)
                    moscow_time = field.unsigned_value == 1699989200ULL;
                if (field.kind == cgate::Plaza2DecodedValueKind::Timestamp && field.unsigned_value > 1000000000)
                    precise_time |= field.timestamp_ns == field.unsigned_value * 1000000000ULL + 321000000ULL;
            }
        };
        CgateSession additions(config);
        require(!additions.start(), "start additive schema");
        for (int i = 0; i < 10; ++i) {
            now += std::chrono::seconds(1);
            require(!additions.poll(), "additive schema rejected");
        }
        require(additions.runtime_health().private_active, "extra table field caused stream failure");
        require(unknown_clears == 0, "unknown ClearDeleted index mapped onto compacted known table plan");
        require(precise_time, "CGate timestamp fractional milliseconds lost");
        require(moscow_time, "CGate Moscow wall time was not converted to UTC");
        require(!additions.stop(), "stop additions");
        flag(moex::plaza2::test::fake::Option::CgateClearDeletedInsideTransaction, false);
        flag(moex::plaza2::test::fake::Option::TimestampMilliseconds, false);
        config.listener_event_log = {};
        flag(moex::plaza2::test::fake::Option::SchemeExtraTable, false);
        for (auto mutation : {moex::plaza2::test::fake::Option::SchemeMissingPrice,
                              moex::plaza2::test::fake::Option::SchemeRetypePrice}) {
            flag(mutation, true);
            CgateSession incompatible(config);
            require(!incompatible.start(), "bad stream schema should keep session up");
            for (int i = 0; i < 6; ++i) {
                now += std::chrono::seconds(1);
                require(!incompatible.poll(), "incompatible listener killed session");
            }
            require(incompatible.runtime_health().private_active, "incompatible AGGR affected private streams");
            require(incompatible.last_callback_error().find("INCOMPATIBLE_SCHEME orders_aggr.price") !=
                        std::string::npos,
                    "missing clear scheme diagnostic");
            const auto initial_opens = fake.opens(generated::StreamCode::kFortsAggrRepl);
            for (int i = 0; i < 600; ++i) {
                now += std::chrono::seconds(1);
                require(!incompatible.poll(), "latched scheme pump");
            }
            require(fake.opens(generated::StreamCode::kFortsAggrRepl) == initial_opens,
                    "incompatible scheme reopened without new connection generation");
            flag(moex::plaza2::test::fake::Option::ConnectionError, true);
            require(!incompatible.poll(), "latched listener reconnect");
            flag(moex::plaza2::test::fake::Option::ConnectionError, false);
            for (int i = 0; i < 10; ++i) {
                now += std::chrono::seconds(1);
                require(!incompatible.poll(), "latched listener reconnect pump");
            }
            require(fake.opens(generated::StreamCode::kFortsAggrRepl) == initial_opens + 1,
                    "connection restart did not recheck latched scheme exactly once");
            require(!incompatible.stop(), "stop incompatible");
            flag(mutation, false);
        }
        test::fake::Scenario unrecognized_schema;
        unrecognized_schema.unrecognized_schema_stream = generated::StreamCode::kFortsAggrRepl;
        fake.configure(unrecognized_schema);
        CgateSession missing_tables(config);
        require(!missing_tables.start(), "unknown AGGR tables session start");
        for (int i = 0; i < 6; ++i) {
            now += std::chrono::seconds(1);
            require(!missing_tables.poll(), "unknown AGGR tables bootstrap");
        }
        require(missing_tables.runtime_health().private_active, "unknown AGGR tables affected private streams");
        const auto missing_table_opens = fake.opens(generated::StreamCode::kFortsAggrRepl);
        for (int i = 0; i < 600; ++i) {
            now += std::chrono::seconds(1);
            require(!missing_tables.poll(), "unknown AGGR tables pump");
        }
        require(fake.opens(generated::StreamCode::kFortsAggrRepl) == missing_table_opens,
                "unrecognized required tables reopened repeatedly");
        require(!missing_tables.stop(), "unknown AGGR tables stop");
        fake.configure({});
        // A terminal create-time configuration error is never retried.
        for (bool listener : {true, false}) {
            test::fake::Scenario invalid_configuration;
            (listener ? invalid_configuration.listener_create_result : invalid_configuration.publisher_create_result) =
                131073;
            fake.configure(invalid_configuration);
            CgateSession invalid_session(config);
            require(!invalid_session.start(), "configured create errors arise on poll");
            require(static_cast<bool>(invalid_session.poll()), "create INVALIDARGUMENT retried as recoverable");
            require(invalid_session.recovery_status().operation == Plaza2SessionOperation::Failed,
                    "create INVALIDARGUMENT did not latch Failed");
            for (int i = 0; i < 10; ++i) {
                now += std::chrono::seconds(1);
                require(static_cast<bool>(invalid_session.poll()), "terminal create error was cleared");
            }
            require(!invalid_session.stop(), "invalid session stop");
            fake.configure({});
        }
        config.publisher_messages_per_second = 1;
        CgateSession limited(config);
        require(!limited.start(), "standalone rate session start");
        for (int i = 0; i < 10; ++i) {
            now += std::chrono::seconds(1);
            require(!limited.poll(), "standalone rate warmup");
        }
        require(limited.post_command(command, 201).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "standalone first post");
        require(limited.post_command(command, 202).certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent,
                "standalone rate protection missing");
        require(!limited.stop(), "standalone stop");
        for (const auto owner : {PublisherRateOwner::Session, PublisherRateOwner::External}) {
            test::fake::Scenario scenario;
            scenario.suppress_auto_replies = true;
            fake.configure(scenario);
            auto late_config = config;
            late_config.publisher_rate_owner = owner;
            late_config.reply_timeout_ms = 10;
            CgateSession late(late_config);
            require(!late.start(), "late reply tracking session start");
            for (int i = 0; i < 10; ++i) {
                now += std::chrono::seconds(1);
                require(!late.poll(), "late reply tracking warmup");
            }
            auto move = test_support::make_move_order();
            move.isin_id = 1001;
            move.regime = 3;
            move.order_id2 = 0;
            move.amount2 = 0;
            move.price1 = "103000";
            const auto encoded = Plaza2TradeCodec{}.encode(move);
            require(encoded.validation.ok(), "late Move fixture codec validation");
            require(late.post_command(encoded, 401).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                    "late Move fixture post");
            now += std::chrono::milliseconds(20);
            require(!late.poll(), "late Move tracking expiry");
            require(late.take_reply_events().empty(), "tracking expiry manufactured a native timeout");
            official_cgate99::FORTS_MSG176 reply{};
            reply.order_id1 = 64001;
            std::vector<std::byte> payload(sizeof(reply));
            std::memcpy(payload.data(), &reply, payload.size());
            fake.enqueue({.kind = test::fake::EventKind::Reply, .message_id = 176, .user_id = 401, .payload = payload});
            require(!late.poll(), "expired correlation late176 pump");
            const auto replies = late.take_reply_events();
            if (owner == PublisherRateOwner::External) {
                require(replies.size() == 1 && replies.front().user_id == 401 && replies.front().message_id == 176 &&
                            replies.front().raw_payload == payload,
                        "external owner lost176 after session correlation expired");
                fake.enqueue({.kind = test::fake::EventKind::Reply,
                              .message_id = 99,
                              .user_id = 499,
                              .payload = {std::byte{1}, std::byte{2}, std::byte{3}}});
                require(!late.poll(), "unknown malformed99 pump");
                const auto malformed = late.take_reply_events();
                require(malformed.empty() && late.runtime_health().reply == 1,
                        "unknown malformed99 bypassed existing flood validation");
            } else
                require(replies.empty(), "standalone session forwarded an unknown late reply");
            require(!late.stop(), "late reply tracking session stop");
        }
        fake.configure({});
        config.publisher_rate_owner = PublisherRateOwner::External;
        CgateSession external(config);
        require(!external.start(), "external rate session start");
        for (int i = 0; i < 10; ++i) {
            now += std::chrono::seconds(1);
            require(!external.poll(), "external rate warmup");
        }
        fake.set(test::fake::Option::PubReplyFamily, "99");
        require(external.post_command(command, 301).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "external first post");
        require(external.post_command(command, 302).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "duplicate transport limiter rejected queue-admitted command");
        require(!external.poll(), "external flood reply pump");
        require(external.take_reply_events().size() == 2, "99 replies were not forwarded to external rate owner");
        require(external.publisher_rate_metrics().penalty_until_ms == 0,
                "external rate owner received duplicate transport penalty");
        fake.set(test::fake::Option::PubReplyMalformed);
        require(external.post_command(command, 303).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "transport blocked externally-owned 99 recovery queue");
        require(!external.poll(), "malformed99 pump");
        const auto malformed = external.take_reply_events();
        require(malformed.size() == 1 && malformed.front().user_id == 303 && malformed.front().raw_payload.size() == 3,
                "malformed99 lost terminal correlation event");
        fake.clear(test::fake::Option::PubReplyMalformed);
        fake.clear(test::fake::Option::PubReplyFamily);
        for (int i = 0; i < 3; ++i) {
            now += std::chrono::seconds(1);
            require(!external.poll(), "malformed99 reply listener recovery");
        }
        require(external.post_command(command, 303).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "malformed99 correlation leaked until timeout");
        // Committed halted instrument blocks Add/Move while cancel remains available.
        fake.enqueue(
            {.kind = test::fake::EventKind::Begin, .stream_code = generated::StreamCode::kFortsInstrumentstateRepl});
        fake.enqueue(
            {.kind = test::fake::EventKind::Row,
             .stream_code = generated::StreamCode::kFortsInstrumentstateRepl,
             .table_code = generated::TableCode::kFortsInstrumentstateReplInstrumentState,
             .revision = 100,
             .fields = {{.field_code = generated::FieldCode::kFortsInstrumentstateReplInstrumentStateIsinId,
                         .signed_value = 1001},
                        {.field_code = generated::FieldCode::kFortsInstrumentstateReplInstrumentStatePublicState,
                         .signed_value = 0}}});
        fake.enqueue(
            {.kind = test::fake::EventKind::Commit, .stream_code = generated::StreamCode::kFortsInstrumentstateRepl});
        require(!external.poll(), "committed instrument halt pump");
        require(external.post_command(command, 304).certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent,
                "Add ignored committed instrument halt");
        require(external.post_command(cancel, 305).certainty == cgate::Plaza2SubmissionCertainty::Posted,
                "halted instrument blocked risk reduction");
        require(!external.stop(), "external stop");
        ::dlclose(dso);
        test::remove_tree(root);
        std::cout << "CGate persistent session recovery PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
