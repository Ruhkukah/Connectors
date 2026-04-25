#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

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
    return config;
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

        cleanup();
        ::unsetenv("MOEX_PLAZA2_CGATE_SOFTWARE_KEY");
        ::unsetenv("MOEX_FAKE_CGATE_REQUIRE_ABSOLUTE_SCHEME");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
