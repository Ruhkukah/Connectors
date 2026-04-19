#pragma once

#include "local_tcp_server.hpp"

#include "moex/twime_trade/transport/twime_tcp_transport.hpp"

#include "twime_trade_test_support.hpp"

#include <chrono>
#include <functional>
#include <thread>

namespace moex::twime_trade::test {

inline void require_tcp_open(transport::TwimeTcpTransport& tcp_transport, int max_polls = 64) {
    std::vector<std::byte> buffer(256);
    for (int attempt = 0; attempt < max_polls; ++attempt) {
        if (tcp_transport.state() == transport::TwimeTransportState::Open) {
            return;
        }
        const auto result = tcp_transport.poll_read(buffer);
        if (result.event == transport::TwimeTransportEvent::OpenSucceeded &&
            tcp_transport.state() == transport::TwimeTransportState::Open) {
            return;
        }
        if (result.status == transport::TwimeTransportStatus::Fault ||
            result.status == transport::TwimeTransportStatus::InvalidState) {
            moex::twime_sbe::test::require(false, "transport failed while waiting for TCP open");
        }
    }
    moex::twime_sbe::test::require(false, "transport did not reach Open state");
}

inline std::vector<std::byte>
wait_for_server_bytes(moex::test::LocalTcpServer& server, std::size_t expected_size,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    return server.receive_exact(expected_size, timeout);
}

inline transport::TwimeTransportPollResult poll_transport_until(
    transport::TwimeTcpTransport& tcp_transport,
    const std::function<bool(const transport::TwimeTransportPollResult&, const transport::TwimeTcpTransport&)>&
        predicate,
    int max_polls = 128) {
    std::vector<std::byte> buffer(512);
    transport::TwimeTransportPollResult last_result{};
    for (int attempt = 0; attempt < max_polls; ++attempt) {
        last_result = tcp_transport.poll_read(buffer);
        if (predicate(last_result, tcp_transport)) {
            return last_result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    moex::twime_sbe::test::require(false, "transport did not reach the expected poll condition");
    return last_result;
}

inline void pump_session_until(moex::twime_trade::TwimeSession& session,
                               const std::function<bool(const moex::twime_trade::TwimeSession&)>& predicate,
                               int max_polls = 128) {
    for (int attempt = 0; attempt < max_polls; ++attempt) {
        if (predicate(session)) {
            return;
        }
        session.poll_transport();
    }
    moex::twime_sbe::test::require(false, "session did not reach the expected condition");
}

} // namespace moex::twime_trade::test
