#include "moex/plaza2/cgate/plaza2_trade_connectivity_qualifier.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <algorithm>
#include <array>
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
        .settings = "p2repl://" + std::string(name),
        .listener_url_mode = "negotiated",
    };
}

Plaza2TradeConnectivityQualifierConfig make_config(const moex::plaza2::test::RuntimeFixturePaths& fixture) {
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
         .settings = "p2repl://FORTS_AGGR20_REPL",
         .listener_url_mode = "negotiated",
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
        require(result.terminal == Plaza2QualificationTerminal::Ready,
                "a fully qualified run must classify its terminal receipt as READY");
        require(result.target_found && result.target_isin_id == 1001, "target identity mismatch");
        require(result.target_sess_id == 321 && result.target_session_status_available,
                "target session evidence mismatch");
        require(result.target_session_status == 1 && result.target_session_add_capable,
                "target session should be add-capable only at public_state=1");
        require(result.target_instrument_status_available && result.target_refdata_present,
                "target instrument evidence mismatch");
        require(result.target_instrument_status == 1 && result.target_instrument_add_capable,
                "target instrument should be add-capable only at public_state=1");
        require(result.target_aggr20_two_sided && result.target_aggr20_uncrossed &&
                    result.target_aggr20_repl_id == 2102 && result.target_aggr20_repl_rev == 22,
                "target AGGR20 evidence mismatch");
        require(result.participant_limit_unique && result.participant_limits_set,
                "participant limit evidence mismatch");
        require(result.position_identity_exact && result.position_account_type == 2 && result.position_xpos == 4,
                "position identity evidence mismatch");
        require(result.p2mqreply_open && result.publisher_open, "publisher/reply listener evidence mismatch");

        const std::array required_streams = {
            std::string_view{"FORTS_TRADE_REPL"},
            std::string_view{"FORTS_USERORDERBOOK_REPL"},
            std::string_view{"FORTS_POS_REPL"},
            std::string_view{"FORTS_PART_REPL"},
            std::string_view{"FORTS_REFDATA_REPL"},
            std::string_view{"FORTS_SESSIONSTATE_REPL"},
            std::string_view{"FORTS_INSTRUMENTSTATE_REPL"},
            std::string_view{"FORTS_AGGR_REPL"},
        };
        for (const auto stream_name : required_streams) {
            const auto it = std::find_if(qualifier.private_session().health_snapshot().streams.begin(),
                                         qualifier.private_session().health_snapshot().streams.end(),
                                         [&](const auto& stream) { return stream.stream_name == stream_name; });
            require(it != qualifier.private_session().health_snapshot().streams.end(),
                    "bare listener should be present in health receipt");
            require(it->listener_url_mode == "negotiated" && it->created && it->opened && it->online &&
                        it->snapshot_complete,
                    "bare listener should negotiate a scheme and complete its snapshot");
        }
        const auto reply_it = std::find_if(qualifier.private_session().health_snapshot().streams.begin(),
                                           qualifier.private_session().health_snapshot().streams.end(),
                                           [](const auto& stream) { return stream.stream_name == "p2mqreply"; });
        require(reply_it != qualifier.private_session().health_snapshot().streams.end() && reply_it->created &&
                    reply_it->opened,
                "p2mqreply should remain independently open");

        {
            ::setenv("MOEX_FAKE_REMOVE_TARGET_AFTER_READY", "1", 1);
            Plaza2TradeConnectivityQualifier refresh_qualifier(make_config(fixture));
            require(refresh_qualifier.start().ok, "target-removal refresh start should succeed");
            require(refresh_qualifier.poll_once().ok, "target-removal initial poll should succeed");
            const auto& present = refresh_qualifier.qualification();
            require(present.target_found && present.target_refdata_present,
                    "target-removal regression must begin with target evidence");
            require(refresh_qualifier.poll_once().ok, "target-removal invalidation poll should succeed");
            const auto& removed = refresh_qualifier.qualification();
            require(!removed.target_found && removed.target_isin_id == 0 && removed.target_sess_id == 0 &&
                        !removed.target_current_session_member && removed.target_min_step.empty() &&
                        removed.target_trade_mode_id == 0 && !removed.target_session_status_available &&
                        !removed.target_instrument_status_available && !removed.target_refdata_present &&
                        !removed.target_aggr20_two_sided && !removed.target_aggr20_uncrossed &&
                        removed.target_aggr20_repl_id == 0 && removed.target_aggr20_repl_rev == 0 &&
                        removed.applicable_position_count == 0 && !removed.position_identity_exact &&
                        removed.position_account_type == 0 && removed.position_xpos == 0,
                    "refresh must clear all target-derived evidence after target removal");
            ::unsetenv("MOEX_FAKE_REMOVE_TARGET_AFTER_READY");
        }

        const auto stop = qualifier.stop();
        require(stop.ok, "qualification fixture stop should succeed");

        {
            auto wrong_config = make_config(fixture);
            for (auto& stream : wrong_config.session.streams) {
                if (stream.stream_code == StreamCode::kFortsPartRepl) {
                    stream.settings = "p2repl://FORTS_PART_REPL;scheme=|FILE|scheme/forts_scheme.ini|WRONG_SCHEME";
                    stream.listener_url_mode = "explicit_override";
                }
            }
            ::setenv("MOEX_FAKE_WRONG_SCHEME_OVERRIDE", "1", 1);
            Plaza2TradeConnectivityQualifier wrong_qualifier(std::move(wrong_config));
            const auto wrong_start = wrong_qualifier.start();
            require(!wrong_start.ok, "an explicitly wrong scheme override must fail closed");
            const auto& wrong_health = wrong_qualifier.private_session().health_snapshot();
            require(wrong_health.failing_listener == "FORTS_PART_REPL" &&
                        wrong_health.last_error_code != Plaza2ErrorCode::None,
                    "wrong scheme failure should identify the affected listener and runtime error");
            ::unsetenv("MOEX_FAKE_WRONG_SCHEME_OVERRIDE");
        }

        {
            ::setenv("MOEX_FAKE_AGGR_ONE_SIDED", "1", 1);
            Plaza2TradeConnectivityQualifier one_sided_qualifier(make_config(fixture));
            require(one_sided_qualifier.start().ok, "one-sided-book qualification start should succeed");
            require(one_sided_qualifier.poll_once().ok, "one-sided-book qualification poll should succeed");
            const auto& one_sided = one_sided_qualifier.qualification();
            require(!one_sided.target_aggr20_two_sided && !one_sided.target_aggr20_uncrossed,
                    "one-sided AGGR20 fixture should fail two-sided and uncrossed validation");
            require(std::find(one_sided.failure_reasons.begin(), one_sided.failure_reasons.end(),
                              "target AGGR20 BBO is not two-sided") != one_sided.failure_reasons.end(),
                    "one-sided AGGR20 BBO should report the missing-side failure");
            require(std::find(one_sided.failure_reasons.begin(), one_sided.failure_reasons.end(),
                              "target AGGR20 BBO is crossed or locked") == one_sided.failure_reasons.end(),
                    "one-sided AGGR20 BBO must not be described as crossed or locked");
            ::unsetenv("MOEX_FAKE_AGGR_ONE_SIDED");
        }

        {
            ::setenv("MOEX_FAKE_AGGR_CROSSED", "1", 1);
            Plaza2TradeConnectivityQualifier crossed_qualifier(make_config(fixture));
            require(crossed_qualifier.start().ok, "crossed-book qualification start should succeed");
            require(crossed_qualifier.poll_once().ok, "crossed-book qualification poll should succeed");
            const auto& crossed = crossed_qualifier.qualification();
            require(crossed.target_aggr20_two_sided && !crossed.target_aggr20_uncrossed,
                    "crossed AGGR20 fixture should remain two-sided but fail uncrossed validation");
            require(!crossed.market_state_ready && !crossed.add_order_qualified,
                    "crossed AGGR20 BBO must fail market and add-order qualification");
            require(std::find(crossed.failure_reasons.begin(), crossed.failure_reasons.end(),
                              "target AGGR20 BBO is crossed or locked") != crossed.failure_reasons.end(),
                    "crossed AGGR20 BBO should have an explicit failure reason");
            ::unsetenv("MOEX_FAKE_AGGR_CROSSED");
        }

        const auto run_status_case = [&](const char* flag, const char* label) {
            ::setenv(flag, "1", 1);
            {
                Plaza2TradeConnectivityQualifier status_qualifier(make_config(fixture));
                const auto status_start = status_qualifier.start();
                require(status_start.ok, std::string(label) + " start should succeed");
                const auto status_poll = status_qualifier.poll_once();
                require(status_poll.ok, std::string(label) + " poll should succeed");
                const auto& status = status_qualifier.qualification();
                require(status.target_found, std::string(label) + " target should remain present");
                require(!status.market_state_ready && !status.add_order_qualified,
                        std::string(label) + " must fail market readiness");
                require(status.terminal == Plaza2QualificationTerminal::NotReady,
                        std::string(label) + " must classify its bounded receipt as NOT_READY");
            }
            ::unsetenv(flag);
        };

        run_status_case("MOEX_FAKE_SCHEDULED_SESSION", "scheduled session");
        {
            ::setenv("MOEX_FAKE_NONTRADABLE_INSTRUMENT", "1", 1);
            Plaza2TradeConnectivityQualifier status_qualifier(make_config(fixture));
            const auto status_start = status_qualifier.start();
            require(status_start.ok, "non-tradable instrument start should succeed");
            const auto status_poll = status_qualifier.poll_once();
            require(status_poll.ok, "non-tradable instrument poll should succeed");
            const auto& status = status_qualifier.qualification();
            require(status.target_instrument_status_available && status.target_instrument_status == 0,
                    "non-tradable instrument raw public_state should be preserved");
            require(!status.target_instrument_add_capable && !status.market_state_ready && !status.add_order_qualified,
                    "non-tradable instrument must fail market readiness");
            ::unsetenv("MOEX_FAKE_NONTRADABLE_INSTRUMENT");
        }

        const auto run_aggr_invalidation_case = [&](const char* flag, const char* label) {
            ::setenv(flag, "1", 1);
            {
                Plaza2TradeConnectivityQualifier aggr_qualifier(make_config(fixture));
                const auto aggr_start = aggr_qualifier.start();
                require(aggr_start.ok, std::string(label) + " start should succeed");
                const auto first_poll = aggr_qualifier.poll_once();
                require(first_poll.ok && aggr_qualifier.qualification().target_aggr20_two_sided,
                        std::string(label) + " should initially receive a two-sided BBO");
                const auto second_poll = aggr_qualifier.poll_once();
                require(second_poll.ok, std::string(label) + " invalidation poll should succeed");
                const auto& invalidated = aggr_qualifier.qualification();
                require(!invalidated.market_state_ready && !invalidated.target_aggr20_two_sided &&
                            !invalidated.add_order_qualified,
                        std::string(label) + " must fail closed after AGGR20 invalidation");
                require(!aggr_qualifier.aggr20_projector().snapshot_for_isin(1001).has_value(),
                        std::string(label) + " must clear the old AGGR20 snapshot");
            }
            ::unsetenv(flag);
        };

        run_aggr_invalidation_case("MOEX_FAKE_AGGR_LIFENUM_AFTER_READY", "AGGR20 lifenum change");
        run_aggr_invalidation_case("MOEX_FAKE_AGGR_CLOSE_AFTER_READY", "AGGR20 close");

        {
            ::setenv("MOEX_FAKE_DISABLE_REPLY_LISTENER", "1", 1);
            Plaza2TradeConnectivityQualifier failed_qualifier(make_config(fixture));
            const auto failed_start = failed_qualifier.start();
            require(!failed_start.ok, "reply-listener failure should fail start");
            require(failed_qualifier.qualification().terminal == Plaza2QualificationTerminal::Error,
                    "a listener start failure must classify its receipt as ERROR");
            const auto& failed_health = failed_qualifier.private_session().health_snapshot();
            require(failed_health.failing_listener == "p2mqreply",
                    "failure receipt should identify the failing p2mqreply listener");
            require(failed_health.last_error_code != Plaza2ErrorCode::None &&
                        failed_health.last_error_runtime_code != 0 && !failed_health.last_error.empty(),
                    "failure receipt should preserve typed runtime error details");
            bool typed_listener_opened = false;
            for (const auto& stream : failed_health.streams) {
                if (stream.stream_name == "FORTS_TRADE_REPL") {
                    typed_listener_opened = stream.created && stream.opened;
                }
            }
            require(typed_listener_opened, "failure receipt should preserve previously created/opened typed listeners");
            ::unsetenv("MOEX_FAKE_DISABLE_REPLY_LISTENER");
        }

        ::unsetenv("MOEX_PLAZA2_CGATE_SOFTWARE_KEY");
        remove_tree(fixture_root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
