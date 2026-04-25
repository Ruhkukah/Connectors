#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <iostream>

namespace {

moex::plaza2::cgate::Plaza2Aggr20MdConfig make_config() {
    using namespace moex::plaza2::cgate;

    Plaza2Aggr20MdConfig config;
    config.profile_id = "phase5d_aggr20_validation";
    config.endpoint_host = "localhost";
    config.endpoint_port = 4001;
    config.runtime.environment = Plaza2Environment::Test;
    config.runtime.runtime_root = "/tmp/plaza2-runtime-placeholder";
    config.runtime.env_open_settings = "ini=config/t1.ini;key=${MOEX_PLAZA2_TEST_CREDENTIALS}";
    config.connection_settings = "p2tcp://localhost:4001;app_name=connectors_phase5d_aggr20";
    config.stream.settings = "p2repl://FORTS_AGGR20_REPL;scheme=|FILE|scheme/forts_scheme.ini|FORTS_AGGR20_REPL";
    config.credentials.source = Plaza2CredentialSource::Env;
    config.credentials.env_var = "MOEX_PLAZA2_TEST_CREDENTIALS";
    config.arm_state.test_network_armed = true;
    config.arm_state.test_session_armed = true;
    config.arm_state.test_plaza2_armed = true;
    config.test_market_data_armed = true;
    return config;
}

} // namespace

int main() {
    try {
        using namespace moex::plaza2::cgate;
        using moex::plaza2::test::require;

        require(!validate_plaza2_aggr20_md_config(make_config()), "valid AGGR20 config should pass");

        {
            auto config = make_config();
            config.test_market_data_armed = false;
            const auto error = validate_plaza2_aggr20_md_config(config);
            require(static_cast<bool>(error), "AGGR20 config should require --armed-test-market-data");
        }

        {
            auto config = make_config();
            config.stream.settings =
                "p2repl://FORTS_ORDLOG_REPL;scheme=|FILE|scheme/forts_scheme.ini|FORTS_ORDLOG_REPL";
            const auto error = validate_plaza2_aggr20_md_config(config);
            require(static_cast<bool>(error), "ORDLOG must be rejected in Phase 5D");
        }

        {
            auto config = make_config();
            config.stream.settings = "p2repl://FORTS_DEALS_REPL;scheme=|FILE|scheme/forts_scheme.ini|FORTS_DEALS_REPL";
            const auto error = validate_plaza2_aggr20_md_config(config);
            require(static_cast<bool>(error), "DEALS must be rejected in Phase 5D");
        }

        {
            auto config = make_config();
            config.stream.settings =
                "p2repl://FORTS_ORDBOOK_REPL;scheme=|FILE|scheme/forts_scheme.ini|FORTS_ORDBOOK_REPL";
            const auto error = validate_plaza2_aggr20_md_config(config);
            require(static_cast<bool>(error), "ORDBOOK must be rejected in Phase 5D");
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
