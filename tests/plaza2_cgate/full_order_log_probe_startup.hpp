#pragma once
#include "full_order_log_content_comparison.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include <limits>
#include <set>

namespace full_order_log_probe {
// Diagnostic only: staged inspection is never published or used for entry admission.
// A snapshot records the last processed revision; ORDLOG requests its exact successor.
class StartupComparison {
    struct Raw {
        OrderSet orders;
        PublicationInfo info;
        bool done{}, equal{};
    };
    std::map<std::int32_t, OrderSet> snapshot_;
    std::map<std::int32_t, Raw> raw_;
    std::set<std::int32_t> ids_;
    std::size_t capacity_, raw_rows_{};
    std::ostream* output_;
    std::pair<std::uint64_t, std::int64_t> request_{}, application_info_{};
    bool captured_{}, sealed_{}, usable_{true}, opened_[2]{};
    std::string boundary_;
    static bool resumes_after(const std::pair<std::uint64_t, std::int64_t>& request,
                              const std::pair<std::uint64_t, std::int64_t>& processed) {
        return processed.first > 0 && processed.second > 0 &&
               processed.second < std::numeric_limits<std::int64_t>::max() && request.first == processed.first &&
               request.second == processed.second + 1;
    }
    static void order(std::ostream& out, std::int64_t id, const OrderSet::mapped_type& value) {
        const auto [quantity, price, session, bid] = value;
        out << "{\"order_id\":" << std::quoted(std::to_string(id))
            << ",\"remaining\":" << std::quoted(std::to_string(quantity))
            << ",\"price_scaled\":" << std::quoted(std::to_string(price)) << ",\"session\":" << session
            << ",\"bid\":" << (bid ? "true" : "false") << '}';
    }
    void compare(bool finish = false) {
        for (auto isin : ids_) {
            auto at = raw_.find(isin);
            if (at != raw_.end() && at->second.done)
                continue;
            if (!finish && (!captured_ || !sealed_ || !request_.first || !request_.second || at == raw_.end()))
                continue;
            const Raw* raw = at == raw_.end() ? nullptr : &at->second;
            const bool anchored = raw && raw->info.provided &&
                                  resumes_after(request_, {raw->info.log_life, raw->info.trades_revision}) &&
                                  (!application_info_.first || resumes_after(request_, application_info_));
            const bool comparable = captured_ && sealed_ && usable_ && anchored;
            const bool equal = comparable && snapshot_.at(isin) == raw->orders;
            matched += equal;
            different += comparable && !equal;
            unavailable += !comparable;
            if (output_) {
                *output_ << "{\"kind\":\"startup_snapshot_comparison\",\"isin_id\":" << isin << ",\"result\":\""
                         << (equal        ? "equal_same_publication"
                             : comparable ? "different_same_publication"
                                          : "not_comparable")
                         << "\",\"reason\":\""
                         << (comparable                            ? "native_request_follows_raw_trades_revision"
                             : !usable_                            ? "snapshot_boundary_invalid"
                             : !captured_ || !sealed_              ? "snapshot_boundary_unavailable"
                             : !raw                                ? "first_raw_unavailable"
                             : !request_.first || !request_.second ? "native_request_unavailable"
                             : application_info_.first && !resumes_after(request_, application_info_)
                                 ? "application_info_conflicts_with_native_request"
                                 : "different_or_unknown_publication")
                         << "\",\"native_request_lifenum\":" << std::quoted(std::to_string(request_.first))
                         << ",\"native_request_revision\":" << std::quoted(std::to_string(request_.second))
                         << ",\"raw_info_lifenum\":" << std::quoted(std::to_string(raw ? raw->info.log_life : 0))
                         << ",\"raw_info_trades_revision\":"
                         << std::quoted(std::to_string(raw ? raw->info.trades_revision : 0))
                         << ",\"boundary\":" << std::quoted(boundary_) << ",\"differences\":[";
                bool comma = false;
                if (comparable) {
                    const auto& expected = snapshot_.at(isin);
                    std::set<std::int64_t> all;
                    for (const auto& [id, _] : expected)
                        all.insert(id);
                    for (const auto& [id, _] : raw->orders)
                        all.insert(id);
                    for (auto id : all) {
                        const auto a = expected.find(id), b = raw->orders.find(id);
                        if (a != expected.end() && b != raw->orders.end() && a->second == b->second)
                            continue;
                        if (comma)
                            *output_ << ',';
                        comma = true;
                        *output_ << "{\"order_id\":" << std::quoted(std::to_string(id)) << ",\"composite_snapshot\":";
                        if (a == expected.end())
                            *output_ << "null";
                        else
                            order(*output_, id, a->second);
                        *output_ << ",\"raw\":";
                        if (b == raw->orders.end())
                            *output_ << "null";
                        else
                            order(*output_, id, b->second);
                        *output_ << '}';
                    }
                }
                *output_ << "]}\n" << std::flush;
            }
            if (at == raw_.end())
                at = raw_.emplace(isin, Raw{}).first;
            at->second.done = true;
            at->second.equal = equal;
        }
    }

  public:
    std::uint64_t matched{}, different{}, unavailable{};
    StartupComparison(std::span<const std::int32_t> ids, std::size_t capacity, std::ostream* output = nullptr)
        : ids_(ids.begin(), ids.end()), capacity_(capacity), output_(output) {
        if (ids_.empty() || ids_.size() != ids.size() || !capacity || *ids_.begin() <= 0)
            throw std::invalid_argument("invalid startup comparison bounds");
    }
    void reset(const char* = "lifecycle_change") {
        snapshot_.clear();
        raw_.clear();
        raw_rows_ = 0;
        request_ = application_info_ = {};
        captured_ = sealed_ = false;
        usable_ = true;
        boundary_.clear();
    }
    void opened(bool composite) {
        const unsigned side = composite ? 0 : 1;
        if (opened_[side]) {
            if (composite)
                reset();
            else
                retire();
        }
        opened_[side] = true;
    }
    void retire() {
        if (captured_) {
            usable_ = false;
            snapshot_.clear();
            // Retain the captured marker: ONLINE must not recapture partly replayed state.
        }
    }
    void capture(const moex::plaza2::cgate::Plaza2FullOrderLog& book, const char* boundary) {
        if (captured_)
            return;
        std::map<std::int32_t, OrderSet> value;
        for (auto isin : ids_)
            value[isin];
        std::size_t rows = 0;
        // Existing inspection API reads actual hidden state; it does not commit or authorize anything.
        book.inspect_staged_orders_for_replay([&](const moex::plaza2::cgate::FullOrderLogOrder& o) {
            if (++rows > capacity_)
                throw std::invalid_argument("bounded startup snapshot exhausted");
            value.at(o.isin).emplace(o.id, std::tuple{o.remaining, o.price, o.session, o.bid});
        });
        snapshot_ = std::move(value);
        captured_ = true;
        boundary_ = boundary;
        if (output_)
            for (const auto& [isin, orders] : snapshot_) {
                *output_ << "{\"kind\":\"composite_startup_snapshot\",\"isin_id\":" << isin
                         << ",\"boundary\":" << std::quoted(boundary_) << ",\"staged_not_visible\":true,\"orders\":[";
                bool comma = false;
                for (const auto& [id, value] : orders) {
                    if (comma)
                        *output_ << ',';
                    comma = true;
                    order(*output_, id, value);
                }
                *output_ << "]}\n" << std::flush;
            }
    }
    void saw_later_snapshot_row() {
        if (captured_ && !sealed_)
            usable_ = false;
    }
    void seal() {
        sealed_ = captured_;
        compare();
    }
    void raw(std::int32_t isin, OrderSet orders, PublicationInfo info) {
        if (!ids_.contains(isin))
            throw std::invalid_argument("untracked startup snapshot instrument");
        if (raw_.contains(isin))
            return;
        if (orders.size() > capacity_ - raw_rows_)
            throw std::invalid_argument("bounded initial raw snapshot exhausted");
        raw_rows_ += orders.size();
        raw_.emplace(isin, Raw{std::move(orders), std::move(info)});
        compare();
    }
    void application_info(std::uint64_t life, std::int64_t revision) {
        if (!life || revision <= 0) {
            usable_ = false; // A delivered unknown field cannot prove agreement. Absence is a different case.
            return;
        }
        const std::pair<std::uint64_t, std::int64_t> value{life, revision};
        if (sealed_ && request_.first && !resumes_after(request_, value))
            throw std::invalid_argument("late application info contradicts native startup request");
        application_info_ = value;
    }
    void request(std::uint64_t life, std::int64_t revision) {
        if (!life || revision <= 0)
            return;
        const std::pair<std::uint64_t, std::int64_t> key{life, revision};
        if (sealed_ && request_.first) {
            if (output_)
                *output_ << "{\"kind\":\"startup_later_request\",\"lifenum\":" << std::quoted(std::to_string(life))
                         << ",\"revision\":" << std::quoted(std::to_string(revision))
                         << ",\"startup_anchor_preserved\":true}\n"
                         << std::flush;
            return; // Later reconnects cannot rewrite or discard the initial publication evidence.
        }
        if (request_.first && request_ != key)
            throw std::invalid_argument("conflicting native startup request");
        request_ = key;
        compare();
    }
    void finish() {
        compare(true);
    }
    [[nodiscard]] std::size_t pending() const {
        return std::count_if(raw_.begin(), raw_.end(), [](const auto& row) { return !row.second.done; });
    }
    [[nodiscard]] bool complete() const {
        return captured_ && sealed_ && usable_ && !different && !unavailable && matched == ids_.size() &&
               raw_.size() == ids_.size() && std::all_of(raw_.begin(), raw_.end(), [](const auto& row) {
                   return row.second.done && row.second.equal;
               });
    }
};
} // namespace full_order_log_probe
