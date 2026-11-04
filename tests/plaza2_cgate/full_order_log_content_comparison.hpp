#pragma once
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <iomanip>
#include <map>
#include <ostream>
#include <tuple>

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
struct ContentWindow {
    struct State {
        std::int64_t at;
        std::string hash;
    };
    struct Publication {
        std::int32_t isin;
        std::int64_t begin, end;
        std::string hash;
        bool eligible{}, truncated{};
    };
    std::map<std::int32_t, std::deque<State>> history;
    std::deque<Publication> pending;
    std::map<std::int32_t, bool> lost_history;
    std::int64_t tolerance_ms;
    std::size_t capacity;
    std::ostream* output;
    std::function<void()> resample;
    std::uint64_t matched{}, different{}, unresolved{}, evictions{}, observations{};
    explicit ContentWindow(std::int64_t tolerance = 2000, std::size_t cap = 4096, std::ostream* out = nullptr)
        : tolerance_ms(tolerance), capacity(cap), output(out) {
        if (tolerance < 0 || tolerance > 30000 || cap == 0 || cap > 65536)
            throw std::invalid_argument("invalid content comparison bounds");
    }
    static std::int64_t now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    void emit(const Publication& p, const char* result) {
        if (output)
            *output << "{\"kind\":\"content_comparison\",\"isin_id\":" << p.isin << ",\"result\":\"" << result
                    << "\",\"raw_begin_monotonic_ms\":" << p.begin << ",\"raw_end_monotonic_ms\":" << p.end
                    << ",\"tolerance_ms\":" << tolerance_ms << ",\"raw_order_set_sha256\":" << std::quoted(p.hash)
                    << ",\"same_revision_proven\":false}\n"
                    << std::flush;
    }
    void poll(std::int64_t at = now()) {
        for (auto p = pending.begin(); p != pending.end();) {
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
            if (equal) {
                ++matched;
                emit(*p, "equal_within_arrival_window");
            } else if (at <= p->end + tolerance_ms) {
                ++p;
                continue;
            } else if (p->truncated) {
                ++unresolved;
                emit(*p, "unresolved_history_evicted");
            } else if (p->eligible) {
                ++different;
                emit(*p, "different_within_arrival_window");
            } else {
                ++unresolved;
                emit(*p, "unresolved_no_committed_sample");
            }
            p = pending.erase(p);
        }
    }
    void record(std::int32_t isin, std::string hash, std::int64_t at = now()) {
        auto& h = history[isin];
        h.push_back({at, std::move(hash)});
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
    void raw(std::int32_t isin, std::string hash, std::int64_t begin, std::int64_t end) {
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
        const bool truncated = lost_history[isin] && h != history.end() && !h->second.empty() &&
                               begin - tolerance_ms < h->second.front().at;
        pending.push_back({isin, begin, end, std::move(hash), false, truncated});
        poll(end);
    }
    void retire() {
        for (const auto& p : pending) {
            ++unresolved;
            emit(p, "unresolved_lifecycle_change");
        }
        pending.clear();
        history.clear();
        lost_history.clear();
    }
};
} // namespace full_order_log_probe
