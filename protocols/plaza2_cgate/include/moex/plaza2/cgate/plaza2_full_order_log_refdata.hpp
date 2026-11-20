#pragma once

#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moex::plaza2::cgate {

inline constexpr std::string_view kFullOrderLogRefdataFilename = "refdata.tsv";
inline constexpr std::size_t kFullOrderLogRefdataMaxBytes = 4 << 20;
inline constexpr std::string_view kFullOrderLogRefdataHeaderV1 =
    "MOEXFOL_REFDATA_V1\n"
    "effective_after_sequence\teffective_utc_ns\teffective_monotonic_ns\tisin_id\tavailable\t"
    "isin\tshort_code\tmin_step\tdecimals\tsess_id\tinstrument_table_code\tinstrument_repl_rev\t"
    "instrument_lifenum\tsession_repl_rev\tsession_lifenum\n";
inline constexpr std::string_view kFullOrderLogRefdataHeader =
    "MOEXFOL_REFDATA_V2\n"
    "effective_after_sequence\teffective_utc_ns\teffective_monotonic_ns\tisin_id\tavailable\t"
    "isin\tshort_code\tmin_step\tdecimals\tsess_id\tinstrument_table_code\tinstrument_repl_rev\t"
    "instrument_lifenum\tsession_repl_rev\tsession_lifenum\tfull_terms_hex\n";

// Complete projected public terms, shared by the FOL DTC identity check and
// capture. Missing terms stay explicit; V1 captures never acquire this proof.
struct FullOrderLogDefinitionIdentity {
    std::optional<private_state::InstrumentSnapshot> instrument;
    std::optional<private_state::TradingSessionSnapshot> session;
    std::uint64_t session_status_lifenum{}, instrument_status_lifenum{};
    std::int8_t matching_id{};
    bool complete{};

    bool operator==(const FullOrderLogDefinitionIdentity&) const = default;
};

struct FullOrderLogRefdataValue {
    std::int32_t isin_id{};
    bool available{};
    std::string isin, short_code, min_step;
    std::optional<std::int32_t> decimals;
    std::int32_t sess_id{};
    private_state::SourceRowProvenance instrument_source, session_source;
    private_state::SourceRowProvenance future_instruments_source, future_sess_contents_source, future_vcb_source;
    FullOrderLogDefinitionIdentity definition_identity;

    bool operator==(const FullOrderLogRefdataValue&) const = default;
};

struct FullOrderLogRefdataRow : FullOrderLogRefdataValue {
    // Owner observation of committed metadata, in both local clock domains.
    // These are availability timestamps, not an inferred exchange effective time.
    std::uint64_t effective_after_sequence{}, effective_utc_ns{}, effective_monotonic_ns{};
};

[[nodiscard]] std::string serialize_full_order_log_refdata_row(const FullOrderLogRefdataRow& row);
[[nodiscard]] std::vector<FullOrderLogRefdataRow> parse_full_order_log_refdata(std::string_view contents);

} // namespace moex::plaza2::cgate
