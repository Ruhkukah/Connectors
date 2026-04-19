#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::TwimeSessionConfig config;
        config.session_id = "establish_test";
        moex::twime_trade::TwimeFakeTransport transport;
        moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
        moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
        moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        moex::twime_sbe::test::require(session.state() == moex::twime_trade::TwimeSessionState::Establishing,
                                       "session did not enter Establishing");
        moex::twime_sbe::test::require(session.outbound_journal().size() == 1, "Establish was not journaled");
        moex::twime_sbe::test::require(
            session.outbound_journal().last_n(1).front().message_name == "Establish", "first outbound is not Establish");

        auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
        for (auto& field : ack.fields) {
            if (field.name == "NextSeqNo") {
                field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(11);
            }
        }
        moex::twime_trade::test::script_message(transport, ack);
        session.poll_transport();

        moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Active,
                                               "session did not become Active after EstablishmentAck");
        moex::twime_sbe::test::require(session.sequence_state().next_outbound_seq() == 11,
                                       "outbound sequence was not restored from EstablishmentAck");
        moex::twime_sbe::test::require(!session.cert_log_lines().empty(), "cert log lines were not recorded");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
