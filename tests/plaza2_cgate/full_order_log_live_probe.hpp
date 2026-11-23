#pragma once
#include "full_order_log_content_comparison.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include "moex/plaza2/cgate/plaza2_decimal.hpp"
#include <algorithm>
#include <deque>
#include <iomanip>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <ostream>
#include <sstream>

namespace full_order_log_probe {
namespace cg = moex::plaza2::cgate;
// MOEX9.9: https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/docs/p2gate_en.html
// #g_3_3_1 (table-local revisions), #stream_FORTS_ORDBOOK_REPL (info binds orders_log).
// CGate API printed pp11–12: TN_COMMIT completes a consistent block, without a revision payload.
// https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/docs/cgate_en.pdf
using Key =
    std::pair<std::uint64_t, std::int64_t>; // Native log life and orders_log cutover; never local book revision.
using Levels = std::map<std::tuple<std::int32_t, bool, std::int64_t>, std::int64_t>;
inline void require(bool ok, const char* text) {
    if (!ok)
        throw std::invalid_argument(text);
}
// Keep diagnostic context; redact only values actually loaded as secrets.
inline std::string redact_probe_error(std::string_view message, std::span<const std::string> secrets) {
    std::string output;
    output.reserve(message.size());
    for (std::size_t at = 0; at < message.size();) {
        std::size_t matched{};
        for (const auto& secret : secrets)
            if (!secret.empty() && message.substr(at).starts_with(secret))
                matched = std::max(matched, secret.size());
        if (matched) {
            output += "[REDACTED]";
            at += matched;
        } else
            output += message[at++];
    }
    return output;
}
inline std::string digest(const Levels& levels, std::span<const std::int32_t> ids) {
    std::string text = "full_order_log_probe_exact_d16_5_v1\n";
    for (auto id : ids)
        text += "isin=" + std::to_string(id) + "\n";
    for (const auto& [key, quantity] : levels) {
        const auto [isin, bid, price] = key;
        text += std::to_string(isin) + "," + std::to_string(bid) + "," + std::to_string(price) + "," +
                std::to_string(quantity) + "\n";
    }
    return cg::plaza2_sha256_hex(text);
}
struct Window {
    struct Entry {
        std::string hash;
        bool ambiguous{}, paired{}, paired_equal{};
    };
    std::map<Key, Entry> states[2];
    std::deque<Key> order[2];
    std::size_t capacity;
    std::ostream* output;
    std::uint64_t matched{}, mismatched{}, ambiguous{}, evicted{}, raw_snapshots{}, composite_commits{},
        unresolved_raw_retired{}, generation{};
    explicit Window(std::size_t cap = 4096, std::ostream* out = nullptr) : capacity(cap), output(out) {
        require(cap > 0 && cap <= 65536, "invalid bounded comparison window");
    }
    void retire(unsigned side) {
        if (side == 1)
            unresolved_raw_retired += std::count_if(states[1].begin(), states[1].end(),
                                                    [](const auto& entry) { return !entry.second.paired; });
        states[side].clear();
        order[side].clear();
    }
    void disqualify(unsigned side, Key key) {
        const auto own = states[side].find(key), other = states[1 - side].find(key);
        if (own == states[side].end())
            return;
        if (!own->second.ambiguous) {
            own->second.ambiguous = true;
            ++ambiguous;
        }
        if (own->second.paired) {
            if (own->second.paired_equal) {
                if (matched)
                    --matched;
            } else if (mismatched)
                --mismatched;
            own->second.paired = false;
            if (other != states[1 - side].end())
                other->second.paired = false;
        }
    }
    void record(unsigned side, Key key, std::string hash) {
        if (side)
            ++raw_snapshots;
        else
            ++composite_commits;
        auto& own = states[side];
        auto at = own.find(key);
        const bool inserted = at == own.end();
        if (inserted)
            at = own.emplace(key, Entry{hash}).first;
        if (inserted)
            order[side].push_back(key);
        else if (at->second.hash != hash && !at->second.ambiguous) {
            disqualify(side, key);
        }
        if (own.size() > capacity) {
            if (side == 1 && !own.at(order[side].front()).paired)
                ++unresolved_raw_retired;
            own.erase(order[side].front());
            order[side].pop_front();
            ++evicted;
        }
        const auto other = states[1 - side].find(key);
        if (other == states[1 - side].end() || at->second.ambiguous || other->second.ambiguous || at->second.paired ||
            other->second.paired)
            return;
        at->second.paired = other->second.paired = true;
        const bool equal = at->second.hash == other->second.hash;
        at->second.paired_equal = other->second.paired_equal = equal;
        if (equal)
            ++matched;
        else
            ++mismatched;
        if (output)
            *output << "{\"kind\":\"comparison\",\"log_life\":" << key.first << ",\"trades_rev\":" << key.second
                    << ",\"equal\":" << (equal ? "true" : "false")
                    << ",\"composite_sha256\":" << std::quoted(states[0].at(key).hash)
                    << ",\"raw_sha256\":" << std::quoted(states[1].at(key).hash) << "}\n"
                    << std::flush;
    }
};
inline cg::Plaza2RawFieldBinding field(const cg::Plaza2RawTableBinding& table, std::string_view name,
                                       std::string_view type, std::size_t size) {
    for (const auto& f : table.fields)
        if (f.name == name) {
            require(f.type_token == type && f.size == size && f.offset <= table.row_size &&
                        size <= table.row_size - f.offset,
                    "incompatible probe field");
            return f;
        }
    throw std::invalid_argument("missing probe field: " + std::string(name));
}
template <class T> T read(const cg::Plaza2ListenerEvent& e, const cg::Plaza2RawFieldBinding& f) {
    require(f.offset <= e.raw_payload.size() && sizeof(T) <= e.raw_payload.size() - f.offset &&
                (e.raw_nulls.empty() || (f.index < e.raw_nulls.size() && !e.raw_nulls[f.index])),
            "truncated or null probe field");
    return moex::plaza2::public_wire::load<T>(e.raw_payload, f.offset);
}
// info.moment lacks a nanosecond peer. Preserve its calendar and establish the
// Moscow-to-UTC conversion by requiring raw orders.moment to agree with explicit UTC moment_ns.
inline std::uint64_t utc_ns(moex::plaza2::public_wire::Time t) {
    using namespace std::chrono;
    const auto date = year{t.year} / month{t.month} / day{t.day};
    if (!date.ok() || t.year < 1970 || t.year > 2200 || t.hour >= 24 || t.minute >= 60 || t.second >= 60 ||
        t.msec >= 1000)
        return 0;
    const auto value = duration_cast<nanoseconds>((sys_days{date} + hours{t.hour} + minutes{t.minute} +
                                                   seconds{t.second} + milliseconds{t.msec} - hours{3})
                                                      .time_since_epoch())
                           .count();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}
inline std::string calendar(moex::plaza2::public_wire::Time t) {
    std::ostringstream text;
    text << t.year << '-' << static_cast<unsigned>(t.month) << '-' << static_cast<unsigned>(t.day) << 'T'
         << static_cast<unsigned>(t.hour) << ':' << static_cast<unsigned>(t.minute) << ':'
         << static_cast<unsigned>(t.second) << '.' << t.msec;
    return text.str();
}
class Observer final : public cg::Plaza2ListenerEventHandler {
    struct Order {
        std::int32_t isin;
        bool bid;
        std::int64_t price, quantity, revision, public_id;
        std::int32_t session;
        std::uint64_t moment_ns{}, moment_t_ns{};
        bool time_consistent{};
    };
    struct OrderBinding {
        cg::Plaza2RawFieldBinding row, rev, act, isin, dir, price, rest, status, public_id, session, moment, moment_ns;
    };
    Window& window_;
    ContentWindow* content_{};
    cg::Plaza2FullOrderLog::Clock::time_point content_commit_{};
    std::int64_t raw_begin_{};
    cg::Plaza2FullOrderLog* book_;
    std::vector<std::int32_t> ids_;
    std::unordered_map<std::int64_t, Order> orders_;
    OrderBinding fields_;
    cg::Plaza2RawFieldBinding publication_, info_rev_, info_life_, info_row_rev_, info_moment_, info_act_, log_rev_,
        log_status_, log_act_, log_id_, log_isin_, log_action_, log_rest_, log_moment_, log_moment_ns_;
    PublicationInfo publication_info_{.provided = true};
    struct PendingChange {
        std::int32_t isin;
        std::int64_t id;
        ContentWindow::Change event;
    };
    std::vector<PendingChange> changes_;
    void publish_changes() {
        if (content_)
            for (const auto& c : changes_)
                content_->change(c.isin, c.id, c.event);
        changes_.clear();
    }
    std::size_t order_index_{}, info_index_{}, log_index_{}, order_size_{}, info_size_{}, log_size_{}, order_capacity_;
    Key committed_{}, pending_{};
    bool transaction_{}, info_done_{}, have_info_{}, online_{}, fenced_{}, log_started_{}, progress_needed_{};
    bool committed_lastrec_{true}, pending_lastrec_{true};
    bool unframed_snapshot_{}, published_at_online_{};
    std::ostream* output_;
    std::uint64_t diagnostic_limit_, diagnostics_{}, generation_{};
    void retire_generation() {
        // Native table revisions may restart without a different log life; neither old peer may survive.
        changes_.clear();
        publication_info_ = {.provided = true};
        if (content_)
            content_->retire();
        window_.retire(0);
        window_.retire(1);
        generation_ = ++window_.generation;
        progress_needed_ = true;
    }
    Levels levels() const {
        Levels out;
        if (book_) {
            for (auto isin : ids_)
                for (bool bid : {true, false})
                    for (const auto& [price, quantity] : book_->levels(isin, bid))
                        out[{isin, bid, price}] = quantity;
        } else
            for (const auto& [id, o] : orders_) {
                auto& quantity = out[{o.isin, o.bid, o.price}];
                require(o.quantity <= INT64_MAX - quantity, "reference quantity overflow");
                quantity += o.quantity;
            }
        return out;
    }
    void bind(std::span<const cg::Plaza2RawTableBinding> tables) {
        bool orders = false, info = false, log = false;
        for (const auto& t : tables) {
            if (t.name == "orders") {
                order_index_ = t.index;
                order_size_ = t.row_size;
                orders = true;
                fields_ = {field(t, "replID", "i8", 8),
                           field(t, "replRev", "i8", 8),
                           field(t, "replAct", "i8", 8),
                           field(t, "isin_id", "i4", 4),
                           field(t, "dir", "i1", 1),
                           field(t, "price", "d16.5", 11),
                           field(t, "public_amount_rest", "i8", 8),
                           field(t, "xstatus", "i8", 8),
                           field(t, "public_order_id", "i8", 8),
                           field(t, "sess_id", "i4", 4),
                           field(t, "moment", "t", 10),
                           field(t, "moment_ns", "u8", 8)};
            } else if (book_ && t.name == "orders_log") {
                log = true;
                log_index_ = t.index;
                log_size_ = t.row_size;
                log_rev_ = field(t, "replRev", "i8", 8);
                log_status_ = field(t, "xstatus", "i8", 8);
                log_act_ = field(t, "replAct", "i8", 8);
                log_id_ = field(t, "public_order_id", "i8", 8);
                log_isin_ = field(t, "isin_id", "i4", 4);
                log_action_ = field(t, "public_action", "i1", 1);
                log_rest_ = field(t, "public_amount_rest", "i8", 8);
                log_moment_ = field(t, "moment", "t", 10);
                log_moment_ns_ = field(t, "moment_ns", "u8", 8);
            } else if (t.name == "info") {
                info = true;
                info_index_ = t.index;
                info_size_ = t.row_size;
                publication_ = field(t, "publication_state", "i1", 1);
                info_rev_ = field(t, "trades_rev", "i8", 8);
                info_life_ = field(t, "trades_lifenum", "i8", 8);
                info_row_rev_ = field(t, "replRev", "i8", 8);
                info_act_ = field(t, "replAct", "i8", 8);
                info_moment_ = field(t, "moment", "t", 10);
            }
        }
        require(orders && info && (!book_ || log),
                "probe requires native orders, info and composite orders_log tables");
    }
    void data(const cg::Plaza2ListenerEvent& e) {
        require(e.raw_table != nullptr, "probe row without binding");
        const auto name = e.raw_table->name;
        const auto size = name == "orders"       ? order_size_
                          : name == "info"       ? info_size_
                          : name == "orders_log" ? log_size_
                                                 : e.raw_table->row_size;
        require(e.raw_payload.size() >= size, "probe row size");
        require(transaction_ || (book_ && !online_), "probe row outside bound transaction");
        if (book_ && !online_ && !transaction_)
            unframed_snapshot_ = true;
        if (online_)
            published_at_online_ = false;
        if (name == "info") {
            require(e.table_index == info_index_, "probe info index mismatch");
            const auto publication = read<std::int8_t>(e, publication_);
            const auto rev = read<std::int64_t>(e, info_rev_), life = read<std::int64_t>(e, info_life_);
            require((publication == 0 || publication == 1) && rev >= 0 && life >= 0, "invalid probe snapshot info");
            info_done_ = publication == 1 && read<std::int64_t>(e, info_act_) == 0;
            if (!book_ && !read<std::int64_t>(e, info_act_)) {
                const auto time = read<moex::plaza2::public_wire::Time>(e, info_moment_);
                publication_info_.info_revision = read<std::int64_t>(e, info_row_rev_);
                publication_info_.trades_revision = rev;
                publication_info_.log_life = static_cast<std::uint64_t>(life);
                if (publication == 0) {
                    publication_info_.begin_utc_ns = utc_ns(time);
                    publication_info_.begin_calendar = calendar(time);
                } else {
                    publication_info_.end_utc_ns = utc_ns(time);
                    publication_info_.end_calendar = calendar(time);
                }
            }
            if (info_done_ && book_ && log_started_) {
                fenced_ = true; // Snapshot info cannot roll back/complete an already advancing online cutover.
                return;
            }
            if (info_done_) {
                pending_.first = static_cast<std::uint64_t>(life);
                pending_.second = rev;
                have_info_ = true;
                pending_lastrec_ = true;
                progress_needed_ = false; // A fresh native completed publication seeds this cohort.
            }
            return;
        }
        if (book_) {
            if (name == "orders_log") {
                log_started_ = online_;
                require(e.table_index == log_index_, "probe orders_log index mismatch");
                const auto revision = read<std::int64_t>(e, log_rev_);
                require(revision > 0, "invalid native orders_log revision");
                const auto isin = read<std::int32_t>(e, log_isin_);
                if (content_ && !read<std::int64_t>(e, log_act_) &&
                    std::binary_search(ids_.begin(), ids_.end(), isin)) {
                    const auto id = read<std::int64_t>(e, log_id_);
                    const auto ns = read<std::uint64_t>(e, log_moment_ns_);
                    const auto time = utc_ns(read<moex::plaza2::public_wire::Time>(e, log_moment_));
                    require(changes_.size() < order_capacity_, "bounded pending ORDLOG diagnostics exhausted");
                    changes_.push_back({isin,
                                        id,
                                        {read<std::int8_t>(e, log_action_), revision, read<std::int64_t>(e, log_rest_),
                                         ns, time, ns && time && ns / 1000000 == time / 1000000}});
                }
                // All rows advance this one table's cutover, including filtered instruments/statuses.
                if (read<std::int64_t>(e, log_act_))
                    fenced_ = true; // Replication administration has no documented matching cutover.
                if (revision > pending_.second) {
                    pending_.second = revision;
                    progress_needed_ = false; // New source progress, never an empty cached commit.
                    pending_lastrec_ = (read<std::int64_t>(e, log_status_) & 0x1000) != 0;
                }
            }
            return;
        }
        if (name != "orders")
            return;
        require(e.table_index == order_index_, "reference orders index mismatch");
        const auto row = read<std::int64_t>(e, fields_.row), rev = read<std::int64_t>(e, fields_.rev);
        require(row > 0 && rev >= 0, "invalid reference row identity");
        const auto previous = orders_.find(row);
        if (previous != orders_.end() && previous->second.revision > rev)
            return;
        orders_.erase(row);
        if (read<std::int64_t>(e, fields_.act))
            return;
        const auto isin = read<std::int32_t>(e, fields_.isin);
        if (!std::binary_search(ids_.begin(), ids_.end(), isin))
            return;
        const auto status = static_cast<std::uint64_t>(read<std::int64_t>(e, fields_.status));
        if (status & (0x4ULL | 0x80002ULL | 0x8000000ULL))
            return;
        const auto qty = read<std::int64_t>(e, fields_.rest);
        const auto dir = read<std::int8_t>(e, fields_.dir);
        const auto price =
            moex::plaza2::public_wire::decimal_scaled(read<moex::plaza2::public_wire::Bcd16_5>(e, fields_.price));
        require(price && qty >= 0 && (dir == 1 || dir == 2), "invalid reference quote");
        if (!qty)
            return;
        require(orders_.size() < order_capacity_, "bounded reference order capacity exhausted");
        const auto public_id = read<std::int64_t>(e, fields_.public_id);
        const auto session = read<std::int32_t>(e, fields_.session);
        require(public_id > 0 && session > 0, "invalid reference public order identity");
        const auto ns = read<std::uint64_t>(e, fields_.moment_ns);
        const auto time = utc_ns(read<moex::plaza2::public_wire::Time>(e, fields_.moment));
        orders_.emplace(row, Order{isin, dir == 1, *price, qty, rev, public_id, session, ns, time,
                                   ns && time && ns / 1000000 == time / 1000000});
    }

  public:
    std::uint64_t p9_events{}, p9_erased{}, clear_events{}, unanchored_commits{};
    Observer(Window& window, std::span<const std::int32_t> ids, cg::Plaza2FullOrderLog* book = nullptr,
             std::size_t capacity = 200000, std::ostream* output = nullptr, std::uint64_t diagnostic_limit = 512,
             ContentWindow* content = nullptr)
        : window_(window), content_(content), book_(book), ids_(ids.begin(), ids.end()), order_capacity_(capacity),
          output_(output), diagnostic_limit_(diagnostic_limit) {
        std::sort(ids_.begin(), ids_.end());
        require(!ids_.empty() && capacity > 0, "invalid probe universe or capacity");
        orders_.reserve(capacity);
    }
    void sample_content() {
        if (!content_ || !book_)
            return;
        std::map<std::int32_t, OrderSet> sets;
        for (auto isin : ids_)
            sets[isin];
        if (book_->inspect_committed_orders([&](const cg::FullOrderLogOrder& o) {
                sets.at(o.isin).emplace(o.id, std::tuple{o.remaining, o.price, o.session, o.bid});
            })) {
            const auto at = ContentWindow::now();
            const bool commit = content_commit_ != book_->committed_at();
            content_commit_ = book_->committed_at();
            for (auto& [isin, orders] : sets)
                content_->record(isin, std::move(orders), at, commit);
        }
    }
    bool wants_raw_replication(std::string_view) const noexcept override {
        return true;
    }
    bool should_log_listener_event(const cg::Plaza2ListenerEvent&) const noexcept override {
        return false;
    }
    bool needs_fresh_snapshot() const noexcept override {
        return book_ && book_->needs_fresh_snapshot();
    }
    void on_plaza2_listener_error(const cg::Plaza2Error& error) noexcept override {
        try {
            orders_.clear();
            changes_.clear();
            publication_info_ = {.provided = true};
            raw_begin_ = 0;
            transaction_ = info_done_ = have_info_ = online_ = fenced_ = log_started_ = false;
            unframed_snapshot_ = published_at_online_ = false;
            committed_ = pending_ = {};
            retire_generation();
            if (book_)
                book_->on_plaza2_listener_error(error);
        } catch (...) {
        }
    }
    cg::Plaza2Error on_plaza2_listener_event(const cg::Plaza2ListenerEvent& e) override {
        try {
            using Kind = cg::Plaza2ListenerEventKind;
            const unsigned side = book_ ? 0 : 1;
            if (generation_ != window_.generation) {
                generation_ = window_.generation;
                progress_needed_ = true;
            }
            if (e.kind == Kind::Open) {
                raw_begin_ = 0;
                bind(e.raw_tables);
                retire_generation();
                orders_.clear();
                changes_.clear();
                publication_info_ = {.provided = true};
                committed_ = {};
                pending_ = {};
                transaction_ = info_done_ = have_info_ = online_ = fenced_ = log_started_ = false;
                unframed_snapshot_ = published_at_online_ = false;
                committed_lastrec_ = pending_lastrec_ = true;
            }
            if (e.kind == Kind::TransactionBegin) {
                require(!transaction_, "nested probe transaction");
                transaction_ = true;
                if (!book_ && !raw_begin_)
                    raw_begin_ = ContentWindow::now();
                if (book_ && online_)
                    changes_.clear();
                if (!book_ || online_) {
                    pending_ = committed_;
                    pending_lastrec_ = committed_lastrec_;
                    info_done_ = false;
                }
            }
            if (e.kind == Kind::StreamData)
                data(e);
            std::uint64_t erased = 0;
            const auto prior = book_ ? book_->metrics().clear_deleted_erased : 0;
            const auto epoch = book_ ? book_->epoch() : 0;
            if (book_) {
                const auto error = book_->on_plaza2_listener_event(e);
                if (error) {
                    on_plaza2_listener_error(error);
                    return error;
                }
                erased = book_->metrics().clear_deleted_erased - prior;
            }
            if (e.kind == Kind::ClearDeleted) {
                ++clear_events;
                if (book_) {
                    window_.disqualify(side, committed_);
                    fenced_ = true; // A later business revision in this block cannot rehabilitate the clear.
                }
                const auto name = e.raw_table ? e.raw_table->name : std::string("unresolved");
                if (!book_ && name == "orders") {
                    for (auto i = orders_.begin(); i != orders_.end();) {
                        if (e.signed_value == INT64_MAX || i->second.revision < e.signed_value) {
                            i = orders_.erase(i);
                            ++erased;
                        } else
                            ++i;
                    }
                    if (e.signed_value == INT64_MAX) {
                        have_info_ = info_done_ = false;
                        committed_ = pending_ = {};
                        retire_generation();
                    }
                }
                if (book_ && name == "orders_log") {
                    ++p9_events;
                    p9_erased += erased;
                }
                if (output_ && diagnostics_++ < diagnostic_limit_)
                    *output_ << "{\"kind\":\"clear_deleted\",\"source\":\"" << (book_ ? "composite" : "reference")
                             << "\",\"table\":" << std::quoted(name) << ",\"table_index\":" << e.table_index
                             << ",\"cutoff_revision\":" << e.signed_value << ",\"flags\":" << e.clear_deleted_flags
                             << ",\"erased\":" << erased << "}\n"
                             << std::flush;
            }
            if (book_ && (epoch != book_->epoch() || e.kind == Kind::LifeNum)) {
                retire_generation();
                have_info_ = online_ = fenced_ = log_started_ = false;
                unframed_snapshot_ = published_at_online_ = false;
                committed_lastrec_ = pending_lastrec_ = true;
                committed_ = {};
                pending_ = {};
            }
            if (e.kind == Kind::Online) {
                online_ = true;
                if (book_ && book_->valid()) {
                    committed_ = pending_;
                    committed_lastrec_ = pending_lastrec_;
                    if (!progress_needed_ && have_info_ && info_done_ && pending_lastrec_ && !fenced_)
                        window_.record(side, committed_, digest(levels(), ids_));
                    else
                        ++unanchored_commits; // ONLINE proves a book, not an exposed exact life/revision anchor.
                    published_at_online_ = true;
                    unframed_snapshot_ = false;
                }
            }
            if (e.kind == Kind::TransactionCommit) {
                require(transaction_, "probe commit without begin");
                transaction_ = false;
                committed_ = pending_;
                committed_lastrec_ = pending_lastrec_;
                // Capture only the actual published maps after native TN_COMMIT. A raw target
                // inside this CGate block has no committed digest and remains UNALIGNED.
                if (!published_at_online_ && !progress_needed_ &&
                    (book_ ? have_info_ && online_ && book_->valid() && pending_lastrec_ && !fenced_
                           : have_info_ && info_done_))
                    window_.record(side, committed_, digest(levels(), ids_));
                else if (book_ && online_ && !published_at_online_)
                    ++unanchored_commits;
                published_at_online_ = false;
            }
            if (content_ && book_ && (e.kind == Kind::Online || e.kind == Kind::TransactionCommit)) {
                if (online_ && book_->valid())
                    publish_changes();
                sample_content();
            }
            if (content_ && !book_ && e.kind == Kind::TransactionCommit && have_info_ && info_done_) {
                std::map<std::int32_t, OrderSet> sets;
                for (auto isin : ids_)
                    sets[isin];
                for (const auto& [row, o] : orders_)
                    require(
                        sets.at(o.isin).emplace(o.public_id, std::tuple{o.quantity, o.price, o.session, o.bid}).second,
                        "duplicate reference public order ID");
                const auto at = ContentWindow::now();
                for (auto& [isin, orders] : sets) {
                    auto metadata = publication_info_;
                    metadata.received_utc_ns = content_->utc_clock();
                    bool checked = false;
                    for (const auto& [row, order] : orders_)
                        if (order.isin == isin) {
                            metadata.max_order_moment_ns = std::max(metadata.max_order_moment_ns, order.moment_ns);
                            metadata.timestamp_consistent &= order.time_consistent;
                            checked |= order.time_consistent;
                        }
                    // No valid peer UTC timestamp means info calendar timezone is not proved.
                    metadata.timestamp_consistent &= checked;
                    content_->raw(isin, std::move(orders), raw_begin_ ? raw_begin_ : at, at, metadata);
                }
                publication_info_ = {.provided = true};
                raw_begin_ = 0;
            }
            if (e.kind == Kind::Close || (!book_ && e.kind == Kind::LifeNum)) {
                orders_.clear();
                changes_.clear();
                publication_info_ = {.provided = true};
                raw_begin_ = 0;
                transaction_ = info_done_ = have_info_ = online_ = false;
                unframed_snapshot_ = published_at_online_ = false;
                committed_ = pending_ = {};
                retire_generation();
            }
            return {};
        } catch (const std::exception& error) {
            const auto result = cg::Plaza2Error{.code = cg::Plaza2ErrorCode::DecodeFailed, .message = error.what()};
            on_plaza2_listener_error(result);
            return result;
        }
    }
};
} // namespace full_order_log_probe
