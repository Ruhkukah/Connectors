#include "full_order_log_capture_format.hpp"
#include <algorithm>
#include <array>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unordered_map>
#include <unistd.h>
#include "moex/plaza2/cgate/plaza2_full_order_log_deals.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_oi.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_refdata.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"

namespace moex::plaza2::cgate {
using namespace capture_format;
namespace {
// DBN v3 MBO wire layout. Source: databento/dbn record.rs and encode/dbn/sync.rs.
struct Mbo {
    std::uint8_t length{14}, rtype{0xa0};
    std::uint16_t publisher{};
    std::uint32_t instrument{};
    std::uint64_t event{}, id{};
    std::int64_t price{};
    std::uint32_t size{};
    std::uint8_t flags{}, channel{};
    char action{}, side{};
    std::uint64_t recv{};
    std::int32_t delta{INT32_MAX};
    std::uint32_t sequence{};
};
static_assert(sizeof(Mbo) == 56 && offsetof(Mbo, recv) == 40);
struct OiStat {
    std::uint8_t length{20}, rtype{0x18};
    std::uint16_t publisher{};
    std::uint32_t instrument{};
    std::uint64_t event{}, recv{}, reference{};
    std::int64_t price{INT64_MAX}, quantity{};
    std::uint32_t sequence{};
    std::int32_t delta{INT32_MAX};
    std::uint16_t type{9}, channel{};
    std::uint8_t update{1}, flags{};
    std::array<std::uint8_t, 18> reserved{};
};
static_assert(sizeof(OiStat) == 80 && offsetof(OiStat, quantity) == 40);
class Output {
    std::filesystem::path path, temporary, parent;
    int fd{-1};

  public:
    explicit Output(const std::filesystem::path& p)
        : path(p), temporary(p.string() + ".partial-" + std::to_string(::getpid())),
          parent(p.has_parent_path() ? p.parent_path() : std::filesystem::path(".")) {
        require(!std::filesystem::exists(p), "DBN output already exists");
        fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        require(fd >= 0, "DBN temporary output open failed");
    }
    ~Output() {
        if (fd >= 0)
            ::close(fd);
        ::unlink(temporary.c_str());
    }
    void write(const void* bytes, std::size_t length) {
        auto* p = static_cast<const char*>(bytes);
        while (length) {
            const auto n = ::write(fd, p, length);
            if (n < 0 && errno == EINTR)
                continue;
            require(n > 0, "DBN write failed");
            p += n;
            length -= n;
        }
    }
    void publish() {
        require(sync_data(fd) == 0, "DBN sync failed");
        require(::link(temporary.c_str(), path.c_str()) == 0, "DBN output already exists or publish failed");
        if (!sync_parent()) {
            unpublish();
            throw std::runtime_error("DBN directory sync failed");
        }
    }
    std::string sha256() const {
        return plaza2_sha256_file(temporary);
    }
    void unpublish() noexcept {
        struct stat owned {
        }, linked{};
        if (::fstat(fd, &owned) == 0 && ::lstat(path.c_str(), &linked) == 0 && owned.st_dev == linked.st_dev &&
            owned.st_ino == linked.st_ino) {
            ::unlink(path.c_str());
            (void)sync_parent();
        }
    }

  private:
    bool sync_parent() const noexcept {
        const int d = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const bool ok = d >= 0 && ::fsync(d) == 0;
        if (d >= 0)
            ::close(d);
        return ok;
    }
};
Mbo mbo(const FullOrderLogOrder& order, char action, std::uint64_t moment, std::uint64_t recv, std::uint8_t flags = 0) {
    require(order.isin > 0 && order.id > 0 && order.remaining > 0 && order.remaining < UINT32_MAX,
            "DBN quantity/identity cannot represent MOEX row");
    require(order.price > INT64_MIN / 10000 && order.price < INT64_MAX / 10000, "DBN price overflow");
    Mbo m;
    m.instrument = order.isin;
    m.event = moment ? moment : UINT64_MAX;
    m.id = order.id;
    m.price = order.price * 10000;
    m.size = order.remaining;
    m.action = action;
    m.side = order.bid ? 'B' : 'A';
    m.recv = recv;
    m.flags = flags;
    if (moment > recv)
        m.flags |= 8;
    return m;
}
Mbo trade_mbo(const Plaza2PublicDeal& trade, std::uint64_t recv) {
    require(trade.isin_id > 0 && trade.isin_id <= UINT32_MAX && trade.quantity > 0 && trade.quantity < UINT32_MAX,
            "DBN quantity/identity cannot represent MOEX trade");
    require(trade.price_scaled > 0 && trade.price_scaled <= INT64_MAX / 10000, "DBN trade price overflow");
    require(trade.moment_ns != 0 && trade.moment_ns != UINT64_MAX, "DBN trade timestamp unavailable");
    require(trade.at_bid_or_ask >= 0 && trade.at_bid_or_ask <= 2, "DBN trade aggressor out of range");
    Mbo m;
    m.instrument = static_cast<std::uint32_t>(trade.isin_id);
    m.event = trade.moment_ns;
    // DEALS has a deal identifier. A print without a verified aggressor order ID uses zero.
    m.price = trade.price_scaled * 10000;
    m.size = static_cast<std::uint32_t>(trade.quantity);
    m.action = 'T';
    m.side = trade.at_bid_or_ask == 2 ? 'B' : trade.at_bid_or_ask == 1 ? 'A' : 'N';
    m.recv = recv;
    m.flags = 128 | (trade.moment_ns > recv ? 8 : 0);
    return m;
}
bool has_trade_schema(const Plaza2ListenerEvent& e) {
    // Older captures may describe OI alone. Treat any trade field as an
    // attempt at the trade schema and let the bridge validate the full binding.
    return std::ranges::any_of(e.raw_tables, [](const auto& table) {
        return std::ranges::any_of(table.fields, [](const auto& f) {
            return f.name == "price" || f.name == "xamount" || f.name == "nosystem" || f.name == "moment" ||
                   f.name == "xstatus_buy" || f.name == "xstatus_sell";
        });
    });
}

struct RefdataJoin {
    std::string tsv;
    std::vector<FullOrderLogRefdataRow> rows;
    std::vector<std::uint64_t> effective_after_dbn_sequence;
    std::set<std::int32_t> instruments, known;
    std::map<std::int32_t, bool> available;
    std::size_t next{};
    std::uint64_t unjoined{};

    void advance(std::uint64_t archive_sequence, std::uint64_t dbn_sequence) {
        while (next < rows.size() && rows[next].effective_after_sequence < archive_sequence) {
            const auto& row = rows[next++];
            effective_after_dbn_sequence.push_back(dbn_sequence);
            available[row.isin_id] = row.available;
            if (row.available)
                known.insert(row.isin_id);
        }
    }

    void observe(std::uint32_t instrument) {
        if (tsv.empty())
            return;
        const auto id = static_cast<std::int32_t>(instrument);
        instruments.insert(id);
        if (!available[id])
            ++unjoined;
    }

    std::string companion(const FullOrderLogArchiveSummary& summary) const {
        require(effective_after_dbn_sequence.size() == rows.size(), "DBN REFDATA mapping incomplete");
        require(std::ranges::includes(known, instruments), "DBN instrument has no pinned REFDATA mapping");
        std::string ids, boundaries;
        for (const auto instrument : instruments) {
            if (!ids.empty())
                ids += ',';
            ids += std::to_string(instrument);
        }
        for (const auto boundary : effective_after_dbn_sequence) {
            if (!boundaries.empty())
                boundaries += ',';
            boundaries += std::to_string(boundary);
        }
        return "{\"format\":\"MOEX.FOL.DBN.REFDATA.1\",\"dbn_sha256\":" + text::json_quote_utf8(summary.dbn_sha256) +
               ",\"capture_identity\":\"" + std::to_string(summary.capture_identity) + "\"" +
               ",\"capture_source_sha\":" + text::json_quote_utf8(summary.source_sha) +
               ",\"capture_last_good_sequence\":" + std::to_string(summary.last_good_sequence) +
               ",\"capture_refdata_sha256\":" + text::json_quote_utf8(summary.refdata_sha256) +
               ",\"dbn_instrument_ids\":[" + ids + "]" + ",\"effective_after_dbn_sequence\":[" + boundaries + "]" +
               ",\"unjoined_dbn_records\":" + std::to_string(unjoined) +
               ",\"refdata_tsv\":" + text::json_quote_utf8(tsv) + "}\n";
    }
};
} // namespace
FullOrderLogArchiveSummary export_full_order_log_dbn(const std::filesystem::path& directory,
                                                     const std::filesystem::path& output, bool allow_incomplete) {
    const auto checked = read_full_order_log_capture(directory, [](const auto&) {}, allow_incomplete);
    require(!checked.recovery_cursor && !checked.orders_log_only,
            "ORDLOG recovery capture is overlap evidence; use check-backfill instead of DBN export");
    RefdataJoin refdata;
    if (!checked.refdata_sha256.empty()) {
        refdata.tsv = read_pinned_refdata(directory, checked.refdata_sha256);
        refdata.rows = parse_full_order_log_refdata(refdata.tsv);
    }
    Output file(output);
    // Minimal 128-byte DBN v3 mixed metadata. Numeric MOEX ISIN IDs, local publisher=0.
    std::array<std::byte, 128> metadata{};
    std::memcpy(metadata.data(), "DBN\3", 4);
    const auto put = [&]<class T>(std::size_t offset, T value) {
        std::memcpy(metadata.data() + offset, &value, sizeof value);
    };
    put(4, std::uint32_t{120});
    put(24, std::uint16_t{UINT16_MAX}); // Mixed MBO + Statistics.
    std::memcpy(metadata.data() + 8, "MOEX.FOL", 8);
    put(34, UINT64_MAX);
    put(53, std::uint16_t{71});
    file.write(metadata.data(), metadata.size());
    Plaza2FullOrderLog book(checked.isins);
    std::unordered_map<std::int64_t, FullOrderLogOrder> state;
    std::vector<Mbo> pending;
    bool initialized{}, committed{}, invalidated{};
    std::uint64_t valid{}, last_count{}, sequence{};
    const auto write_record = [&](const auto& record) {
        refdata.observe(record.instrument);
        file.write(&record, sizeof record);
    };
    book.on_commit = [&](const auto& b) {
        committed = true;
        ++valid;
        last_count = b.order_count();
    };
    book.on_invalidate = [&] {
        invalidated = true;
        initialized = false;
        state.clear();
        pending.clear();
    };
    const auto flush = [&](const FullOrderLogCaptureRecord& r) {
        std::map<std::uint32_t, std::size_t> last;
        for (std::size_t i = 0; i < pending.size(); ++i)
            last[pending[i].instrument] = i;
        for (std::size_t i = 0; i < pending.size(); ++i) {
            auto& m = pending[i];
            require(++sequence < UINT32_MAX, "DBN sequence exhausted; split export");
            m.sequence = sequence;
            m.recv = r.utc_ns;
            if (last[m.instrument] == i)
                m.flags |= 128;
            write_record(m);
        }
        pending.clear();
    };
    const auto reset = [&](const FullOrderLogCaptureRecord& r, std::uint8_t flags) {
        for (auto isin : checked.isins) {
            Mbo m;
            m.instrument = isin;
            m.event = UINT64_MAX;
            m.price = INT64_MAX;
            m.action = 'R';
            m.side = 'N';
            m.recv = r.utc_ns;
            m.flags = flags;
            pending.push_back(m);
        }
    };
    const auto change = [&](std::int64_t id, const FullOrderLogCaptureRecord& r) {
        const auto after = book.inspect_staged_order_for_replay(id);
        const auto before = state.find(id);
        auto moment = book.exchange_moment_ns(after ? after->isin : before != state.end() ? before->second.isin : 0);
        if (r.event.kind == Plaza2ListenerEventKind::StreamData && r.event.raw_table)
            for (const auto& f : r.event.raw_table->fields)
                if (f.name == "moment_ns")
                    moment = load<std::uint64_t>(r.event.raw_payload, f.offset);
        if (!after) {
            if (before != state.end()) {
                pending.push_back(mbo(before->second, 'C', moment, r.utc_ns));
                state.erase(before);
            }
        } else {
            if (before == state.end())
                pending.push_back(mbo(*after, 'A', moment, r.utc_ns));
            else if (after->remaining != before->second.remaining || after->price != before->second.price ||
                     after->bid != before->second.bid)
                pending.push_back(mbo(*after, 'M', moment, r.utc_ns));
            state.insert_or_assign(id, *after);
        }
        require(pending.size() <= 1000000, "DBN transaction exceeds bounded export buffer");
    };
    FullOrderLogOpenInterest oi(checked.isins);
    FullOrderLogDeals deals(checked.isins);
    bool trade_schema{};
    std::optional<std::uint64_t> pending_deals_life;
    const FullOrderLogCaptureRecord* current{};
    FullOrderLogCaptureRecord last_record{};
    std::uint64_t oi_updates{}, trade_prints{};
    oi.on_update = [&](const auto& v) {
        require(v.quantity < INT64_MAX, "DBN OI quantity conflicts with undefined sentinel");
        require(++sequence < UINT32_MAX, "DBN sequence exhausted; split export");
        OiStat stat;
        stat.instrument = v.isin;
        stat.event = v.moment_ns;
        stat.recv = current->utc_ns;
        stat.reference = v.moment_ns;
        stat.quantity = v.quantity;
        stat.sequence = sequence;
        write_record(stat);
        ++oi_updates;
    };
    deals.on_trade = [&](const auto& trade) {
        auto m = trade_mbo(trade, current->utc_ns);
        require(++sequence < UINT32_MAX, "DBN sequence exhausted; split export");
        m.sequence = static_cast<std::uint32_t>(sequence);
        write_record(m);
        ++trade_prints;
    };
    auto result = read_full_order_log_capture(
        directory,
        [&](const auto& r) {
            refdata.advance(r.sequence, sequence);
            current = &r;
            last_record.utc_ns = r.utc_ns;
            if (r.event.stream_code == generated::StreamCode::kFortsDealsRepl) {
                if (const auto error = oi.on_plaza2_listener_event(r.event); error)
                    throw std::runtime_error(error.message);
                const auto send_deals = [&](const Plaza2ListenerEvent& e) {
                    if (const auto error = deals.on_event(e); error)
                        throw std::runtime_error("DBN replay: " + error.message);
                };
                if (r.event.kind == Plaza2ListenerEventKind::LifeNum && !trade_schema)
                    pending_deals_life = r.event.unsigned_value;
                if (r.event.kind == Plaza2ListenerEventKind::Open) {
                    const bool previous_trade_schema = trade_schema;
                    trade_schema = has_trade_schema(r.event);
                    if (previous_trade_schema && !trade_schema)
                        send_deals({.kind = Plaza2ListenerEventKind::Close, .stream_code = r.event.stream_code});
                    if (trade_schema && pending_deals_life)
                        send_deals({.kind = Plaza2ListenerEventKind::LifeNum,
                                    .stream_code = r.event.stream_code,
                                    .unsigned_value = *pending_deals_life});
                    pending_deals_life.reset();
                }
                if (trade_schema)
                    send_deals(r.event);
                if (r.event.kind == Plaza2ListenerEventKind::Close) {
                    pending_deals_life.reset();
                    trade_schema = false;
                }
                return;
            }
            committed = false;
            invalidated = false;
            std::optional<std::int64_t> id;
            const auto& e = r.event;
            if (initialized && e.kind == Plaza2ListenerEventKind::StreamData && e.raw_table &&
                (e.raw_table->name == "orders" || e.raw_table->name == "orders_log"))
                for (const auto& f : e.raw_table->fields)
                    if (f.name == "public_order_id")
                        id = load<std::int64_t>(e.raw_payload, f.offset);
            if (const auto error = book.on_plaza2_listener_event(e); error)
                throw std::runtime_error("DBN replay: " + error.message);
            if (invalidated || r.listener_error) {
                reset(r, 4);
                flush(r);
            }
            if (initialized && id)
                change(*id, r);
            if (initialized && e.kind == Plaza2ListenerEventKind::ClearDeleted) {
                std::vector<std::int64_t> ids;
                ids.reserve(state.size());
                for (const auto& [key, value] : state)
                    ids.push_back(key);
                for (auto key : ids)
                    change(key, r);
            }
            if (committed) {
                if (!initialized) {
                    pending.clear();
                    reset(r, 32);
                    book.inspect_staged_orders_for_replay([&](const auto& o) {
                        state.insert_or_assign(o.id, o);
                        pending.push_back(mbo(o, 'A', book.exchange_moment_ns(o.isin), r.utc_ns, 32));
                    });
                    initialized = true;
                }
                flush(r);
            }
        },
        allow_incomplete);
    if (result.truncated) {
        // Never expose staged changes from a cut-off transaction. Withdraw the last book.
        pending.clear();
        reset(last_record, 4);
        flush(last_record);
    } else
        require(pending.empty(), "DBN archive ends with uncommitted book changes");
    result.open_interest_updates = oi_updates;
    result.trade_prints = trade_prints;
    result.valid_commits = valid;
    result.last_valid_order_count = last_count;
    require(result.capture_identity == checked.capture_identity && result.source_sha == checked.source_sha &&
                result.last_good_sequence == checked.last_good_sequence && result.isins == checked.isins &&
                result.refdata_sha256 == checked.refdata_sha256,
            "capture changed during DBN export");
    if (refdata.tsv.empty()) {
        file.publish();
    } else {
        // Rows observed after the final capture callback still belong to the
        // source sidecar. They become effective after the last DBN record.
        refdata.advance(result.last_good_sequence, sequence);
        result.refdata_unjoined_records = refdata.unjoined;
        result.dbn_sha256 = file.sha256();
        const auto companion = refdata.companion(result);
        Output metadata_file(output.string() + ".refdata.json");
        metadata_file.write(companion.data(), companion.size());
        metadata_file.publish();
        try {
            file.publish();
        } catch (...) {
            metadata_file.unpublish();
            throw;
        }
    }
    return result;
}
} // namespace moex::plaza2::cgate
