#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <new>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <stdexcept>
#include <string_view>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace test_allocation_injection {

bool enabled = false;
std::size_t remaining = 0;
std::size_t attempts = 0;

void* allocate(std::size_t size) {
    if (enabled) {
        ++attempts;
        if (remaining == 0)
            throw std::bad_alloc();
        --remaining;
    }
    if (void* pointer = std::malloc(size == 0 ? 1 : size))
        return pointer;
    throw std::bad_alloc();
}

} // namespace test_allocation_injection

void* operator new(std::size_t size) {
    return test_allocation_injection::allocate(size);
}

void* operator new[](std::size_t size) {
    return test_allocation_injection::allocate(size);
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

namespace {

using moex::plaza2::cgate::parse_fixed_point;
using moex::plaza2::cgate::Plaza2Aggr20InstrumentSnapshot;
using moex::plaza2::cgate::Plaza2Aggr20Level;
using moex::plaza2::cgate::Plaza2Aggr20QualificationObserver;
using moex::plaza2::cgate::Plaza2Aggr20Snapshot;
using moex::plaza2::cgate::Plaza2DecodedFieldValue;
using moex::plaza2::cgate::Plaza2DecodedValueKind;
using moex::plaza2::cgate::Plaza2Error;
using moex::plaza2::generated::FieldCode;

struct InputRow {
    std::uint64_t repl_id{0};
    std::int64_t repl_rev{0};
    std::int64_t repl_act{0};
    std::int64_t isin_id{0};
    std::int64_t dir{0};
    std::int64_t volume{0};
    std::uint64_t moment{0};
    std::uint64_t moment_ns{0};
    std::string price;
    std::string synth_volume;
};

std::array<Plaza2DecodedFieldValue, 10> decoded_fields(const InputRow& row) {
    return {
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplOrdersAggrReplId,
                                .kind = Plaza2DecodedValueKind::UnsignedInteger,
                                .unsigned_value = row.repl_id},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplOrdersAggrReplRev,
                                .kind = Plaza2DecodedValueKind::SignedInteger,
                                .signed_value = row.repl_rev},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplOrdersAggrReplAct,
                                .kind = Plaza2DecodedValueKind::SignedInteger,
                                .signed_value = row.repl_act},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplOrdersAggrIsinId,
                                .kind = Plaza2DecodedValueKind::SignedInteger,
                                .signed_value = row.isin_id},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplOrdersAggrDir,
                                .kind = Plaza2DecodedValueKind::SignedInteger,
                                .signed_value = row.dir},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplOrdersAggrVolume,
                                .kind = Plaza2DecodedValueKind::SignedInteger,
                                .signed_value = row.volume},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplOrdersAggrPrice,
                                .kind = Plaza2DecodedValueKind::Decimal,
                                .text_value = row.price},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplOrdersAggrMoment,
                                .kind = Plaza2DecodedValueKind::UnsignedInteger,
                                .unsigned_value = row.moment},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplOrdersAggrMomentNs,
                                .kind = Plaza2DecodedValueKind::UnsignedInteger,
                                .unsigned_value = row.moment_ns},
        Plaza2DecodedFieldValue{.field_code = FieldCode::kFortsAggrReplOrdersAggrSynthVolume,
                                .kind = Plaza2DecodedValueKind::String,
                                .text_value = row.synth_volume},
    };
}

Plaza2Aggr20Level make_level(const InputRow& row) {
    Plaza2Aggr20Level level;
    level.isin_id = row.isin_id;
    level.price = row.price;
    level.price_scaled = parse_fixed_point(row.price, 5, true, 16).value();
    level.volume = row.repl_act == 0 ? row.volume : 0;
    level.dir = static_cast<std::int32_t>(row.dir);
    level.repl_id = row.repl_id;
    level.repl_rev = row.repl_rev;
    level.moment = row.moment;
    level.moment_ns = row.moment_ns;
    level.synth_volume = row.synth_volume;
    return level;
}

bool equal_level(const Plaza2Aggr20Level& lhs, const Plaza2Aggr20Level& rhs) {
    return lhs.isin_id == rhs.isin_id && lhs.price_scaled == rhs.price_scaled && lhs.volume == rhs.volume &&
           lhs.dir == rhs.dir && lhs.repl_id == rhs.repl_id && lhs.repl_rev == rhs.repl_rev &&
           lhs.moment == rhs.moment && lhs.moment_ns == rhs.moment_ns && lhs.price == rhs.price &&
           lhs.synth_volume == rhs.synth_volume;
}

bool equal_optional_level(const std::optional<Plaza2Aggr20Level>& lhs, const std::optional<Plaza2Aggr20Level>& rhs) {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    return !lhs.has_value() || equal_level(*lhs, *rhs);
}

bool equal_instrument_snapshot(const Plaza2Aggr20InstrumentSnapshot& lhs, const Plaza2Aggr20InstrumentSnapshot& rhs) {
    if (lhs.isin_id != rhs.isin_id || lhs.row_count != rhs.row_count || lhs.bid_depth_levels != rhs.bid_depth_levels ||
        lhs.ask_depth_levels != rhs.ask_depth_levels || lhs.last_repl_id != rhs.last_repl_id ||
        lhs.last_repl_rev != rhs.last_repl_rev || !equal_optional_level(lhs.top_bid, rhs.top_bid) ||
        !equal_optional_level(lhs.top_ask, rhs.top_ask) || lhs.source_snapshot_version != rhs.source_snapshot_version ||
        lhs.source_snapshot_hash != rhs.source_snapshot_hash || lhs.committed_at != rhs.committed_at ||
        lhs.exchange_moment != rhs.exchange_moment || lhs.exchange_moment_ns != rhs.exchange_moment_ns ||
        lhs.levels.size() != rhs.levels.size()) {
        return false;
    }
    return std::equal(lhs.levels.begin(), lhs.levels.end(), rhs.levels.begin(), equal_level);
}

bool equal_snapshot(const Plaza2Aggr20Snapshot& lhs, const Plaza2Aggr20Snapshot& rhs) {
    if (lhs.row_count != rhs.row_count || lhs.instrument_count != rhs.instrument_count ||
        lhs.bid_depth_levels != rhs.bid_depth_levels || lhs.ask_depth_levels != rhs.ask_depth_levels ||
        lhs.last_repl_id != rhs.last_repl_id || lhs.last_repl_rev != rhs.last_repl_rev ||
        !equal_optional_level(lhs.top_bid, rhs.top_bid) || !equal_optional_level(lhs.top_ask, rhs.top_ask) ||
        lhs.committed_at != rhs.committed_at || lhs.exchange_moment != rhs.exchange_moment ||
        lhs.exchange_moment_ns != rhs.exchange_moment_ns || lhs.levels.size() != rhs.levels.size()) {
        return false;
    }
    return std::equal(lhs.levels.begin(), lhs.levels.end(), rhs.levels.begin(), equal_level);
}

bool aggr_level_less(const Plaza2Aggr20Level& lhs, const Plaza2Aggr20Level& rhs) {
    const auto side_rank = [](std::int32_t dir) {
        if (dir == 1)
            return 0;
        if (dir == 2)
            return 1;
        return 2;
    };
    const auto lhs_rank = side_rank(lhs.dir);
    const auto rhs_rank = side_rank(rhs.dir);
    if (lhs_rank != rhs_rank)
        return lhs_rank < rhs_rank;
    if (lhs.dir == 1 && lhs.price_scaled != rhs.price_scaled)
        return lhs.price_scaled > rhs.price_scaled;
    if (lhs.dir == 2 && lhs.price_scaled != rhs.price_scaled)
        return lhs.price_scaled < rhs.price_scaled;
    if (lhs.price_scaled != rhs.price_scaled)
        return lhs.price_scaled < rhs.price_scaled;
    if (lhs.repl_id != rhs.repl_id)
        return lhs.repl_id < rhs.repl_id;
    if (lhs.repl_rev != rhs.repl_rev)
        return lhs.repl_rev < rhs.repl_rev;
    if (lhs.moment != rhs.moment)
        return lhs.moment < rhs.moment;
    return lhs.moment_ns < rhs.moment_ns;
}

class LegacyProjector {
  public:
    using Clock = std::chrono::steady_clock;
    using NowFn = std::function<Clock::time_point()>;

    explicit LegacyProjector(NowFn now) : now_(std::move(now)) {}

    void reset() {
        staged_rows_.clear();
        affected_isin_ids_.clear();
        committed_ = {};
        instrument_snapshots_.clear();
        transaction_open_ = false;
    }

    void begin_transaction() {
        staged_rows_.clear();
        affected_isin_ids_.clear();
        transaction_open_ = true;
    }

    void stage(const InputRow& row) {
        const auto level = make_level(row);
        affected_isin_ids_.insert(level.isin_id);
        staged_rows_.push_back(level);
    }

    void commit() {
        for (const auto& row : staged_rows_) {
            auto existing = std::find_if(committed_.levels.begin(), committed_.levels.end(),
                                         [&](const auto& level) { return level.repl_id == row.repl_id; });
            if (existing != committed_.levels.end()) {
                affected_isin_ids_.insert(existing->isin_id);
            }
            committed_.last_repl_id = std::max(committed_.last_repl_id, row.repl_id);
            committed_.last_repl_rev = std::max(committed_.last_repl_rev, row.repl_rev);
            if (row.volume <= 0) {
                if (existing != committed_.levels.end()) {
                    committed_.levels.erase(existing);
                }
                continue;
            }
            if (existing == committed_.levels.end()) {
                committed_.levels.push_back(row);
            } else {
                *existing = row;
            }
        }

        committed_.row_count = committed_.levels.size();
        committed_.committed_at = now_();
        committed_.exchange_moment = 0;
        committed_.exchange_moment_ns = 0;
        committed_.bid_depth_levels = 0;
        committed_.ask_depth_levels = 0;
        committed_.top_bid.reset();
        committed_.top_ask.reset();
        for (const auto& isin_id : affected_isin_ids_) {
            const auto previous = instrument_snapshots_.find(isin_id);
            Plaza2Aggr20InstrumentSnapshot scoped;
            if (previous != instrument_snapshots_.end())
                scoped = previous->second;
            scoped.isin_id = isin_id;
            for (const auto& row : staged_rows_) {
                if (row.isin_id != isin_id)
                    continue;
                scoped.last_repl_id = std::max(scoped.last_repl_id, row.repl_id);
                scoped.last_repl_rev = std::max(scoped.last_repl_rev, row.repl_rev);
                scoped.exchange_moment = std::max(scoped.exchange_moment, row.moment);
                scoped.exchange_moment_ns = std::max(scoped.exchange_moment_ns, row.moment_ns);
            }
            scoped.row_count = 0;
            scoped.bid_depth_levels = 0;
            scoped.ask_depth_levels = 0;
            scoped.top_bid.reset();
            scoped.top_ask.reset();
            scoped.levels.clear();
            for (const auto& level : committed_.levels) {
                if (level.isin_id != isin_id)
                    continue;
                scoped.levels.push_back(level);
                ++scoped.row_count;
                if (level.dir == 1) {
                    ++scoped.bid_depth_levels;
                    if (!scoped.top_bid.has_value() || level.price_scaled > scoped.top_bid->price_scaled)
                        scoped.top_bid = level;
                } else if (level.dir == 2) {
                    ++scoped.ask_depth_levels;
                    if (!scoped.top_ask.has_value() || level.price_scaled < scoped.top_ask->price_scaled)
                        scoped.top_ask = level;
                }
            }
            std::ranges::sort(scoped.levels, aggr_level_less);
            const auto source_hash = hash_instrument_snapshot(scoped.isin_id, scoped.last_repl_id, scoped.last_repl_rev,
                                                              scoped.exchange_moment, scoped.exchange_moment_ns,
                                                              std::span<const Plaza2Aggr20Level>(scoped.levels));
            const bool source_changed =
                previous == instrument_snapshots_.end() || scoped.source_snapshot_hash != source_hash;
            scoped.source_snapshot_hash = source_hash;
            if (source_changed) {
                scoped.source_snapshot_version = ++snapshot_version_counter_;
                scoped.committed_at = committed_.committed_at;
            }
            instrument_snapshots_[isin_id] = std::move(scoped);
        }

        std::set<std::int64_t> instruments;
        for (const auto& level : committed_.levels) {
            instruments.insert(level.isin_id);
            committed_.exchange_moment = std::max(committed_.exchange_moment, level.moment);
            committed_.exchange_moment_ns = std::max(committed_.exchange_moment_ns, level.moment_ns);
            if (level.dir == 1) {
                ++committed_.bid_depth_levels;
                if (!committed_.top_bid.has_value() || level.price_scaled > committed_.top_bid->price_scaled)
                    committed_.top_bid = level;
            } else if (level.dir == 2) {
                ++committed_.ask_depth_levels;
                if (!committed_.top_ask.has_value() || level.price_scaled < committed_.top_ask->price_scaled)
                    committed_.top_ask = level;
            }
        }
        committed_.instrument_count = instruments.size();
        staged_rows_.clear();
        affected_isin_ids_.clear();
        transaction_open_ = false;
    }

    void rollback() {
        staged_rows_.clear();
        affected_isin_ids_.clear();
        transaction_open_ = false;
    }

    [[nodiscard]] const Plaza2Aggr20Snapshot& snapshot() const {
        return committed_;
    }

    [[nodiscard]] std::optional<Plaza2Aggr20InstrumentSnapshot> snapshot_for_isin(std::int64_t isin_id) const {
        const auto it = instrument_snapshots_.find(isin_id);
        return it == instrument_snapshots_.end() ? std::nullopt : std::optional{it->second};
    }

  private:
    static std::uint64_t hash_instrument_snapshot(std::int64_t isin_id, std::uint64_t source_repl_id,
                                                  std::int64_t source_repl_rev, std::uint64_t exchange_moment,
                                                  std::uint64_t exchange_moment_ns,
                                                  std::span<const Plaza2Aggr20Level> levels) {
        auto append_byte = [](std::uint64_t& hash, std::uint8_t value) {
            hash ^= value;
            hash *= 1099511628211ULL;
        };
        auto append_u64 = [&](std::uint64_t& hash, std::uint64_t value) {
            for (unsigned index = 0; index < 8; ++index)
                append_byte(hash, static_cast<std::uint8_t>(value >> (index * 8)));
        };
        std::uint64_t hash = 14695981039346656037ULL;
        append_u64(hash, static_cast<std::uint64_t>(isin_id));
        append_u64(hash, source_repl_id);
        append_u64(hash, static_cast<std::uint64_t>(source_repl_rev));
        append_u64(hash, exchange_moment);
        append_u64(hash, exchange_moment_ns);
        append_u64(hash, levels.size());
        for (const auto& level : levels) {
            append_u64(hash, static_cast<std::uint64_t>(level.isin_id));
            append_u64(hash, static_cast<std::uint64_t>(level.price_scaled));
            append_u64(hash, static_cast<std::uint64_t>(level.volume));
            append_u64(hash, static_cast<std::uint64_t>(level.dir));
            append_u64(hash, level.repl_id);
            append_u64(hash, static_cast<std::uint64_t>(level.repl_rev));
            append_u64(hash, level.moment);
            append_u64(hash, level.moment_ns);
        }
        return hash;
    }

    std::vector<Plaza2Aggr20Level> staged_rows_;
    std::unordered_set<std::int64_t> affected_isin_ids_;
    Plaza2Aggr20Snapshot committed_;
    std::unordered_map<std::int64_t, Plaza2Aggr20InstrumentSnapshot> instrument_snapshots_;
    std::uint64_t snapshot_version_counter_{0};
    NowFn now_;
    bool transaction_open_{false};
};

class RecordingObserver final : public Plaza2Aggr20QualificationObserver {
  public:
    void committed(const Plaza2Aggr20Snapshot& snapshot) noexcept override {
        last = snapshot;
    }

    std::optional<Plaza2Aggr20Snapshot> last;
};

void verify_observer_global_order(const RecordingObserver& observer, const Plaza2Aggr20Snapshot& actual,
                                  std::string_view phase) {
    if (!observer.last.has_value() || !equal_snapshot(*observer.last, actual))
        throw std::runtime_error("qualification observer global ordered view mismatch: " + std::string(phase));
    if (observer.last->levels.size() != actual.levels.size() ||
        !std::equal(observer.last->levels.begin(), observer.last->levels.end(), actual.levels.begin(), equal_level))
        throw std::runtime_error("qualification observer global level order mismatch: " + std::string(phase));
}

struct DifferentialCounts {
    std::size_t transactions{0};
    std::size_t commits{0};
    std::size_t rollbacks{0};
    std::size_t rows{0};
};

template <typename Actual, typename Legacy>
void compare_projectors(Actual& actual, Legacy& legacy, std::span<const std::int64_t> isins, std::string_view phase) {
    const auto& actual_global = actual.snapshot();
    const auto& legacy_global = legacy.snapshot();
    if (!equal_snapshot(actual_global, legacy_global)) {
        throw std::runtime_error("global differential mismatch: " + std::string(phase));
    }
    for (const auto isin_id : isins) {
        const auto actual_scoped = actual.snapshot_for_isin(isin_id);
        const auto legacy_scoped = legacy.snapshot_for_isin(isin_id);
        if (actual_scoped.has_value() != legacy_scoped.has_value() ||
            (actual_scoped.has_value() && !equal_instrument_snapshot(*actual_scoped, *legacy_scoped))) {
            std::cerr << phase << " isin=" << isin_id << " actual=";
            if (!actual_scoped) {
                std::cerr << "absent";
            } else {
                std::cerr << "rows=" << actual_scoped->row_count << " last=" << actual_scoped->last_repl_id << '/'
                          << actual_scoped->last_repl_rev << " version/hash=" << actual_scoped->source_snapshot_version
                          << '/' << actual_scoped->source_snapshot_hash << " depth=" << actual_scoped->bid_depth_levels
                          << '/' << actual_scoped->ask_depth_levels << " exch=" << actual_scoped->exchange_moment << '/'
                          << actual_scoped->exchange_moment_ns << " levels=";
                for (const auto& level : actual_scoped->levels)
                    std::cerr << '[' << level.repl_id << ',' << level.isin_id << ',' << level.dir << ',' << level.price
                              << ',' << level.volume << ',' << level.repl_rev << ',' << level.moment << ','
                              << level.moment_ns << ',' << level.synth_volume << ']';
            }
            std::cerr << " legacy=";
            if (!legacy_scoped) {
                std::cerr << "absent";
            } else {
                std::cerr << "rows=" << legacy_scoped->row_count << " last=" << legacy_scoped->last_repl_id << '/'
                          << legacy_scoped->last_repl_rev << " version/hash=" << legacy_scoped->source_snapshot_version
                          << '/' << legacy_scoped->source_snapshot_hash << " depth=" << legacy_scoped->bid_depth_levels
                          << '/' << legacy_scoped->ask_depth_levels << " exch=" << legacy_scoped->exchange_moment << '/'
                          << legacy_scoped->exchange_moment_ns << " levels=";
                for (const auto& level : legacy_scoped->levels)
                    std::cerr << '[' << level.repl_id << ',' << level.isin_id << ',' << level.dir << ',' << level.price
                              << ',' << level.volume << ',' << level.repl_rev << ',' << level.moment << ','
                              << level.moment_ns << ',' << level.synth_volume << ']';
            }
            std::cerr << '\n';
            throw std::runtime_error("scoped differential mismatch: " + std::string(phase) +
                                     " isin=" + std::to_string(isin_id));
        }
    }
}

void stage_actual_and_legacy(moex::plaza2::cgate::Plaza2Aggr20BookProjector& actual, LegacyProjector& legacy,
                             const InputRow& row) {
    auto fields = decoded_fields(row);
    const Plaza2Error actual_error = actual.on_row(fields);
    if (actual_error) {
        throw std::runtime_error("optimized projector rejected a valid differential row");
    }
    legacy.stage(row);
}

void replay_transaction(moex::plaza2::cgate::Plaza2Aggr20BookProjector& actual, LegacyProjector& legacy,
                        std::span<const InputRow> rows, bool rollback, std::span<const std::int64_t> isins,
                        std::string_view phase, const RecordingObserver& observer, DifferentialCounts& counts) {
    ++counts.transactions;
    counts.rows += rows.size();
    if (rollback)
        ++counts.rollbacks;
    else
        ++counts.commits;
    actual.begin_transaction();
    legacy.begin_transaction();
    for (const auto& row : rows)
        stage_actual_and_legacy(actual, legacy, row);
    compare_projectors(actual, legacy, isins, std::string(phase) + " before publication");
    if (rollback) {
        actual.rollback();
        legacy.rollback();
    } else {
        const auto actual_error = actual.commit();
        if (actual_error)
            throw std::runtime_error("optimized projector commit failed");
        legacy.commit();
        verify_observer_global_order(observer, actual.snapshot(), phase);
    }
    compare_projectors(actual, legacy, isins, phase);
}

InputRow make_row(std::uint64_t repl_id, std::int64_t rev, std::int64_t isin_id, std::int64_t dir,
                  std::string_view price, std::int64_t volume, std::int64_t repl_act = 0, std::uint64_t moment = 0,
                  std::uint64_t moment_ns = 0) {
    return {.repl_id = repl_id,
            .repl_rev = rev,
            .repl_act = repl_act,
            .isin_id = isin_id,
            .dir = dir,
            .volume = volume,
            .moment = moment,
            .moment_ns = moment_ns,
            .price = std::string(price),
            .synth_volume = {}};
}

struct LargeIndexScenario {
    std::vector<InputRow> seed;
    std::vector<InputRow> mutation;
    std::vector<InputRow> rollback;
    std::vector<std::int64_t> isins;
    std::size_t existing_update_rows{0};
    std::size_t delete_rows{0};
    std::size_t reinsertion_rows{0};
    std::size_t follow_up_update_rows{0};
};

std::string large_index_price(std::size_t level) {
    const auto whole = level < 20 ? 100 - static_cast<int>(level) : 101 + static_cast<int>(level - 20);
    return std::to_string(whole) + ".00000";
}

LargeIndexScenario make_large_index_scenario() {
    constexpr std::size_t kInstrumentCount = 8;
    constexpr std::size_t kLevelsPerInstrument = 40;
    constexpr std::size_t kSeededSlots = kInstrumentCount * kLevelsPerInstrument;
    constexpr std::size_t kDeletedSlots = 16;

    LargeIndexScenario scenario;
    scenario.seed.reserve(kSeededSlots);
    scenario.mutation.reserve(kSeededSlots + kDeletedSlots * 3);
    scenario.rollback.reserve(kSeededSlots + kDeletedSlots * 3);
    scenario.existing_update_rows = kSeededSlots - kDeletedSlots;
    scenario.delete_rows = kDeletedSlots;
    scenario.reinsertion_rows = kDeletedSlots;
    scenario.follow_up_update_rows = kDeletedSlots;
    for (std::size_t instrument = 0; instrument < kInstrumentCount; ++instrument)
        scenario.isins.push_back(5000 + static_cast<std::int64_t>(instrument));
    for (std::size_t instrument = 0; instrument < 2; ++instrument)
        scenario.isins.push_back(6000 + static_cast<std::int64_t>(instrument));
    scenario.isins.insert(scenario.isins.end(), {6002, 6003, 6004, 6005});

    for (std::size_t slot = 0; slot < kSeededSlots; ++slot) {
        const auto level = slot % kLevelsPerInstrument;
        const auto isin_id = 5000 + static_cast<std::int64_t>(slot / kLevelsPerInstrument);
        const auto dir = level < 20 ? 1 : 2;
        scenario.seed.push_back(make_row(10'000 + slot, 1, isin_id, dir, large_index_price(level),
                                         10 + static_cast<std::int64_t>(level), 0, 1 + slot, slot));
    }

    // One transaction has 304 existing-row updates, 16 deletes, 16 reinsertions,
    // and 16 same-burst follow-up updates. It therefore crosses both 256-row
    // thresholds while exercising migration and free-slot reuse.
    for (std::size_t slot = 0; slot < kSeededSlots; ++slot) {
        const auto level = slot % kLevelsPerInstrument;
        const auto repl_id = 10'000 + slot;
        const auto old_isin = 5000 + static_cast<std::int64_t>(slot / kLevelsPerInstrument);
        const auto dir = level < 20 ? 1 : 2;
        if (slot < kDeletedSlots) {
            scenario.mutation.push_back(make_row(repl_id, 2, old_isin, dir, large_index_price(level), 0, 0,
                                                 1000 + slot, slot));
            continue;
        }
        const auto isin_id = slot >= 64 && slot < 96
                                 ? 6000 + static_cast<std::int64_t>(slot % 2)
                                 : old_isin;
        scenario.mutation.push_back(make_row(repl_id, 2, isin_id, dir, large_index_price(level),
                                             20 + static_cast<std::int64_t>(level), 0, 1000 + slot, slot));
    }
    for (std::size_t index = 0; index < 8; ++index) {
        scenario.mutation.push_back(make_row(10'000 + index, 3, 6002, index % 2 == 0 ? 1 : 2, "200.00000",
                                             30 + static_cast<std::int64_t>(index), 0, 2000 + index, index));
    }
    for (std::size_t index = 8; index < kDeletedSlots; ++index) {
        scenario.mutation.push_back(make_row(20'000 + index, 1, 6003, index % 2 == 0 ? 1 : 2, "201.00000",
                                             40 + static_cast<std::int64_t>(index), 0, 2000 + index, index));
    }
    for (std::size_t index = 0; index < kDeletedSlots; ++index) {
        const auto repl_id = index < 8 ? 10'000 + index : 20'000 + index;
        const auto revision = index < 8 ? 4 : 2;
        const auto isin_id = index < 8 ? 6002 : 6003;
        scenario.mutation.push_back(make_row(repl_id, revision, isin_id, index % 2 == 0 ? 2 : 1,
                                             index < 8 ? "200.50000" : "201.50000",
                                             31 + static_cast<std::int64_t>(index), 0, 3000 + index, index));
    }

    for (std::size_t slot = 0; slot < kSeededSlots; ++slot) {
        const auto level = slot % kLevelsPerInstrument;
        const auto repl_id = slot < 8 ? 10'000 + slot : slot < kDeletedSlots ? 20'000 + slot : 10'000 + slot;
        const auto isin_id = slot < 8
                                 ? 6002
                                 : slot < kDeletedSlots
                                       ? 6003
                                       : slot >= 64 && slot < 96 ? 6000 + static_cast<std::int64_t>(slot % 2)
                                                                  : 5000 + static_cast<std::int64_t>(slot / 40);
        const auto dir = level < 20 ? 1 : 2;
        if (slot < kDeletedSlots) {
            scenario.rollback.push_back(make_row(repl_id, 100 + static_cast<std::int64_t>(slot), isin_id, dir,
                                                 "202.00000", 0, 0, 4000 + slot, slot));
        } else {
            scenario.rollback.push_back(make_row(repl_id, 100 + static_cast<std::int64_t>(slot), isin_id, dir,
                                                 "202.00000", 50 + static_cast<std::int64_t>(level), 0, 4000 + slot,
                                                 slot));
        }
    }
    for (std::size_t index = 0; index < 8; ++index) {
        scenario.rollback.push_back(make_row(10'000 + index, 200, 6004, index % 2 == 0 ? 1 : 2, "203.00000", 60, 0,
                                             5000 + index, index));
    }
    for (std::size_t index = 8; index < kDeletedSlots; ++index) {
        scenario.rollback.push_back(make_row(20'000 + index, 200, 6005, index % 2 == 0 ? 1 : 2, "204.00000", 70, 0,
                                             5000 + index, index));
    }
    for (std::size_t index = 0; index < kDeletedSlots; ++index) {
        const auto repl_id = index < 8 ? 10'000 + index : 20'000 + index;
        scenario.rollback.push_back(make_row(repl_id, 201, index < 8 ? 6004 : 6005, index % 2 == 0 ? 2 : 1,
                                             index < 8 ? "203.50000" : "204.50000", 61 + index, 0, 6000 + index,
                                             index));
    }

    if (scenario.seed.size() < 256 || scenario.mutation.size() < 256 || scenario.rollback.size() < 256 ||
        scenario.mutation.size() != scenario.existing_update_rows + scenario.delete_rows +
                                         scenario.reinsertion_rows + scenario.follow_up_update_rows)
        throw std::runtime_error("large-index scenario did not cross the 256-row threshold");
    return scenario;
}

template <typename Actual, typename Legacy>
void compare_scoped_only(Actual& actual, Legacy& legacy, std::span<const std::int64_t> isins, std::string_view phase) {
    for (const auto isin_id : isins) {
        const auto actual_scoped = actual.snapshot_for_isin(isin_id);
        const auto legacy_scoped = legacy.snapshot_for_isin(isin_id);
        if (actual_scoped.has_value() != legacy_scoped.has_value() ||
            (actual_scoped.has_value() && !equal_instrument_snapshot(*actual_scoped, *legacy_scoped)))
            throw std::runtime_error("scoped-only differential mismatch: " + std::string(phase) +
                                     " isin=" + std::to_string(isin_id));
    }
}

void replay_scoped_transaction(moex::plaza2::cgate::Plaza2Aggr20BookProjector& actual, LegacyProjector& legacy,
                               std::span<const InputRow> rows, std::span<const std::int64_t> isins,
                               std::string_view phase) {
    actual.begin_transaction();
    legacy.begin_transaction();
    for (const auto& row : rows)
        stage_actual_and_legacy(actual, legacy, row);
    const auto actual_error = actual.commit();
    if (actual_error)
        throw std::runtime_error("optimized projector commit failed in scoped-only sequence");
    legacy.commit();
    compare_scoped_only(actual, legacy, isins, phase);
}

void stage_projector(moex::plaza2::cgate::Plaza2Aggr20BookProjector& projector, std::span<const InputRow> rows) {
    projector.begin_transaction();
    for (const auto& row : rows) {
        const auto fields = decoded_fields(row);
        if (const auto error = projector.on_row(fields); error)
            throw std::runtime_error("optimized projector rejected a valid allocation-probe row: " + error.message);
    }
}

std::size_t run_lazy_sequence() {
    using moex::plaza2::cgate::Plaza2Aggr20BookProjector;
    auto now = Plaza2Aggr20BookProjector::Clock::time_point{} + std::chrono::seconds(400);
    auto now_fn = [&now] { return now; };
    Plaza2Aggr20BookProjector actual(now_fn);
    LegacyProjector legacy(now_fn);
    const std::array<std::int64_t, 5> isins = {1001, 1002, 1003, 1004, 1005};

    replay_scoped_transaction(actual, legacy,
                              std::array{make_row(1, 1, 1001, 1, "1.00000", 2, 0, 1),
                                         make_row(2, 2, 1002, 2, "2.00000", 3, 0, 2),
                                         make_row(3, 3, 1003, 1, "3.00000", 4, 0, 3)},
                              isins, "lazy seed");
    std::size_t commits = 1;
    for (std::size_t index = 0; index < 8; ++index) {
        now += std::chrono::seconds(1);
        std::vector<InputRow> rows;
        rows.push_back(make_row(1, static_cast<std::int64_t>(10 + index), 1001, index % 2 == 0 ? 2 : 1,
                                index % 2 == 0 ? "1.50000" : "1.25000", 5 + static_cast<std::int64_t>(index), 0,
                                10 + index));
        rows.push_back(make_row(2, static_cast<std::int64_t>(20 + index), 1002 + static_cast<std::int64_t>(index % 2),
                                2, "2.25000", 6, 0, 20 + index));
        if (index % 3 == 0)
            rows.push_back(
                make_row(100 + index, static_cast<std::int64_t>(30 + index), 1004, 1, "4.00000", 1, 0, 30 + index));
        if (index % 3 == 1)
            rows.push_back(
                make_row(100 + index - 1, static_cast<std::int64_t>(40 + index), 1004, 1, "4.00000", 0, 0, 40 + index));
        replay_scoped_transaction(actual, legacy, rows, isins, "lazy commit " + std::to_string(index));
        ++commits;
    }

    // This is the first global observation after several commits. The
    // scoped comparisons above deliberately never materialize the global
    // diagnostic vector.
    compare_projectors(actual, legacy, isins, "lazy final global");
    return commits;
}

std::size_t run_unique_churn() {
    using moex::plaza2::cgate::Plaza2Aggr20BookProjector;
    auto now = Plaza2Aggr20BookProjector::Clock::time_point{} + std::chrono::seconds(800);
    auto now_fn = [&now] { return now; };
    Plaza2Aggr20BookProjector actual(now_fn);
    LegacyProjector legacy(now_fn);
    const std::array<std::int64_t, 2> isins = {1001, 1002};
    replay_scoped_transaction(
        actual, legacy,
        std::array{make_row(1, 1, 1001, 1, "1.00000", 10, 0, 1), make_row(2, 1, 1002, 2, "2.00000", 10, 0, 2)}, isins,
        "churn seed");
    std::size_t transactions = 1;
    constexpr std::size_t cycles = 128;
    for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
        const auto repl_id = static_cast<std::uint64_t>(10'000 + cycle);
        const auto revision = static_cast<std::int64_t>(10 + cycle * 4);
        now += std::chrono::microseconds(1);
        replay_scoped_transaction(actual, legacy,
                                  std::array{make_row(repl_id, revision, 1001, 1, "5.00000", 3, 0, repl_id)}, isins,
                                  "churn insert " + std::to_string(cycle));
        now += std::chrono::microseconds(1);
        replay_scoped_transaction(actual, legacy,
                                  std::array{make_row(repl_id, revision + 1, 1001, 1, "5.00000", 0, 0, repl_id + 1)},
                                  isins, "churn delete " + std::to_string(cycle));
        now += std::chrono::microseconds(1);
        replay_scoped_transaction(actual, legacy,
                                  std::array{make_row(repl_id, revision + 2, 1001, 2, "5.50000", 4, 0, repl_id + 2)},
                                  isins, "churn reinsert " + std::to_string(cycle));
        now += std::chrono::microseconds(1);
        replay_scoped_transaction(actual, legacy,
                                  std::array{make_row(repl_id, revision + 3, 1001, 2, "5.50000", 0, 0, repl_id + 3)},
                                  isins, "churn final delete " + std::to_string(cycle));
        transactions += 4;
    }
    compare_projectors(actual, legacy, isins, "churn final global");
    return transactions;
}

struct AllocationProbeResult {
    std::size_t commit_failures{0};
    std::size_t commit_success_threshold{0};
    std::size_t global_materialization_failures{0};
};

AllocationProbeResult run_allocation_failure_probe() {
    using moex::plaza2::cgate::Plaza2Aggr20BookProjector;
    using test_allocation_injection::attempts;
    using test_allocation_injection::enabled;
    using test_allocation_injection::remaining;
    auto now = Plaza2Aggr20BookProjector::Clock::time_point{} + std::chrono::seconds(1200);
    auto now_fn = [&now] { return now; };
    const auto seed =
        std::array{make_row(1, 1, 1001, 1, "1.00000", 2, 0, 1), make_row(2, 2, 1001, 2, "2.00000", 3, 0, 2),
                   make_row(3, 3, 1002, 1, "3.00000", 4, 0, 3)};
    const auto mutation =
        std::array{make_row(1, 10, 1002, 2, "1.50000", 5, 0, 10), make_row(2, 11, 1001, 1, "2.50000", 6, 0, 11),
                   make_row(3, 12, 1002, 1, "3.00000", 0, 0, 12), make_row(1000, 13, 1001, 2, "4.00000", 7, 0, 13),
                   make_row(1000, 14, 1003, 1, "4.50000", 8, 0, 14)};
    AllocationProbeResult result;
    bool commit_succeeded = false;
    for (std::size_t threshold = 0; threshold < 1024 && !commit_succeeded; ++threshold) {
        Plaza2Aggr20BookProjector candidate(now_fn);
        stage_projector(candidate, seed);
        if (const auto error = candidate.commit(); error)
            throw std::runtime_error("allocation probe seed commit failed: " + error.message);
        const auto before = candidate.snapshot();
        stage_projector(candidate, mutation);
        enabled = true;
        remaining = threshold;
        attempts = 0;
        bool failed = false;
        try {
            if (const auto error = candidate.commit(); error)
                throw std::runtime_error("allocation probe commit returned an error: " + error.message);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        enabled = false;
        if (failed) {
            ++result.commit_failures;
            if (!candidate.transaction_open())
                throw std::runtime_error("allocation failure closed the transaction before rollback");
            if (!equal_snapshot(before, candidate.snapshot()))
                throw std::runtime_error("allocation failure changed the published snapshot");
            candidate.rollback();
            if (candidate.transaction_open() || !equal_snapshot(before, candidate.snapshot()))
                throw std::runtime_error("rollback did not restore the allocation-failure snapshot");
        } else {
            result.commit_success_threshold = threshold;
            commit_succeeded = true;
        }
    }
    if (!commit_succeeded || result.commit_failures == 0)
        throw std::runtime_error("allocation probe did not cover both failing and successful commit plans");

    Plaza2Aggr20BookProjector global_candidate(now_fn);
    stage_projector(global_candidate, seed);
    if (const auto error = global_candidate.commit(); error)
        throw std::runtime_error("global allocation probe seed commit failed: " + error.message);
    const auto before_global = global_candidate.snapshot();
    stage_projector(global_candidate, std::array{make_row(2000, 20, 1001, 1, "6.00000", 9, 0, 20)});
    if (const auto error = global_candidate.commit(); error)
        throw std::runtime_error("global allocation probe commit failed: " + error.message);
    enabled = true;
    remaining = 0;
    attempts = 0;
    bool global_failed = false;
    try {
        static_cast<void>(global_candidate.snapshot());
    } catch (const std::bad_alloc&) {
        global_failed = true;
    }
    enabled = false;
    if (!global_failed)
        throw std::runtime_error("lazy global materialization did not expose the injected allocation failure");
    ++result.global_materialization_failures;
    const auto& recovered_global = global_candidate.snapshot();
    if (recovered_global.row_count != before_global.row_count + 1 || recovered_global.levels.size() != 4)
        throw std::runtime_error("lazy global materialization retry did not publish the complete book");
    return result;
}

struct LargeIndexProbeResult {
    std::size_t seeded_slots{0};
    std::size_t mutation_rows{0};
    std::size_t rollback_rows{0};
    std::size_t existing_update_rows{0};
    std::size_t delete_rows{0};
    std::size_t reinsertion_rows{0};
    std::size_t follow_up_update_rows{0};
    std::size_t allocation_commit_failures{0};
    std::size_t allocation_commit_success_threshold{0};
};

LargeIndexProbeResult run_large_index_allocation_probe(const LargeIndexScenario& scenario,
                                                       std::span<const std::int64_t> isins) {
    using moex::plaza2::cgate::Plaza2Aggr20BookProjector;
    using test_allocation_injection::attempts;
    using test_allocation_injection::enabled;
    using test_allocation_injection::remaining;

    auto now = Plaza2Aggr20BookProjector::Clock::time_point{} + std::chrono::seconds(1600);
    auto now_fn = [&now] { return now; };
    LargeIndexProbeResult result{.seeded_slots = scenario.seed.size(),
                                 .mutation_rows = scenario.mutation.size(),
                                 .rollback_rows = scenario.rollback.size(),
                                 .existing_update_rows = scenario.existing_update_rows,
                                 .delete_rows = scenario.delete_rows,
                                 .reinsertion_rows = scenario.reinsertion_rows,
                                 .follow_up_update_rows = scenario.follow_up_update_rows};
    bool commit_succeeded = false;

    // The threshold sweep repeats a fully seeded candidate so every attempted
    // large commit has at least 320 retained slots before its 352-row mutation.
    constexpr std::size_t kMaxAllocationThreshold = 8192;
    for (std::size_t threshold = 0; threshold < kMaxAllocationThreshold && !commit_succeeded; ++threshold) {
        Plaza2Aggr20BookProjector candidate(now_fn);
        LegacyProjector legacy(now_fn);
        RecordingObserver observer;
        candidate.set_qualification_observer(&observer);

        candidate.begin_transaction();
        legacy.begin_transaction();
        for (const auto& row : scenario.seed)
            stage_actual_and_legacy(candidate, legacy, row);
        if (const auto error = candidate.commit(); error)
            throw std::runtime_error("large-index allocation probe seed commit failed: " + error.message);
        legacy.commit();
        verify_observer_global_order(observer, candidate.snapshot(), "large-index allocation seed");
        compare_projectors(candidate, legacy, isins, "large-index allocation seed");
        const auto before = candidate.snapshot();

        candidate.begin_transaction();
        legacy.begin_transaction();
        for (const auto& row : scenario.mutation)
            stage_actual_and_legacy(candidate, legacy, row);
        compare_projectors(candidate, legacy, isins, "large-index allocation before publication");

        enabled = true;
        remaining = threshold;
        attempts = 0;
        bool failed = false;
        try {
            if (const auto error = candidate.commit(); error) {
                enabled = false;
                throw std::runtime_error("large-index allocation probe commit returned an error: " + error.message);
            }
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        enabled = false;

        if (failed) {
            ++result.allocation_commit_failures;
            if (!candidate.transaction_open())
                throw std::runtime_error("large-index allocation failure closed the transaction before rollback");
            if (!equal_snapshot(before, candidate.snapshot()))
                throw std::runtime_error("large-index allocation failure changed the published snapshot");
            if (!observer.last.has_value() || !equal_snapshot(*observer.last, before))
                throw std::runtime_error("large-index allocation failure changed the observer snapshot");
            candidate.rollback();
            legacy.rollback();
            compare_projectors(candidate, legacy, isins, "large-index allocation rollback");
            if (candidate.transaction_open() || !equal_snapshot(before, candidate.snapshot()))
                throw std::runtime_error("large-index allocation rollback did not restore the seed snapshot");
        } else {
            legacy.commit();
            verify_observer_global_order(observer, candidate.snapshot(), "large-index allocation commit");
            compare_projectors(candidate, legacy, isins, "large-index allocation commit");
            result.allocation_commit_success_threshold = threshold;
            commit_succeeded = true;
        }
    }

    if (!commit_succeeded || result.allocation_commit_failures == 0)
        throw std::runtime_error("large-index allocation probe did not cover both failing and successful commits");
    return result;
}

LargeIndexProbeResult run_large_index_differential_case() {
    using moex::plaza2::cgate::Plaza2Aggr20BookProjector;

    const auto scenario = make_large_index_scenario();
    auto now = Plaza2Aggr20BookProjector::Clock::time_point{} + std::chrono::seconds(1400);
    auto now_fn = [&now] { return now; };
    Plaza2Aggr20BookProjector actual(now_fn);
    LegacyProjector legacy(now_fn);
    RecordingObserver observer;
    DifferentialCounts counts;
    actual.set_qualification_observer(&observer);
    const std::span<const std::int64_t> isins(scenario.isins);

    replay_transaction(actual, legacy, scenario.seed, false, isins, "large-index seed", observer, counts);
    replay_transaction(actual, legacy, scenario.mutation, false, isins, "large-index mutation", observer, counts);
    const auto before_rollback = actual.snapshot();
    const auto observer_before_rollback = observer.last;
    replay_transaction(actual, legacy, scenario.rollback, true, isins, "large-index rollback", observer, counts);
    if (!equal_snapshot(before_rollback, actual.snapshot()))
        throw std::runtime_error("large-index rollback changed the published global snapshot");
    if (!observer_before_rollback.has_value() || !observer.last.has_value() ||
        !equal_snapshot(*observer_before_rollback, *observer.last))
        throw std::runtime_error("large-index rollback changed the qualification observer snapshot");

    return run_large_index_allocation_probe(scenario, isins);
}

} // namespace

int main() {
    try {
        using namespace moex::plaza2::cgate;

        auto now = Plaza2Aggr20BookProjector::Clock::time_point{} + std::chrono::seconds(100);
        auto now_fn = [&now] { return now; };
        Plaza2Aggr20BookProjector actual(now_fn);
        LegacyProjector legacy(now_fn);
        RecordingObserver observer;
        DifferentialCounts counts;
        actual.set_qualification_observer(&observer);
        const std::array<std::int64_t, 12> isins = {0, 1001, 1002, 1003, 2001, 2002, 3001, 3002, 4001, 4002, 9999, -7};

        compare_projectors(actual, legacy, isins, "initial");
        replay_transaction(actual, legacy,
                           std::array{make_row(1, 11, 1001, 1, "-100.50000", 3, 0, 7, 42),
                                      make_row(2, 12, 1001, 2, "0", 4, 0, 8, 43),
                                      make_row(3, 13, 2002, 1, "100.50000", 5, 0, 9, 44),
                                      make_row(4, 14, 2002, 2, "0.00000", 6, 0, 10, 45)},
                           false, isins, "insert negative-zero", observer, counts);

        now += std::chrono::seconds(1);
        replay_transaction(actual, legacy,
                           std::array{make_row(1, 15, 1001, 2, "12.92700", 8, 0, 11, 46),
                                      make_row(2, 16, 2002, 1, "12.92800", 9, 0, 12, 47)},
                           false, isins, "price-side-isin migration", observer, counts);

        now += std::chrono::seconds(1);
        replay_transaction(actual, legacy,
                           std::array{make_row(3, 17, 2002, 1, "-1", 0, 0, 13, 48),
                                      make_row(4, 18, 2002, 2, "0", 1, 1, 14, 49),
                                      make_row(5, 19, 3001, 1, "0.00000", 2), make_row(5, 20, 3001, 2, "100.50000", 4)},
                           false, isins, "delete tombstone and same-burst updates", observer, counts);

        const auto before_rollback = actual.snapshot();
        now += std::chrono::seconds(1);
        replay_transaction(actual, legacy,
                           std::array{make_row(1, 21, 9999, 1, "-100.50000", 11), make_row(6, 22, 4001, 2, "0", 3)},
                           true, isins, "rollback", observer, counts);
        if (!equal_snapshot(before_rollback, actual.snapshot()))
            throw std::runtime_error("rollback changed the published global snapshot");

        now += std::chrono::seconds(1);
        actual.reset();
        legacy.reset();
        compare_projectors(actual, legacy, isins, "LifeNum/reset");
        replay_transaction(actual, legacy,
                           std::array{make_row(50, 50, 4002, 1, "-0.00001", 1), make_row(51, 51, 4002, 2, "0", 1)},
                           false, isins, "post-reset publication", observer, counts);

        std::mt19937_64 random(0x9a7f'20c5'4d11'6301ULL);
        constexpr std::array<std::string_view, 8> prices = {"-100.50000", "-0.00001", "0",         "0.00000",
                                                            "12.92600",   "12.92700", "100.50000", "999.99999"};
        for (std::size_t burst = 0; burst < 300; ++burst) {
            const auto row_count = static_cast<std::size_t>(1 + random() % 16);
            std::vector<InputRow> rows;
            rows.reserve(row_count);
            for (std::size_t index = 0; index < row_count; ++index) {
                const auto repl_id = 1 + random() % 80;
                const auto isin_id = 1001 + static_cast<std::int64_t>(random() % 8);
                const auto dir = random() % 12 == 0 ? 3 : static_cast<std::int64_t>(1 + random() % 2);
                const auto volume = random() % 5 == 0 ? 0 : static_cast<std::int64_t>(1 + random() % 20);
                const auto repl_act = volume > 0 && random() % 17 == 0 ? 1 : 0;
                rows.push_back(make_row(repl_id, static_cast<std::int64_t>(1000 + burst * 20 + index), isin_id, dir,
                                        prices[random() % prices.size()], volume, repl_act,
                                        static_cast<std::uint64_t>(burst * 100 + index),
                                        static_cast<std::uint64_t>(random() % 1000)));
            }
            now += std::chrono::microseconds(1 + random() % 100);
            replay_transaction(actual, legacy, rows, burst % 11 == 0, isins,
                               "random multi-row burst " + std::to_string(burst), observer, counts);
        }
        const auto lazy_commits = run_lazy_sequence();
        const auto churn_transactions = run_unique_churn();
        const auto allocation_probe = run_allocation_failure_probe();
        const auto large_index_probe = run_large_index_differential_case();
        std::cout << "differential transactions=" << counts.transactions << " commits=" << counts.commits
                  << " rollbacks=" << counts.rollbacks << " rows=" << counts.rows << " lazy_commits=" << lazy_commits
                  << " churn_transactions=" << churn_transactions
                  << " allocation_commit_failures=" << allocation_probe.commit_failures
                  << " allocation_commit_success_threshold=" << allocation_probe.commit_success_threshold
                  << " allocation_global_failures=" << allocation_probe.global_materialization_failures
                  << " large_index_seeded_slots=" << large_index_probe.seeded_slots
                  << " large_index_mutation_rows=" << large_index_probe.mutation_rows
                  << " large_index_rollback_rows=" << large_index_probe.rollback_rows
                  << " large_index_existing_updates=" << large_index_probe.existing_update_rows
                  << " large_index_delete_rows=" << large_index_probe.delete_rows
                  << " large_index_reinsert_rows=" << large_index_probe.reinsertion_rows
                  << " large_index_follow_up_updates=" << large_index_probe.follow_up_update_rows
                  << " large_index_allocation_failures=" << large_index_probe.allocation_commit_failures
                  << " large_index_allocation_success_threshold="
                  << large_index_probe.allocation_commit_success_threshold
                  << " small_40x1000_index_path=threshold_disabled_linear_unchanged\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
