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

struct RowSpec {
    std::uint64_t repl_id{0};
    std::int64_t repl_rev{0};
    std::int64_t isin_id{0};
    std::int64_t dir{0};
    std::int64_t volume{10};
    std::int64_t repl_act{0};
    std::string price;
};

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
            .price = std::to_string(price) + ".00000"};
}

struct Result {
    int instruments{0};
    int retained_rows{0};
    std::string updated;
    int rows_per_commit{1};
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
};

struct ChurnResult {
    int retained_rows{0};
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
};

Result measure(cg::Plaza2Aggr20BookProjector& projector, int instruments, std::vector<RowSpec> rows,
               std::string updated, int rows_per_commit, int measured_commits) {
    for (int warmup = 0; warmup < 20; ++warmup) {
        for (auto& row : rows)
            ++row.repl_rev;
        if (rows_per_commit == 1)
            replay(projector, rows.front());
        else
            replay_burst(projector, rows);
    }

    const auto before = projector.snapshot_for_isin(1);
    check(before && before->levels.size() == 40);
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
        const auto elapsed = std::chrono::duration<double, std::micro>(finished - started).count();
        samples.push_back(elapsed);
        total_us += elapsed;
    }
    const auto measured_allocations = allocations;
    const auto measured_bytes = allocated_bytes;
    const auto after = projector.snapshot_for_isin(1);
    check(after && after->levels.size() == 40);
    if (updated == "unrelated_last_slot")
        check(after->source_snapshot_version == before->source_snapshot_version &&
              after->source_snapshot_hash == before->source_snapshot_hash &&
              after->committed_at == before->committed_at);
    else
        check(after->source_snapshot_version > before->source_snapshot_version);

    allocations = 0;
    allocated_bytes = 0;
    count_allocations = true;
    const auto snapshot_started = Clock::now();
    std::uint64_t checksum = 0;
    for (int iteration = 0; iteration < 1000; ++iteration)
        checksum += projector.snapshot_for_isin(1)->levels.size();
    const auto snapshot_finished = Clock::now();
    count_allocations = false;
    check(checksum == 40'000);
    const auto snapshot_allocations = allocations;
    const auto snapshot_bytes = allocated_bytes;

    allocations = 0;
    allocated_bytes = 0;
    count_allocations = true;
    const auto global_started = Clock::now();
    const auto& global = projector.snapshot();
    const auto global_finished = Clock::now();
    count_allocations = false;
    check(global.row_count == static_cast<std::size_t>(instruments * 40));

    std::sort(samples.begin(), samples.end());
    return {.instruments = instruments,
            .retained_rows = instruments * 40,
            .updated = std::move(updated),
            .rows_per_commit = rows_per_commit,
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
            .global_instrument_count = global.instrument_count};
}

void emit(const Result& result, bool& first) {
    if (!first)
        std::cout << ',';
    first = false;
    std::cout << "{\"instruments\":" << result.instruments << ",\"retained_rows\":" << result.retained_rows
              << ",\"updated\":\"" << result.updated << "\",\"rows_per_commit\":" << result.rows_per_commit
              << ",\"commits\":500,\"median_us\":" << result.median_us << ",\"p95_us\":" << result.p95_us
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
              << ",\"global_instrument_count\":" << result.global_instrument_count << '}';
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
                   .price = "50.00000"};
        rows[1] = {.repl_id = next_repl_id,
                   .repl_rev = warmup + 1,
                   .isin_id = 1,
                   .dir = 1,
                   .volume = 10,
                   .repl_act = 0,
                   .price = "50.00000"};
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
                   .price = "50.00000"};
        rows[1] = {.repl_id = next_repl_id,
                   .repl_rev = 101 + iteration * 2,
                   .isin_id = 1,
                   .dir = 1,
                   .volume = 10,
                   .repl_act = 0,
                   .price = "50.00000"};
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
    check(projector.snapshot_for_isin(1)->levels.size() == retained_rows);

    allocations = 0;
    allocated_bytes = 0;
    count_allocations = true;
    const auto global_started = Clock::now();
    const auto& global = projector.snapshot();
    const auto global_finished = Clock::now();
    count_allocations = false;
    check(global.row_count == retained_rows && global.instrument_count == 1);

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
            .global_instrument_count = global.instrument_count};
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
              << ",\"global_instrument_count\":" << result.global_instrument_count << '}';
}

} // namespace

int main() {
    ::alarm(45);
    const auto started = Clock::now();
    std::cout << std::fixed << std::setprecision(3) << "{\"single\":[";
    bool first = true;
    for (const int instruments : {1, 100, 1000}) {
        for (int mode = 0; mode < (instruments == 1 ? 1 : 2); ++mode) {
            cg::Plaza2Aggr20BookProjector projector;
            projector.begin_transaction();
            for (int isin = 1; isin <= instruments; ++isin) {
                for (int level = 0; level < 40; ++level) {
                    const auto row = seed_row(isin, level);
                    const auto decoded = fields(row);
                    check(!projector.on_row(decoded));
                }
            }
            check(!projector.commit());
            check(projector.snapshot_for_isin(1)->levels.size() == 40);
            const int isin = mode ? instruments : 1;
            const int level = mode ? 39 : 0;
            emit(measure(projector, instruments, {seed_row(isin, level)},
                         mode ? "unrelated_last_slot" : "selected_first_slot", 1, 500),
                 first);
        }
    }
    std::cout << "],\"bursts\":[";
    first = true;
    for (const int burst_rows : {8, 32, 64}) {
        cg::Plaza2Aggr20BookProjector projector;
        constexpr int instruments = 1000;
        projector.begin_transaction();
        for (int isin = 1; isin <= instruments; ++isin) {
            for (int level = 0; level < 40; ++level) {
                const auto row = seed_row(isin, level);
                const auto decoded = fields(row);
                check(!projector.on_row(decoded));
            }
        }
        check(!projector.commit());
        std::vector<RowSpec> rows;
        rows.reserve(static_cast<std::size_t>(burst_rows));
        for (int index = 0; index < burst_rows; ++index) {
            const int isin = 1 + index % 8;
            const int level = index % 40;
            rows.push_back(seed_row(isin, level));
        }
        emit(measure(projector, instruments, std::move(rows), "multi_instrument_burst", burst_rows, 500), first);
    }
    std::cout << "],\"churn\":[";
    first = true;
    cg::Plaza2Aggr20BookProjector churn_projector;
    churn_projector.begin_transaction();
    for (int level = 0; level < 40; ++level) {
        const auto row = seed_row(1, level);
        const auto decoded = fields(row);
        check(!churn_projector.on_row(decoded));
    }
    check(!churn_projector.commit());
    emit_churn(measure_churn(churn_projector, 500), first);
    std::cout << "],\"wall_seconds\":" << std::chrono::duration<double>(Clock::now() - started).count() << "}\n";
}
