#pragma once

#include <cstddef>

namespace moex::twime_trade::transport {

enum class TwimeTransportState {
    Created,
    Opening,
    Open,
    Closing,
    Closed,
    Faulted,
};

enum class TwimeTransportEvent {
    Opened,
    Closed,
    BytesWritten,
    BytesRead,
    PartialWrite,
    PartialRead,
    ReadWouldBlock,
    WriteWouldBlock,
    RemoteClose,
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
    TwimeTransportEvent event{TwimeTransportEvent::Opened};
    std::size_t bytes_transferred{0};
};

using TwimeTransportPollResult = TwimeTransportResult;

} // namespace moex::twime_trade::transport
