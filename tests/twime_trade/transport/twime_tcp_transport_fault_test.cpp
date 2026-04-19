#include "moex/twime_trade/transport/twime_tcp_transport.hpp"

#include "twime_tcp_test_support.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::uint16_t reserve_unused_local_port() {
    moex::test::LocalTcpServer server;
    const auto port = server.port();
    server.stop();
    return port;
}

} // namespace

int main() {
    try {
        const auto require_local_only_violation = [](std::string_view host, bool allow_non_loopback,
                                                     bool allow_non_localhost_dns) {
            auto config = moex::twime_trade::test::make_local_tcp_config(12001);
            config.endpoint.host = std::string(host);
            config.endpoint.allow_non_loopback = allow_non_loopback;
            config.endpoint.allow_non_localhost_dns = allow_non_localhost_dns;

            moex::twime_trade::transport::TwimeTcpTransport transport(config);
            const auto open_result = transport.open();
            moex::twime_sbe::test::require(open_result.status ==
                                               moex::twime_trade::transport::TwimeTransportStatus::Fault,
                                           "TCP transport must reject non-loopback endpoints in Phase 2D");
            moex::twime_sbe::test::require(
                open_result.error_code == moex::twime_trade::transport::TwimeTransportErrorCode::LocalOnlyViolation,
                "TCP transport non-loopback endpoint must fail with LocalOnlyViolation");
        };

        const auto require_exact_loopback_host_allowed = [](std::string_view host) {
            auto config = moex::twime_trade::test::make_local_tcp_config(reserve_unused_local_port());
            config.endpoint.host = std::string(host);

            moex::twime_trade::transport::TwimeTcpTransport transport(config);
            const auto open_result = transport.open();
            moex::twime_sbe::test::require(
                open_result.error_code != moex::twime_trade::transport::TwimeTransportErrorCode::LocalOnlyViolation,
                "exact loopback hosts must pass Phase 2D host validation");
        };

        {
            auto config = moex::twime_trade::test::make_local_tcp_config(reserve_unused_local_port());
            moex::twime_trade::transport::TwimeTcpTransport transport(config);
            const auto open_result = transport.open();
            if (open_result.status == moex::twime_trade::transport::TwimeTransportStatus::WouldBlock) {
                const auto poll_result =
                    moex::twime_trade::test::poll_transport_until(transport, [](const auto& candidate, const auto&) {
                        return candidate.status == moex::twime_trade::transport::TwimeTransportStatus::Fault;
                    });
                moex::twime_sbe::test::require(
                    poll_result.status == moex::twime_trade::transport::TwimeTransportStatus::Fault,
                    "TCP transport did not fault after connection attempt to an unopened local port");
            } else {
                moex::twime_sbe::test::require(open_result.status ==
                                                   moex::twime_trade::transport::TwimeTransportStatus::Fault,
                                               "TCP transport unexpected status for unopened local port");
            }
        }

        {
            auto config = moex::twime_trade::test::make_local_tcp_config(12000);
            config.environment = moex::twime_trade::transport::TwimeTcpEnvironment::Prod;
            moex::twime_trade::transport::TwimeTcpTransport transport(config);
            const auto open_result = transport.open();
            moex::twime_sbe::test::require(open_result.status ==
                                               moex::twime_trade::transport::TwimeTransportStatus::Fault,
                                           "TCP transport must reject non-test environments");
            moex::twime_sbe::test::require(
                open_result.error_code == moex::twime_trade::transport::TwimeTransportErrorCode::EnvironmentBlocked,
                "TCP transport non-test environment error mismatch");
        }

        {
            require_exact_loopback_host_allowed("127.0.0.1");
            require_exact_loopback_host_allowed("::1");
            require_exact_loopback_host_allowed("localhost");

            const std::vector<std::string_view> blocked_hosts = {
                "192.168.1.10", "10.0.0.5",  "172.16.0.7", "8.8.8.8",
                "example.com",  "foo.local", "127.0.0.2",  "::ffff:127.0.0.1",
            };

            for (const auto host : blocked_hosts) {
                require_local_only_violation(host, false, false);
                require_local_only_violation(host, true, false);
                require_local_only_violation(host, false, true);
                require_local_only_violation(host, true, true);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
