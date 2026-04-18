#pragma once

#include <cstdint>

namespace moex::time {

struct EventTimestamps {
    std::int64_t monotonic_ns = 0;
    std::int64_t exchange_utc_ns = 0;
    std::int64_t source_utc_ns = 0;
    std::int64_t socket_receive_monotonic_ns = 0;
    std::int64_t decode_monotonic_ns = 0;
    std::int64_t publish_monotonic_ns = 0;
    std::int64_t managed_poll_monotonic_ns = 0;
};

}  // namespace moex::time
