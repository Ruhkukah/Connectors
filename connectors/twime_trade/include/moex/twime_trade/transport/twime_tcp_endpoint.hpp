#pragma once

#include <cstdint>
#include <string>

namespace moex::twime_trade::transport {

struct TwimeTcpEndpoint {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
    bool allow_non_loopback{false};
    bool allow_non_localhost_dns{false};
};

} // namespace moex::twime_trade::transport
