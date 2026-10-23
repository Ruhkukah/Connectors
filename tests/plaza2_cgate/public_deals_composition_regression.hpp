#pragma once

#include "fake_cgate_control.hpp"
#include "moex/connector_host/operator_config.hpp"
#include "plaza2_runtime_test_support.hpp"

namespace moex::plaza2::test::deals_composition {

inline connector_host::Plaza2HostConfig operator_config(const RuntimeFixturePaths& fixture,
                                                        std::int64_t isin_id = 1001) {
    connector_host::Plaza2HostConfigInputs inputs;
    inputs.read_only_market_data = true;
    inputs.public_deals = true;
    inputs.runtime_root = fixture.root;
    inputs.library_path = fixture.library_path;
    inputs.scheme_dir = fixture.scheme_dir;
    inputs.config_dir = fixture.config_dir;
    inputs.env_open_settings = "ini=config/t1.ini;key=00000000";
    inputs.software_key_env_var = "MOEX_PLAZA2_CGATE_SOFTWARE_KEY";
    inputs.expected_spectra_release = "SPECTRA93";
    inputs.isin_ids = {isin_id};
    auto result = connector_host::build_plaza2_host_config(inputs);
    result.transport.host.process_timeout_ms = 0;
    return result;
}

inline void pump(connector_host::ConnectorHost& host, int count = 20) {
    for (int i = 0; i < count; ++i)
        require(!host.poll(), "public-DEALS composed host polls");
}

inline void require_read_only(const connector_host::ConnectorHost& host, const fake::Control& runtime,
                              std::size_t commands_before) {
    const auto state = host.snapshot();
    require(!host.has_publisher_or_reply_handles() && !host.order_entry_ready(1001) &&
                state.publisher_calls.msgnew == 0 && state.publisher_calls.post == 0 &&
                runtime.commands().size() == commands_before,
            "public-DEALS composition acquired a publisher, reply handle, allocation, or order surface");
}

inline void live_deal(const fake::Control& runtime, std::int32_t session, std::int64_t id, std::int64_t revision) {
    using enum generated::StreamCode;
    using enum generated::TableCode;
    using enum generated::FieldCode;
    runtime.enqueue(
        {.stream_code = kFortsDealsRepl,
         .table_code = kFortsDealsReplDeal,
         .revision = revision,
         .fields = {{.field_code = kFortsDealsReplDealReplId, .signed_value = revision},
                    {.field_code = kFortsDealsReplDealReplRev, .signed_value = revision},
                    {.field_code = kFortsDealsReplDealReplAct, .signed_value = 0},
                    {.field_code = kFortsDealsReplDealSessId, .signed_value = session},
                    {.field_code = kFortsDealsReplDealIsinId, .signed_value = 1001},
                    {.field_code = kFortsDealsReplDealIdDeal, .signed_value = id},
                    {.field_code = kFortsDealsReplDealXamount, .signed_value = 1},
                    {.field_code = kFortsDealsReplDealPrice, .kind = fake::FieldKind::Text, .text = "102500.12500"},
                    {.field_code = kFortsDealsReplDealMoment,
                     .kind = fake::FieldKind::Timestamp,
                     .unsigned_value = 1700000000},
                    {.field_code = kFortsDealsReplDealMomentNs,
                     .kind = fake::FieldKind::UnsignedInteger,
                     .unsigned_value = 1700000000123456789ULL},
                    {.field_code = kFortsDealsReplDealNosystem, .signed_value = 0},
                    {.field_code = kFortsDealsReplDealXstatusBuy, .signed_value = 0},
                    {.field_code = kFortsDealsReplDealXstatusSell, .signed_value = 0}}});
}

inline void run(const RuntimeFixturePaths& fixture, const fake::Control& runtime) {
    using enum generated::StreamCode;
    const auto commands_before = runtime.commands().size();
    runtime.configure({});
    {
        const auto cfg = operator_config(fixture);
        require(cfg.transport.host.public_deals_target_isin_id == 1001 &&
                    cfg.transport.host.aggr20_target_session_id == 0,
                "operator public-DEALS target must follow the selected ISIN without a fixed session");
        const auto opens_before = runtime.opens(kFortsDealsRepl);
        connector_host::ConnectorHost host(cfg);
        require(!host.start(), "operator-composed public-DEALS host starts");
        require(runtime.opens(kFortsDealsRepl) == opens_before, "DEALS opened before committed metadata was available");
        pump(host);
        const auto initial = host.public_deals_snapshot();
        require(initial.valid && initial.online && initial.trades.size() == 2 && initial.lifenum == 7 &&
                    initial.trades[0].deal_id == 9002 && initial.trades[1].deal_id == 9003 &&
                    initial.trades[0].isin_id == 1001 && initial.trades[0].session_id == 321 &&
                    initial.trades[0].stream_epoch == initial.stream_epoch &&
                    initial.trades[0].received_at_unix_ns > 0 && runtime.opens(kFortsDealsRepl) == opens_before + 1,
                "operator-to-native composition did not deliver only committed, current-session live deals");
        require_read_only(host, runtime, commands_before);
        runtime.enqueue({.kind = fake::EventKind::Begin, .stream_code = kFortsDealsRepl});
        live_deal(runtime, 321, 9100, 100);
        pump(host);
        require(host.public_deals_snapshot().last_sequence == initial.last_sequence,
                "native public deal escaped before its transaction commit");
        runtime.enqueue({.kind = fake::EventKind::Commit, .stream_code = kFortsDealsRepl});
        pump(host);
        const auto committed = host.public_deals_snapshot(initial.last_sequence);
        require(committed.trades.size() == 1 && committed.trades.front().deal_id == 9100,
                "native public deal did not reach the selected target after commit");
        require(!host.stop(), "operator-composed public-DEALS host stops");
        require(!host.public_deals_snapshot().valid && host.public_deals_snapshot().trades.empty(),
                "stop retained public-deal authority or buffered trades");
        require(!host.start(), "same operator-composed host restarts offline");
        pump(host);
        require(host.public_deals_snapshot().valid && host.public_deals_snapshot().stream_epoch > initial.stream_epoch,
                "same-owner stop/start reused a public-DEALS epoch");
        require_read_only(host, runtime, commands_before);
        require(!host.stop(), "restarted public-DEALS host stops");
    }
    for (const auto barrier : {fake::Option::RefdataPauseBeforeCommit, fake::Option::RefdataPauseBeforeOnline,
                               fake::Option::SessionstatePauseBeforeCommit}) {
        runtime.configure({});
        runtime.set(barrier);
        const auto opens_before = runtime.opens(kFortsDealsRepl);
        connector_host::ConnectorHost host(operator_config(fixture));
        require(!host.start(), "metadata-barrier public-DEALS host starts");
        pump(host);
        require(!host.public_deals_snapshot().valid && host.public_deals_snapshot().trades.empty() &&
                    runtime.opens(kFortsDealsRepl) == opens_before,
                "uncommitted or incomplete metadata enabled a public-DEALS listener");
        const auto stream =
            barrier == fake::Option::SessionstatePauseBeforeCommit ? kFortsSessionstateRepl : kFortsRefdataRepl;
        if (barrier != fake::Option::RefdataPauseBeforeOnline)
            runtime.enqueue({.kind = fake::EventKind::Commit, .stream_code = stream});
        runtime.enqueue({.kind = fake::EventKind::Online, .stream_code = stream});
        pump(host);
        require(host.public_deals_snapshot().valid && host.public_deals_snapshot().trades.size() == 2 &&
                    runtime.opens(kFortsDealsRepl) == opens_before + 1,
                "completed current metadata did not release the deferred DEALS listener exactly once");
        require_read_only(host, runtime, commands_before);
        require(!host.stop(), "metadata-barrier public-DEALS host stops");
    }
    for (const auto scenario : {fake::Option::MissingInstrument, fake::Option::MissingSession}) {
        runtime.configure({});
        runtime.set(scenario);
        const auto opens_before = runtime.opens(kFortsDealsRepl);
        connector_host::ConnectorHost host(operator_config(fixture));
        require(!host.start(), "missing current metadata public-DEALS host starts");
        pump(host);
        require(!host.public_deals_snapshot().valid && host.public_deals_snapshot().trades.empty() &&
                    runtime.opens(kFortsDealsRepl) == opens_before,
                "missing instrument or mismatched current session opened public DEALS");
        require_read_only(host, runtime, commands_before);
        require(!host.stop(), "missing current metadata public-DEALS host stops");
    }
    {
        runtime.configure({});
        auto now = std::chrono::steady_clock::time_point{std::chrono::seconds{100}};
        auto cfg = operator_config(fixture);
        cfg.transport.host.recovery_now = [&] { return now; };
        connector_host::ConnectorHost host(cfg);
        require(!host.start(), "recovering public-DEALS host starts");
        pump(host);
        const auto old = host.public_deals_snapshot();
        require(old.valid && old.trades.size() == 2, "recovery starts with actual bound public deals");
        runtime.set(fake::Option::ConnectionError);
        pump(host, 1);
        runtime.clear(fake::Option::ConnectionError);
        require(!host.public_deals_snapshot().valid && host.public_deals_snapshot().trades.empty(),
                "connection loss retained public-deal authority or buffered trades");
        runtime.set(fake::Option::SessionId, "322");
        for (int i = 0; i < 40; ++i) {
            now += std::chrono::milliseconds(100);
            pump(host, 1);
        }
        const auto next = host.public_deals_snapshot();
        require(next.valid && next.online && next.stream_epoch > old.stream_epoch && next.trades.size() == 2 &&
                    next.trades.front().session_id == 322 && next.trades.front().stream_epoch == next.stream_epoch,
                "recovery did not rebind current metadata with a strictly newer public-DEALS epoch");
        require_read_only(host, runtime, commands_before);
        require(!host.stop(), "recovering public-DEALS host stops");
    }
    {
        runtime.configure({});
        connector_host::ConnectorHost host(operator_config(fixture));
        require(!host.start(), "session-rollover public-DEALS host starts");
        pump(host);
        const auto original = host.public_deals_snapshot();
        runtime.set(fake::Option::PrepublishNextSession);
        pump(host);
        runtime.clear(fake::Option::PrepublishNextSession);
        require(host.public_deals_snapshot().stream_epoch == original.stream_epoch &&
                    host.public_deals_snapshot().trades.front().session_id == 321,
                "prepublished future-session membership replaced the current DEALS target");
        runtime.set(fake::Option::LiveSessionSwitch);
        pump(host, 1);
        runtime.clear(fake::Option::LiveSessionSwitch);
        runtime.set(fake::Option::SessionId, "322");
        pump(host);
        const auto next = host.public_deals_snapshot();
        require(next.valid && next.online && next.stream_epoch > original.stream_epoch && next.trades.size() == 2 &&
                    next.trades.front().session_id == 322,
                "live session rollover retained the old DEALS target or reused its epoch");
        runtime.enqueue({.kind = fake::EventKind::Begin, .stream_code = kFortsDealsRepl});
        live_deal(runtime, 321, 9200, 200);
        live_deal(runtime, 322, 9201, 201);
        runtime.enqueue({.kind = fake::EventKind::Commit, .stream_code = kFortsDealsRepl});
        pump(host);
        const auto tail = host.public_deals_snapshot(next.last_sequence);
        require(tail.trades.size() == 1 && tail.trades.front().deal_id == 9201 &&
                    tail.trades.front().session_id == 322 && tail.trades.front().stream_epoch == next.stream_epoch,
                "old-session deal replay crossed the newly bound public-DEALS epoch");
        require_read_only(host, runtime, commands_before);
        require(!host.stop(), "session-rollover public-DEALS host stops");
    }
    {
        runtime.configure({});
        auto now = std::chrono::steady_clock::time_point{std::chrono::seconds{100}};
        auto cfg = operator_config(fixture);
        cfg.transport.host.recovery_now = [&] { return now; };
        connector_host::ConnectorHost host(cfg);
        require(!host.start(), "destroy-retry public-DEALS host starts");
        pump(host);
        const auto original = host.public_deals_snapshot();
        const auto original_opens = runtime.opens(kFortsDealsRepl);
        require(original.valid && runtime.listener_handles(kFortsDealsRepl) == 1,
                "destroy-retry test starts with exactly one native DEALS handle");
        runtime.set(fake::Option::DealsDestroyOnce);
        runtime.enqueue({.kind = fake::EventKind::Close, .stream_code = kFortsSessionstateRepl});
        pump(host, 1);
        require(!host.public_deals_snapshot().valid && host.public_deals_snapshot().trades.empty() &&
                    runtime.listener_handles(kFortsDealsRepl) == 1 &&
                    runtime.opens(kFortsDealsRepl) == original_opens &&
                    host.snapshot().last_error.find("cg_lsn_destroy") != std::string::npos,
                "failed native destroy lost its handler, exposed stale trades, or created a second DEALS listener");
        pump(host);
        require(!host.public_deals_snapshot().valid && runtime.listener_handles(kFortsDealsRepl) == 1 &&
                    runtime.opens(kFortsDealsRepl) == original_opens,
                "failed native destroy bypassed its paced retry boundary");
        for (int i = 0; i < 40; ++i) {
            now += std::chrono::milliseconds(100);
            pump(host, 1);
        }
        require(
            host.public_deals_snapshot().valid && host.public_deals_snapshot().stream_epoch > original.stream_epoch &&
                runtime.listener_handles(kFortsDealsRepl) == 1 && runtime.opens(kFortsDealsRepl) == original_opens + 1,
            "successful native destroy retry did not recreate exactly one fresh DEALS listener");
        require_read_only(host, runtime, commands_before);
        require(!host.stop() && runtime.listener_handles(kFortsDealsRepl) == 0,
                "destroy-retry public-DEALS host did not release its final native handle");
    }
    runtime.configure({});
}
} // namespace moex::plaza2::test::deals_composition
