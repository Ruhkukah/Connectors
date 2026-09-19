#include "moex/connector_host/dtc_market_data.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
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

bool positive_float(std::string_view text, float& value) noexcept {
    if (text.empty() || text.size() > 64)
        return false;
    const auto parsed = plaza2::private_state::parse_session_decimal(text);
    if (!parsed || parsed->units <= 0)
        return false;
    value = static_cast<float>(parsed->units) / static_cast<float>(plaza2::private_state::SessionDecimal::scale);
    return std::isfinite(value) && value > 0;
}

std::uint64_t definition_fingerprint(const DtcSecurityDefinitionSnapshot& definition) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto add_byte = [&](std::uint8_t byte) { hash = (hash ^ byte) * 1099511628211ULL; };
    const auto add_number = [&](std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            add_byte(static_cast<std::uint8_t>(value >> shift));
    };
    const auto add_string = [&](std::string_view value) {
        add_number(value.size());
        for (const auto byte : value)
            add_byte(static_cast<std::uint8_t>(byte));
    };
    add_string(definition.symbol);
    add_string(definition.exchange);
    add_string(definition.underlying_board);
    add_number(static_cast<std::uint32_t>(definition.security_type));
    add_string(definition.description);
    add_number(std::bit_cast<std::uint32_t>(definition.min_price_increment));
    add_number(std::bit_cast<std::uint32_t>(definition.currency_value_per_increment));
    add_string(definition.currency);
    add_number(std::bit_cast<std::uint32_t>(definition.contract_size));
    add_number(static_cast<std::uint64_t>(definition.security_identifier));
    add_number(static_cast<std::uint32_t>(definition.session_id));
    add_string(definition.base_contract_code);
    add_number(static_cast<std::uint32_t>(definition.base_contract_id));
    add_string(definition.min_step_source);
    add_string(definition.contract_size_source);
    add_string(definition.currency_value_per_increment_source);
    add_number(static_cast<std::uint32_t>(definition.definition_source_provenance.stream_code));
    add_number(static_cast<std::uint32_t>(definition.definition_source_provenance.table_code));
    add_number(static_cast<std::uint64_t>(definition.definition_source_provenance.repl_rev));
    add_number(definition.definition_source_provenance.lifenum);
    add_number(definition.definition_source_provenance.present ? 1 : 0);
    add_number(static_cast<std::uint32_t>(definition.future_instruments_provenance.stream_code));
    add_number(static_cast<std::uint32_t>(definition.future_instruments_provenance.table_code));
    add_number(static_cast<std::uint64_t>(definition.future_instruments_provenance.repl_rev));
    add_number(definition.future_instruments_provenance.lifenum);
    add_number(definition.future_instruments_provenance.present ? 1 : 0);
    add_number(static_cast<std::uint32_t>(definition.future_sess_contents_provenance.stream_code));
    add_number(static_cast<std::uint32_t>(definition.future_sess_contents_provenance.table_code));
    add_number(static_cast<std::uint64_t>(definition.future_sess_contents_provenance.repl_rev));
    add_number(definition.future_sess_contents_provenance.lifenum);
    add_number(definition.future_sess_contents_provenance.present ? 1 : 0);
    add_number(static_cast<std::uint32_t>(definition.session_provenance.stream_code));
    add_number(static_cast<std::uint32_t>(definition.session_provenance.table_code));
    add_number(static_cast<std::uint64_t>(definition.session_provenance.repl_rev));
    add_number(definition.session_provenance.lifenum);
    add_number(definition.session_provenance.present ? 1 : 0);
    add_number(static_cast<std::uint32_t>(definition.future_vcb_provenance.stream_code));
    add_number(static_cast<std::uint32_t>(definition.future_vcb_provenance.table_code));
    add_number(static_cast<std::uint64_t>(definition.future_vcb_provenance.repl_rev));
    add_number(definition.future_vcb_provenance.lifenum);
    add_number(definition.future_vcb_provenance.present ? 1 : 0);
    return hash ? hash : 1;
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
            // ConnectorHost supplies target identity, board, tick, and (when
            // joined) fut_vcb currency metadata. Operator bindings remain
            // visible separately; live contract size and tick currency value
            // are current REFDATA terms. The DTC server never infers values.
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
    out.session_id = market_data.target_session_id;
    out.symbol = market_data.symbol;
    out.underlying_board = market_data.underlying_board;
    out.min_step = market_data.min_step;
    out.description = market_data.description;
    out.currency = market_data.currency;
    out.contract_size = market_data.contract_size;
    out.currency_value_per_increment = market_data.currency_value_per_increment;
    out.refdata_vcb_join_current = market_data.refdata_vcb_join_current;
    out.refdata_vcb_join_ambiguous = market_data.refdata_vcb_join_ambiguous;
    out.refdata_board_proven = market_data.refdata_board_proven;
    out.refdata_currency_proven = market_data.refdata_currency_proven;
    out.future_vcb_provenance_present = market_data.future_vcb_provenance.present;
    out.future_vcb_repl_rev = market_data.future_vcb_provenance.repl_rev;
    out.future_vcb_lifenum = market_data.future_vcb_provenance.lifenum;
    out.target_is_future = market_data.target_is_future;
    out.target_is_spread = market_data.target_is_spread;
    out.target_is_multileg = market_data.target_is_multileg;
    out.future_vcb_base_contract_code = market_data.future_vcb_base_contract_code;
    out.future_vcb_base_contract_id = market_data.future_vcb_base_contract_id;
    out.definition_source_provenance = market_data.definition_source_provenance;
    out.future_instruments_provenance = market_data.future_instruments_provenance;
    out.future_sess_contents_provenance = market_data.future_sess_contents_provenance;
    out.session_provenance = market_data.session_provenance;
    out.future_vcb_provenance = market_data.future_vcb_provenance;
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

DtcSecurityDefinitionValidation validate_dtc_security_definition(const DtcMarketDataSnapshot& snapshot,
                                                                 DtcSourceMode source_mode,
                                                                 const DtcReplayDefinitionTerms& replay_terms) {
    const auto unavailable = [](std::string reason) {
        return DtcSecurityDefinitionValidation{.definition = std::nullopt, .reason = std::move(reason)};
    };
    if (!snapshot.refdata_metadata_current)
        return unavailable("target REFDATA metadata is not current");
    if (snapshot.isin_id <= 0)
        return unavailable("current target has no positive instrument identifier");
    if (snapshot.symbol.empty() || snapshot.symbol.size() > 128 || !plaza2::cgate::text::valid_utf8(snapshot.symbol))
        return unavailable("current instrument symbol is empty, oversized, or invalid UTF-8");
    if (snapshot.underlying_board.empty() || snapshot.underlying_board.size() > 128 ||
        !plaza2::cgate::text::valid_utf8(snapshot.underlying_board))
        return unavailable("raw underlying ASTS SECBOARD metadata is empty, oversized, or invalid UTF-8");

    DtcSecurityDefinitionSnapshot definition;
    definition.symbol = snapshot.symbol;
    definition.exchange = std::string(kDtcMoexSpectraExchange);
    definition.underlying_board = snapshot.underlying_board;
    definition.security_type = DtcSecurityType::Future;
    definition.security_identifier = snapshot.isin_id;
    definition.session_id = snapshot.session_id;
    definition.base_contract_code = snapshot.future_vcb_base_contract_code;
    definition.base_contract_id = snapshot.future_vcb_base_contract_id;
    definition.definition_source_provenance = snapshot.definition_source_provenance;
    definition.future_instruments_provenance = snapshot.future_instruments_provenance;
    definition.future_sess_contents_provenance = snapshot.future_sess_contents_provenance;
    definition.session_provenance = snapshot.session_provenance;

    float min_price_increment{};
    if (!positive_float(snapshot.min_step, min_price_increment))
        return unavailable("current futures min_step is missing or not positive finite DTC float32");
    definition.min_price_increment = min_price_increment;
    definition.min_step_source = snapshot.min_step;

    if (source_mode == DtcSourceMode::Replay) {
        if (replay_terms.currency.empty() || replay_terms.currency.size() > 16 ||
            !plaza2::cgate::text::valid_utf8(replay_terms.currency) || replay_terms.description.empty() ||
            replay_terms.description.size() > 512 || !plaza2::cgate::text::valid_utf8(replay_terms.description) ||
            !std::isfinite(replay_terms.contract_size) || replay_terms.contract_size <= 0 ||
            !std::isfinite(replay_terms.currency_value_per_increment) || replay_terms.currency_value_per_increment <= 0)
            return unavailable("explicit replay fixture definition terms are missing or invalid");
        definition.currency = replay_terms.currency;
        definition.description = replay_terms.description;
        definition.contract_size = replay_terms.contract_size;
        definition.currency_value_per_increment = replay_terms.currency_value_per_increment;
        definition.contract_size_source = std::to_string(replay_terms.contract_size);
        definition.currency_value_per_increment_source = std::to_string(replay_terms.currency_value_per_increment);
    } else {
        if (!snapshot.target_is_future || snapshot.target_is_spread || snapshot.target_is_multileg)
            return unavailable("current REFDATA target is not a supported outright futures instrument");
        if (!snapshot.refdata_vcb_join_current || snapshot.refdata_vcb_join_ambiguous)
            return unavailable(snapshot.refdata_vcb_join_ambiguous ? "fut_vcb join is ambiguous"
                                                                   : "current committed fut_vcb join is missing");
        const auto& provenance = snapshot.future_vcb_provenance;
        if (!provenance.present || provenance.stream_code != plaza2::generated::StreamCode::kFortsRefdataRepl ||
            provenance.table_code != plaza2::generated::TableCode::kFortsRefdataReplFutVcb ||
            provenance.repl_rev <= 0 || provenance.lifenum == 0)
            return unavailable("fut_vcb source generation/provenance is absent or does not identify REFDATA.fut_vcb");
        if (!snapshot.future_vcb_provenance_present || snapshot.future_vcb_repl_rev != provenance.repl_rev ||
            snapshot.future_vcb_lifenum != provenance.lifenum)
            return unavailable("fut_vcb provenance projection is inconsistent");
        const auto& session_provenance = snapshot.session_provenance;
        if (snapshot.session_id <= 0 || !session_provenance.present ||
            session_provenance.stream_code != plaza2::generated::StreamCode::kFortsRefdataRepl ||
            session_provenance.table_code != plaza2::generated::TableCode::kFortsRefdataReplSession ||
            session_provenance.repl_rev <= 0 || session_provenance.lifenum != provenance.lifenum)
            return unavailable("active session REFDATA source generation/provenance is missing or inconsistent");
        const auto valid_instrument_provenance = [](const plaza2::private_state::SourceRowProvenance& source,
                                                    plaza2::generated::TableCode expected_table,
                                                    std::uint64_t vcb_lifenum) {
            return source.present && source.stream_code == plaza2::generated::StreamCode::kFortsRefdataRepl &&
                   source.table_code == expected_table && source.repl_rev > 0 && source.lifenum != 0 &&
                   source.lifenum == vcb_lifenum;
        };
        const auto& definition_source = snapshot.definition_source_provenance;
        const bool terms_from_fut_instruments = valid_instrument_provenance(
            definition_source, plaza2::generated::TableCode::kFortsRefdataReplFutInstruments, provenance.lifenum);
        const bool terms_from_fut_sess_contents = valid_instrument_provenance(
            definition_source, plaza2::generated::TableCode::kFortsRefdataReplFutSessContents, provenance.lifenum);
        if (!terms_from_fut_instruments && !terms_from_fut_sess_contents)
            return unavailable("current futures terms lack matching-generation source-table provenance");
        if ((snapshot.future_instruments_provenance.present &&
             !valid_instrument_provenance(snapshot.future_instruments_provenance,
                                          plaza2::generated::TableCode::kFortsRefdataReplFutInstruments,
                                          provenance.lifenum)) ||
            (snapshot.future_sess_contents_provenance.present &&
             !valid_instrument_provenance(snapshot.future_sess_contents_provenance,
                                          plaza2::generated::TableCode::kFortsRefdataReplFutSessContents,
                                          provenance.lifenum)))
            return unavailable("additional current futures-table provenance is from another REFDATA generation");
        if (snapshot.future_vcb_base_contract_code.empty() || snapshot.future_vcb_base_contract_id <= 0)
            return unavailable("fut_vcb base-contract identity is missing");
        if (!snapshot.refdata_board_proven)
            return unavailable("raw fut_vcb.board_md underlying-board provenance is not proven");
        if (!snapshot.refdata_currency_proven || snapshot.currency != "RUB")
            return unavailable("unsupported or unproven quote/tick denomination; only the verified RUB path is mapped");
        if (snapshot.description.empty() || snapshot.description.size() > 512 ||
            !plaza2::cgate::text::valid_utf8(snapshot.description))
            return unavailable("current futures description is missing, oversized, or invalid UTF-8");
        definition.description = snapshot.description;
        definition.currency = snapshot.currency;
        if (!positive_float(snapshot.contract_size, definition.contract_size))
            return unavailable("current fut_instruments.lot_volume is missing or invalid");
        std::uint32_t lot_units{};
        const auto lot_parse = std::from_chars(
            snapshot.contract_size.data(), snapshot.contract_size.data() + snapshot.contract_size.size(), lot_units);
        if (lot_parse.ec != std::errc{} ||
            lot_parse.ptr != snapshot.contract_size.data() + snapshot.contract_size.size() || lot_units == 0)
            return unavailable("current fut_instruments.lot_volume is not a positive whole contract-unit count");
        const auto exact_contract_size = static_cast<float>(lot_units);
        if (static_cast<double>(exact_contract_size) != static_cast<double>(lot_units))
            return unavailable(
                "current fut_instruments.lot_volume cannot be represented exactly by DTC float32 ContractSize");
        definition.contract_size = exact_contract_size;
        definition.contract_size_source = snapshot.contract_size;
        if (!positive_float(snapshot.currency_value_per_increment, definition.currency_value_per_increment))
            return unavailable("current fut_instruments.step_price_curr is missing or not positive finite DTC float32");
        definition.currency_value_per_increment_source = snapshot.currency_value_per_increment;
        definition.future_vcb_provenance = provenance;
    }

    definition.definition_version = definition_fingerprint(definition);
    return {.definition = std::move(definition), .reason = {}};
}

} // namespace moex::connector_host::dtc
