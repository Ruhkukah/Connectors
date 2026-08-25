#include "moex/plaza2/cgate/plaza2_trade_connectivity_qualifier.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace moex::plaza2::cgate;
using moex::plaza2::generated::StreamCode;

Plaza2LiveStreamConfig typed_stream(StreamCode code, std::string_view name) {
    return {
        .stream_code = code,
        .label = std::string(name),
        .settings = "p2repl://" + std::string(name) + ";scheme=|FILE|scheme/forts_scheme.ini|" +
                    std::string(name),
    };
}

Plaza2TradeConnectivityQualifierConfig make_config(
    const moex::plaza2::test::RuntimeFixturePaths& fixture) {
    Plaza2TradeConnectivityQualifierConfig config;
    config.session.profile_id = "plaza2_qualification_fixture";
    config.session.endpoint_host = "198.51.100.10";
    config.session.endpoint_port = 4001;
    config.session.runtime.environment = Plaza2Environment::Test;
    config.session.runtime.runtime_root = fixture.root;
    config.session.runtime.expected_spectra_release = "SPECTRA93";
    config.session.runtime.env_open_settings = "ini=config/t1.ini;key=${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}";
    config.session.connection_settings = "p2tcp://198.51.100.10:4001;app_name=plaza2_qualification_fixture";
    config.session.streams = {
        typed_stream(StreamCode::kFortsTradeRepl, "FORTS_TRADE_REPL"),
        typed_stream(StreamCode::kFortsUserorderbookRepl, "FORTS_USERORDERBOOK_REPL"),
        typed_stream(StreamCode::kFortsPosRepl, "FORTS_POS_REPL"),
        typed_stream(StreamCode::kFortsPartRepl, "FORTS_PART_REPL"),
        typed_stream(StreamCode::kFortsRefdataRepl, "FORTS_REFDATA_REPL"),
        typed_stream(StreamCode::kFortsSessionstateRepl, "FORTS_SESSIONSTATE_REPL"),
        typed_stream(StreamCode::kFortsInstrumentstateRepl, "FORTS_INSTRUMENTSTATE_REPL"),
        {.stream_code = StreamCode::kFortsAggrRepl,
         .label = "FORTS_AGGR_REPL",
         .settings = "p2repl://FORTS_AGGR20_REPL;scheme=|FILE|scheme/forts_scheme.ini|Aggr",
         .require_online = false},
        {.stream_code = kNoStreamCode,
         .label = "p2mqreply",
         .settings = "p2mqreply://qualification-replies",
         .require_online = false},
    };
    config.session.software_key.source = Plaza2CredentialSource::Env;
    config.session.software_key.env_var = "MOEX_PLAZA2_CGATE_SOFTWARE_KEY";
    config.session.open_publisher = true;
    config.session.publisher_settings = "p2mq://qualification-publisher";
    config.session.arm_state.test_network_armed = true;
    config.session.arm_state.test_session_armed = true;
    config.session.arm_state.test_plaza2_armed = true;
    config.session.process_timeout_ms = 0;
    config.target.isin = "RIH6";
    config.target.participant = "CL001";
    config.target.expected_position_account_type = 2;
    config.test_market_data_armed = true;
    config.max_aggr20_age_ms = 5000;
    return config;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "expected fake runtime library path\n";
            return 1;
        }

        using namespace moex::plaza2::test;
        const auto fixture_root = make_temp_directory("plaza2_trade_connectivity_qualifier");
        const auto fake_library = std::filesystem::path(argv[1]);
        const auto scheme = build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test");
        const auto fixture = materialize_runtime_fixture(fixture_root, fake_library, Plaza2Environment::Test, scheme);
        ::setenv("MOEX_PLAZA2_CGATE_SOFTWARE_KEY", "QUALIFIER-FAKE-KEY", 1);

        Plaza2TradeConnectivityQualifier qualifier(make_config(fixture));
        const auto start = qualifier.start();
        if (!start.ok) {
            for (const auto& line : qualifier.private_session().operator_log_lines()) {
                std::cerr << line << '\n';
            }
        }
        require(start.ok, start.message);
        const auto poll = qualifier.poll_once();
        require(poll.ok, poll.message);

        const auto& result = qualifier.qualification();
        if (!result.market_state_ready) {
            for (const auto& reason : result.failure_reasons) {
                std::cerr << "failure=" << reason << '\n';
            }
            std::cerr << "target=" << result.target_found << " refdata=" << result.target_refdata_present
                      << " session=" << result.target_session_status_available
                      << " instrument=" << result.target_instrument_status_available
                      << " aggr=" << result.target_aggr20_two_sided << " age=" << result.target_aggr20_age_ms
                      << " aggr_repl=" << result.target_aggr20_repl_id << "/" << result.target_aggr20_repl_rev << '\n';
            const auto aggr = qualifier.aggr20_projector().snapshot_for_isin(result.target_isin_id);
            if (aggr.has_value()) {
                std::cerr << "bid_repl=" << (aggr->top_bid.has_value() ? aggr->top_bid->repl_id : 0) << '\n';
                std::cerr << "ask_repl=" << (aggr->top_ask.has_value() ? aggr->top_ask->repl_id : 0) << '\n';
            }
        }
        require(result.connectivity_ready, "connectivity readiness should pass");
        require(result.market_state_ready, "market-state readiness should pass");
        require(result.account_state_ready, "account-state readiness should pass");
        require(result.publisher_ready, "publisher readiness should pass");
        require(result.add_order_qualified, "informational add-order qualification should pass");
        require(result.target_found && result.target_isin_id == 1001, "target identity mismatch");
        require(result.target_sess_id == 321 && result.target_session_status_available,
                "target session evidence mismatch");
        require(result.target_instrument_status_available && result.target_refdata_present,
                "target instrument evidence mismatch");
        require(result.target_aggr20_two_sided && result.target_aggr20_repl_id == 2102 &&
                    result.target_aggr20_repl_rev == 22,
                "target AGGR20 evidence mismatch");
        require(result.participant_limit_unique && result.participant_limits_set,
                "participant limit evidence mismatch");
        require(result.position_identity_exact && result.position_account_type == 2 && result.position_xpos == 4,
                "position identity evidence mismatch");
        require(result.p2mqreply_open && result.publisher_open, "publisher/reply listener evidence mismatch");

        const auto stop = qualifier.stop();
        require(stop.ok, "qualification fixture stop should succeed");
        ::unsetenv("MOEX_PLAZA2_CGATE_SOFTWARE_KEY");
        remove_tree(fixture_root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
