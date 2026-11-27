#include "moex/plaza2/cgate/plaza2_full_order_log_refdata.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace moex::plaza2::cgate {
namespace {
using generated::StreamCode;
using generated::TableCode;
constexpr auto refdata = StreamCode::kFortsRefdataRepl;
constexpr auto session = TableCode::kFortsRefdataReplSession;

void require(bool valid) {
    if (!valid)
        throw std::runtime_error("invalid FOL REFDATA sidecar");
}

// One fixed V2 record of the existing projected types. The hex TSV column
// preserves integer widths, optional/unknown values and original string bytes;
// it never rounds decimals or reconstructs omitted V1 terms.
template <class Codec> void terms(Codec& c, private_state::SourceRowProvenance& v) {
    c(v.stream_code, v.table_code, v.repl_rev, v.lifenum, v.present);
}
template <class Codec> void terms(Codec& c, private_state::SessionDecimal& v) {
    c(v.units);
}
template <class Codec> void terms(Codec& c, private_state::FuturePriceBounds& v) {
    c(v.lower, v.upper, v.formula_confirmed, v.interval_valid);
}
template <class Codec> void terms(Codec& c, private_state::FutureSessionTerms& v) {
    c(v.isin_id, v.sess_id, v.settlement_price, v.raw_limit_up, v.raw_limit_down, v.min_step, v.lot_volume_units,
      v.repl_id, v.source, v.bounds);
}
template <class Codec> void terms(Codec& c, private_state::InstrumentLegSnapshot& v) {
    c(v.leg_isin_id, v.qty_ratio, v.leg_order_no);
}
template <class Codec> void terms(Codec& c, private_state::InstrumentSnapshot& v) {
    c(v.isin_id, v.sess_id, v.kind, v.isin, v.short_isin, v.name, v.base_contract_code, v.base_contract_id,
      v.future_vcb_join_status, v.future_vcb_currency, v.future_vcb_board_md, v.future_vcb_provenance,
      v.definition_source_provenance, v.fut_isin_id, v.option_series_id, v.inst_term, v.roundto, v.lot_volume,
      v.lot_volume_units, v.trade_mode_id, v.state, v.current_session_member, v.current_session_state,
      v.has_current_status, v.current_status, v.current_status_refdata_bound, v.current_status_session_epoch, v.signs,
      v.put, v.is_spread, v.min_step, v.step_price, v.step_price_curr, v.settlement_price, v.strike, v.last_trade_date,
      v.group_mask, v.trade_period_access, v.legs, v.future_session_terms);
}
template <class Codec> void terms(Codec& c, private_state::TradingSessionSnapshot& v) {
    c(v.sess_id, v.begin, v.end, v.state, v.has_current_status, v.current_status, v.eve_on, v.eve_begin, v.eve_end,
      v.mon_on, v.mon_begin, v.mon_end, v.settl_sess_begin, v.clr_sess_begin, v.settl_price_calc_time,
      v.settl_sess_t1_begin, v.margin_call_fix_schedule);
}
template <class Codec> void terms(Codec& c, FullOrderLogDefinitionIdentity& v) {
    c(v.instrument, v.session, v.session_status_lifenum, v.instrument_status_lifenum, v.matching_id, v.complete);
}
struct TermsWriter {
    std::string bytes;
    template <class... T> void operator()(T&... values) {
        (put(values), ...);
    }
    template <class T> void put(T& value) {
        if constexpr (std::is_same_v<T, bool>)
            bytes.push_back(value ? 1 : 0);
        else if constexpr (std::is_integral_v<T>) {
            // The capture format is little-endian on every supported host.
            using U = std::make_unsigned_t<T>;
            auto n = static_cast<U>(value);
            for (std::size_t i = 0; i < sizeof(T); ++i)
                bytes.push_back(static_cast<char>(n >> (i * 8)));
        } else if constexpr (std::is_enum_v<T>) {
            auto n = static_cast<std::underlying_type_t<T>>(value);
            put(n);
        } else
            terms(*this, value);
    }
    void put(std::string& value) {
        require(value.size() <= 4096);
        auto size = static_cast<std::uint32_t>(value.size());
        put(size);
        bytes += value;
    }
    template <class T> void put(std::optional<T>& value) {
        auto present = value.has_value();
        put(present);
        if (present)
            put(*value);
    }
    void put(std::vector<private_state::InstrumentLegSnapshot>& value) {
        require(value.size() <= 64);
        auto size = static_cast<std::uint32_t>(value.size());
        put(size);
        for (auto& leg : value)
            put(leg);
    }
};
struct TermsReader {
    std::string_view bytes;
    template <class... T> void operator()(T&... values) {
        (get(values), ...);
    }
    template <class T> void get(T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            require(!bytes.empty() && static_cast<unsigned char>(bytes.front()) <= 1);
            value = bytes.front() != 0;
            bytes.remove_prefix(1);
        } else if constexpr (std::is_integral_v<T>) {
            require(bytes.size() >= sizeof(T));
            std::make_unsigned_t<T> n{};
            for (std::size_t i = 0; i < sizeof(T); ++i)
                n |= static_cast<std::make_unsigned_t<T>>(static_cast<unsigned char>(bytes[i])) << (i * 8);
            std::memcpy(&value, &n, sizeof value);
            bytes.remove_prefix(sizeof(T));
        } else if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> n{};
            get(n);
            value = static_cast<T>(n);
        } else
            terms(*this, value);
    }
    void get(std::string& value) {
        std::uint32_t size{};
        get(size);
        require(size <= 4096 && size <= bytes.size());
        value = bytes.substr(0, size);
        bytes.remove_prefix(size);
    }
    template <class T> void get(std::optional<T>& value) {
        bool present{};
        get(present);
        if (present) {
            value.emplace();
            get(*value);
        } else
            value.reset();
    }
    void get(std::vector<private_state::InstrumentLegSnapshot>& value) {
        std::uint32_t size{};
        get(size);
        require(size <= 64);
        value.resize(size);
        for (auto& leg : value)
            get(leg);
    }
};
std::string full_terms(FullOrderLogRefdataRow row) {
    TermsWriter writer;
    writer(row.future_instruments_source, row.future_sess_contents_source, row.future_vcb_source,
           row.definition_identity);
    require(writer.bytes.size() <= 65536);
    constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(writer.bytes.size() * 2);
    for (const unsigned char byte : writer.bytes) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex;
}
void read_full_terms(FullOrderLogRefdataRow& row, std::string_view hex) {
    require(!hex.empty() && hex.size() <= 131072 && hex.size() % 2 == 0);
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    const auto digit = [](char c) {
        require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        return c <= '9' ? c - '0' : c - 'a' + 10;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2)
        bytes += static_cast<char>((digit(hex[i]) << 4) | digit(hex[i + 1]));
    TermsReader reader{bytes};
    reader(row.future_instruments_source, row.future_sess_contents_source, row.future_vcb_source,
           row.definition_identity);
    require(reader.bytes.empty());
}

bool clean_text(std::string_view value) {
    return value.size() <= 128 && text::valid_utf8(value) &&
           std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

void validate(const FullOrderLogRefdataRow& row) {
    require(row.isin_id > 0 && row.effective_after_sequence && row.effective_utc_ns && row.effective_monotonic_ns);
    if (!row.available) {
        require(row.isin.empty() && row.short_code.empty() && row.min_step.empty() && !row.decimals && !row.sess_id &&
                row.instrument_source == private_state::SourceRowProvenance{} &&
                row.session_source == private_state::SourceRowProvenance{} &&
                row.future_instruments_source == private_state::SourceRowProvenance{} &&
                row.future_sess_contents_source == private_state::SourceRowProvenance{} &&
                row.future_vcb_source == private_state::SourceRowProvenance{} &&
                row.definition_identity == FullOrderLogDefinitionIdentity{});
        return;
    }
    const auto& terms = row.instrument_source;
    const auto& day = row.session_source;
    const auto step = private_state::parse_session_decimal(row.min_step);
    require(!row.isin.empty() && clean_text(row.isin) && clean_text(row.short_code) && clean_text(row.min_step) &&
            step && step->units > 0 && row.sess_id > 0 &&
            (!row.decimals || (*row.decimals >= 0 && *row.decimals <= 18)) && terms.present && day.present &&
            terms.stream_code == refdata && day.stream_code == refdata &&
            (terms.table_code == TableCode::kFortsRefdataReplFutInstruments ||
             terms.table_code == TableCode::kFortsRefdataReplFutSessContents) &&
            day.table_code == session && terms.repl_rev > 0 && day.repl_rev > 0 && terms.lifenum > 0 &&
            terms.lifenum == day.lifenum);
    if (row.definition_identity.complete) {
        require(row.definition_identity.instrument && row.definition_identity.session &&
                row.definition_identity.session_status_lifenum > 0 &&
                row.definition_identity.instrument_status_lifenum > 0 &&
                row.definition_identity.instrument->isin_id == row.isin_id &&
                row.definition_identity.instrument->sess_id == row.sess_id &&
                row.definition_identity.session->sess_id == row.sess_id && row.definition_identity.matching_id > 0);
        const auto& instrument = *row.definition_identity.instrument;
        require(instrument.has_current_status && instrument.current_status_refdata_bound &&
                instrument.current_session_member && row.definition_identity.session->has_current_status &&
                instrument.future_session_terms);
        const auto& limits = *instrument.future_session_terms;
        require(limits.isin_id == row.isin_id && limits.sess_id == row.sess_id && limits.settlement_price &&
                limits.raw_limit_up && limits.raw_limit_down && limits.min_step && limits.lot_volume_units);
    }
}

template <typename T> T integer(std::string_view text) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    require(!text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size());
    return value;
}
} // namespace

std::string serialize_full_order_log_refdata_row(const FullOrderLogRefdataRow& row) {
    validate(row);
    return std::to_string(row.effective_after_sequence) + '\t' + std::to_string(row.effective_utc_ns) + '\t' +
           std::to_string(row.effective_monotonic_ns) + '\t' + std::to_string(row.isin_id) + '\t' +
           (row.available ? "1" : "0") + '\t' + row.isin + '\t' + row.short_code + '\t' + row.min_step + '\t' +
           (row.decimals ? std::to_string(*row.decimals) : std::string{}) + '\t' + std::to_string(row.sess_id) + '\t' +
           std::to_string(static_cast<std::uint32_t>(row.instrument_source.table_code)) + '\t' +
           std::to_string(row.instrument_source.repl_rev) + '\t' + std::to_string(row.instrument_source.lifenum) +
           '\t' + std::to_string(row.session_source.repl_rev) + '\t' + std::to_string(row.session_source.lifenum) +
           '\t' + full_terms(row) + '\n';
}

std::vector<FullOrderLogRefdataRow> parse_full_order_log_refdata(std::string_view contents) {
    const bool v2 = contents.starts_with(kFullOrderLogRefdataHeader);
    require(contents.size() <= kFullOrderLogRefdataMaxBytes &&
            (v2 || contents.starts_with(kFullOrderLogRefdataHeaderV1)));
    contents.remove_prefix(v2 ? kFullOrderLogRefdataHeader.size() : kFullOrderLogRefdataHeaderV1.size());
    std::vector<FullOrderLogRefdataRow> rows;
    while (!contents.empty()) {
        const auto nl = contents.find('\n');
        require(nl != std::string_view::npos);
        auto line = contents.substr(0, nl);
        contents.remove_prefix(nl + 1);
        std::array<std::string_view, 16> fields;
        const auto count = v2 ? 16u : 15u;
        for (std::size_t i = 0; i < count - 1; ++i) {
            const auto sep = line.find('\t');
            require(sep != std::string_view::npos);
            fields[i] = line.substr(0, sep);
            line.remove_prefix(sep + 1);
        }
        fields[count - 1] = line;
        require(fields[4] == "0" || fields[4] == "1");
        FullOrderLogRefdataRow row;
        row.effective_after_sequence = integer<std::uint64_t>(fields[0]);
        row.effective_utc_ns = integer<std::uint64_t>(fields[1]);
        row.effective_monotonic_ns = integer<std::uint64_t>(fields[2]);
        row.isin_id = integer<std::int32_t>(fields[3]);
        row.available = fields[4] == "1";
        row.isin = fields[5];
        row.short_code = fields[6];
        row.min_step = fields[7];
        if (!fields[8].empty())
            row.decimals = integer<std::int32_t>(fields[8]);
        row.sess_id = integer<std::int32_t>(fields[9]);
        const auto table = static_cast<TableCode>(integer<std::uint32_t>(fields[10]));
        const auto terms_rev = integer<std::int64_t>(fields[11]);
        const auto terms_life = integer<std::uint64_t>(fields[12]);
        const auto day_rev = integer<std::int64_t>(fields[13]);
        const auto day_life = integer<std::uint64_t>(fields[14]);
        if (row.available) {
            row.instrument_source = {refdata, table, terms_rev, terms_life, true};
            row.session_source = {refdata, session, day_rev, day_life, true};
        } else {
            require(table == TableCode{} && !terms_rev && !terms_life && !day_rev && !day_life);
        }
        if (v2)
            read_full_terms(row, fields[15]);
        validate(row);
        require(rows.empty() || (row.effective_after_sequence >= rows.back().effective_after_sequence &&
                                 row.effective_monotonic_ns >= rows.back().effective_monotonic_ns));
        rows.push_back(std::move(row));
    }
    require(!rows.empty());
    return rows;
}

} // namespace moex::plaza2::cgate
