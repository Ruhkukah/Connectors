#include "moex/twime_trade/twime_cert_scenario_runner.hpp"

#include "twime_test_support.hpp"

#include <iostream>

int main() {
    try {
        for (const auto* scenario_id : {"twime_session_establish", "twime_terminate", "twime_retransmit_last5",
                                        "twime_flood_reject", "twime_business_reject"}) {
            const auto scenario = moex::twime_trade::TwimeCertScenarioRunner::builtin(scenario_id);
            moex::twime_sbe::test::require(scenario.has_value(), "builtin TWIME scenario missing");
            const auto result = moex::twime_trade::TwimeCertScenarioRunner{}.run(*scenario);
            moex::twime_sbe::test::require(result.scenario_id == scenario_id, "scenario id mismatch");
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
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
