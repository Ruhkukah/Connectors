#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
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
        if (!observer.last.has_value() || !equal_snapshot(*observer.last, actual.snapshot()))
            throw std::runtime_error("qualification observer did not receive the committed global snapshot");
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
        std::cout << "differential transactions=" << counts.transactions << " commits=" << counts.commits
                  << " rollbacks=" << counts.rollbacks << " rows=" << counts.rows << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
