#pragma once

#include <cstddef>

namespace moex::twime_trade::transport {

struct TwimeTransportMetrics {
    std::size_t open_calls{0};
    std::size_t close_calls{0};
    std::size_t read_calls{0};
    std::size_t write_calls{0};
    std::size_t bytes_read{0};
    std::size_t bytes_written{0};
    std::size_t partial_read_events{0};
    std::size_t partial_write_events{0};
    std::size_t read_would_block_events{0};
    std::size_t write_would_block_events{0};
    std::size_t remote_close_events{0};
    std::size_t fault_events{0};
    std::size_t max_read_buffer_depth{0};
    std::size_t max_write_buffer_depth{0};
};

} // namespace moex::twime_trade::transport
