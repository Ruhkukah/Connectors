#pragma once

#include "moex/twime_trade/transport/twime_transport_errors.hpp"

#include <cstddef>

namespace moex::twime_trade::transport {

enum class TwimeTransportState {
    Created,
    Opening,
    Open,
    Closing,
    Closed,
    Faulted,
    ReconnectBackoff,
};

enum class TwimeTransportEvent {
    OpenStarted,
    OpenSucceeded,
    OpenFailed,
    Closed,
    BytesWritten,
    BytesRead,
    PartialWrite,
    PartialRead,
    ReadWouldBlock,
    WriteWouldBlock,
    RemoteClose,
    LocalClose,
    ReconnectScheduled,
    ReconnectSuppressed,
    Fault,
};

enum class TwimeTransportStatus {
    Ok,
    WouldBlock,
    Closed,
    RemoteClosed,
    Fault,
    InvalidState,
    BufferTooSmall,
};

struct TwimeTransportResult {
    TwimeTransportStatus status{TwimeTransportStatus::Ok};
    TwimeTransportEvent event{TwimeTransportEvent::OpenSucceeded};
    std::size_t bytes_transferred{0};
    TwimeTransportErrorCode error_code{TwimeTransportErrorCode::None};
    int os_error{0};
};

using TwimeTransportPollResult = TwimeTransportResult;

} // namespace moex::twime_trade::transport
