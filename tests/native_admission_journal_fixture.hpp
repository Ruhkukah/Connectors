#pragma once

#include "private_delta_host_regression.hpp"

#include <iterator>

namespace moex::connector_host::regression {
inline void emit_native_admission_journal(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                          const std::filesystem::path& root, const std::filesystem::path& output) {
    using namespace private_delta_host_detail;
    using enum gen::FieldCode;
    struct Checkpoint {
        std::string stage;
        std::int32_t session;
        bool admitted;
        bool native_ready;
        std::uintmax_t journal_end{};
    };
    std::vector<Checkpoint> checkpoints;
    const auto isin = config.isin_ids.front();
    require(config.isin_ids.size() == 1, "native admission fixture requires one configured instrument");
    config.journal_path = root / "native-admission.ndjson";
    config.identity_state_path = root / "native-admission.state";
    control.configure({.suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = config.orders.broker_code + config.orders.client_code,
                       .session_id = 321});
    CgateTradingHost host(config);
    bootstrap(host);
    const auto checkpoint = [&](std::string stage, std::int32_t session, bool admitted) {
        const auto status = host.status();
        const bool ready = status.find("\"order_entry_ready\":true") != std::string::npos;
        require(ready == admitted, "native admission fixture status disagrees at " + stage);
        require(status.find("\"sess_id\":" + std::to_string(session)) != std::string::npos,
                "native admission fixture session disagrees at " + stage);
        host.record_operator_input("admission_checkpoint:" + stage, "test");
        checkpoints.push_back({std::move(stage), session, admitted, ready});
    };
    const auto drain = [&] {
        for (int i = 0; i < 10; ++i)
            require(!host.poll(), "native admission fixture callback drain");
    };
    const auto transaction = [&](fake::Event row) {
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = row.stream_code});
        control.enqueue(row);
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = row.stream_code});
        drain();
    };
    const auto marker = [&](std::int32_t session, std::int64_t revision) {
        transaction({.stream_code = gen::StreamCode::kFortsInstrumentstateRepl,
                     .table_code = gen::TableCode::kFortsInstrumentstateReplSysEvents,
                     .revision = revision,
                     .fields = {integer(kFortsInstrumentstateReplSysEventsReplId, revision),
                                integer(kFortsInstrumentstateReplSysEventsReplRev, revision),
                                integer(kFortsInstrumentstateReplSysEventsEventId, revision),
                                integer(kFortsInstrumentstateReplSysEventsSessId, session),
                                integer(kFortsInstrumentstateReplSysEventsEventType, 1)}});
    };
    const auto status_row = [&](std::int32_t value, std::int64_t revision) {
        transaction({.stream_code = gen::StreamCode::kFortsInstrumentstateRepl,
                     .table_code = gen::TableCode::kFortsInstrumentstateReplInstrumentState,
                     .revision = revision,
                     .fields = {integer(kFortsInstrumentstateReplInstrumentStateReplId, isin),
                                integer(kFortsInstrumentstateReplInstrumentStateReplRev, revision),
                                integer(kFortsInstrumentstateReplInstrumentStateIsinId, isin),
                                integer(kFortsInstrumentstateReplInstrumentStatePublicState, value)}});
    };
    const auto tombstone = [&](std::int64_t identity, std::int64_t revision) {
        transaction({.stream_code = gen::StreamCode::kFortsInstrumentstateRepl,
                     .table_code = gen::TableCode::kFortsInstrumentstateReplSysEvents,
                     .revision = revision,
                     .fields = {integer(kFortsInstrumentstateReplSysEventsReplId, identity),
                                integer(kFortsInstrumentstateReplSysEventsReplRev, revision),
                                integer(kFortsInstrumentstateReplSysEventsReplAct, 1)}});
    };
    marker(321, 100);
    status_row(1, 101);
    checkpoint("open_current", 321, true);
    status_row(0, 102);
    checkpoint("instrument_closed", 321, false);
    status_row(1, 103);
    checkpoint("instrument_reopened", 321, true);
    marker(322, 99);
    checkpoint("older_marker", 321, true);
    tombstone(999, 104);
    checkpoint("unrelated_tombstone", 321, true);
    marker(322, 105);
    checkpoint("new_session_marker", 321, false);
    control.set(fake::Option::PrepublishNextSession);
    drain();
    control.clear(fake::Option::PrepublishNextSession);
    control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsSessionstateRepl});
    for (const auto [session, state] : {std::pair{321, 3}, std::pair{322, 1}})
        control.enqueue({.stream_code = gen::StreamCode::kFortsSessionstateRepl,
                         .table_code = gen::TableCode::kFortsSessionstateReplSessionState,
                         .revision = 106,
                         .fields = {integer(kFortsSessionstateReplSessionStateReplId, session),
                                    integer(kFortsSessionstateReplSessionStateReplRev, 106),
                                    integer(kFortsSessionstateReplSessionStateSessId, session),
                                    integer(kFortsSessionstateReplSessionStatePublicState, state)}});
    control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsSessionstateRepl});
    drain();
    checkpoint("new_membership_old_status", 322, false);
    status_row(1, 107);
    checkpoint("new_session_status", 322, true);
    tombstone(105, 110);
    checkpoint("latest_tombstone", 322, false);
    marker(322, 111);
    checkpoint("marker_restored_old_status", 322, false);
    status_row(1, 112);
    checkpoint("status_restored", 322, true);
    require(control.commands().empty(), "native admission journal fixture posted an order command");
    require(!host.stop(), "native admission journal fixture stop");

    // stop() flushes the asynchronous journal. Marker offsets delimit complete
    // prefixes before shutdown without adding a production flush API.
    std::ifstream journal(config.journal_path, std::ios::binary);
    std::string line;
    std::uintmax_t offset{};
    while (std::getline(journal, line)) {
        offset += line.size() + 1;
        for (auto& item : checkpoints)
            if (line.find("admission_checkpoint:" + item.stage + "\"") != std::string::npos)
                item.journal_end = offset;
    }
    std::filesystem::copy_file(config.journal_path, output, std::filesystem::copy_options::overwrite_existing);
    std::ofstream sidecar(output.string() + ".checkpoints.json");
    sidecar << "{\"fixture\":\"CgateTradingHost_OfflineFake_native_callbacks\",\"command_posts\":0,\"checkpoints\":[";
    bool first = true;
    for (const auto& item : checkpoints) {
        require(item.journal_end > 0, "native admission journal checkpoint was not flushed");
        if (!first)
            sidecar << ',';
        first = false;
        sidecar << "{\"stage\":\"" << item.stage << "\",\"journal_end\":" << item.journal_end
                << ",\"sess_id\":" << item.session << ",\"isin_ids\":[" << isin
                << "],\"expected_admitted\":" << (item.admitted ? "true" : "false")
                << ",\"native_order_entry_ready\":" << (item.native_ready ? "true" : "false") << '}';
    }
    sidecar << "]}\n";
    require(sidecar.good(), "native admission journal sidecar write failed");
}
} // namespace moex::connector_host::regression
