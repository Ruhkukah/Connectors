#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

bool count_allocations = false;
std::uint64_t allocations = 0;
std::uint64_t allocated_bytes = 0;

} // namespace

void* operator new(std::size_t size) {
    if (count_allocations) {
        ++allocations;
        allocated_bytes += size;
    }
    if (void* pointer = std::malloc(size ? size : 1))
        return pointer;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

namespace {

namespace cg = moex::plaza2::cgate;
using Field = moex::plaza2::generated::FieldCode;
using Kind = cg::Plaza2DecodedValueKind;
using Clock = std::chrono::steady_clock;

constexpr int kLevelsPerInstrument = 40;
constexpr int kWarmupCommits = 20;
constexpr int kSnapshotCopies = 1000;
constexpr int kSingleMeasuredCommits = 500;
constexpr int kBurstMeasuredCommits = 500;
constexpr unsigned kHardRuntimeAlarmSeconds = 45;

struct RowSpec {
    std::uint64_t repl_id{0};
    std::int64_t repl_rev{0};
    std::int64_t isin_id{0};
    std::int64_t dir{0};
    std::int64_t volume{10};
    std::int64_t repl_act{0};
    std::string price;
    std::int64_t price_scaled{0};
};

struct RetentionProfile {
    std::string_view name;
    int instruments;
};

constexpr std::array kRetentionProfiles{
    RetentionProfile{"small_40", 1},
    RetentionProfile{"medium_4000", 100},
    RetentionProfile{"large_40000", 1000},
};

using ExpectedRows = std::unordered_map<std::uint64_t, RowSpec>;

using Fields = std::array<cg::Plaza2DecodedFieldValue, 8>;

Fields fields(const RowSpec& row) {
    return {{
        {.field_code = Field::kFortsAggrReplOrdersAggrIsinId, .kind = Kind::SignedInteger, .signed_value = row.isin_id},
        {.field_code = Field::kFortsAggrReplOrdersAggrReplId,
         .kind = Kind::UnsignedInteger,
         .unsigned_value = row.repl_id},
        {.field_code = Field::kFortsAggrReplOrdersAggrReplRev,
         .kind = Kind::SignedInteger,
         .signed_value = row.repl_rev},
        {.field_code = Field::kFortsAggrReplOrdersAggrReplAct,
         .kind = Kind::SignedInteger,
         .signed_value = row.repl_act},
        {.field_code = Field::kFortsAggrReplOrdersAggrDir, .kind = Kind::SignedInteger, .signed_value = row.dir},
        {.field_code = Field::kFortsAggrReplOrdersAggrVolume, .kind = Kind::SignedInteger, .signed_value = row.volume},
        {.field_code = Field::kFortsAggrReplOrdersAggrPrice,
         .kind = Kind::Decimal,
         .text_value = row.price,
         .decimal_mantissa = 0,
         .decimal_scale = 0,
         .decimal_exact = false},
        {.field_code = Field::kFortsAggrReplOrdersAggrMoment, .kind = Kind::UnsignedInteger, .unsigned_value = 100},
    }};
}

void check(bool condition) {
    if (!condition) {
        std::cerr << "benchmark invariant failed\n";
        std::exit(2);
    }
}

void replay(cg::Plaza2Aggr20BookProjector& projector, const RowSpec& row) {
    projector.begin_transaction();
    const auto decoded = fields(row);
    check(!projector.on_row(decoded));
    check(!projector.commit());
}

void replay_burst(cg::Plaza2Aggr20BookProjector& projector, std::vector<RowSpec>& rows) {
    projector.begin_transaction();
    for (const auto& row : rows) {
        const auto decoded = fields(row);
        check(!projector.on_row(decoded));
    }
    check(!projector.commit());
}

RowSpec seed_row(int isin, int level) {
    const int price = level < 20 ? 100 - level : 101 + level - 20;
    return {.repl_id = static_cast<std::uint64_t>((isin - 1) * 40 + level + 1),
            .repl_rev = 1,
            .isin_id = isin,
            .dir = level < 20 ? 1 : 2,
            .volume = 10,
            .price = std::to_string(price) + ".00000",
            .price_scaled = static_cast<std::int64_t>(price) * 100'000};
}

std::vector<RowSpec> seed_rows(int instruments) {
    std::vector<RowSpec> rows;
    rows.reserve(static_cast<std::size_t>(instruments * kLevelsPerInstrument));
    for (int isin = 1; isin <= instruments; ++isin) {
        for (int level = 0; level < kLevelsPerInstrument; ++level) {
            auto row = seed_row(isin, level);
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

void seed_projector(cg::Plaza2Aggr20BookProjector& projector, const std::vector<RowSpec>& rows) {
    std::unordered_set<std::uint64_t> repl_ids;
    repl_ids.reserve(rows.size());
    for (const auto& row : rows)
        check(repl_ids.insert(row.repl_id).second);

    projector.begin_transaction();
    for (const auto& row : rows) {
        const auto decoded = fields(row);
        check(!projector.on_row(decoded));
    }
    check(!projector.commit());
}

ExpectedRows expected_rows(const std::vector<RowSpec>& rows) {
    ExpectedRows expected;
    expected.reserve(rows.size());
    for (const auto& row : rows)
        check(expected.emplace(row.repl_id, row).second);
    return expected;
}

std::size_t distinct_repl_id_count(const std::vector<RowSpec>& rows) {
    std::unordered_set<std::uint64_t> repl_ids;
    repl_ids.reserve(rows.size());
    for (const auto& row : rows)
        repl_ids.insert(row.repl_id);
    return repl_ids.size();
}

std::vector<RowSpec> burst_rows(int instruments, int rows_per_commit) {
    const auto retained_rows = static_cast<std::size_t>(instruments * kLevelsPerInstrument);
    check(retained_rows > 0);
    std::vector<RowSpec> rows;
    rows.reserve(static_cast<std::size_t>(rows_per_commit));
    std::unordered_map<std::uint64_t, std::int64_t> occurrences;
    occurrences.reserve(std::min<std::size_t>(retained_rows, static_cast<std::size_t>(rows_per_commit)));
    for (int index = 0; index < rows_per_commit; ++index) {
        const auto slot = static_cast<std::size_t>(index) % retained_rows;
        const int isin = static_cast<int>(slot / kLevelsPerInstrument) + 1;
        const int level = static_cast<int>(slot % kLevelsPerInstrument);
        auto row = seed_row(isin, level);
        // Start above the seeded revision and advance repeated occurrences of
        // the same retained slot within one transaction monotonically.
        row.repl_rev = 2 + occurrences[row.repl_id]++;
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string_view burst_pattern(std::size_t retained_rows, std::size_t distinct_count, int rows_per_commit) {
    if (distinct_count == static_cast<std::size_t>(rows_per_commit))
        return "distinct_existing_slots";
    check(distinct_count == retained_rows);
    return "capacity_limited_repeated_updates";
}

std::uint64_t checksum_mix(std::uint64_t checksum, std::uint64_t value) {
    checksum ^= value;
    checksum *= 1099511628211ULL;
    return checksum;
}

void validate_level(const cg::Plaza2Aggr20Level& level, const RowSpec& expected) {
    check(level.isin_id == expected.isin_id);
    check(level.price_scaled == expected.price_scaled);
    check(level.volume == expected.volume);
    check(level.dir == expected.dir);
    check(level.repl_id == expected.repl_id);
    check(level.repl_rev == expected.repl_rev);
    check(level.moment == 100);
    check(level.moment_ns == 0);
    check(level.price == expected.price);
    check(level.synth_volume.empty());
}

std::uint64_t validate_semantic_state(const cg::Plaza2Aggr20BookProjector& projector,
                                      const ExpectedRows& expected, int instruments) {
    std::uint64_t checksum = 1469598103934665603ULL;
    for (int isin = 1; isin <= instruments; ++isin) {
        const auto snapshot = projector.snapshot_for_isin(isin);
        check(snapshot.has_value());
        check(snapshot->isin_id == isin);
        check(snapshot->row_count == kLevelsPerInstrument);
        check(snapshot->levels.size() == kLevelsPerInstrument);
        check(snapshot->bid_depth_levels == 20);
        check(snapshot->ask_depth_levels == 20);
        check(snapshot->source_snapshot_version > 0);
        check(snapshot->exchange_moment == 100);
        check(snapshot->exchange_moment_ns == 0);

        const auto first_bid_id = seed_row(isin, 0).repl_id;
        const auto first_ask_id = seed_row(isin, 20).repl_id;
        check(snapshot->top_bid.has_value() && snapshot->top_bid->repl_id == first_bid_id &&
              snapshot->top_bid->price_scaled == 100 * 100'000);
        check(snapshot->top_ask.has_value() && snapshot->top_ask->repl_id == first_ask_id &&
              snapshot->top_ask->price_scaled == 101 * 100'000);

        std::unordered_set<std::uint64_t> seen;
        seen.reserve(snapshot->levels.size());
        std::uint64_t max_repl_id = 0;
        std::int64_t max_repl_rev = 0;
        for (const auto& level : snapshot->levels) {
            const auto expected_it = expected.find(level.repl_id);
            check(expected_it != expected.end());
            validate_level(level, expected_it->second);
            check(seen.insert(level.repl_id).second);
            max_repl_id = std::max(max_repl_id, level.repl_id);
            max_repl_rev = std::max(max_repl_rev, level.repl_rev);
            checksum = checksum_mix(checksum, static_cast<std::uint64_t>(level.isin_id));
            checksum = checksum_mix(checksum, static_cast<std::uint64_t>(level.price_scaled));
            checksum = checksum_mix(checksum, static_cast<std::uint64_t>(level.volume));
            checksum = checksum_mix(checksum, static_cast<std::uint64_t>(level.dir));
            checksum = checksum_mix(checksum, level.repl_id);
            checksum = checksum_mix(checksum, static_cast<std::uint64_t>(level.repl_rev));
            checksum = checksum_mix(checksum, level.moment);
            checksum = checksum_mix(checksum, level.moment_ns);
        }
        check(snapshot->last_repl_id == max_repl_id);
        check(snapshot->last_repl_rev == max_repl_rev);
    }
    return checksum;
}

std::uint64_t validate_churn_state(const cg::Plaza2Aggr20BookProjector& projector) {
    const auto snapshot = projector.snapshot_for_isin(1);
    check(snapshot.has_value());
    check(snapshot->isin_id == 1);
    check(snapshot->row_count == kLevelsPerInstrument);
    check(snapshot->levels.size() == kLevelsPerInstrument);
    check(snapshot->source_snapshot_version > 0);
    std::unordered_set<std::uint64_t> repl_ids;
    repl_ids.reserve(snapshot->levels.size());
    std::uint64_t checksum = 1469598103934665603ULL;
    for (const auto& level : snapshot->levels) {
        check(level.isin_id == 1);
        check(level.volume == 10);
        check(repl_ids.insert(level.repl_id).second);
        checksum = checksum_mix(checksum, level.repl_id);
        checksum = checksum_mix(checksum, static_cast<std::uint64_t>(level.repl_rev));
        checksum = checksum_mix(checksum, static_cast<std::uint64_t>(level.price_scaled));
        checksum = checksum_mix(checksum, static_cast<std::uint64_t>(level.dir));
    }
    return checksum;
}

struct Result {
    int instruments{0};
    std::size_t retained_rows{0};
    std::string retention;
    std::string updated;
    std::string update_pattern;
    int rows_per_commit{1};
    std::size_t distinct_repl_ids{0};
    int commits{0};
    double median_us{0};
    double p95_us{0};
    double commits_per_second{0};
    double new_calls_per_commit{0};
    double new_bytes_per_commit{0};
    double selected_snapshot_mean_us{0};
    double snapshot_new_calls{0};
    double snapshot_new_bytes{0};
    double global_materialization_us{0};
    double global_materialization_new_calls{0};
    double global_materialization_new_bytes{0};
    std::size_t global_row_count{0};
    std::size_t global_instrument_count{0};
    std::uint64_t semantic_checksum{0};
};

struct ChurnResult {
    std::size_t retained_rows{0};
    int rows_per_commit{2};
    int commits{0};
    int distinct_repl_ids{0};
    double median_us{0};
    double p95_us{0};
    double commits_per_second{0};
    double new_calls_per_commit{0};
    double new_bytes_per_commit{0};
    double global_materialization_us{0};
    double global_materialization_new_calls{0};
    double global_materialization_new_bytes{0};
    std::size_t global_row_count{0};
    std::size_t global_instrument_count{0};
    std::uint64_t semantic_checksum{0};
};

Result measure(cg::Plaza2Aggr20BookProjector& projector, const RetentionProfile& profile,
               std::vector<RowSpec> rows, std::string updated, std::string update_pattern, int rows_per_commit,
               int measured_commits) {
    check(rows_per_commit == static_cast<int>(rows.size()));
    const auto retained_rows = static_cast<std::size_t>(profile.instruments * kLevelsPerInstrument);
    const auto expected_seed = seed_rows(profile.instruments);
    auto expected = expected_rows(expected_seed);
    const auto distinct_count = distinct_repl_id_count(rows);
    for (int warmup = 0; warmup < kWarmupCommits; ++warmup) {
        for (auto& row : rows)
            ++row.repl_rev;
        if (rows_per_commit == 1)
            replay(projector, rows.front());
        else
            replay_burst(projector, rows);
        for (const auto& row : rows)
            expected[row.repl_id] = row;
    }

    const auto before = projector.snapshot_for_isin(1);
    check(before && before->levels.size() == kLevelsPerInstrument);
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(measured_commits));
    allocations = 0;
    allocated_bytes = 0;
    double total_us = 0;
    for (int iteration = 0; iteration < measured_commits; ++iteration) {
        for (auto& row : rows)
            ++row.repl_rev;
        count_allocations = true;
        const auto started = Clock::now();
        if (rows_per_commit == 1)
            replay(projector, rows.front());
        else
            replay_burst(projector, rows);
        const auto finished = Clock::now();
        count_allocations = false;
        for (const auto& row : rows)
            expected[row.repl_id] = row;
        const auto elapsed = std::chrono::duration<double, std::micro>(finished - started).count();
        samples.push_back(elapsed);
        total_us += elapsed;
    }
    const auto measured_allocations = allocations;
    const auto measured_bytes = allocated_bytes;
    const auto after = projector.snapshot_for_isin(1);
    check(after && after->levels.size() == kLevelsPerInstrument);
    if (updated == "unrelated_last_slot")
        check(after->source_snapshot_version == before->source_snapshot_version &&
              after->source_snapshot_hash == before->source_snapshot_hash &&
              after->committed_at == before->committed_at);
    else
        check(after->source_snapshot_version > before->source_snapshot_version);
    const auto semantic_checksum = validate_semantic_state(projector, expected, profile.instruments);

    allocations = 0;
    allocated_bytes = 0;
    count_allocations = true;
    const auto snapshot_started = Clock::now();
    std::uint64_t checksum = 0;
    for (int iteration = 0; iteration < kSnapshotCopies; ++iteration) {
        const auto snapshot = projector.snapshot_for_isin(1);
        check(snapshot.has_value());
        checksum += snapshot->levels.size();
    }
    const auto snapshot_finished = Clock::now();
    count_allocations = false;
    check(checksum == static_cast<std::uint64_t>(kSnapshotCopies * kLevelsPerInstrument));
    const auto snapshot_allocations = allocations;
    const auto snapshot_bytes = allocated_bytes;

    allocations = 0;
    allocated_bytes = 0;
    count_allocations = true;
    const auto global_started = Clock::now();
    const auto& global = projector.snapshot();
    const auto global_finished = Clock::now();
    count_allocations = false;
    check(global.row_count == retained_rows);
    check(global.instrument_count == static_cast<std::size_t>(profile.instruments));
    check(global.levels.size() == retained_rows);

    std::sort(samples.begin(), samples.end());
    return {.instruments = profile.instruments,
            .retained_rows = retained_rows,
            .retention = std::string(profile.name),
            .updated = std::move(updated),
            .update_pattern = std::move(update_pattern),
            .rows_per_commit = rows_per_commit,
            .distinct_repl_ids = distinct_count,
            .commits = measured_commits,
            .median_us = samples[static_cast<std::size_t>(measured_commits / 2)],
            .p95_us = samples[static_cast<std::size_t>(measured_commits * 95 / 100)],
            .commits_per_second = measured_commits * 1'000'000.0 / total_us,
            .new_calls_per_commit = static_cast<double>(measured_allocations) / measured_commits,
            .new_bytes_per_commit = static_cast<double>(measured_bytes) / measured_commits,
            .selected_snapshot_mean_us =
                std::chrono::duration<double, std::micro>(snapshot_finished - snapshot_started).count() / 1000.0,
            .snapshot_new_calls = static_cast<double>(snapshot_allocations) / 1000.0,
            .snapshot_new_bytes = static_cast<double>(snapshot_bytes) / 1000.0,
            .global_materialization_us =
                std::chrono::duration<double, std::micro>(global_finished - global_started).count(),
            .global_materialization_new_calls = static_cast<double>(allocations),
            .global_materialization_new_bytes = static_cast<double>(allocated_bytes),
            .global_row_count = global.row_count,
            .global_instrument_count = global.instrument_count,
            .semantic_checksum = semantic_checksum};
}

void emit(const Result& result, bool& first) {
    if (!first)
        std::cout << ',';
    first = false;
    std::cout << "{\"instruments\":" << result.instruments << ",\"retained_rows\":" << result.retained_rows
              << ",\"retention\":\"" << result.retention << "\",\"updated\":\"" << result.updated
              << "\",\"update_pattern\":\"" << result.update_pattern
              << "\",\"rows_per_commit\":" << result.rows_per_commit
              << ",\"distinct_repl_ids\":" << result.distinct_repl_ids << ",\"commits\":" << result.commits
              << ",\"median_us\":" << result.median_us << ",\"p95_us\":" << result.p95_us
              << ",\"commits_per_second\":" << result.commits_per_second
              << ",\"new_calls_per_commit\":" << result.new_calls_per_commit
              << ",\"new_bytes_per_commit\":" << result.new_bytes_per_commit
              << ",\"selected_snapshot_mean_us\":" << result.selected_snapshot_mean_us
              << ",\"snapshot_new_calls\":" << result.snapshot_new_calls
              << ",\"snapshot_new_bytes\":" << result.snapshot_new_bytes
              << ",\"global_materialization_us\":" << result.global_materialization_us
              << ",\"global_materialization_new_calls\":" << result.global_materialization_new_calls
              << ",\"global_materialization_new_bytes\":" << result.global_materialization_new_bytes
              << ",\"global_row_count\":" << result.global_row_count
              << ",\"global_instrument_count\":" << result.global_instrument_count
              << ",\"semantic_validation\":\"pass\",\"semantic_checksum\":" << result.semantic_checksum << '}';
}

ChurnResult measure_churn(cg::Plaza2Aggr20BookProjector& projector, int measured_commits) {
    constexpr int retained_rows = 40;
    constexpr std::uint64_t first_churn_repl_id = 1'000'000;
    std::vector<RowSpec> rows(2);
    std::uint64_t current_repl_id = 1;
    std::uint64_t next_repl_id = first_churn_repl_id;
    for (int warmup = 0; warmup < 20; ++warmup) {
        rows[0] = {.repl_id = current_repl_id,
                   .repl_rev = warmup + 1,
                   .isin_id = 1,
                   .dir = 1,
                   .volume = 0,
                   .repl_act = 0,
                   .price = "50.00000",
                   .price_scaled = 5'000'000};
        rows[1] = {.repl_id = next_repl_id,
                   .repl_rev = warmup + 1,
                   .isin_id = 1,
                   .dir = 1,
                   .volume = 10,
                   .repl_act = 0,
                   .price = "50.00000",
                   .price_scaled = 5'000'000};
        replay_burst(projector, rows);
        current_repl_id = next_repl_id++;
    }
    check(projector.snapshot_for_isin(1)->levels.size() == retained_rows);

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(measured_commits));
    allocations = 0;
    allocated_bytes = 0;
    double total_us = 0;
    for (int iteration = 0; iteration < measured_commits; ++iteration) {
        rows[0] = {.repl_id = current_repl_id,
                   .repl_rev = 100 + iteration * 2,
                   .isin_id = 1,
                   .dir = 1,
                   .volume = 0,
                   .repl_act = 0,
                   .price = "50.00000",
                   .price_scaled = 5'000'000};
        rows[1] = {.repl_id = next_repl_id,
                   .repl_rev = 101 + iteration * 2,
                   .isin_id = 1,
                   .dir = 1,
                   .volume = 10,
                   .repl_act = 0,
                   .price = "50.00000",
                   .price_scaled = 5'000'000};
        ++next_repl_id;
        count_allocations = true;
        const auto started = Clock::now();
        replay_burst(projector, rows);
        const auto finished = Clock::now();
        count_allocations = false;
        const auto elapsed = std::chrono::duration<double, std::micro>(finished - started).count();
        samples.push_back(elapsed);
        total_us += elapsed;
        current_repl_id = rows[1].repl_id;
    }
    const auto measured_allocations = allocations;
    const auto measured_bytes = allocated_bytes;
    const auto semantic_checksum = validate_churn_state(projector);

    allocations = 0;
    allocated_bytes = 0;
    count_allocations = true;
    const auto global_started = Clock::now();
    const auto& global = projector.snapshot();
    const auto global_finished = Clock::now();
    count_allocations = false;
    check(global.row_count == retained_rows && global.instrument_count == 1);
    check(global.levels.size() == retained_rows);

    std::sort(samples.begin(), samples.end());
    return {.retained_rows = retained_rows,
            .rows_per_commit = 2,
            .commits = measured_commits,
            .distinct_repl_ids = static_cast<int>(next_repl_id - first_churn_repl_id),
            .median_us = samples[static_cast<std::size_t>(measured_commits / 2)],
            .p95_us = samples[static_cast<std::size_t>(measured_commits * 95 / 100)],
            .commits_per_second = measured_commits * 1'000'000.0 / total_us,
            .new_calls_per_commit = static_cast<double>(measured_allocations) / measured_commits,
            .new_bytes_per_commit = static_cast<double>(measured_bytes) / measured_commits,
            .global_materialization_us =
                std::chrono::duration<double, std::micro>(global_finished - global_started).count(),
            .global_materialization_new_calls = static_cast<double>(allocations),
            .global_materialization_new_bytes = static_cast<double>(allocated_bytes),
            .global_row_count = global.row_count,
            .global_instrument_count = global.instrument_count,
            .semantic_checksum = semantic_checksum};
}

void emit_churn(const ChurnResult& result, bool& first) {
    if (!first)
        std::cout << ',';
    first = false;
    std::cout << "{\"instruments\":1,\"retained_rows\":" << result.retained_rows
              << ",\"updated\":\"unique_insert_delete_churn\",\"rows_per_commit\":" << result.rows_per_commit
              << ",\"commits\":" << result.commits << ",\"distinct_repl_ids\":" << result.distinct_repl_ids
              << ",\"median_us\":" << result.median_us << ",\"p95_us\":" << result.p95_us
              << ",\"commits_per_second\":" << result.commits_per_second
              << ",\"new_calls_per_commit\":" << result.new_calls_per_commit
              << ",\"new_bytes_per_commit\":" << result.new_bytes_per_commit
              << ",\"global_materialization_us\":" << result.global_materialization_us
              << ",\"global_materialization_new_calls\":" << result.global_materialization_new_calls
              << ",\"global_materialization_new_bytes\":" << result.global_materialization_new_bytes
              << ",\"global_row_count\":" << result.global_row_count
              << ",\"global_instrument_count\":" << result.global_instrument_count
              << ",\"semantic_validation\":\"pass\",\"semantic_checksum\":" << result.semantic_checksum << '}';
}

} // namespace

int main() {
    ::alarm(kHardRuntimeAlarmSeconds);
    const auto started = Clock::now();
    std::cout << std::fixed << std::setprecision(3) << "{\"single\":[";
    bool first = true;
    for (const auto& profile : kRetentionProfiles) {
        for (int mode = 0; mode < (profile.instruments == 1 ? 1 : 2); ++mode) {
            cg::Plaza2Aggr20BookProjector projector;
            seed_projector(projector, seed_rows(profile.instruments));
            const int isin = mode ? profile.instruments : 1;
            const int level = mode ? 39 : 0;
            emit(measure(projector, profile, {seed_row(isin, level)},
                         mode ? "unrelated_last_slot" : "selected_first_slot", "single_existing_slot", 1,
                         kSingleMeasuredCommits),
                 first);
        }
    }
    std::cout << "],\"bursts\":[";
    first = true;
    for (const auto& profile : kRetentionProfiles) {
        for (const int burst_size : {10, 100, 1000}) {
            cg::Plaza2Aggr20BookProjector projector;
            seed_projector(projector, seed_rows(profile.instruments));
            auto rows = burst_rows(profile.instruments, burst_size);
            const auto distinct_count = distinct_repl_id_count(rows);
            const auto pattern = burst_pattern(
                static_cast<std::size_t>(profile.instruments * kLevelsPerInstrument), distinct_count, burst_size);
            emit(measure(projector, profile, std::move(rows), "existing_slot_burst", std::string(pattern),
                         burst_size, kBurstMeasuredCommits),
                 first);
        }
    }
    std::cout << "],\"churn\":[";
    first = true;
    cg::Plaza2Aggr20BookProjector churn_projector;
    seed_projector(churn_projector, seed_rows(1));
    emit_churn(measure_churn(churn_projector, kSingleMeasuredCommits), first);
    std::cout << "],\"wall_seconds\":" << std::chrono::duration<double>(Clock::now() - started).count() << "}\n";
}
