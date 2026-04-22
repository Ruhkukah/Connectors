#include "moex/plaza2/cgate/plaza2_runtime.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "expected fake runtime library path\n";
            return 1;
        }

        using namespace moex::plaza2::cgate;
        using namespace moex::plaza2::test;

        const auto fake_library = std::filesystem::path(argv[1]);
        const auto fixture_root = make_temp_directory("plaza2_runtime_adapter_test");
        const auto cleanup = [&]() { remove_tree(fixture_root); };

        const auto scheme_text = build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test");
        const auto fixture = materialize_runtime_fixture(fixture_root, fake_library, Plaza2Environment::Test, scheme_text);

        Plaza2Settings settings;
        settings.environment = Plaza2Environment::Test;
        settings.runtime_root = fixture.root;
        settings.env_open_settings = "ini=config/t1.ini;key=00000000";

        Plaza2Env env;
        require(!env.open(settings), "environment open should succeed");

        Plaza2Connection connection;
        const auto app_name = make_plaza2_application_name("Connectors", "phase3c", 7);
        require(app_name == "connectors_phase3c_7", "application name should be deterministic and sanitized");
        require(!connection.create(env, "p2tcp://127.0.0.1:4001;app_name=" + app_name),
                "connection create should succeed");
        require(!connection.open({}), "connection open should succeed");

        std::uint32_t connection_state = 0;
        require(!connection.state(connection_state) && connection_state == 2, "fake connection should become active");

        std::uint32_t process_code = 0;
        require(!connection.process(0, &process_code), "timeout process should not be treated as error");
        require(process_code == 131075, "fake runtime should report CG_ERR_TIMEOUT from process");

        Plaza2Listener listener;
        require(!listener.create(connection, "p2repl://FORTS_TRADE_REPL;scheme=|FILE|scheme/forts_scheme.ini|TRADES"),
                "listener create should succeed");
        require(!listener.open({}), "listener open should succeed");

        std::uint32_t listener_state = 0;
        require(!listener.state(listener_state) && listener_state == 2, "fake listener should become active");

        const auto reopen_error = connection.open({});
        require(reopen_error && reopen_error.code == Plaza2ErrorCode::AdapterState,
                "reopening an active connection should translate to adapter-state error");

        require(!listener.close(), "listener close should succeed");
        require(!listener.destroy(), "listener destroy should succeed");
        require(!connection.close(), "connection close should succeed");
        require(!connection.destroy(), "connection destroy should succeed");
        require(!env.close(), "environment close should succeed");

        cleanup();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
