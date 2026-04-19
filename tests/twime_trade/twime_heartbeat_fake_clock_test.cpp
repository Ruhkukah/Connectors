#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::TwimeSessionConfig config;
        config.session_id = "heartbeat_test";
        config.keepalive_interval_ms = 1000;
        moex::twime_trade::TwimeFakeTransport transport;
        moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
        moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
        moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
        for (auto& field : ack.fields) {
            if (field.name == "KeepaliveInterval") {
                field.value = moex::twime_sbe::TwimeFieldValue::delta_millisecs(1500);
            }
            if (field.name == "NextSeqNo") {
                field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(11);
            }
        }
        moex::twime_trade::test::script_message(transport, ack);
        session.poll_transport();
        static_cast<void>(session.drain_events());

        clock.advance(1499);
        session.on_timer_tick();
        moex::twime_sbe::test::require(session.outbound_journal().size() == 1,
                                       "heartbeat sent before acknowledged keepalive deadline");

        clock.advance(1);
        session.on_timer_tick();
        moex::twime_sbe::test::require(session.outbound_journal().size() == 2,
                                       "heartbeat not sent after acknowledged keepalive deadline");

        const auto heartbeat =
            moex::twime_trade::test::decode_bytes(session.outbound_journal().last_n(1).front().bytes);
        const auto* next_seq = moex::twime_trade::test::find_field(heartbeat, "NextSeqNo");
        moex::twime_sbe::test::require(next_seq != nullptr, "Sequence heartbeat NextSeqNo field missing");
        moex::twime_sbe::test::require(next_seq->metadata->nullable && next_seq->metadata->has_null_value,
                                       "Sequence NextSeqNo is not nullable");
        moex::twime_sbe::test::require(next_seq->value.unsigned_value == next_seq->metadata->null_value,
                                       "client Sequence heartbeat must encode NextSeqNo as null");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
