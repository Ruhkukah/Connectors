#include "moex/plaza2/cgate/plaza2_live_session_runner.hpp"

#include "plaza2_runtime_test_support.hpp"

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

moex::plaza2::cgate::Plaza2LiveSessionConfig make_config(const moex::plaza2::test::RuntimeFixturePaths& fixture) {
    using namespace moex::plaza2::cgate;

    Plaza2LiveSessionConfig config;
    config.profile_id = "phase3f_validation";
    config.endpoint_host = "198.51.100.10";
    config.endpoint_port = 4001;
    config.runtime.environment = Plaza2Environment::Test;
    config.runtime.runtime_root = fixture.root;
    config.runtime.expected_spectra_release = "SPECTRA93";
    config.runtime.env_open_settings = "ini=config/t1.ini;key=00000000";
    config.connection_settings = "p2tcp://198.51.100.10:4001;app_name=connectors_phase3f_validation";
    config.streams = make_private_streams();
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
        const auto fixture_root = make_temp_directory("plaza2_live_session_validation_test");
        const auto cleanup = [&]() { remove_tree(fixture_root); };

        const auto scheme_text = build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test");
        const auto fixture =
            materialize_runtime_fixture(fixture_root, fake_library, Plaza2Environment::Test, scheme_text);

        {
            auto config = make_config(fixture);
            config.arm_state.test_network_armed = true;
            config.arm_state.test_session_armed = true;
            Plaza2LiveSessionRunner runner(config);
            const auto result = runner.start();
            require(!result.ok, "runner must refuse external TEST bring-up without --armed-test-plaza2");
        }

        {
            auto config = make_config(fixture);
            config.arm_state.test_network_armed = true;
            config.arm_state.test_session_armed = true;
            config.arm_state.test_plaza2_armed = true;
            config.runtime.expected_spectra_release = "SPECTRA95";
            Plaza2LiveSessionRunner runner(config);
            const auto result = runner.start();
            require(!result.ok, "runner must refuse incompatible runtime scheme drift");
            require(runner.probe_report().compatibility == Plaza2Compatibility::Incompatible,
                    "probe report should expose incompatible runtime drift");
        }

        {
            auto config = make_config(fixture);
            config.arm_state.test_network_armed = true;
            config.arm_state.test_session_armed = true;
            config.arm_state.test_plaza2_armed = true;
            config.runtime.env_open_settings = "ini=config/t1.ini;key=${PLAZA2_TEST_CREDENTIALS}";
            config.credentials.source = Plaza2CredentialSource::File;
            config.credentials.file_path = fixture.root / "missing_auth.ini";
            Plaza2LiveSessionRunner runner(config);
            const auto result = runner.start();
            require(!result.ok, "runner must refuse missing credential file");
        }

        cleanup();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
