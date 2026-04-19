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
    moex::twime_sbe::test::require(codec.encode_message(request, bytes) == moex::twime_sbe::TwimeDecodeError::Ok,
                                   "failed to encode fake-transport message");
    transport.script_inbound_frame(TwimeFakeTransportFrame{
        .bytes = bytes,
        .sequence_number = sequence_number,
        .consumes_sequence = consumes_sequence,
    });
}

inline moex::twime_sbe::DecodedTwimeMessage decode_bytes(std::span<const std::byte> bytes) {
    moex::twime_sbe::TwimeCodec codec;
    moex::twime_sbe::DecodedTwimeMessage decoded;
    moex::twime_sbe::test::require(codec.decode_message(bytes, decoded) == moex::twime_sbe::TwimeDecodeError::Ok,
                                   "failed to decode TWIME frame");
    return decoded;
}

inline const moex::twime_sbe::DecodedTwimeField* find_field(const moex::twime_sbe::DecodedTwimeMessage& message,
                                                            std::string_view name) {
    for (const auto& field : message.fields) {
        if (field.metadata != nullptr && field.metadata->name == name) {
            return &field;
        }
    }
    return nullptr;
}

inline const TwimeSessionEvent* find_last_event(const std::vector<TwimeSessionEvent>& events,
                                                TwimeSessionEventType type) {
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

} // namespace moex::twime_trade::test
