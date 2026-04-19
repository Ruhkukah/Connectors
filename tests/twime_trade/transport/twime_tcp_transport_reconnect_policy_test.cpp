#include "moex/twime_trade/transport/twime_reconnect_policy.hpp"
#include "moex/twime_trade/transport/twime_tcp_transport.hpp"

#include "twime_tcp_test_support.hpp"

#include <iostream>

int main() {
    try {
        using clock = std::chrono::steady_clock;

        const auto base = clock::now();
        moex::twime_trade::transport::TwimeReconnectPolicy policy;
        moex::twime_sbe::test::require(
            !moex::twime_trade::transport::twime_reconnect_allowed(base + std::chrono::milliseconds(999), base, policy),
            "reconnect must be suppressed before 1000ms");
        moex::twime_sbe::test::require(
            moex::twime_trade::transport::twime_reconnect_allowed(base + std::chrono::milliseconds(1000), base, policy),
            "reconnect must be allowed at or after 1000ms");

        auto config = moex::twime_trade::test::make_local_tcp_config(40123);
        moex::twime_trade::transport::TwimeTcpTransport transport(config);
        clock::time_point now = base;
        transport.set_time_source([&now] { return now; });

        static_cast<void>(transport.open());
        static_cast<void>(
            moex::twime_trade::test::poll_transport_until(transport, [](const auto& candidate, const auto&) {
                return candidate.status == moex::twime_trade::transport::TwimeTransportStatus::Fault;
            }));
        now += std::chrono::milliseconds(200);
        const auto suppressed = transport.open();
        moex::twime_sbe::test::require(suppressed.event ==
                                           moex::twime_trade::transport::TwimeTransportEvent::ReconnectSuppressed,
                                       "TCP reconnect before 1000ms was not suppressed");
        moex::twime_sbe::test::require(suppressed.error_code ==
                                           moex::twime_trade::transport::TwimeTransportErrorCode::ReconnectTooSoon,
                                       "TCP reconnect suppression error mismatch");

        now += std::chrono::milliseconds(1000);
        const auto allowed = transport.open();
        moex::twime_sbe::test::require(allowed.event !=
                                           moex::twime_trade::transport::TwimeTransportEvent::ReconnectSuppressed,
                                       "TCP reconnect at or after 1000ms was still suppressed");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
