#include "moex/connector_host/dtc_market_data.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace moex::connector_host::dtc {
namespace {

std::uint16_t read_u16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint64_t unix_now_ns() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

} // namespace

DtcFrameDecoder::DtcFrameDecoder(std::size_t max_frame_size)
    : max_frame_size_(std::clamp(max_frame_size, kDtcFrameHeaderSize, kDtcMaxFrameSize)) {
    buffer_.reserve(max_frame_size_);
}

bool DtcFrameDecoder::append(std::span<const std::uint8_t> bytes,
                             std::vector<DtcFrame>& completed,
                             std::string& error) {
    error.clear();
    if (bytes.size() > max_frame_size_ || buffer_.size() > max_frame_size_ - bytes.size()) {
        error = "DTC receive buffer exceeds the configured frame bound";
        reset();
        return false;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    while (buffer_.size() >= kDtcFrameHeaderSize) {
        const auto total_size = static_cast<std::size_t>(read_u16(buffer_.data()));
        if (total_size < kDtcFrameHeaderSize || total_size > max_frame_size_) {
            error = "invalid DTC frame length " + std::to_string(total_size);
            reset();
            return false;
        }
        if (buffer_.size() < total_size)
            break;

        DtcFrame frame;
        frame.message_type = read_u16(buffer_.data() + 2);
        frame.payload.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(kDtcFrameHeaderSize),
                             buffer_.begin() + static_cast<std::ptrdiff_t>(total_size));
        completed.push_back(std::move(frame));
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(total_size));
    }
    return true;
}

void DtcFrameDecoder::reset() noexcept {
    buffer_.clear();
}

ConnectorHostDtcMarketDataSource::ConnectorHostDtcMarketDataSource(ConnectorHost& host,
                                                                   std::string board)
    : host_(host), board_(std::move(board)) {}

DtcReadOnlyCapabilities ConnectorHostDtcMarketDataSource::capabilities() const noexcept {
    return {.market_data = false,
            .market_depth = true,
            .security_definitions = false,
            .accounts = false,
            .positions = false,
            .orders = false,
            .order_entry = false};
}

DtcMarketDataSnapshot ConnectorHostDtcMarketDataSource::snapshot() const {
    const auto host_view = host_.snapshot();
    const auto qualification = host_.qualification_snapshot();

    DtcMarketDataSnapshot out;
    out.connector_generation = host_view.recovery.generation;
    out.sampled_at_unix_ns = unix_now_ns();
    out.isin_id = host_view.target_isin_id;
    out.symbol = host_view.target;
    out.board = board_;
    out.min_step = host_view.min_step;
    out.source_online = qualification.aggr_online;
    out.snapshot_complete = qualification.aggr_snapshot_complete;
    out.exchange_moment_ns = qualification.book.exchange_moment_ns;
    out.source_repl_id = qualification.book.last_repl_id;
    out.source_repl_rev = qualification.book.last_repl_rev;

    for (const auto& level : qualification.book.levels) {
        if (level.isin_id != out.isin_id || (level.dir != 1 && level.dir != 2))
            continue;
        out.levels.push_back({.price_scaled = level.price_scaled,
                              .volume = level.volume,
                              .side = level.dir == 1 ? DtcDepthSide::Bid : DtcDepthSide::Ask,
                              .source_repl_id = level.repl_id,
                              .source_repl_rev = level.repl_rev,
                              .exchange_moment_ns = level.moment_ns,
                              .price = level.price});
    }

    const auto bid = std::find_if(out.levels.begin(), out.levels.end(), [](const auto& level) {
        return level.side == DtcDepthSide::Bid;
    });
    const auto ask = std::find_if(out.levels.begin(), out.levels.end(), [](const auto& level) {
        return level.side == DtcDepthSide::Ask;
    });
    out.two_sided = bid != out.levels.end() && ask != out.levels.end();
    out.valid = out.isin_id != 0 && out.source_online && out.snapshot_complete;
    if (!out.valid) {
        if (out.isin_id == 0)
            out.invalid_reason = "target instrument is not selected";
        else if (!out.source_online)
            out.invalid_reason = "AGGR20 source is offline";
        else if (!out.snapshot_complete)
            out.invalid_reason = "AGGR20 snapshot is incomplete";
    }
    return out;
}

} // namespace moex::connector_host::dtc
