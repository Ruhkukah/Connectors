#pragma once

#include "moex/twime_trade/twime_session.hpp"

#include "twime_test_support.hpp"

namespace moex::twime_trade::test {

inline moex::twime_sbe::TwimeEncodeRequest make_request(std::string_view message_name) {
    return moex::twime_sbe::test::make_sample_request(message_name);
}

inline void script_message(TwimeFakeTransport& transport, const moex::twime_sbe::TwimeEncodeRequest& request,
                           std::uint64_t sequence_number = 0, bool consumes_sequence = false) {
    moex::twime_sbe::TwimeCodec codec;
    std::vector<std::byte> bytes;
    moex::twime_sbe::test::require(
        codec.encode_message(request, bytes) == moex::twime_sbe::TwimeDecodeError::Ok,
        "failed to encode fake-transport message");
    transport.script_inbound_frame(TwimeFakeTransportFrame{
        .bytes = bytes,
        .sequence_number = sequence_number,
        .consumes_sequence = consumes_sequence,
    });
}

inline const TwimeSessionEvent* find_last_event(const std::vector<TwimeSessionEvent>& events, TwimeSessionEventType type) {
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (it->type == type) {
            return &*it;
        }
    }
    return nullptr;
}

inline void require_state(TwimeSessionState actual, TwimeSessionState expected, const std::string& message) {
    moex::twime_sbe::test::require(actual == expected, message);
}

}  // namespace moex::twime_trade::test
