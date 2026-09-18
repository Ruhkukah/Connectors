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
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void append_frame(std::span<const std::uint8_t> bytes, std::vector<DtcFrame>& completed) {
    DtcFrame frame;
    frame.message_type = read_u16(bytes.data() + 2);
    frame.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kDtcFrameHeaderSize), bytes.end());
    completed.push_back(std::move(frame));
}

} // namespace

DtcFrameDecoder::DtcFrameDecoder(std::size_t max_frame_size)
    : max_frame_size_(std::clamp(max_frame_size, kDtcFrameHeaderSize, kDtcMaxFrameSize)) {
    buffer_.reserve(max_frame_size_);
}

bool DtcFrameDecoder::append(std::span<const std::uint8_t> bytes, std::vector<DtcFrame>& completed,
                             std::string& error) {
    error.clear();
    if (faulted_) {
        error = "DTC decoder is fenced after a protocol error; reset is required";
        return false;
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        // Parse complete frames directly from a large coalesced receive
        // chunk. The bounded buffer is only used for one incomplete frame.
        if (buffer_.empty() && bytes.size() - offset >= kDtcFrameHeaderSize) {
            const auto total_size = static_cast<std::size_t>(read_u16(bytes.data() + offset));
            if (total_size < kDtcFrameHeaderSize || total_size > max_frame_size_) {
                error = "invalid DTC frame length " + std::to_string(total_size);
                buffer_.clear();
                faulted_ = true;
                return false;
            }
            if (bytes.size() - offset >= total_size) {
                append_frame(bytes.subspan(offset, total_size), completed);
                offset += total_size;
                continue;
            }
        }

        if (buffer_.size() < kDtcFrameHeaderSize) {
            const auto needed_header = kDtcFrameHeaderSize - buffer_.size();
            const auto header_bytes = std::min(needed_header, bytes.size() - offset);
            buffer_.insert(buffer_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                           bytes.begin() + static_cast<std::ptrdiff_t>(offset + header_bytes));
            offset += header_bytes;
        }
        if (buffer_.size() < kDtcFrameHeaderSize)
            break;

        const auto total_size = static_cast<std::size_t>(read_u16(buffer_.data()));
        if (total_size < kDtcFrameHeaderSize || total_size > max_frame_size_) {
            error = "invalid DTC frame length " + std::to_string(total_size);
            buffer_.clear();
            faulted_ = true;
            return false;
        }
        const auto remaining = total_size - buffer_.size();
        const auto body_bytes = std::min(remaining, bytes.size() - offset);
        buffer_.insert(buffer_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                       bytes.begin() + static_cast<std::ptrdiff_t>(offset + body_bytes));
        offset += body_bytes;
        if (buffer_.size() < total_size)
            break;
        append_frame(std::span<const std::uint8_t>(buffer_.data(), buffer_.size()), completed);
        buffer_.clear();
    }
    return true;
}

bool DtcFrameDecoder::finish(std::string& error) {
    error.clear();
    if (faulted_) {
        error = "DTC decoder is fenced after a protocol error; reset is required";
        return false;
    }
    if (buffer_.empty())
        return true;
    error = "truncated DTC frame on disconnect";
    buffer_.clear();
    faulted_ = true;
    return false;
}

void DtcFrameDecoder::reset() noexcept {
    buffer_.clear();
    faulted_ = false;
}

ConnectorHostDtcMarketDataSource::ConnectorHostDtcMarketDataSource(ConnectorHost& host) : host_(host) {}

DtcReadOnlyCapabilities ConnectorHostDtcMarketDataSource::capabilities() const noexcept {
    return {.market_data = false,
            .market_depth = true,
            // ConnectorHost supplies authoritative target identity, board and
            // tick metadata.  The DTC server supplies only explicitly
            // configured replay terms that are absent from this source; it
            // never infers financial values.
            .security_definitions = true,
            .accounts = false,
            .positions = false,
            .orders = false,
            .order_entry = false};
}

DtcMarketDataSnapshot ConnectorHostDtcMarketDataSource::snapshot() const {
    return make_dtc_market_data_snapshot(host_.market_data_snapshot());
}

DtcMarketDataSnapshot make_dtc_market_data_snapshot(const ConnectorHostMarketDataSnapshot& market_data) {
    DtcMarketDataSnapshot out;
    out.connector_generation = market_data.connector_generation;
    out.market_data_authority_epoch = market_data.market_data_authority_epoch;
    out.stream_epoch = market_data.stream_epoch;
    out.dtc_batch_sequence = 0;
    out.source_snapshot_version = market_data.source_snapshot_version;
    out.snapshot_watermark = market_data.snapshot_watermark;
    out.snapshot_level_count = market_data.levels.size();
    out.source_snapshot_hash = market_data.source_snapshot_hash;
    // ConnectorHost is the source adapter, not the future DTC server. These
    // timestamps are populated only when a server emits a DTC batch; a
    // source snapshot must not manufacture engine timing provenance.
    out.engine_ingress_unix_ms = 0;
    out.engine_emit_unix_ms = 0;
    out.sampled_at_unix_ns = unix_now_ns();
    out.isin_id = market_data.target_isin_id;
    out.symbol = market_data.symbol;
    out.board = market_data.board;
    out.min_step = market_data.min_step;
    out.transport_active = market_data.transport_active;
    // Compatibility field: "online" means the target AGGR stream reached
    // ONLINE/snapshot-complete, not that the book is authoritative.
    out.source_online = market_data.transport_active && market_data.aggr_online && market_data.snapshot_complete;
    out.snapshot_complete = market_data.snapshot_complete;
    out.session_data_ready = market_data.session_data_ready;
    out.target_authoritative = market_data.target_authoritative;
    out.aggr_online = market_data.aggr_online;
    out.book_snapshot_current = market_data.book_snapshot_current;
    out.session_ready_witness = market_data.session_ready_witness;
    out.session_ready_witness_kind = market_data.session_ready_witness_kind;
    out.market_data_display_allowed = market_data.market_data_display_allowed;
    out.source_consistent = market_data.source_consistent;
    out.market_data_live = market_data.market_data_live;
    out.session_tradable = market_data.session_tradable;
    out.instrument_tradable = market_data.instrument_tradable;
    out.order_entry_allowed = false;
    out.refdata_metadata_current = market_data.refdata_metadata_current;
    out.exchange_moment_ns = market_data.exchange_moment_ns;
    out.source_repl_id = market_data.source_repl_id;
    out.source_row_id = market_data.source_repl_id;
    out.source_repl_rev = market_data.source_repl_rev;

    for (const auto& level : market_data.levels) {
        if (level.side != static_cast<std::int32_t>(DtcDepthSide::Bid) &&
            level.side != static_cast<std::int32_t>(DtcDepthSide::Ask))
            continue;
        out.levels.push_back(
            {.price_scaled = level.price_scaled,
             .volume = level.volume,
             .side = level.side == static_cast<std::int32_t>(DtcDepthSide::Bid) ? DtcDepthSide::Bid : DtcDepthSide::Ask,
             .source_repl_id = level.source_repl_id,
             .source_repl_rev = level.source_repl_rev,
             .source_row_id = level.source_repl_id,
             .source_sequence = level.source_repl_rev > 0 ? static_cast<std::uint64_t>(level.source_repl_rev) : 0,
             .exchange_moment_ns = level.exchange_moment_ns,
             .price = level.price});
    }

    const auto bid = std::find_if(out.levels.begin(), out.levels.end(),
                                  [](const auto& level) { return level.side == DtcDepthSide::Bid; });
    const auto ask = std::find_if(out.levels.begin(), out.levels.end(),
                                  [](const auto& level) { return level.side == DtcDepthSide::Ask; });
    out.two_sided = bid != out.levels.end() && ask != out.levels.end();
    out.valid = market_data.valid && out.market_data_display_allowed && out.isin_id != 0;
    if (!out.valid) {
        if (out.isin_id == 0)
            out.invalid_reason = "target instrument is not selected";
        else
            out.invalid_reason = market_data.invalid_reason;
    }
    return out;
}

} // namespace moex::connector_host::dtc
