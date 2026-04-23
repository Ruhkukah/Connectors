#include "moex/plaza2/cgate/plaza2_live_session_runner.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

std::vector<moex::plaza2::cgate::Plaza2LiveStreamConfig> make_private_streams() {
    using moex::plaza2::cgate::Plaza2LiveStreamConfig;
    using moex::plaza2::generated::StreamCode;

    return {
        {.stream_code = StreamCode::kFortsTradeRepl,
         .settings = "p2repl://FORTS_TRADE_REPL;scheme=|FILE|scheme/forts_scheme.ini|FORTS_TRADE_REPL"},
        {.stream_code = StreamCode::kFortsUserorderbookRepl,
         .settings = "p2repl://FORTS_USERORDERBOOK_REPL;scheme=|FILE|scheme/forts_scheme.ini|FORTS_USERORDERBOOK_REPL"},
        {.stream_code = StreamCode::kFortsPosRepl,
         .settings = "p2repl://FORTS_POS_REPL;scheme=|FILE|scheme/forts_scheme.ini|FORTS_POS_REPL"},
        {.stream_code = StreamCode::kFortsPartRepl,
         .settings = "p2repl://FORTS_PART_REPL;scheme=|FILE|scheme/forts_scheme.ini|FORTS_PART_REPL"},
        {.stream_code = StreamCode::kFortsRefdataRepl,
         .settings = "p2repl://FORTS_REFDATA_REPL;scheme=|FILE|scheme/forts_scheme.ini|FORTS_REFDATA_REPL"},
    };
}

moex::plaza2::cgate::Plaza2LiveSessionConfig
make_config(const moex::plaza2::test::RuntimeFixturePaths& fixture) {
    using namespace moex::plaza2::cgate;

    Plaza2LiveSessionConfig config;
    config.profile_id = "phase3f_success";
    config.endpoint_host = "198.51.100.10";
    config.endpoint_port = 4001;
    config.runtime.environment = Plaza2Environment::Test;
    config.runtime.runtime_root = fixture.root;
    config.runtime.expected_spectra_release = "SPECTRA93";
    config.runtime.env_open_settings = "ini=config/t1.ini;key=${PLAZA2_TEST_CREDENTIALS}";
    config.connection_settings = "p2tcp://198.51.100.10:4001;app_name=connectors_phase3f_success";
    config.streams = make_private_streams();
    config.credentials.source = Plaza2CredentialSource::Env;
    config.credentials.env_var = "MOEX_PLAZA2_TEST_CREDENTIALS";
    config.arm_state.test_network_armed = true;
    config.arm_state.test_session_armed = true;
    config.arm_state.test_plaza2_armed = true;
    config.process_timeout_ms = 0;
    return config;
}

std::size_t find_log_index(std::span<const std::string> lines, std::string_view needle) {
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].find(needle) != std::string::npos) {
            return index;
        }
    }
    return lines.size();
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
        const auto fixture_root = make_temp_directory("plaza2_live_session_runner_test");
        const auto cleanup = [&]() { remove_tree(fixture_root); };

        const auto scheme_text = build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test");
        const auto fixture =
            materialize_runtime_fixture(fixture_root, fake_library, Plaza2Environment::Test, scheme_text);

        ::setenv("MOEX_PLAZA2_TEST_CREDENTIALS", "PHASE3F-SUPER-SECRET", 1);

        Plaza2LiveSessionRunner runner(make_config(fixture));
        const auto start = runner.start();
        require(start.ok, "runner start should succeed with fake TEST runtime and all arm flags");

        bool ready = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const auto poll = runner.poll_once();
            require(poll.ok, "runner poll should succeed");
            if (runner.health_snapshot().ready) {
                ready = true;
                break;
            }
        }
        require(ready, "runner should reach ready state after deterministic fake replay");

        const auto& health = runner.health_snapshot();
        require(health.runtime_probe_ok, "health should report runtime probe success");
        require(health.scheme_drift_ok, "health should report compatible scheme drift state");
        require(health.ready, "runner health should report ready");
        require(health.counts.session_count == 1, "session count mismatch");
        require(health.counts.instrument_count == 1, "instrument count mismatch");
        require(health.counts.matching_map_count == 1, "matching-map count mismatch");
        require(health.counts.limit_count == 1, "limit count mismatch");
        require(health.counts.position_count == 1, "position count mismatch");
        require(health.counts.own_order_count == 1, "own-order count mismatch");
        require(health.counts.own_trade_count == 1, "own-trade count mismatch");
        require(health.resume_markers.has_lifenum && health.resume_markers.last_lifenum == 7,
                "lifenum marker mismatch");
        require(health.resume_markers.last_replstate == "lifenum=7;rev.private_state=1", "replstate mismatch");

        for (const auto& stream : health.streams) {
            require(stream.created, "all required streams must be created");
            require(stream.opened, "all required streams must be opened");
            require(stream.online, "all required streams must become online");
            require(stream.snapshot_complete, "all required streams must become snapshot-complete");
        }

        const auto& logs = runner.operator_log_lines();
        for (const auto& line : logs) {
            require(line.find("PHASE3F-SUPER-SECRET") == std::string::npos,
                    "runner operator log must not leak raw credential values");
        }
        const auto probe_index = find_log_index(logs, "runtime_probe=compatible");
        const auto env_index = find_log_index(logs, "env=open");
        const auto ready_index = find_log_index(logs, "state=ready");
        const auto redaction_index = find_log_index(logs, "[REDACTED]");
        require(probe_index < env_index, "runtime probe log must appear before env open");
        require(env_index < ready_index, "ready log must appear after env open");
        require(redaction_index < logs.size(), "runner operator log should expose explicit credential redaction");

        const auto stop = runner.stop();
        require(stop.ok, "runner stop should succeed");

        cleanup();
        ::unsetenv("MOEX_PLAZA2_TEST_CREDENTIALS");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
