#include "moex/twime_trade/twime_cert_scenario_runner.hpp"

#include "twime_test_support.hpp"

#include <iostream>

int main() {
    try {
        for (const auto* scenario_id : {"twime_session_establish", "twime_session_establish_ack_sets_inbound_counter",
                                        "twime_client_sequence_heartbeat_null_nextseqno", "twime_terminate",
                                        "twime_terminate_requires_inbound_terminate", "twime_retransmit_last5",
                                        "twime_normal_retransmit_limit_10", "twime_full_recovery_retransmit_limit_1000",
                                        "twime_heartbeat_rate_violation", "twime_message_counter_reset",
                                        "twime_business_reject_non_recoverable"}) {
            const auto scenario = moex::twime_trade::TwimeCertScenarioRunner::builtin(scenario_id);
            moex::twime_sbe::test::require(scenario.has_value(), "builtin TWIME scenario missing");
            const auto result = moex::twime_trade::TwimeCertScenarioRunner{}.run(*scenario);
            moex::twime_sbe::test::require(result.scenario_id == scenario_id, "scenario id mismatch");
            moex::twime_sbe::test::require(result.error_message.empty(), "scenario returned unexpected error");
            moex::twime_sbe::test::require(!result.cert_log_lines.empty(), "cert log lines missing");
        }

        const auto establish = moex::twime_trade::TwimeCertScenarioRunner::builtin("twime_session_establish");
        const auto establish_result = moex::twime_trade::TwimeCertScenarioRunner{}.run(*establish);
        moex::twime_sbe::test::require(establish_result.final_state == moex::twime_trade::TwimeSessionState::Active,
                                       "session establish scenario did not finish Active");

        const auto terminate = moex::twime_trade::TwimeCertScenarioRunner::builtin("twime_terminate");
        const auto terminate_result = moex::twime_trade::TwimeCertScenarioRunner{}.run(*terminate);
        moex::twime_sbe::test::require(terminate_result.final_state == moex::twime_trade::TwimeSessionState::Terminated,
                                       "terminate scenario did not finish Terminated");

        const auto terminate_requires =
            moex::twime_trade::TwimeCertScenarioRunner::builtin("twime_terminate_requires_inbound_terminate");
        const auto terminate_requires_result = moex::twime_trade::TwimeCertScenarioRunner{}.run(*terminate_requires);
        moex::twime_sbe::test::require(terminate_requires_result.final_state ==
                                           moex::twime_trade::TwimeSessionState::Faulted,
                                       "terminate_requires_inbound_terminate should finish Faulted");

        const auto message_counter_reset =
            moex::twime_trade::TwimeCertScenarioRunner::builtin("twime_message_counter_reset");
        const auto message_counter_reset_result =
            moex::twime_trade::TwimeCertScenarioRunner{}.run(*message_counter_reset);
        moex::twime_sbe::test::require(message_counter_reset_result.final_state ==
                                           moex::twime_trade::TwimeSessionState::Active,
                                       "message_counter_reset should finish Active");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
