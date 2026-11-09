#include "moex/plaza2/cgate/plaza2_full_order_log_refdata.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>

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

bool clean_text(std::string_view value) {
    return value.size() <= 128 && text::valid_utf8(value) &&
           std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

void validate(const FullOrderLogRefdataRow& row) {
    require(row.isin_id > 0 && row.effective_after_sequence && row.effective_utc_ns && row.effective_monotonic_ns);
    if (!row.available) {
        require(row.isin.empty() && row.short_code.empty() && row.min_step.empty() && !row.decimals && !row.sess_id &&
                row.instrument_source == private_state::SourceRowProvenance{} &&
                row.session_source == private_state::SourceRowProvenance{});
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
           '\n';
}

std::vector<FullOrderLogRefdataRow> parse_full_order_log_refdata(std::string_view contents) {
    require(contents.size() <= kFullOrderLogRefdataMaxBytes && contents.starts_with(kFullOrderLogRefdataHeader));
    contents.remove_prefix(kFullOrderLogRefdataHeader.size());
    std::vector<FullOrderLogRefdataRow> rows;
    while (!contents.empty()) {
        const auto nl = contents.find('\n');
        require(nl != std::string_view::npos);
        auto line = contents.substr(0, nl);
        contents.remove_prefix(nl + 1);
        std::array<std::string_view, 15> fields;
        for (std::size_t i = 0; i < fields.size() - 1; ++i) {
            const auto sep = line.find('\t');
            require(sep != std::string_view::npos);
            fields[i] = line.substr(0, sep);
            line.remove_prefix(sep + 1);
        }
        fields.back() = line;
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
        validate(row);
        require(rows.empty() || (row.effective_after_sequence >= rows.back().effective_after_sequence &&
                                 row.effective_monotonic_ns >= rows.back().effective_monotonic_ns));
        rows.push_back(std::move(row));
    }
    require(!rows.empty());
    return rows;
}

} // namespace moex::plaza2::cgate
