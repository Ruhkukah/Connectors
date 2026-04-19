#pragma once

#include "moex/twime_trade/twime_session.hpp"
#include "moex/twime_trade/transport/twime_tcp_config.hpp"

#include "twime_test_support.hpp"

#include "moex/twime_sbe/twime_frame_assembler.hpp"

#include <span>

namespace moex::twime_trade::test {

inline moex::twime_sbe::TwimeEncodeRequest make_request(std::string_view message_name) {
    return moex::twime_sbe::test::make_sample_request(message_name);
}

inline std::vector<std::byte> encode_bytes(const moex::twime_sbe::TwimeEncodeRequest& request) {
    moex::twime_sbe::TwimeCodec codec;
    std::vector<std::byte> bytes;
    moex::twime_sbe::test::require(codec.encode_message(request, bytes) == moex::twime_sbe::TwimeDecodeError::Ok,
                                   "failed to encode TWIME message");
    return bytes;
}

inline void script_message(TwimeFakeTransport& transport, const moex::twime_sbe::TwimeEncodeRequest& request,
                           std::uint64_t sequence_number = 0, bool consumes_sequence = false) {
    const auto bytes = encode_bytes(request);
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

inline std::vector<std::byte> concatenate_frames(const std::vector<std::vector<std::byte>>& frames) {
    std::size_t total_size = 0;
    for (const auto& frame : frames) {
        total_size += frame.size();
    }

    std::vector<std::byte> bytes;
    bytes.reserve(total_size);
    for (const auto& frame : frames) {
        bytes.insert(bytes.end(), frame.begin(), frame.end());
    }
    return bytes;
}

inline std::vector<moex::twime_sbe::DecodedTwimeMessage> decode_streamed_frames(std::span<const std::byte> bytes,
                                                                                std::size_t max_frame_size = 4096) {
    moex::twime_sbe::TwimeFrameAssembler assembler(max_frame_size);
    const auto feed_result = assembler.feed(bytes);
    moex::twime_sbe::test::require(feed_result.error == moex::twime_sbe::TwimeDecodeError::Ok,
                                   "failed to assemble TWIME frames from byte stream");

    std::vector<moex::twime_sbe::DecodedTwimeMessage> decoded_messages;
    while (assembler.has_frame()) {
        decoded_messages.push_back(decode_bytes(assembler.pop_frame().bytes));
    }
    return decoded_messages;
}

inline moex::twime_trade::transport::TwimeTcpConfig make_local_tcp_config(std::uint16_t port) {
    moex::twime_trade::transport::TwimeTcpConfig config;
    config.environment = moex::twime_trade::transport::TwimeTcpEnvironment::Test;
    config.endpoint.host = "127.0.0.1";
    config.endpoint.port = port;
    config.endpoint.allow_non_loopback = false;
    config.endpoint.allow_non_localhost_dns = false;
    return config;
}

} // namespace moex::twime_trade::test
