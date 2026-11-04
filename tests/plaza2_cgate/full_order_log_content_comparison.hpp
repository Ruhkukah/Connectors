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
    };
    struct Publication {
        std::int32_t isin;
        std::int64_t begin, end;
        std::string hash;
        bool eligible{}, truncated{};
        ImagePtr image;
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
    void emit(const Publication& p, const char* result) {
        if (!output)
            return;
        const State* nearest = nullptr;
        std::uint64_t commits = 0;
        const auto h = history.find(p.isin);
        const auto distance = [&](const State& s) { return s.at < p.end ? p.end - s.at : s.at - p.end; };
        if (h != history.end())
            for (const auto& s : h->second) {
                if (s.commit && s.at >= p.begin - tolerance_ms && s.at <= p.end + tolerance_ms)
                    ++commits;
                if (!nearest || distance(s) < distance(*nearest))
                    nearest = &s;
            }
        std::uint64_t missing = 0, extra = 0, changed = 0;
        const bool available = nearest && nearest->image && p.image;
        if (available) {
            for (const auto& [id, value] : p.image->orders) {
                const auto i = nearest->image->orders.find(id);
                if (i == nearest->image->orders.end())
                    ++missing;
                else if (i->second != value)
                    ++changed;
            }
            for (const auto& [id, value] : nearest->image->orders)
                extra += !p.image->orders.contains(id);
        }
        *output << "{\"kind\":\"content_comparison\",\"isin_id\":" << p.isin << ",\"result\":\"" << result
                << "\",\"raw_begin_monotonic_ms\":" << p.begin << ",\"raw_end_monotonic_ms\":" << p.end
                << ",\"tolerance_ms\":" << tolerance_ms << ",\"raw_order_set_sha256\":" << std::quoted(p.hash)
                << ",\"composite_commits_in_window\":" << commits << ",\"nearest_sample_in_window\":"
                << (nearest && nearest->at >= p.begin - tolerance_ms && nearest->at <= p.end + tolerance_ms ? "true"
                                                                                                            : "false")
                << ",\"missing_orders\":" << (available ? std::to_string(missing) : "null")
                << ",\"extra_orders\":" << (available ? std::to_string(extra) : "null")
                << ",\"changed_orders\":" << (available ? std::to_string(changed) : "null")
                << ",\"same_revision_proven\":false}\n"
                << std::flush;
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
        h.push_back({at, std::move(hash), std::move(value), commit});
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
    void raw(std::int32_t isin, OrderSet orders, std::int64_t begin, std::int64_t end) {
        const auto hash = order_set_digest(orders);
        raw(isin, hash, begin, end, image(std::move(orders)));
    }
    void raw(std::int32_t isin, std::string hash, std::int64_t begin, std::int64_t end, ImagePtr value = {}) {
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
        pending.push_back({isin, begin, end, std::move(hash), false, truncated, std::move(value)});
        poll(end);
    }
    void retire(const char* reason = "unresolved_lifecycle_change") {
        for (const auto& p : pending) {
            ++unresolved;
            emit(p, reason);
        }
        pending.clear();
        history.clear();
        lost_history.clear();
    }
};
} // namespace full_order_log_probe
