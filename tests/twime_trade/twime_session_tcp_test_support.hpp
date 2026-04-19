#pragma once

#include "local_tcp_server.hpp"

#include "moex/twime_trade/twime_session.hpp"

#include "transport/twime_tcp_test_support.hpp"
#include "twime_trade_test_support.hpp"

#include <chrono>

namespace moex::twime_trade::test {

inline moex::twime_sbe::TwimeEncodeRequest make_expected_establish_request(const TwimeSessionConfig& config,
                                                                           const TwimeFakeClock& clock) {
    moex::twime_sbe::TwimeEncodeRequest request;
    request.message_name = "Establish";
    request.template_id = 5000;
    request.fields = {
        {"Timestamp", moex::twime_sbe::TwimeFieldValue::timestamp(clock.now_ns())},
        {"KeepaliveInterval", moex::twime_sbe::TwimeFieldValue::delta_millisecs(config.keepalive_interval_ms)},
        {"Credentials", moex::twime_sbe::TwimeFieldValue::string(config.credentials)},
    };
    return request;
}

inline std::vector<std::byte> wait_for_establish(moex::test::LocalTcpServer& server, TwimeSession& session,
                                                 const TwimeSessionConfig& config, const TwimeFakeClock& clock) {
    const auto expected = encode_bytes(make_expected_establish_request(config, clock));
    std::vector<std::byte> bytes;
    bytes.reserve(expected.size());
    for (int attempt = 0; attempt < 128 && bytes.size() < expected.size(); ++attempt) {
        session.poll_transport();
        const auto chunk = server.receive_up_to(expected.size() - bytes.size(), std::chrono::milliseconds(25));
        bytes.insert(bytes.end(), chunk.begin(), chunk.end());
    }
    moex::twime_sbe::test::require(bytes.size() == expected.size(), "session did not emit full Establish over TCP");
    return bytes;
}

inline void complete_establish_handshake(TwimeSession& session, moex::test::LocalTcpServer& server,
                                         std::uint64_t next_seq_no = 11,
                                         std::size_t ack_chunk_size = static_cast<std::size_t>(-1)) {
    auto ack = make_request("EstablishmentAck");
    for (auto& field : ack.fields) {
        if (field.name == "NextSeqNo") {
            field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(next_seq_no);
        }
    }
    server.send_bytes(encode_bytes(ack), ack_chunk_size);
    pump_session_until(session,
                       [](const TwimeSession& candidate) { return candidate.state() == TwimeSessionState::Active; });
}

} // namespace moex::twime_trade::test
