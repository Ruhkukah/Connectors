#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace moex::twime_trade::transport {

struct TwimeTcpEndpoint {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
    bool allow_non_loopback{false};
    bool allow_non_localhost_dns{false};
};

[[nodiscard]] bool twime_is_explicit_loopback_host(std::string_view host) noexcept;
[[nodiscard]] bool twime_host_looks_production_like(std::string_view host) noexcept;

} // namespace moex::twime_trade::transport
