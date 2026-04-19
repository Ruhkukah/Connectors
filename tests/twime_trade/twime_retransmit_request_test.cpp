#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::TwimeSessionConfig config;
        config.session_id = "retransmit_request_test";
        moex::twime_trade::TwimeFakeTransport transport;
        moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
        moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
        moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
        moex::twime_trade::test::script_message(transport, ack);
        session.poll_transport();
        static_cast<void>(session.drain_events());

        session.apply_command({
            .type = moex::twime_trade::TwimeSessionCommandType::RequestRetransmit,
            .from_seq_no = 21,
            .count = 5,
        });

        const auto last = session.outbound_journal().last_n(1).front();
        moex::twime_sbe::test::require(last.message_name == "RetransmitRequest", "wrong outbound message");
        moex::twime_sbe::test::require(last.cert_log_line.find("FromSeqNo=21") != std::string::npos,
                                       "RetransmitRequest cert log missing FromSeqNo");
        moex::twime_sbe::test::require(last.cert_log_line.find("Count=5") != std::string::npos,
                                       "RetransmitRequest cert log missing Count");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
