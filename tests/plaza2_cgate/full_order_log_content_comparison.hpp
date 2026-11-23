#pragma once
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <iomanip>
#include <map>
#include <ostream>
#include <tuple>
#include <vector>
#include <functional>

namespace full_order_log_probe {
// An observation of identical order content within a bounded local arrival window.
// This is deliberately NOT a claim that two exchange revisions are equal.
using OrderSet = std::map<std::int64_t, std::tuple<std::int64_t, std::int64_t, std::int32_t, bool>>;
inline std::string order_set_digest(const OrderSet& orders) {
    std::string text = "fol_order_set_v1\n";
    for (const auto& [id, value] : orders) {
        const auto [remaining, price, session, bid] = value;
        text += std::to_string(id) + "," + std::to_string(remaining) + "," + std::to_string(price) + "," +
                std::to_string(session) + "," + std::to_string(bid) + "\n";
    }
    return moex::plaza2::cgate::plaza2_sha256_hex(text);
}
// info.moment is a CGate millisecond timestamp, not an invented moment_ns field.
struct PublicationInfo {
    bool provided{}, timestamp_consistent{true};
    std::int64_t info_revision{}, trades_revision{};
    std::uint64_t log_life{}, begin_utc_ns{}, end_utc_ns{}, max_order_moment_ns{}, received_utc_ns{};
    std::string begin_calendar, end_calendar;
};
struct ContentWindow {
    static constexpr std::uint64_t time_precision_ns = 1000000, clock_tolerance_ns = 1000000000;
    struct Change {
        std::int8_t action{};
        std::int64_t revision{}, remaining{};
        std::uint64_t moment_ns{}, moment_t_ns{};
        bool consistent{};
        std::uint64_t committed_sequence{};
    };
    using ChangeKey = std::pair<std::int32_t, std::int64_t>;
    std::map<ChangeKey, std::deque<Change>> changes;
    std::deque<ChangeKey> change_order;
    std::map<std::int32_t, std::uint64_t> first_composite_utc, publication_counts, fresh_counts;
    std::map<std::int32_t, PublicationInfo> publication_high_water;
    std::uint64_t state_sequence{}, change_evictions{}, pre_connection{}, unknown_publication{},
        unknown_later_publication{}, fresh_observations{}, fresh_equal{}, fresh_different{}, fresh_unresolved{},
        suspected_defects{}, stale_hypothesis_proven{};
    std::function<std::uint64_t()> utc_clock = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    void change(std::int32_t isin, std::int64_t id, Change event) {
        const ChangeKey key{isin, id};
        if (!event.committed_sequence)
            event.committed_sequence = state_sequence + 1;
        changes[key].push_back(event);
        change_order.push_back(key);
        while (change_order.size() > order_budget) {
            const auto oldest = change_order.front();
            change_order.pop_front();
            auto at = changes.find(oldest);
            at->second.pop_front();
            if (at->second.empty())
                changes.erase(at);
            ++change_evictions;
        }
    }

    struct Image {
        OrderSet orders;
        std::shared_ptr<std::size_t> retained;
        Image(OrderSet value, std::shared_ptr<std::size_t> count)
            : orders(std::move(value)), retained(std::move(count)) {
            *retained += orders.size();
        }
        ~Image() {
            *retained -= orders.size();
        }
    };
    using ImagePtr = std::shared_ptr<const Image>;
    struct State {
        std::int64_t at;
        std::string hash;
        ImagePtr image;
        bool commit{};
        std::uint64_t utc_ns{}, committed_sequence{};
    };
    struct Publication {
        std::int32_t isin;
        std::int64_t begin, end;
        std::string hash;
        bool eligible{}, truncated{};
        ImagePtr image;
        PublicationInfo info;
        bool initial{}, fresh{}, stale{}, time_unknown{}, advancing{};
    };
    std::map<std::int32_t, std::deque<State>> history;
    std::deque<Publication> pending;
    std::map<std::int32_t, bool> lost_history;
    std::int64_t tolerance_ms;
    std::size_t capacity;
    std::size_t order_budget;
    std::shared_ptr<std::size_t> retained_rows{std::make_shared<std::size_t>(0)};
    std::ostream* output;
    std::function<void()> resample;
    std::uint64_t matched{}, different{}, unresolved{}, evictions{}, observations{};
    explicit ContentWindow(std::int64_t tolerance = 2000, std::size_t cap = 4096, std::ostream* out = nullptr,
                           std::size_t rows = 1000000)
        : tolerance_ms(tolerance), capacity(cap), order_budget(rows), output(out) {
        if (tolerance < 0 || tolerance > 30000 || cap == 0 || cap > 65536 || !rows || rows > 10000000)
            throw std::invalid_argument("invalid content comparison bounds");
    }
    static std::int64_t now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    // State time is a simultaneous host-UTC capture, never the newest resting order's age.
    const State* nearest(const Publication& p) const {
        const State* result = nullptr;
        const auto h = history.find(p.isin);
        if (h != history.end())
            for (const auto& s : h->second)
                if (!result || std::abs(s.at - p.end) < std::abs(result->at - p.end))
                    result = &s;
        return result;
    }
    struct Difference {
        std::int64_t id;
        const char* kind;
    };
    std::vector<Difference> differences(const Publication& p, const State* state) const {
        std::vector<Difference> result;
        if (!state || !state->image || !p.image)
            return result;
        for (const auto& [id, value] : p.image->orders) {
            const auto at = state->image->orders.find(id);
            if (at == state->image->orders.end())
                result.push_back({id, "missing"});
            else if (at->second != value)
                result.push_back({id, "changed"});
        }
        for (const auto& [id, value] : state->image->orders)
            if (!p.image->orders.contains(id))
                result.push_back({id, "extra"});
        return result;
    }
    // 0=unproven, 1=every differing ID fits, 2=known contrary/changed-order evidence.
    int hypothesis(const Publication& p, const State* state) const {
        if (!state || !state->image || !p.image || !p.info.end_utc_ns || change_evictions)
            return 0;
        const auto ids = differences(p, state);
        if (ids.empty())
            return 0;
        bool unknown = false;
        for (const auto& difference : ids) {
            if (std::string_view(difference.kind) == "changed")
                return 2;
            const auto at = changes.find({p.isin, difference.id});
            const Change* last = nullptr;
            const Change* transition = nullptr;
            bool uncertain = false;
            if (at != changes.end())
                for (const auto& event : at->second) {
                    if (event.committed_sequence > state->committed_sequence)
                        continue;
                    if (!event.consistent || !event.moment_ns) {
                        uncertain = true;
                        continue;
                    }
                    if (event.committed_sequence <= state->committed_sequence &&
                        (!last || event.revision > last->revision))
                        last = &event;
                    if ((event.action == 1 || event.action == 0 || (event.action == 2 && !event.remaining)) &&
                        (!transition || event.revision > transition->revision))
                        transition = &event;
                }
            if (!last || !transition || uncertain) {
                unknown = true;
                continue;
            }
            if (last->action != 0 && last->action != 1 && last->action != 2) {
                unknown = true;
                continue;
            }
            const bool extra = std::string_view(difference.kind) == "extra";
            const bool removed = last->action == 0 || (last->action == 2 && last->remaining == 0);
            const bool fits = transition->moment_ns >= p.info.end_utc_ns + time_precision_ns &&
                              last->moment_ns >= transition->moment_ns &&
                              (extra ? transition->action == 1 && !removed && last->remaining > 0 &&
                                           last->remaining == std::get<0>(state->image->orders.at(difference.id))
                                     : removed);
            if (!fits)
                return 2;
        }
        return unknown ? 0 : 1;
    }
    void emit(const Publication& p, const char* result) {
        if (!output)
            return;
        const auto* state = nearest(p);
        const auto delta = differences(p, state);
        std::uint64_t commits = 0, missing = 0, extra = 0, changed = 0;
        if (const auto h = history.find(p.isin); h != history.end())
            for (const auto& s : h->second)
                commits += s.commit && s.at >= p.begin - tolerance_ms && s.at <= p.end + tolerance_ms;
        for (const auto& d : delta) {
            missing += std::string_view(d.kind) == "missing";
            extra += std::string_view(d.kind) == "extra";
            changed += std::string_view(d.kind) == "changed";
        }
        const bool available = state && state->image && p.image;
        const auto causal = p.stale ? hypothesis(p, state) : 0;
        *output << "{\"kind\":\"content_comparison\",\"isin_id\":" << p.isin << ",\"result\":\"" << result
                << "\",\"raw_begin_monotonic_ms\":" << p.begin << ",\"raw_end_monotonic_ms\":" << p.end
                << ",\"tolerance_ms\":" << tolerance_ms << ",\"raw_order_set_sha256\":" << std::quoted(p.hash)
                << ",\"composite_commits_in_window\":" << commits << ",\"nearest_sample_in_window\":"
                << (state && state->at >= p.begin - tolerance_ms && state->at <= p.end + tolerance_ms ? "true"
                                                                                                      : "false")
                << ",\"missing_orders\":" << (available ? std::to_string(missing) : "null")
                << ",\"extra_orders\":" << (available ? std::to_string(extra) : "null")
                << ",\"changed_orders\":" << (available ? std::to_string(changed) : "null")
                << ",\"raw_info_revision\":" << std::quoted(std::to_string(p.info.info_revision))
                << ",\"raw_trades_revision\":" << std::quoted(std::to_string(p.info.trades_revision))
                << ",\"raw_trades_lifenum\":" << std::quoted(std::to_string(p.info.log_life))
                << ",\"raw_info_begin_moment_utc_ns\":" << std::quoted(std::to_string(p.info.begin_utc_ns))
                << ",\"raw_info_moment_utc_ns\":" << std::quoted(std::to_string(p.info.end_utc_ns))
                << ",\"raw_info_state0_calendar\":" << std::quoted(p.info.begin_calendar)
                << ",\"raw_info_state1_calendar\":" << std::quoted(p.info.end_calendar)
                << ",\"raw_info_moment_source\":\"info.moment,t (vendor Snapshot time); UTC conversion validated "
                   "against orders.moment/moment_ns\""
                << ",\"timestamp_precision_ns\":" << time_precision_ns
                << ",\"classification_clock_tolerance_ns\":" << clock_tolerance_ns
                << ",\"max_raw_order_moment_ns\":" << std::quoted(std::to_string(p.info.max_order_moment_ns))
                << ",\"raw_timestamps_consistent\":" << (p.info.timestamp_consistent ? "true" : "false")
                << ",\"first_composite_state_utc_ns\":" << std::quoted(std::to_string(first_composite_utc[p.isin]))
                << ",\"nearest_composite_commit_sequence\":"
                << std::quoted(std::to_string(state ? state->committed_sequence : 0))
                << ",\"initial_publication\":" << (p.initial ? "true" : "false")
                << ",\"fresh_publication\":" << (p.fresh ? "true" : "false")
                << ",\"publication_identity_advancing\":" << (p.advancing ? "true" : "false")
                << ",\"lifecycle_history_evictions\":" << change_evictions << ",\"stale_snapshot_hypothesis\":\""
                << (causal == 1   ? "proven_for_every_differing_id"
                    : causal == 2 ? "contrary_evidence_possible_connector_defect"
                                  : "unproven")
                << "\",\"differing_orders\":[";
        bool comma = false;
        for (const auto& d : delta) {
            if (comma)
                *output << ',';
            comma = true;
            *output << "{\"order_id\":" << std::quoted(std::to_string(d.id))
                    << ",\"difference\":" << std::quoted(d.kind) << ",\"composite_ordlog_events\":[";
            bool event_comma = false;
            if (const auto at = changes.find({p.isin, d.id}); at != changes.end())
                for (const auto& event : at->second) {
                    if (event_comma)
                        *output << ',';
                    event_comma = true;
                    *output << "{\"action\":" << static_cast<int>(event.action)
                            << ",\"revision\":" << std::quoted(std::to_string(event.revision))
                            << ",\"moment_ns\":" << std::quoted(std::to_string(event.moment_ns))
                            << ",\"moment_t_utc_ns\":" << std::quoted(std::to_string(event.moment_t_ns))
                            << ",\"committed_sequence\":" << std::quoted(std::to_string(event.committed_sequence))
                            << ",\"remaining\":" << std::quoted(std::to_string(event.remaining))
                            << ",\"timestamps_consistent\":" << (event.consistent ? "true" : "false") << '}';
                }
            *output << "]}";
        }
        *output << "],\"same_revision_proven\":false}\n" << std::flush;
    }
    ImagePtr image(OrderSet orders) {
        if (orders.size() > order_budget)
            throw std::invalid_argument("content order set exceeds storage bound");
        if (*retained_rows > order_budget - orders.size()) {
            ++evictions;
            auto lost = lost_history;
            for (const auto& [isin, states] : history)
                lost[isin] = true;
            retire("unresolved_order_storage_limit");
            lost_history = std::move(lost);
        }
        if (*retained_rows > order_budget - orders.size())
            throw std::invalid_argument("active content observations exceed storage bound");
        return std::make_shared<Image>(std::move(orders), retained_rows);
    }
    void classify(Publication& p) {
        const auto first = first_composite_utc[p.isin];
        const auto& info = p.info;
        const auto upper = info.end_utc_ns ? info.end_utc_ns + time_precision_ns : 0;
        const bool valid =
            info.timestamp_consistent && info.info_revision > 0 && info.trades_revision > 0 && info.log_life > 0 &&
            info.end_utc_ns && info.received_utc_ns && upper <= info.received_utc_ns + clock_tolerance_ns &&
            (!info.begin_utc_ns || info.begin_utc_ns <= info.end_utc_ns) && info.max_order_moment_ns <= upper;
        p.stale = info.provided && p.initial && valid && first && upper + clock_tolerance_ns < first;
        // Vendor info.moment is Snapshot time. Differing state0/state1 values form a conservative interval.
        // Old resting orders are valid in a fresh publication; max_order_moment never supplies its age.
        const auto lower = info.begin_utc_ns ? info.begin_utc_ns : info.end_utc_ns;
        p.fresh = info.provided && valid && p.advancing && first && lower > first + clock_tolerance_ns;
        p.time_unknown = info.provided && p.initial && !p.stale && !p.fresh;
    }
    void poll(std::int64_t at = now()) {
        for (auto p = pending.begin(); p != pending.end();) {
            if (p->info.provided && p->initial && !first_composite_utc[p->isin]) {
                ++p; // Bootstrap may deliver raw before the first committed composite image.
                continue;
            }
            classify(*p);
            const auto h = history.find(p->isin);
            bool equal = false;
            if (h != history.end())
                for (std::size_t i = 0; i < h->second.size(); ++i) {
                    const auto& s = h->second[i];
                    // A quiet committed state remains current until replaced/retired.
                    const auto until = i + 1 < h->second.size() ? h->second[i + 1].at : at;
                    if (s.at <= p->end + tolerance_ms && until >= p->begin - tolerance_ms) {
                        p->eligible = true;
                        equal |= s.hash == p->hash;
                    }
                }
            if ((p->stale || p->time_unknown) && at <= p->end + tolerance_ms) {
                ++p;
                continue;
            }
            if (p->stale) {
                ++pre_connection;
                const auto proof = hypothesis(*p, nearest(*p));
                stale_hypothesis_proven += proof == 1;
                suspected_defects += proof == 2;
                emit(*p, "not_comparable_pre_connection_publication");
            } else if (p->time_unknown) {
                ++unknown_publication;
                emit(*p, "not_comparable_unknown_publication_time");
            } else if (equal) {
                ++matched;
                fresh_equal += p->fresh;
                emit(*p, "equal_within_arrival_window");
            } else if (at <= p->end + tolerance_ms) {
                ++p;
                continue;
            } else if (p->truncated) {
                ++unresolved;
                fresh_unresolved += p->fresh;
                emit(*p, "unresolved_history_evicted");
            } else if (p->eligible) {
                ++different;
                fresh_different += p->fresh;
                emit(*p, "different_within_arrival_window");
            } else {
                ++unresolved;
                fresh_unresolved += p->fresh;
                emit(*p, "unresolved_no_committed_sample");
            }
            unknown_later_publication += p->info.provided && !p->initial && !p->fresh;
            fresh_observations += p->fresh;
            fresh_counts[p->isin] += p->fresh;
            p = pending.erase(p);
        }
    }
    void record(std::int32_t isin, OrderSet orders, std::int64_t at = now(), bool commit = true) {
        const auto hash = order_set_digest(orders);
        const auto h = history.find(isin);
        auto value = h != history.end() && !h->second.empty() && h->second.back().hash == hash ? h->second.back().image
                                                                                               : ImagePtr{};
        if (!value)
            value = image(std::move(orders));
        record(isin, hash, at, std::move(value), commit);
    }
    void record(std::int32_t isin, std::string hash, std::int64_t at = now(), ImagePtr value = {}, bool commit = true) {
        auto& h = history[isin];
        if (!commit && !h.empty() && h.back().hash == hash) {
            poll(at);
            return;
        }
        const auto utc = utc_clock();
        if (!first_composite_utc[isin])
            first_composite_utc[isin] = utc;
        if (commit)
            ++state_sequence;
        h.push_back({at, std::move(hash), std::move(value), commit, utc, state_sequence});
        if (h.size() > capacity) {
            h.pop_front();
            lost_history[isin] = true;
            ++evictions;
            // Lost samples can contain a transient match; do not label pending differences.
            for (auto p = pending.begin(); p != pending.end();)
                if (p->isin == isin) {
                    ++unresolved;
                    emit(*p, "unresolved_history_evicted");
                    p = pending.erase(p);
                } else
                    ++p;
        }
        poll(at);
    }
    void raw(std::int32_t isin, OrderSet orders, std::int64_t begin, std::int64_t end, PublicationInfo info = {}) {
        const auto hash = order_set_digest(orders);
        raw(isin, hash, begin, end, image(std::move(orders)), info);
    }
    void raw(std::int32_t isin, std::string hash, std::int64_t begin, std::int64_t end, ImagePtr value = {},
             PublicationInfo info = {}) {
        if (begin > end)
            throw std::invalid_argument("reversed raw publication interval");
        if (resample)
            resample();
        if (pending.size() == capacity) {
            ++unresolved;
            ++evictions;
            emit(pending.front(), "unresolved_pending_evicted");
            pending.pop_front();
        }
        ++observations;
        const auto h = history.find(isin);
        const bool truncated = lost_history[isin] &&
                               (h == history.end() || h->second.empty() || begin - tolerance_ms < h->second.front().at);
        const bool initial = ++publication_counts[isin] == 1;
        const auto previous = publication_high_water.find(isin);
        const bool advancing =
            initial ||
            (previous != publication_high_water.end() && info.log_life == previous->second.log_life &&
             info.info_revision > previous->second.info_revision &&
             info.trades_revision >= previous->second.trades_revision && info.end_utc_ns > previous->second.end_utc_ns);
        if (info.provided && advancing)
            publication_high_water[isin] = info;
        pending.push_back({isin, begin, end, std::move(hash), false, truncated, std::move(value), info, initial, false,
                           false, false, advancing});
        poll(end);
    }
    bool fresh_check_pass(double elapsed_seconds) const {
        return elapsed_seconds >= 600 && fresh_observations >= 3 && fresh_equal == fresh_observations &&
               !fresh_different && !fresh_unresolved && !different && !unresolved && !unknown_publication &&
               !unknown_later_publication && !suspected_defects && pending.empty() && !history.empty() &&
               std::all_of(first_composite_utc.begin(), first_composite_utc.end(), [&](const auto& entry) {
                   const auto count = fresh_counts.find(entry.first);
                   return count != fresh_counts.end() && count->second >= 3;
               });
    }
    void retire(const char* reason = "unresolved_lifecycle_change") {
        for (const auto& p : pending) {
            ++unresolved;
            emit(p, reason);
        }
        pending.clear();
        history.clear();
        fresh_counts.clear(); // Three fresh publications are required again after any generation retirement.
        lost_history.clear();
        change_evictions += change_order.size();
        changes.clear();
        change_order.clear();
    }
};
} // namespace full_order_log_probe
