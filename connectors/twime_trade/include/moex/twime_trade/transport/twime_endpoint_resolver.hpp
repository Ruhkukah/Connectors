#pragma once

#include "moex/twime_trade/transport/twime_tcp_endpoint.hpp"
#include "moex/twime_trade/transport/twime_transport_errors.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <sys/socket.h>

namespace moex::twime_trade::transport {

struct TwimeResolvedEndpoint {
    int family{AF_UNSPEC};
    sockaddr_storage address{};
    socklen_t address_size{0};
    std::string resolved_host;
};

struct TwimeEndpointResolveResult {
    std::optional<TwimeResolvedEndpoint> endpoint;
    TwimeTransportErrorCode error_code{TwimeTransportErrorCode::None};
};

[[nodiscard]] bool twime_is_explicit_loopback_host(std::string_view host) noexcept;
[[nodiscard]] bool twime_is_placeholder_host(std::string_view host) noexcept;
[[nodiscard]] bool twime_host_looks_production_like(std::string_view host) noexcept;
[[nodiscard]] bool twime_host_is_private_nonlocal_ipv4(std::string_view host) noexcept;
[[nodiscard]] TwimeEndpointResolveResult resolve_twime_endpoint(const TwimeTcpEndpoint& endpoint);

} // namespace moex::twime_trade::transport
