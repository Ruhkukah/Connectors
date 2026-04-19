#include "moex/twime_trade/transport/twime_endpoint_resolver.hpp"

#include "twime_test_support.hpp"

#include <iostream>

int main() {
    try {
        using namespace moex::twime_trade::transport;

        moex::twime_sbe::test::require(twime_is_explicit_loopback_host("127.0.0.1"), "127.0.0.1 must be loopback");
        moex::twime_sbe::test::require(twime_is_explicit_loopback_host("::1"), "::1 must be loopback");
        moex::twime_sbe::test::require(twime_is_explicit_loopback_host("localhost"), "localhost must be loopback");
        moex::twime_sbe::test::require(!twime_is_explicit_loopback_host("127.0.0.2"),
                                       "127.0.0.2 must not be explicit loopback in Phase 2E");

        moex::twime_sbe::test::require(twime_is_placeholder_host("TEST_ENDPOINT_HOST_PLACEHOLDER"),
                                       "placeholder host should be detected");
        moex::twime_sbe::test::require(twime_host_looks_production_like("twime.moex.example"),
                                       "production-like host heuristic mismatch");
        moex::twime_sbe::test::require(twime_host_is_private_nonlocal_ipv4("10.0.0.5"),
                                       "10.x private host detection mismatch");
        moex::twime_sbe::test::require(twime_host_is_private_nonlocal_ipv4("172.16.0.7"),
                                       "172.16.x private host detection mismatch");
        moex::twime_sbe::test::require(twime_host_is_private_nonlocal_ipv4("192.168.1.10"),
                                       "192.168.x private host detection mismatch");
        moex::twime_sbe::test::require(!twime_host_is_private_nonlocal_ipv4("127.0.0.1"),
                                       "127.0.0.1 must not be treated as private non-loopback");

        TwimeTcpEndpoint endpoint;
        endpoint.host = "127.0.0.1";
        endpoint.port = 19000;
        const auto resolved = resolve_twime_endpoint(endpoint);
        moex::twime_sbe::test::require(resolved.endpoint.has_value(),
                                       "loopback endpoint should resolve for TCP transport");

        endpoint.host = "TEST_ENDPOINT_HOST_PLACEHOLDER";
        const auto placeholder = resolve_twime_endpoint(endpoint);
        moex::twime_sbe::test::require(!placeholder.endpoint.has_value() &&
                                           placeholder.error_code == TwimeTransportErrorCode::InvalidConfiguration,
                                       "placeholder endpoint must not resolve to a concrete address");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
