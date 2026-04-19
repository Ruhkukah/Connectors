#pragma once

#include <cstdint>

namespace moex::twime_trade::transport {

enum class TwimeTransportErrorCode : std::uint16_t {
    None = 0,
    InvalidConfiguration,
    EnvironmentBlocked,
    LocalOnlyViolation,
    ResolveBlocked,
    SocketCreateFailed,
    SetNonBlockingFailed,
    ConnectFailed,
    ConnectInProgress,
    ConnectionRefused,
    RemoteClosed,
    ReadFault,
    WriteFault,
    BufferLimitExceeded,
    ReconnectTooSoon,
    InvalidState,
    Unknown,
};

} // namespace moex::twime_trade::transport
