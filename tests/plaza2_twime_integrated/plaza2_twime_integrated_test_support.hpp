#pragma once

#include "moex/plaza2/cgate/plaza2_live_session_runner.hpp"
#include "moex/plaza2_twime_reconciler/plaza2_twime_integrated_test_runner.hpp"

#include "plaza2_runtime_test_support.hpp"
#include "twime_live_session_test_support.hpp"

#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace moex::plaza2_twime_reconciler::test {

inline void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline std::vector<moex::plaza2::cgate::Plaza2LiveStreamConfig> make_private_streams() {
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

inline moex::plaza2::cgate::Plaza2LiveSessionConfig
make_plaza_config(const moex::plaza2::test::RuntimeFixturePaths& fixture,
                  std::string profile_id = "phase4c_integrated_plaza") {
    using namespace moex::plaza2::cgate;

    Plaza2LiveSessionConfig config;
    config.profile_id = std::move(profile_id);
    config.endpoint_host = "198.51.100.10";
    config.endpoint_port = 4001;
    config.runtime.environment = Plaza2Environment::Test;
    config.runtime.runtime_root = fixture.root;
    config.runtime.expected_spectra_release = "SPECTRA93";
    config.runtime.env_open_settings = "ini=config/t1.ini;key=${PLAZA2_TEST_CREDENTIALS}";
    config.connection_settings = "p2tcp://198.51.100.10:4001;app_name=connectors_phase4c_integrated";
    config.streams = make_private_streams();
    config.credentials.source = Plaza2CredentialSource::Env;
    config.credentials.env_var = "MOEX_PLAZA2_TEST_CREDENTIALS";
    config.arm_state.test_network_armed = true;
    config.arm_state.test_session_armed = true;
    config.arm_state.test_plaza2_armed = true;
    config.process_timeout_ms = 0;
    return config;
}

inline moex::twime_trade::TwimeLiveSessionConfig make_twime_config(std::uint16_t port,
                                                                   std::string session_id = "phase4c_integrated") {
    auto config = moex::twime_trade::test::make_live_session_config(port, std::move(session_id),
                                                                    "MOEX_TWIME_TEST_CREDENTIALS");
    config.tcp.runtime_arm_state.test_network_armed = true;
    config.tcp.runtime_arm_state.test_session_armed = true;
    return config;
}

inline Plaza2TwimeIntegratedTestConfig
make_integrated_config(const moex::plaza2::test::RuntimeFixturePaths& fixture, std::uint16_t twime_port,
                       std::string profile_id = "phase4c_integrated") {
    Plaza2TwimeIntegratedTestConfig config;
    config.profile_id = std::move(profile_id);
    config.twime = make_twime_config(twime_port, config.profile_id + "_twime");
    config.plaza = make_plaza_config(fixture, config.profile_id + "_plaza");
    config.arm_state.test_network_armed = true;
    config.arm_state.test_session_armed = true;
    config.arm_state.test_plaza2_armed = true;
    config.arm_state.test_reconcile_armed = true;
    config.reconciler_stale_after_steps = 4;
    return config;
}

inline void pump_runner_until(Plaza2TwimeIntegratedTestRunner& runner, moex::twime_trade::test::ManualRunnerClock& clock,
                              const std::function<bool(const Plaza2TwimeIntegratedTestRunner&)>& predicate,
                              int max_polls = 256, std::uint64_t step_ms = 25) {
    for (int attempt = 0; attempt < max_polls; ++attempt) {
        if (predicate(runner)) {
            return;
        }
        const auto result = runner.poll_once();
        require(result.ok, "integrated TEST runner poll failed unexpectedly");
        clock.advance(step_ms);
    }
    require(false, "integrated TEST runner did not reach the expected state");
}

inline std::size_t find_log_index(std::span<const std::string> lines, std::string_view needle) {
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].find(needle) != std::string::npos) {
            return index;
        }
    }
    return lines.size();
}

} // namespace moex::plaza2_twime_reconciler::test
