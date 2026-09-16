#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

moex::plaza2::cgate::Plaza2ClockEvidence make_clock_evidence() {
    using namespace moex::plaza2::cgate;
    constexpr std::int64_t base_wall = 1'700'000'000'000'000'000;
    return {
        .sync_source = "chrony",
        .sync_status_ok = true,
        .wall_offset_ns = -100'000'000,
        .offset_uncertainty_ns = 1'000'000,
        .monotonic_clock_id = "CLOCK_MONOTONIC_RAW",
        .paired_samples = {{.local_wall_ns = base_wall,
                            .local_monotonic_ns = 1'000'000'000,
                            .exchange_wall_ns = base_wall + 100'000'000,
                            .provenance = "test-current-clock",
                            .current_reference = true},
                           {.local_wall_ns = base_wall + 500'000'000,
                            .local_monotonic_ns = 2'000'000'000,
                            .exchange_wall_ns = base_wall + 600'000'000,
                            .provenance = "test-current-clock",
                            .current_reference = true}},
        .current_local_wall_ns = base_wall + 1'000'000'000,
        .current_local_monotonic_ns = 3'000'000'000,
        .sync_status_monotonic_ns = 2'500'000'000,
    };
}

moex::plaza2::cgate::Plaza2Aggr20MdConfig make_config(const moex::plaza2::test::RuntimeFixturePaths& fixture) {
    using namespace moex::plaza2::cgate;

    Plaza2Aggr20MdConfig config;
    config.profile_id = "phase5d_aggr20_runner";
    config.endpoint_host = "localhost";
    config.endpoint_port = 4001;
    config.runtime.environment = Plaza2Environment::Test;
    config.runtime.runtime_root = fixture.root;
    config.runtime.expected_spectra_release = "SPECTRA93";
    config.runtime.env_open_settings = "ini=config/t1.ini;key=${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}";
    config.connection_settings = "p2tcp://localhost:4001;app_name=connectors_phase5d_aggr20";
    config.stream.settings = "p2repl://FORTS_AGGR20_REPL;scheme=|FILE|scheme/forts_scheme.ini|Aggr";
    config.software_key.source = Plaza2CredentialSource::Env;
    config.software_key.env_var = "MOEX_PLAZA2_CGATE_SOFTWARE_KEY";
    config.arm_state.test_network_armed = true;
    config.arm_state.test_session_armed = true;
    config.arm_state.test_plaza2_armed = true;
    config.test_market_data_armed = true;
    config.process_timeout_ms = 0;
    config.clock_evidence = make_clock_evidence();
    return config;
}

std::array<moex::plaza2::cgate::Plaza2DecodedFieldValue, 8>
sys_event_fields(std::int64_t repl_id, std::int64_t repl_rev, std::int64_t event_id, std::int64_t sess_id,
                 std::string_view message) {
    using namespace moex::plaza2::cgate;
    using moex::plaza2::generated::FieldCode;
    return {
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplSysEventsReplId,
                                .kind = Plaza2DecodedValueKind::SignedInteger,
                                .signed_value = repl_id},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplSysEventsReplRev,
                                .kind = Plaza2DecodedValueKind::SignedInteger,
                                .signed_value = repl_rev},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplSysEventsReplAct,
                                .kind = Plaza2DecodedValueKind::SignedInteger},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplSysEventsEventType,
                                .kind = Plaza2DecodedValueKind::SignedInteger,
                                .signed_value = 1},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplSysEventsEventId,
                                .kind = Plaza2DecodedValueKind::SignedInteger,
                                .signed_value = event_id},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplSysEventsSessId,
                                .kind = Plaza2DecodedValueKind::SignedInteger,
                                .signed_value = sess_id},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplSysEventsMessage,
                                .kind = Plaza2DecodedValueKind::String,
                                .text_value = message},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplSysEventsServerTime,
                                .kind = Plaza2DecodedValueKind::UnsignedInteger,
                                .unsigned_value = 1700000000},
    };
}

void emit_sys_event(moex::plaza2::cgate::Plaza2Aggr20ListenerBridge& bridge, std::int64_t repl_id,
                    std::int64_t repl_rev, std::int64_t event_id, std::int64_t sess_id, bool in_snapshot) {
    using namespace moex::plaza2::cgate;
    const auto fields = sys_event_fields(repl_id, repl_rev, event_id, sess_id, "session_data_ready");
    if (in_snapshot)
        static_cast<void>(bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::TransactionBegin}));
    static_cast<void>(
        bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                         .table_code = moex::plaza2::generated::TableCode::kFortsAggrReplSysEvents,
                                         .fields = fields}));
    if (in_snapshot)
        static_cast<void>(bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::TransactionCommit}));
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "expected fake runtime library path\n";
            return 1;
        }

        using namespace moex::plaza2::cgate;
        using namespace moex::plaza2::test;

        const auto fake_library = std::filesystem::path(argv[1]);
        const auto fixture_root = make_temp_directory("plaza2_aggr20_md_runner_test");
        const auto cleanup = [&]() { remove_tree(fixture_root); };

        const auto scheme_text = build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test");
        const auto fixture =
            materialize_runtime_fixture(fixture_root, fake_library, Plaza2Environment::Test, scheme_text);

        ::setenv("MOEX_PLAZA2_CGATE_SOFTWARE_KEY", "PHASE5D-REDACTION-SAMPLE", 1);
        ::setenv("MOEX_FAKE_CGATE_REQUIRE_ABSOLUTE_SCHEME", "1", 1);

        ::setenv("MOEX_FAKE_AGGR_CLEAR_ON_BOOTSTRAP", "1", 1);
        Plaza2Aggr20MdRunner runner(make_config(fixture));
        const auto start = runner.start();
        require(start.ok, "AGGR20 runner start should succeed with fake runtime and all arm flags");

        bool ready = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const auto poll = runner.poll_once();
            require(poll.ok, "AGGR20 runner poll should succeed");
            if (runner.health_snapshot().ready) {
                ready = true;
                break;
            }
        }
        require(ready, "AGGR20 runner should reach ready with deterministic fake runtime data");

        const auto& health = runner.health_snapshot();
        require(health.runtime_probe_ok, "AGGR20 runtime probe should pass");
        require(health.scheme_drift_ok, "AGGR20 scheme drift should pass");
        require(health.stream_created && health.stream_opened, "AGGR20 stream should be created and opened");
        require(health.stream_online && health.stream_snapshot_complete,
                "AGGR20 stream should become online and snapshot-complete");
        require(health.snapshot.row_count == 2, "AGGR20 fake runtime should emit two rows");
        require(health.snapshot.instrument_count == 1, "AGGR20 fake runtime instrument count mismatch");
        require(health.snapshot.top_bid.has_value() && health.snapshot.top_bid->price == "102500",
                "AGGR20 fake top bid mismatch");
        require(health.snapshot.top_ask.has_value() && health.snapshot.top_ask->price == "102750",
                "AGGR20 fake top ask mismatch");

        for (const auto& line : runner.operator_log_lines()) {
            require(line.find("PHASE5D-REDACTION-SAMPLE") == std::string::npos,
                    "AGGR20 operator log must not leak raw credentials");
        }

        const auto stop = runner.stop();
        require(stop.ok, "AGGR20 runner stop should succeed");

        require(!runner.health_snapshot().ready && runner.health_snapshot().snapshot.row_count == 0,
                "stop invalidates visible AGGR book and readiness");
        for (const auto* scenario : {"MOEX_FAKE_AGGR_CLOSE_AFTER_READY", "MOEX_FAKE_AGGR_LIFENUM_AFTER_READY",
                                     "MOEX_FAKE_AGGR_CLEAR_AFTER_READY", "MOEX_FAKE_AGGR_ERROR_AFTER_READY"}) {
            auto now = std::chrono::steady_clock::time_point{};
            auto config = make_config(fixture);
            config.now = [&] { return now; };
            Plaza2Aggr20MdRunner recovery(config);
            require(recovery.start().ok && recovery.poll_once().ok && recovery.health_snapshot().ready,
                    "recovery fixture reaches ready");
            ::setenv(scenario, "1", 1);
            require(recovery.poll_once().ok, "loss/ LifeNum callback is handled");
            require(!recovery.health_snapshot().ready && recovery.health_snapshot().snapshot.row_count == 0,
                    "loss/ LifeNum invalidates stale visible levels");
            ::unsetenv(scenario);
            if (std::string_view(scenario) != "MOEX_FAKE_AGGR_LIFENUM_AFTER_READY") {
                require(recovery.health_snapshot().state == Plaza2Aggr20MdRunnerState::Recovering,
                        "listener loss reports recovery");
                now += std::chrono::milliseconds(999);
                require(recovery.poll_once().ok && !recovery.health_snapshot().ready, "no reopen before bounded delay");
                now += std::chrono::milliseconds(1);
                require(recovery.poll_once().ok && recovery.health_snapshot().ready,
                        "fresh snapshot after reopen restores readiness");
                require(recovery.health_snapshot().snapshot.row_count == 2, "recovered book contains only fresh rows");
            } else {
                require(recovery.poll_once().ok && !recovery.health_snapshot().ready,
                        "LifeNum cannot restore readiness without retransmission and ONLINE");
            }
            require(recovery.stop().ok, "recovery fixture stop");
        }

        {
            auto now = std::chrono::steady_clock::time_point{};
            auto config = make_config(fixture);
            config.now = [&] { return now; };
            Plaza2Aggr20MdRunner recovery(config);
            require(recovery.start().ok && recovery.poll_once().ok && recovery.health_snapshot().ready,
                    "AGGR20 reopen-error fixture reaches ready");
            ::setenv("MOEX_FAKE_AGGR_CLOSE_AFTER_READY", "1", 1);
            require(recovery.poll_once().ok && !recovery.health_snapshot().ready,
                    "AGGR20 close invalidates the visible book before reopen");
            ::unsetenv("MOEX_FAKE_AGGR_CLOSE_AFTER_READY");
            now += std::chrono::seconds(1);
            ::setenv("MOEX_FAKE_AGGR_REOPEN_OPEN_RESULT", "36866", 1);
            require(recovery.poll_once().ok &&
                        recovery.health_snapshot().state == Plaza2Aggr20MdRunnerState::Recovering,
                    "transient AGGR20 reopen service failure must remain retryable");
            require(recovery.health_snapshot().failure_classification == "transient_service_unavailable" &&
                        recovery.health_snapshot().last_error.find("SERV:NO_SERVICE") != std::string::npos,
                    "transient AGGR20 reopen cause must remain visible in health");
            ::unsetenv("MOEX_FAKE_AGGR_REOPEN_OPEN_RESULT");
            now += std::chrono::seconds(1);
            require(recovery.poll_once().ok && recovery.health_snapshot().ready,
                    "AGGR20 must recover after transient reopen service restoration");
            require(recovery.stop().ok, "transient AGGR20 reopen fixture stop");
        }

        {
            auto now = std::chrono::steady_clock::time_point{};
            auto config = make_config(fixture);
            config.now = [&] { return now; };
            Plaza2Aggr20MdRunner recovery(config);
            require(recovery.start().ok && recovery.poll_once().ok && recovery.health_snapshot().ready,
                    "fatal AGGR20 reopen fixture reaches ready");
            ::setenv("MOEX_FAKE_AGGR_CLOSE_AFTER_READY", "1", 1);
            require(recovery.poll_once().ok, "fatal AGGR20 reopen fixture enters recovery");
            ::unsetenv("MOEX_FAKE_AGGR_CLOSE_AFTER_READY");
            now += std::chrono::seconds(1);
            ::setenv("MOEX_FAKE_AGGR_REOPEN_OPEN_RESULT", "invalid", 1);
            const auto fatal = recovery.poll_once();
            require(!fatal.ok && recovery.health_snapshot().state == Plaza2Aggr20MdRunnerState::Failed,
                    "static AGGR20 reopen failure must terminate the affected runner");
            require(recovery.health_snapshot().failure_classification == "fatal_static_or_incompatible" &&
                        recovery.health_snapshot().last_error.find("INVALIDARGUMENT") != std::string::npos,
                    "fatal AGGR20 reopen cause must be exposed, not suppressed");
            ::unsetenv("MOEX_FAKE_AGGR_REOPEN_OPEN_RESULT");
            require(recovery.stop().ok, "fatal AGGR20 reopen fixture stop");
        }

        {
            auto now = std::chrono::steady_clock::time_point{};
            auto config = make_config(fixture);
            config.now = [&] { return now; };
            config.listener_bootstrap_watchdog = std::chrono::seconds(3);
            Plaza2Aggr20MdRunner recovery(config);
            ::setenv("MOEX_FAKE_LSN_OPENING_STATE", "1", 1);
            require(recovery.start().ok && recovery.poll_once().ok && !recovery.health_snapshot().ready,
                    "AGGR20 OPENING fixture must remain non-ready before its watchdog expires");
            now += std::chrono::seconds(3);
            require(recovery.poll_once().ok &&
                        recovery.health_snapshot().state == Plaza2Aggr20MdRunnerState::Recovering &&
                        recovery.health_snapshot().failure_classification == "transient_bootstrap_watchdog" &&
                        recovery.health_snapshot().last_error.find("bootstrap watchdog") != std::string::npos,
                    "AGGR20 stuck OPENING must expose a bounded per-attempt recovery diagnostic");
            ::unsetenv("MOEX_FAKE_LSN_OPENING_STATE");
            now += std::chrono::seconds(1);
            require(recovery.poll_once().ok && recovery.health_snapshot().ready,
                    "AGGR20 stuck OPENING must recover after the stream becomes available");
            require(recovery.stop().ok, "AGGR20 watchdog fixture stop");
        }

        Plaza2Aggr20BookProjector authority_projector;
        Plaza2Aggr20ListenerBridge authority_bridge(authority_projector, 321);
        require(!authority_bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::Open}),
                "authority bridge open");
        require(
            !authority_bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::LifeNum, .unsigned_value = 7}),
            "authority bridge lifenum");
        emit_sys_event(authority_bridge, 2301, 23, 23, 321, true);
        require(!authority_bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::Online}),
                "authority bridge online");
        require(authority_bridge.online() && authority_bridge.snapshot_complete() &&
                    !authority_bridge.session_data_ready() && !authority_bridge.authoritative(),
                "bootstrap rows and an old snapshot sys_events row must not authorize the book");
        require(authority_bridge.authority_snapshot().last_sys_event.has_value() &&
                    authority_bridge.authority_snapshot().last_sys_event->seen_during_snapshot,
                "bootstrap sys_events provenance must be retained as non-authoritative");
        const auto before_resync_epoch = authority_bridge.authority_snapshot().stream_epoch;
        emit_sys_event(authority_bridge, 2401, 24, 24, 321, false);
        require(authority_bridge.session_data_ready() && authority_bridge.authoritative(),
                "a current post-bootstrap session_data_ready row must restore authority");
        require(!authority_bridge.authority_snapshot().last_sys_event->seen_during_snapshot,
                "current sys_events provenance must not be marked as bootstrap");
        require(!authority_bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted}) &&
                    authority_bridge.recovering() && !authority_bridge.authoritative() &&
                    authority_projector.snapshot().row_count == 0 &&
                    authority_bridge.authority_snapshot().stream_epoch > before_resync_epoch,
                "clearing an authoritative stream must invalidate the visible book and epoch");
        require(!authority_bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::Open}), "resync open");
        require(
            !authority_bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::LifeNum, .unsigned_value = 8}),
            "resync lifenum");
        require(!authority_bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::Online}), "resync online");
        emit_sys_event(authority_bridge, 2501, 25, 25, 999, false);
        require(!authority_bridge.authoritative(), "wrong-session sys_events must not authorize the target");
        emit_sys_event(authority_bridge, 2502, 26, 26, 321, false);
        require(authority_bridge.authoritative(), "matching current sys_events must restore authority after resync");

        Plaza2Aggr20BookProjector staged;
        Plaza2Aggr20ListenerBridge staged_bridge(staged);
        require(!staged_bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::TransactionBegin}),
                "begin staged bootstrap");
        require(!staged_bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted}) &&
                    staged_bridge.recovering() && !staged_bridge.online(),
                "cleanup during a transaction still requires a fresh snapshot");
        require(!staged_bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::Online}) &&
                    !staged_bridge.online(),
                "ONLINE cannot bypass pending recovery");
        ::unsetenv("MOEX_FAKE_AGGR_CLEAR_ON_BOOTSTRAP");
        cleanup();
        ::unsetenv("MOEX_PLAZA2_CGATE_SOFTWARE_KEY");
        ::unsetenv("MOEX_FAKE_CGATE_REQUIRE_ABSOLUTE_SCHEME");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
