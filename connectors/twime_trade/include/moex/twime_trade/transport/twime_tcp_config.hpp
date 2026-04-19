#pragma once

#include "moex/twime_trade/transport/twime_reconnect_policy.hpp"
#include "moex/twime_trade/transport/twime_tcp_endpoint.hpp"

#include <cstddef>

namespace moex::twime_trade::transport {

enum class TwimeTcpEnvironment {
    Replay,
    Test,
    Prod,
};

struct TwimeTcpBufferPolicy {
    std::size_t max_inbound_bytes{1U << 20};
    std::size_t max_outbound_bytes{1U << 20};
    std::size_t read_chunk_bytes{64U << 10};
    std::size_t write_chunk_bytes{64U << 10};
};

struct TwimeTcpConfig {
    TwimeTcpEnvironment environment{TwimeTcpEnvironment::Test};
    TwimeTcpEndpoint endpoint{};
    TwimeReconnectPolicy reconnect_policy{};
    TwimeTcpBufferPolicy buffer_policy{};
};

} // namespace moex::twime_trade::transport
