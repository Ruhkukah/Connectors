#pragma once
// Conditional reference model only. The synthetic identity is NOT an exchange identity contract.
#include <array>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

namespace c2 {
enum class State { Disabled, Opening, Snapshotting, CatchingUp, Ready, Stale, NeedsResync, Recovering, Failed };
enum class Result { Valid, Duplicate, Replay, NeedsResync, Fatal };
struct Order {
    std::int64_t id{}, session{}, instrument{}, side{}, price{}, remaining{}, status{}, status2{};
    int pair{};
    auto operator<=>(const Order&) const = default;
};
struct Event {
    Order order;
    std::int64_t revision{}, amount{};
    int action{1};
    std::uint64_t fingerprint{}; // Callback fixtures hash every qualified field, excluding padding.
    auto operator<=>(const Event&) const = default;
};
// Test histories allocate unique IDs per logical pair. This is a generator assumption, not a frozen key.
using Key = std::pair<int, std::int64_t>;
using Book = std::map<Key, Order>;
using Revision = std::pair<int, std::int64_t>;
struct Mutation {
    Book book;
    std::map<Revision, Event> seen;
    std::array<std::int64_t, 2> frontier{};
    Result apply(const Event& e) {
        const auto& o = e.order;
        if (o.pair < 0 || o.pair > 1 || e.revision <= 0 || o.remaining < 0 || e.amount < 0 ||
            (o.side != 1 && o.side != 2) || e.action < 0 || e.action > 2)
            return Result::Fatal;
        const Revision revision{o.pair, e.revision};
        if (const auto it = seen.find(revision); it != seen.end()) {
            if (it->second != e)
                return Result::NeedsResync;
            return e.revision == frontier[o.pair] ? Result::Duplicate : Result::Replay;
        }
        if (e.revision <= frontier[o.pair])
            return Result::NeedsResync;
        const Key key{o.pair, o.id};
        const auto it = book.find(key);
        if (e.action == 1) {
            if (it != book.end() || o.remaining == 0)
                return Result::NeedsResync;
            book.emplace(key, o); // Direct remainder; never use operation amount as original quantity.
        } else {
            if (it == book.end())
                return Result::NeedsResync;
            const auto& old = it->second;
            if (std::tie(old.session, old.instrument, old.side, old.price) !=
                std::tie(o.session, o.instrument, o.side, o.price))
                return Result::NeedsResync;
            if (e.action == 0)
                book.erase(it); // Terminal cancel; no subtraction or guessed cancel remainder.
            else {
                if (o.remaining > old.remaining)
                    return Result::NeedsResync;
                if (o.remaining == 0)
                    book.erase(it);
                else
                    it->second = o;
            }
        }
        seen.emplace(revision, e);
        frontier[o.pair] = e.revision;
        return Result::Valid;
    }
    std::uint64_t hash(std::uint64_t generation) const {
        std::uint64_t h = 14695981039346656037ULL;
        const auto mix = [&h](std::uint64_t n) {
            for (unsigned i = 0; i < 8; ++i) {
                h ^= n & 255;
                h *= 1099511628211ULL;
                n >>= 8;
            }
        };
        mix(generation);
        for (const auto& [key, o] : book) {
            mix(key.first);
            mix(o.id);
            mix(o.session);
            mix(o.instrument);
            mix(o.side);
            mix(o.price);
            mix(o.remaining);
            mix(o.status);
            mix(o.status2);
        }
        return h;
    }
};
struct Model {
    State state{State::Disabled};
    std::uint64_t generation{}, life{}, committed{}, drained{}, retry_at{};
    unsigned attempts{};
    bool transaction{}, online{}, usable{}, info_seen{};
    std::int64_t boundary{}, info_life{}, publication{-1};
    std::int64_t info_id{-1};
    Mutation data;
    std::vector<Event> pending;
    static constexpr std::size_t capacity = 20000;
    bool current(std::uint64_t handle) const {
        return handle == generation && state == State::Ready;
    }
    void invalidate(State next) {
        ++generation;
        state = next;
        life = 0;
        committed = drained = 0;
        transaction = online = usable = info_seen = false;
        boundary = info_life = 0;
        publication = info_id = -1;
        data = {};
        pending.clear();
    }
    void open() {
        invalidate(State::Opening);
    }
    void opened() {
        if (state == State::Opening)
            state = State::Snapshotting;
        else
            fail();
    }
    void lifenum(std::uint64_t n) {
        if (!n || (state != State::Snapshotting && state != State::CatchingUp && state != State::Ready &&
                   state != State::Recovering)) {
            fail();
            return;
        }
        if (life == n)
            return;
        const auto next = state == State::Recovering ? State::Recovering : State::Snapshotting;
        invalidate(next);
        life = n;
    }
    void fail(bool fatal = false) {
        invalidate(fatal || state == State::Failed ? State::Failed : State::NeedsResync);
    }
    void close() {
        invalidate(state == State::Failed ? State::Failed : State::Stale);
    }
    void begin() {
        if (transaction || !life ||
            (state != State::Snapshotting && state != State::CatchingUp && state != State::Ready)) {
            fail();
            return;
        }
        transaction = true;
        if (online)
            state = State::CatchingUp;
    }
    void snapshot(const Order& o) {
        if (!transaction || online || o.remaining <= 0 || (o.side != 1 && o.side != 2) || o.pair < 0 || o.pair > 1) {
            fail();
            return;
        }
        if (!data.book.emplace(Key{o.pair, o.id}, o).second)
            fail();
    }
    void info(std::int64_t id, std::int64_t rev, std::int64_t l, std::int64_t pub) {
        if (!transaction || online || rev < 0 || l != static_cast<std::int64_t>(life) || (pub != 0 && pub != 1)) {
            fail();
            return;
        }
        // Repeated publication updates are allowed, but a second snapshot identity is ambiguous.
        if (info_seen && (id != info_id || rev != boundary || l != info_life || publication > pub)) {
            fail();
            return;
        }
        info_seen = true;
        info_id = id;
        boundary = rev;
        info_life = l;
        publication = pub;
        usable = false;
    }
    void row(const Event& e) {
        if (!transaction || !online || pending.size() == capacity) {
            fail();
            return;
        }
        pending.push_back(e);
    }
    void commit() {
        if (!transaction) {
            fail();
            return;
        }
        for (const auto& e : pending) {
            const auto result = data.apply(e);
            if (result == Result::Fatal || result == Result::NeedsResync) {
                fail(result == Result::Fatal);
                return;
            }
        }
        pending.clear();
        transaction = false;
        ++committed;
        if (!online)
            usable = info_seen && publication == 1 && info_life == static_cast<std::int64_t>(life);
        refresh();
    }
    void online_event() {
        if (transaction || !usable || !life || online || state != State::Snapshotting) {
            fail();
            return;
        }
        online = true;
        data.frontier = {boundary, boundary};
        refresh();
    }
    void refresh() {
        if (online && usable && life)
            state = !transaction && drained == committed ? State::Ready : State::CatchingUp;
    }
    void drain() {
        if (state != State::Snapshotting && state != State::CatchingUp && state != State::Ready)
            return;
        drained = committed;
        refresh();
    }
    void clear_deleted(bool relevant, bool certified_historical_only = false) {
        // Only an explicitly proven historical-only event is non-mutating. Unknown relevant cases rebuild.
        if (relevant && !certified_historical_only)
            fail();
    }
    bool retry(std::uint64_t now) {
        if (state == State::Stale || state == State::NeedsResync) {
            state = State::Recovering;
            retry_at = now + 1000;
        }
        if (state != State::Recovering || now < retry_at)
            return false;
        if (++attempts > 3) {
            invalidate(State::Failed);
            return false;
        }
        open();
        return true;
    }
};
} // namespace c2
