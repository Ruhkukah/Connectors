#include "plaza2_target_forensics.hpp"
#include "moex/connector_host/operator_config.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <csignal>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <limits>
#include <optional>
#include <sys/resource.h>
#include <sstream>
#include <thread>
#include <tuple>
#include <unistd.h>

namespace {
namespace cg = moex::plaza2::cgate;
namespace ch = moex::connector_host;
namespace tr = moex::plaza2_trade;
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t stopping{};
void stop_signal(int) {
    stopping = 1;
}
std::uint64_t ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
std::optional<std::int64_t> exact_price(std::string_view value) {
    bool negative = false, decimal = false;
    unsigned digits = 0, fraction = 0;
    std::int64_t scaled = 0;
    if (value.starts_with('-')) {
        negative = true;
        value.remove_prefix(1);
    }
    for (const char c : value) {
        if (c == '.' && !decimal) {
            decimal = true;
            continue;
        }
        if (c < '0' || c > '9' || (decimal && ++fraction > 6))
            return std::nullopt;
        if (scaled > (std::numeric_limits<std::int64_t>::max() - (c - '0')) / 10)
            return std::nullopt;
        scaled = scaled * 10 + (c - '0');
        ++digits;
    }
    if (!digits)
        return std::nullopt;
    while (fraction++ < 6) {
        if (scaled > std::numeric_limits<std::int64_t>::max() / 10)
            return std::nullopt;
        scaled *= 10;
    }
    return negative ? -scaled : scaled;
}

struct PriceLimits {
    std::int64_t isin{}, session{}, revision{}, action{};
    std::string upper, lower;
};

struct Event {
    std::uint64_t time{}, value{};
    std::uint32_t stream{}, kind{}, error{}, message{}, user{};
    std::int64_t signed_value{};
    std::uint32_t table{}, flags{};
    std::size_t text_index{static_cast<std::size_t>(-1)};
};
// This executable is qualification-only. Counters and fixed event storage stay on
// the single CGate owner thread. Formatting and disk I/O happen after host.poll().
struct Evidence final : cg::Plaza2QualificationObserver, cg::Plaza2Aggr20QualificationObserver {
    const std::thread::id owner = std::this_thread::get_id();
    std::atomic<bool> owner_violation{false};
    std::array<Event, 8192> events{};
    std::size_t used{}, text_bytes{};
    std::vector<std::string> texts;
    std::map<std::pair<std::int64_t, std::int64_t>, PriceLimits> limits;
    std::vector<PriceLimits> pending_limits;
    std::optional<std::int64_t> pending_limit_clear;
    std::uint64_t ref_life{};
    bool ref_online{}, ref_transaction{}, ref_valid{true};
    std::uint64_t dropped{}, commits{}, invalid_books{}, callbacks{}, callback_errors{};
    std::map<std::uint32_t, std::array<std::uint64_t, 10>> counts;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<std::uint32_t, std::uint32_t>> states;
    std::vector<std::tuple<std::int64_t, std::int32_t, std::int64_t>> keys;
    Plaza2TargetForensics forensics;
    bool wants_forensic_row(const cg::Plaza2ListenerEvent& e) const noexcept override {
        return forensics.wants(e);
    }
    void forensic_row(cg::Plaza2ForensicRow row) noexcept override {
        forensics.capture(std::move(row));
    }
    Evidence() {
        keys.reserve(100000);
    }
    void observe(const cg::Plaza2ListenerEvent& e, const cg::Plaza2Error& error) noexcept override {
        if (std::this_thread::get_id() != owner) {
            owner_violation = true;
            return;
        }
        forensics.observe(e);
        try {
            using FC = moex::plaza2::generated::FieldCode;
            using EK = cg::Plaza2ListenerEventKind;
            if (e.stream_code == moex::plaza2::generated::StreamCode::kFortsRefdataRepl) {
                if (e.kind == EK::Open || e.kind == EK::Close || e.kind == EK::LifeNum) {
                    limits.clear();
                    pending_limits.clear();
                    pending_limit_clear.reset();
                    ref_online = false;
                    ref_transaction = false;
                    if (e.kind == EK::LifeNum)
                        ref_life = e.unsigned_value;
                } else if (e.kind == EK::ClearDeleted &&
                           e.table_code == moex::plaza2::generated::TableCode::kFortsRefdataReplFutSessContents) {
                    const auto expired = [&](const auto& row) {
                        return e.signed_value == std::numeric_limits<std::int64_t>::max() ||
                               row.revision < e.signed_value;
                    };
                    if (ref_transaction) {
                        pending_limit_clear = std::max(pending_limit_clear.value_or(e.signed_value), e.signed_value);
                        std::erase_if(pending_limits, expired);
                    } else
                        std::erase_if(limits, [&](const auto& entry) { return expired(entry.second); });
                } else if (e.kind == EK::TransactionBegin) {
                    if (ref_transaction)
                        ref_valid = false;
                    pending_limits.clear();
                    pending_limit_clear.reset();
                    ref_transaction = true;
                } else if (e.kind == EK::TransactionCommit) {
                    if (!ref_transaction)
                        ref_valid = false;
                    if (pending_limit_clear) {
                        std::erase_if(limits, [&](const auto& entry) {
                            return *pending_limit_clear == std::numeric_limits<std::int64_t>::max() ||
                                   entry.second.revision < *pending_limit_clear;
                        });
                        pending_limit_clear.reset();
                    }
                    for (auto& row : pending_limits) {
                        const auto key = std::pair{row.isin, row.session};
                        if (row.action)
                            limits.erase(key);
                        else if (limits.size() < 50000 || limits.contains(key))
                            limits[key] = std::move(row);
                        else
                            ref_valid = false;
                    }
                    pending_limits.clear();
                    ref_transaction = false;
                } else if (e.kind == EK::Online)
                    ref_online = !ref_transaction;
                else if (e.kind == EK::StreamData &&
                         e.table_code == moex::plaza2::generated::TableCode::kFortsRefdataReplFutSessContents) {
                    PriceLimits row;
                    for (const auto& f : e.fields) {
                        switch (f.field_code) {
                        case FC::kFortsRefdataReplFutSessContentsIsinId:
                            row.isin = f.signed_value;
                            break;
                        case FC::kFortsRefdataReplFutSessContentsSessId:
                            row.session = f.signed_value;
                            break;
                        case FC::kFortsRefdataReplFutSessContentsReplRev:
                            row.revision = f.signed_value;
                            break;
                        case FC::kFortsRefdataReplFutSessContentsReplAct:
                            row.action = f.signed_value;
                            break;
                        case FC::kFortsRefdataReplFutSessContentsLimitUp:
                            row.upper = f.text_value;
                            break;
                        case FC::kFortsRefdataReplFutSessContentsLimitDown:
                            row.lower = f.text_value;
                            break;
                        default:
                            break;
                        }
                    }
                    if (!ref_transaction || row.isin <= 0 || row.session <= 0 || pending_limits.size() >= 50000)
                        ref_valid = false;
                    else
                        pending_limits.push_back(std::move(row));
                }
            }
            ++callbacks;

            const auto stream = static_cast<std::uint32_t>(e.stream_code);
            const auto kind = static_cast<std::uint32_t>(e.kind);
            if (kind < 10)
                ++counts[stream][kind];
            // TN boundaries are counted exactly; only lifecycle/reply events are
            // retained individually, preventing an unbounded per-record log.
            if (!error && (e.kind == cg::Plaza2ListenerEventKind::TransactionBegin ||
                           e.kind == cg::Plaza2ListenerEventKind::TransactionCommit ||
                           (e.kind == cg::Plaza2ListenerEventKind::StreamData && e.stream_code != cg::kNoStreamCode)))
                return;
            if (used == events.size()) {
                ++dropped;
                return;
            }
            auto& event = events[used++];
            event = {ns(),
                     e.unsigned_value,
                     stream,
                     kind,
                     static_cast<std::uint32_t>(error.code),
                     static_cast<std::uint32_t>(e.message_id),
                     e.user_id,
                     e.signed_value,
                     static_cast<std::uint32_t>(e.table_code),
                     e.kind == EK::Close ? e.close_reason : e.clear_deleted_flags};
            if (!e.text_value.empty()) {
                if (e.text_value.size() > 65536 || text_bytes + e.text_value.size() > 1048576) {
                    ++dropped;
                    return;
                }
                event.text_index = texts.size();
                texts.emplace_back(e.text_value);
                text_bytes += e.text_value.size();
            }
        } catch (...) {
            ++dropped;
        }
    }
    void runtime_state(std::uint32_t kind, std::uint32_t stream, std::uint32_t state,
                       std::uint32_t error) noexcept override {
        if (std::this_thread::get_id() != owner) {
            owner_violation = true;
            return;
        }
        if (kind == 14)
            ++callback_errors;
        try {
            const auto key = std::pair{kind, stream};
            const auto value = std::pair{state, error};
            if (const auto it = states.find(key); it != states.end() && it->second == value)
                return;
            states[key] = value;
            if (used == events.size()) {
                ++dropped;
                return;
            }
            events[used++] = {ns(), state, stream, kind, error, 0, 0};
        } catch (...) {
            ++dropped;
        }
    }
    void committed(const cg::Plaza2Aggr20Snapshot& book) noexcept override {
        if (std::this_thread::get_id() != owner) {
            owner_violation = true;
            return;
        }
        ++commits;
        keys.clear();
        if (book.levels.size() > keys.capacity()) {
            ++invalid_books;
            return;
        }
        bool invalid = false;
        for (const auto& row : book.levels) {
            const auto price = exact_price(row.price);
            invalid |= !price || *price != row.price_scaled;
            invalid |= row.isin_id <= 0 || row.volume <= 0 || (row.dir != 1 && row.dir != 2) || row.price.empty();
            keys.emplace_back(row.isin_id, row.dir, row.price_scaled);
        }
        std::sort(keys.begin(), keys.end());
        std::size_t depth = 0;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const bool same_side = i && std::get<0>(keys[i]) == std::get<0>(keys[i - 1]) &&
                                   std::get<1>(keys[i]) == std::get<1>(keys[i - 1]);
            depth = same_side ? depth + 1 : 1;
            invalid |= depth > 20 || (i && keys[i] == keys[i - 1]);
        }
        invalid_books += invalid;
    }
    std::string limits_json() const {
        std::ostringstream out;
        out << "{\"online\":" << ref_online << ",\"valid\":" << ref_valid << ",\"lifenum\":" << ref_life
            << ",\"rows\":[";
        bool first = true;
        for (const auto& [key, row] : limits) {
            if (!first)
                out << ',';
            first = false;
            out << '[' << row.isin << ',' << row.session << ',' << row.revision << ',' << std::quoted(row.lower) << ','
                << std::quoted(row.upper) << ']';
        }
        out << "]}\n";
        return out.str();
    }
    bool runtime_active() const {
        return states.size() == 11 && std::all_of(states.begin(), states.end(), [](const auto& entry) {
                   return entry.second.first == 3 && entry.second.second == 0;
               });
    }
    void flush(std::ostream& out) {
        for (std::size_t i = 0; i < used; ++i) {
            const auto& e = events[i];
            out << e.time << ' ' << e.stream << ' ' << e.kind << ' ' << e.value << ' ' << e.error << ' ' << e.message
                << ' ' << e.user << ' ' << e.signed_value << ' ' << e.table << ' ' << e.flags << ' ';
            if (e.text_index < texts.size()) {
                constexpr char hex[] = "0123456789abcdef";
                for (const unsigned char c : texts[e.text_index])
                    out << hex[c >> 4] << hex[c & 15];
            }
            out << '\n';
        }
        used = 0;
        texts.clear();
        text_bytes = 0;
        out.flush();
    }
};
void write_file(const std::filesystem::path& path, const std::string& bytes) {
    const auto temp = path.string() + ".tmp";
    std::ofstream out(temp, std::ios::binary);
    out << bytes;
    out.close();
    if (!out)
        throw std::runtime_error("evidence write failed");
    std::filesystem::rename(temp, path);
}
std::string books_json(const ch::ConnectorHostQualificationSnapshot& q) {
    std::ostringstream out;
    out << "{\"online\":" << q.aggr_online << ",\"snapshot_complete\":" << q.aggr_snapshot_complete
        << ",\"revision\":" << q.book.last_repl_rev << ",\"instrument_count\":" << q.book.instrument_count
        << ",\"levels\":[";
    bool first = true;
    for (const auto& row : q.book.levels) {
        if (!first)
            out << ',';
        first = false;
        // Prices are numeric wire strings; scaled integer also retained for auditing.
        out << '[' << row.isin_id << ',' << row.dir << ',' << row.price_scaled << ',' << row.volume << ','
            << row.repl_id << ',' << row.repl_rev << ',' << std::quoted(row.price) << ']';
    }
    out << "],\"canonical_books\":[";
    std::vector<const cg::Plaza2Aggr20Level*> levels;
    levels.reserve(q.book.levels.size());
    for (const auto& row : q.book.levels)
        levels.push_back(&row);
    std::sort(levels.begin(), levels.end(), [](const auto* a, const auto* b) {
        return std::tie(a->isin_id, a->dir, a->price_scaled) < std::tie(b->isin_id, b->dir, b->price_scaled);
    });
    first = true;
    for (std::size_t i = 0; i < levels.size();) {
        const auto isin = levels[i]->isin_id;
        std::ostringstream canonical;
        do {
            const auto& row = *levels[i++];
            canonical << row.dir << ':' << row.price_scaled << ':' << row.volume << '\n';
        } while (i < levels.size() && levels[i]->isin_id == isin);
        if (!first)
            out << ',';
        first = false;
        out << '[' << isin << ',' << std::quoted(cg::plaza2_sha256_hex(canonical.str())) << ']';
    }
    out << "],\"instruments\":[";
    first = true;
    for (const auto& row : q.instruments) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"isin_id\":" << row.isin_id << ",\"session_id\":" << row.sess_id
            << ",\"last_trade_date\":" << row.last_trade_date << ",\"lot_volume\":" << row.lot_volume
            << ",\"kind\":" << static_cast<unsigned>(row.kind) << ",\"current_status\":" << row.current_status
            << ",\"has_current_status\":" << row.has_current_status << ",\"isin\":" << std::quoted(row.isin)
            << ",\"base_contract_code\":" << std::quoted(row.base_contract_code)
            << ",\"min_step\":" << std::quoted(row.min_step) << '}';
    }
    out << "]}\n";
    return out.str();
}
std::string exposure_json(const ch::ConnectorHostQualificationSnapshot& q) {
    std::vector<std::string> positions, orders;
    for (const auto& row : q.positions) {
        std::ostringstream out;
        out << '[' << std::quoted(cg::plaza2_sha256_hex(row.account_code)) << ',' << static_cast<int>(row.scope) << ','
            << static_cast<int>(row.account_type) << ',' << row.isin_id << ',' << row.xpos << ',' << row.xbuys_qty
            << ',' << row.xsells_qty << ',' << row.last_deal_id << ']';
        positions.push_back(out.str());
    }
    for (const auto& row : q.active_orders) {
        std::ostringstream out;
        out << '[' << std::quoted(cg::plaza2_sha256_hex(row.client_code)) << ',' << row.isin_id << ',' << row.sess_id
            << ',' << row.public_order_id << ',' << row.private_order_id << ',' << row.ext_id << ','
            << row.public_amount_rest << ',' << row.private_amount_rest << ',' << row.identity_conflict << ']';
        orders.push_back(out.str());
    }
    std::sort(positions.begin(), positions.end());
    std::sort(orders.begin(), orders.end());
    std::ostringstream out;
    out << "{\"positions\":[";
    const auto emit = [&](const auto& rows) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i)
                out << ',';
            out << rows[i];
        }
    };
    emit(positions);
    out << "],\"active_orders\":[";
    emit(orders);
    out << "]}\n";
    return out.str();
}
std::size_t nonzero_positions(const ch::ConnectorHostQualificationSnapshot& q) {
    return std::count_if(q.positions.begin(), q.positions.end(), [](const auto& row) { return row.xpos != 0; });
}
int idle_connection(cg::Plaza2Settings settings, const std::string& connection_settings,
                    const cg::Plaza2CredentialConfig& software_key, const std::filesystem::path& output) {
    const auto key = cg::load_plaza2_credentials(software_key);
    constexpr std::string_view token = "${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}";
    if (const auto position = settings.env_open_settings.find(token); position != std::string::npos) {
        if (!key)
            throw std::invalid_argument("software key unavailable");
        settings.env_open_settings.replace(position, token.size(), key->value);
    }
    cg::Plaza2Env env;
    cg::Plaza2Connection connection;
    if (env.open(settings) || connection.create(env, connection_settings) || connection.open({}))
        return 3;
    std::ofstream metrics(output / "metrics.jsonl");
    const auto start = Clock::now();
    auto previous = start, sample = start;
    std::uint64_t polls = 0, max_gap = 0;
    bool failed = false;
    while (!stopping && Clock::now() - start < std::chrono::seconds(300)) {
        const auto now = Clock::now();
        max_gap = std::max(max_gap, static_cast<std::uint64_t>(
                                        std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous).count()));
        previous = now;
        std::uint32_t code = 0, state = 0;
        ++polls;
        failed = static_cast<bool>(connection.process(10, &code)) || static_cast<bool>(connection.state(state));
        if (now >= sample || failed) {
            rusage usage{};
            getrusage(RUSAGE_SELF, &usage);
            metrics << "{\"monotonic_ns\":" << ns() << ",\"polls\":" << polls << ",\"max_poll_gap_ns\":" << max_gap
                    << ",\"state\":" << state << ",\"process_code\":" << code
                    << ",\"user_cpu_us\":" << usage.ru_utime.tv_sec * 1000000LL + usage.ru_utime.tv_usec
                    << ",\"system_cpu_us\":" << usage.ru_stime.tv_sec * 1000000LL + usage.ru_stime.tv_usec << "}\n";
            metrics.flush();
            if (!metrics)
                failed = true;
            sample = now + std::chrono::seconds(1);
        }
        if (failed)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool complete = Clock::now() - start >= std::chrono::seconds(300);
    failed |= static_cast<bool>(connection.close());
    failed |= static_cast<bool>(connection.destroy());
    failed |= static_cast<bool>(env.close());
    write_file(output / "result.json",
               std::string("{\"status\":\"") +
                   (failed     ? "FAIL"
                    : complete ? "PASS"
                               : "PARTIAL") +
                   "\",\"scope\":\"300 second connection-only polling, no listeners or publisher\"}\n");
    return failed || !complete ? 3 : 0;
}

bool zero_gate(const ch::ConnectorHostSnapshot& s) {
    return s.observation_ready && s.new_order_allowed && s.zero_starting_position_proven &&
           s.active_own_order_count == 0 && !s.order_epoch_active && s.evidence_consistent;
}
bool terms_gate(const Evidence& evidence, const ch::ConnectorHostQualificationSnapshot& q,
                const ch::ConnectorHostSnapshot& s, const ch::ConnectorHostOrderRequest& order) {
    if (evidence.ref_transaction || !q.active_orders.empty() || nonzero_positions(q))
        return false;
    const auto limits = evidence.limits.find({s.target_isin_id, s.session_id});
    if (limits == evidence.limits.end())
        return false;
    const auto lower = exact_price(limits->second.lower), upper = exact_price(limits->second.upper);
    const auto price = exact_price(order.price), tick = exact_price(s.min_step);
    const auto bid = exact_price(s.bid), ask = exact_price(s.ask);
    if (!lower || !upper || !price || !tick || !bid || !ask || *lower >= *upper || *tick <= 0 || *price < *lower ||
        *price > *upper || *price % *tick || *bid >= *ask || *tick > std::numeric_limits<std::int64_t>::max() / 4)
        return false;
    // Retain the existing live maximum of four ticks and use its most passive boundary.
    if (order.side == tr::Plaza2TradeSide::Buy
            ? (*bid < std::numeric_limits<std::int64_t>::min() + 4 * *tick || *price != *bid - 4 * *tick)
            : (*ask > std::numeric_limits<std::int64_t>::max() - 4 * *tick || *price != *ask + 4 * *tick))
        return false;
    const auto instrument = std::find_if(q.instruments.begin(), q.instruments.end(), [&](const auto& row) {
        return row.isin_id == s.target_isin_id && row.sess_id == s.session_id;
    });
    return instrument != q.instruments.end() && instrument->last_trade_date > std::time(nullptr) + 86400 &&
           instrument->base_contract_code == order.base_contract_code && !instrument->is_spread &&
           instrument->kind == moex::plaza2::private_state::InstrumentKind::kFuture;
}

int self_test() {
    if (exact_price("12.34567") != 12345670 || exact_price("-0.5") != -500000 || exact_price("1x2") ||
        exact_price("1.1234567") || exact_price("99999999999999999999"))
        return 1;
    Evidence evidence;
    cg::Plaza2Aggr20Snapshot book;
    book.levels.push_back({.isin_id = 1, .price_scaled = 12000000, .volume = 1, .dir = 1, .price = "12"});
    evidence.committed(book);
    if (evidence.invalid_books || evidence.commits != 1)
        return 2;
    book.levels.push_back(book.levels.front());
    evidence.committed(book);
    if (evidence.invalid_books != 1)
        return 3;
    book.levels.pop_back();
    book.levels.front().price = "garbage";
    evidence.committed(book);
    if (evidence.invalid_books != 2)
        return 4;
    const cg::Plaza2ListenerEvent row{.kind = cg::Plaza2ListenerEventKind::StreamData,
                                      .stream_code = moex::plaza2::generated::StreamCode::kFortsAggrRepl};
    for (unsigned i = 0; i < 20000; ++i)
        evidence.observe(row, {});
    if (evidence.used || evidence.dropped || evidence.callbacks != 20000)
        return 5;
    evidence.runtime_state(10, 0, 3, 0);
    evidence.runtime_state(10, 0, 3, 0);
    if (evidence.used != 1 || evidence.runtime_active())
        return 6;
    const cg::Plaza2ListenerEvent life{.kind = cg::Plaza2ListenerEventKind::LifeNum};
    for (unsigned i = 0; i < 9000; ++i)
        evidence.observe(life, {});
    if (evidence.used != evidence.events.size() || evidence.dropped == 0)
        return 7;
    ch::ConnectorHostSnapshot state;
    state.target_isin_id = 1;
    state.session_id = 1;
    state.bid = "99";
    state.ask = "100";
    state.min_step = "1";
    ch::ConnectorHostQualificationSnapshot q;
    moex::plaza2::private_state::InstrumentSnapshot instrument;
    instrument.isin_id = 1;
    instrument.sess_id = 1;
    instrument.base_contract_code = "TEST";
    instrument.kind = moex::plaza2::private_state::InstrumentKind::kFuture;
    instrument.last_trade_date = std::time(nullptr) + 172800;
    q.instruments.push_back(instrument);
    evidence.limits[{1, 1}] = {.isin = 1, .session = 1, .upper = "110", .lower = "90"};
    ch::ConnectorHostOrderRequest order{
        .side = tr::Plaza2TradeSide::Sell, .price = "104", .base_contract_code = "TEST"};
    if (!terms_gate(evidence, q, state, order))
        return 8;
    q.positions.push_back({.isin_id = 2, .xpos = 1});
    if (terms_gate(evidence, q, state, order))
        return 13;
    q.positions.clear();
    q.active_orders.push_back({.isin_id = 2, .public_amount_rest = 1});
    if (terms_gate(evidence, q, state, order))
        return 14;
    q.active_orders.clear();
    order.price = "100";
    if (terms_gate(evidence, q, state, order))
        return 9;
    order.price = "104";
    q.instruments.front().last_trade_date = std::time(nullptr);
    if (terms_gate(evidence, q, state, order))
        return 10;
    Evidence text_evidence;
    std::string token = "life=17;rev=123";
    text_evidence.observe({.kind = cg::Plaza2ListenerEventKind::ReplState, .text_value = token}, {});
    token = "overwritten";
    std::ostringstream text_log;
    text_evidence.flush(text_log);
    if (text_log.str().find("6c6966653d31373b7265763d313233") == std::string::npos || text_evidence.text_bytes)
        return 11;
    text_evidence.runtime_state(14, 0, 1, 1);
    if (text_evidence.callback_errors != 1)
        return 12;
    Evidence ref;
    using FC = moex::plaza2::generated::FieldCode;
    using TC = moex::plaza2::generated::TableCode;
    using EK = cg::Plaza2ListenerEventKind;
    constexpr auto stream = moex::plaza2::generated::StreamCode::kFortsRefdataRepl;
    const std::array<cg::Plaza2DecodedFieldValue, 5> fields{{
        {.field_code = FC::kFortsRefdataReplFutSessContentsIsinId, .signed_value = 1},
        {.field_code = FC::kFortsRefdataReplFutSessContentsSessId, .signed_value = 1},
        {.field_code = FC::kFortsRefdataReplFutSessContentsReplRev, .signed_value = 10},
        {.field_code = FC::kFortsRefdataReplFutSessContentsLimitUp, .text_value = "110"},
        {.field_code = FC::kFortsRefdataReplFutSessContentsLimitDown, .text_value = "90"},
    }};
    ref.observe({.kind = EK::TransactionBegin, .stream_code = stream}, {});
    ref.observe({.kind = EK::StreamData,
                 .stream_code = stream,
                 .table_code = TC::kFortsRefdataReplFutSessContents,
                 .fields = fields},
                {});
    ref.observe({.kind = EK::TransactionCommit, .stream_code = stream}, {});
    ref.observe({.kind = EK::ClearDeleted, .stream_code = stream, .table_code = TC::kFortsRefdataReplOptSessContents},
                {});
    ref.observe({.kind = EK::Online, .stream_code = stream}, {});
    if (ref.limits.size() != 1 || !ref.ref_online || !ref.ref_valid)
        return 15;
    ref.observe({.kind = EK::ClearDeleted,
                 .stream_code = stream,
                 .table_code = TC::kFortsRefdataReplFutSessContents,
                 .signed_value = 10},
                {});
    if (ref.limits.size() != 1 || !ref.ref_online)
        return 16;
    ref.observe({.kind = EK::TransactionBegin, .stream_code = stream}, {});
    ref.observe({.kind = EK::ClearDeleted,
                 .stream_code = stream,
                 .table_code = TC::kFortsRefdataReplFutSessContents,
                 .signed_value = std::numeric_limits<std::int64_t>::max()},
                {});
    if (ref.limits.size() != 1)
        return 17; // No publication before commit.
    auto fresh_fields = fields;
    fresh_fields[2].signed_value = 1;
    ref.observe({.kind = EK::StreamData,
                 .stream_code = stream,
                 .table_code = TC::kFortsRefdataReplFutSessContents,
                 .fields = fresh_fields},
                {});
    ref.observe({.kind = EK::TransactionCommit, .stream_code = stream}, {});
    if (ref.limits.size() != 1 || ref.limits.begin()->second.revision != 1 || !ref.ref_valid)
        return 18; // Fresh revision after MAX survives; the previous row does not.
    ref.observe({.kind = EK::ClearDeleted,
                 .stream_code = stream,
                 .table_code = TC::kFortsRefdataReplFutSessContents,
                 .signed_value = 2},
                {});
    if (!ref.limits.empty())
        return 19;
    Plaza2TargetForensics probe;
    probe.isin = 1;
    probe.session = 1;
    probe.expected_symbol = "TEST";
    cg::Plaza2ForensicRow raw;
    raw.fields = {{.name = "replRev", .generic_value = "1", .independent_value = "1", .equal = true},
                  {.name = "isin_id", .generic_value = "1", .independent_value = "1", .equal = true},
                  {.name = "sess_id", .generic_value = "1", .independent_value = "1", .equal = true},
                  {.name = "isin", .generic_value = "TEST", .independent_value = "TEST", .equal = true}};
    probe.observe({.kind = EK::TransactionBegin, .stream_code = stream});
    probe.capture(raw);
    if (!probe.drain(true).empty())
        return 20;
    probe.observe({.kind = EK::LifeNum, .stream_code = stream, .unsigned_value = 9});
    probe.observe({.kind = EK::TransactionCommit, .stream_code = stream});
    if (probe.has_committed())
        return 21;
    Plaza2TargetForensics valid_probe;
    valid_probe.isin = 1;
    valid_probe.session = 1;
    valid_probe.expected_symbol = "TEST";
    valid_probe.observe({.kind = EK::TransactionBegin, .stream_code = stream});
    valid_probe.capture(raw);
    valid_probe.observe({.kind = EK::TransactionCommit, .stream_code = stream});
    if (valid_probe.failed || !valid_probe.has_committed() || valid_probe.drain(true).find("TEST") == std::string::npos)
        return 22;
    raw.fields.back().equal = false;
    valid_probe.observe({.kind = EK::TransactionBegin, .stream_code = stream});
    valid_probe.capture(raw);
    if (!valid_probe.failed)
        return 23;
    std::cout << "qualification evidence self-test PASS\n";
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test")
            return self_test();
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            std::cout << MOEX_SOURCE_GIT_SHA << '\n';
            return 0;
        }
        if (argc < 5)
            throw std::invalid_argument("usage: plaza2_aggr_qualification OUTPUT SECONDS plaza2 qualify OPTIONS");
        std::uint32_t seconds{};
        const std::string_view duration(argv[2]);
        const auto parsed = std::from_chars(duration.data(), duration.data() + duration.size(), seconds);
        if (parsed.ec != std::errc{} || parsed.ptr != duration.data() + duration.size() || !seconds || seconds > 36000)
            throw std::invalid_argument("duration must be in 1..36000 seconds");
        const auto* auth = std::getenv("MOEX_AGGR_T1_AUTH");
        const auto now = std::time(nullptr);
        const auto moscow = now + 3 * 3600;
        std::tm date{};
        gmtime_r(&moscow, &date);
        if (!auth || std::string_view(auth) != "20260909_AGGREGATED_QUALIFICATION" || date.tm_year != 126 ||
            date.tm_mon != 8 || date.tm_mday != 9 || date.tm_hour < 6 || (date.tm_hour == 6 && date.tm_min < 58) ||
            (date.tm_hour * 60 + date.tm_min >= 16 * 60 + 10))
            throw std::invalid_argument("outside authorized 2026-09-09 T1 qualification window");
        std::vector<std::string_view> args;
        for (int i = 3; i < argc; ++i)
            args.emplace_back(argv[i]);
        auto request = ch::parse_operator_arguments(args);
        if (request.command != "qualify")
            throw std::invalid_argument("qualification runner requires plaza2 qualify configuration");
        const auto* journal = std::getenv("MOEX_AGGR_T1_JOURNAL");
        if (!journal || !*journal)
            throw std::invalid_argument("persistent qualification journal path is required");
        request.config.transport.allow_exact_ext_id_recovery = false;
        request.config.order.journal_root = journal;
        request.config.order.run_id = "aggr-t1-20260909";
        request.config.order.profile_id = "main-aggregated-t1-20260909";
        request.config.order.profile_fingerprint = cg::plaza2_sha256_hex(request.config.order.profile_id);
        request.config.order.ext_id = 2026090900;
        request.config.order.add_user_id = 2026090901;
        request.config.order.cancel_user_id = 2026090902;
        request.config.order.recovery_user_id = 2026090903;
        if (const auto* order_auth = std::getenv("MOEX_AGGR_T1_ORDER_AUTH")) {
            if (std::string_view(order_auth) != "20260909_ONE_LOT_ADD_CANCEL")
                throw std::invalid_argument("invalid qualification order authorization");
            request.config.purpose = ch::HostPurpose::OrderTest;
            request.config.transport.host.mode = tr::Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            request.config.transport.host.arm_state.test_order_send_armed = true;
        }

        const std::filesystem::path output(argv[1]);
        if (!std::filesystem::create_directory(output))
            throw std::invalid_argument("output must be a new directory under an existing evidence parent");
        std::signal(SIGTERM, stop_signal);
        std::signal(SIGINT, stop_signal);
        if (std::getenv("MOEX_AGGR_T1_IDLE")) {
            const auto& arms = request.config.transport.host.arm_state;
            if (seconds != 300 || request.config.purpose != ch::HostPurpose::Qualify || !arms.test_network_armed ||
                !arms.test_session_armed || !arms.test_plaza2_armed ||
                date.tm_hour * 3600 + date.tm_min * 60 + date.tm_sec > 16 * 3600 + 5 * 60)
                throw std::invalid_argument(
                    "idle probe requires 300 seconds, three TEST arms and no order authorization");
            return idle_connection(request.config.transport.host.runtime,
                                   request.config.transport.host.connection_settings,
                                   request.config.transport.host.software_key, output);
        }
        Evidence evidence;
        evidence.forensics.isin = request.config.transport.target_isin_id;
        evidence.forensics.session = request.config.transport.target_session_id;
        if (const auto* symbol = std::getenv("MOEX_AGGR_FORENSIC_SYMBOL"))
            evidence.forensics.expected_symbol = symbol;
        std::ofstream forensic_output(output / "target-forensics.jsonl");
        request.config.transport.host.runtime.qualification_observer = &evidence;
        request.config.transport.host.qualification_book_observer = &evidence;
        request.config.transport.host.process_timeout_ms = 10;
        std::ofstream events(output / "events.log"), metrics(output / "metrics.jsonl");
        events << "monotonic_ns stream kind value error message_id user_id signed_value table flags text_hex\n";
        write_file(output / "environment.json",
                   "{\"source_sha\":\"" MOEX_SOURCE_GIT_SHA "\",\"pid\":" + std::to_string(getpid()) +
                       ",\"owner_thread\":" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
                       "}\n");
        ch::ConnectorHost host(request.config);
        write_file(output / "state_before.json", ch::render_snapshot(host.snapshot(), true));
        std::signal(SIGTERM, stop_signal);
        std::signal(SIGINT, stop_signal);
        const auto started = Clock::now();
        auto previous = started;
        auto next_sample = started;
        auto deadline = started + std::chrono::seconds(seconds);
        auto end_date = date;
        end_date.tm_hour = 16;
        end_date.tm_min = 10;
        end_date.tm_sec = 0;
        const auto end_utc = timegm(&end_date) - 3 * 3600;
        // Never continue beyond the authorized end, even when duration is misconfigured.
        const auto end_seconds = (16 * 60 + 10 - (date.tm_hour * 60 + date.tm_min)) * 60 - date.tm_sec;
        deadline = std::min(deadline, started + std::chrono::seconds(std::max(0, end_seconds)));
        bool failed = static_cast<bool>(host.start());
        bool orders_blocked = false;
        bool cancel_sent = false;
        std::uint64_t polls{}, max_gap{};
        std::optional<tr::OrderLifecycleState> last_lifecycle;
        std::optional<bool> last_ready;
        while (!failed && !stopping && Clock::now() < deadline && std::time(nullptr) < end_utc) {
            const auto current = Clock::now();
            max_gap = std::max(max_gap,
                               static_cast<std::uint64_t>(
                                   std::chrono::duration_cast<std::chrono::nanoseconds>(current - previous).count()));
            previous = current;
            ++polls;
            failed = static_cast<bool>(host.poll());
            auto state = host.snapshot();
            if (last_ready != state.observation_ready) {
                events << ns() << " 0 13 " << state.observation_ready << " 0 0 0\n";
                last_ready = state.observation_ready;
            }
            if (state.order_epoch_active) {
                const auto result = host.poll_order();
                state = host.snapshot();
                if (state.lifecycle_state != last_lifecycle) {
                    events << ns() << " order " << ch::render_snapshot(state, true);
                    last_lifecycle = state.lifecycle_state;
                }
                if (state.executed_quantity || !state.evidence_consistent)
                    orders_blocked = true;
                if (state.lifecycle_state == tr::OrderLifecycleState::Working && !cancel_sent) {
                    cancel_sent = true;
                    events << ns() << " CANCEL_BEGIN\n";
                    const auto cancel = host.cancel_current_order();
                    events << ns() << " CANCEL_END " << static_cast<unsigned>(cancel.state) << ' '
                           << cancel.cancel_submission.post_invoked << '\n';
                }
                if (state.lifecycle_state == tr::OrderLifecycleState::Cancelled && state.market_safe &&
                    state.evidence_consistent && !state.executed_quantity) {
                    orders_blocked |= static_cast<bool>(host.finish_order_epoch());
                    cancel_sent = false;
                } else if (state.lifecycle_state == tr::OrderLifecycleState::PossiblySent ||
                           state.lifecycle_state == tr::OrderLifecycleState::UnresolvedOrphanIncident ||
                           state.lifecycle_state == tr::OrderLifecycleState::Rejected ||
                           state.lifecycle_state == tr::OrderLifecycleState::Filled ||
                           state.lifecycle_state == tr::OrderLifecycleState::PartiallyFilled) {
                    orders_blocked = true;
                }
                (void)result;
            }
            evidence.flush(events);
            orders_blocked |= std::time(nullptr) >= end_utc - 15 * 60;
            orders_blocked |= evidence.invalid_books || evidence.dropped || evidence.callback_errors ||
                              evidence.owner_violation || evidence.forensics.failed;
            if (current >= next_sample || failed) {
                const auto q = host.qualification_snapshot();
                if (evidence.forensics.has_committed()) {
                    const bool target_present =
                        q.aggr_online && std::any_of(q.book.levels.begin(), q.book.levels.end(), [&](const auto& row) {
                            return row.isin_id == evidence.forensics.isin;
                        });
                    forensic_output << evidence.forensics.drain(target_present);
                    forensic_output.flush();
                }
                std::ostringstream participants;
                participants << "{\"broker_matches\":" << q.matching_broker_limit_rows
                             << ",\"client_matches\":" << q.matching_client_limit_rows
                             << ",\"unknown_rows\":" << q.unknown_limit_rows
                             << ",\"client_code_is_000\":" << q.client_code_is_brokerage_account << ",\"rows\":[";
                bool separator = false;
                for (const auto& row : q.limit_diagnostics) {
                    if (separator)
                        participants << ',';
                    separator = true;
                    participants << "{\"kind\":" << static_cast<unsigned>(row.kind)
                                 << ",\"code_length\":" << row.code_length << ",\"equals_broker\":" << row.equals_broker
                                 << ",\"equals_client\":" << row.equals_client << ",\"limits_set\":" << row.limits_set
                                 << ",\"auto_update\":" << row.auto_update
                                 << ",\"money_free\":" << std::quoted(row.money_free)
                                 << ",\"money_blocked\":" << std::quoted(row.money_blocked)
                                 << ",\"money_amount\":" << std::quoted(row.money_amount) << '}';
                }
                participants << "]}\n";
                write_file(output / "participant-limits.json", participants.str());
                write_file(output / "state_current.json", ch::render_snapshot(host.snapshot(), true));
                const auto exposure_bytes = exposure_json(q);
                write_file(output / "private_exposure_current.json", exposure_bytes);
                orders_blocked |= nonzero_positions(q) != 0;
                const auto book_bytes = books_json(q);
                write_file(output / "book_current.json", book_bytes);
                write_file(output / "price_limits_current.json", evidence.limits_json());
                rusage resources{};
                getrusage(RUSAGE_SELF, &resources);
                std::uint64_t rss_kib = 0, fds = 0;
#ifdef __linux__
                std::ifstream status("/proc/self/status");
                std::string status_line;
                while (std::getline(status, status_line)) {
                    if (status_line.starts_with("VmRSS:")) {
                        std::istringstream line(status_line.substr(6));
                        line >> rss_kib;
                    }
                }
                std::error_code fd_error;
                for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", fd_error)) {
                    (void)entry;
                    ++fds;
                }
#endif

                metrics << "{\"monotonic_ns\":" << ns() << ",\"polls\":" << polls << ",\"max_poll_gap_ns\":" << max_gap
                        << ",\"callbacks\":" << evidence.callbacks << ",\"commits\":" << evidence.commits
                        << ",\"invalid_books\":" << evidence.invalid_books << ",\"event_loss\":" << evidence.dropped
                        << ",\"callback_errors\":" << evidence.callback_errors
                        << ",\"runtime_active\":" << evidence.runtime_active()
                        << ",\"orders_blocked\":" << orders_blocked << ",\"book_sha256\":\""
                        << cg::plaza2_sha256_hex(book_bytes) << "\""
                        << ",\"owner_violation\":" << evidence.owner_violation << ",\"rss_kib\":" << rss_kib
                        << ",\"fd_count\":" << fds << ",\"maxrss_native_units\":" << resources.ru_maxrss
                        << ",\"user_cpu_us\":" << (resources.ru_utime.tv_sec * 1000000LL + resources.ru_utime.tv_usec)
                        << ",\"system_cpu_us\":" << (resources.ru_stime.tv_sec * 1000000LL + resources.ru_stime.tv_usec)
                        << ",\"visible_limit_rows\":" << q.visible_limit_rows
                        << ",\"matching_client_limit_rows\":" << q.matching_client_limit_rows
                        << ",\"account_active_orders\":" << q.active_orders.size()
                        << ",\"account_nonzero_positions\":" << nonzero_positions(q)
                        << ",\"private_exposure_sha256\":\"" << cg::plaza2_sha256_hex(exposure_bytes) << "\""
                        << ",\"rate_admitted\":" << q.rate.admitted << ",\"rate_throttled\":" << q.rate.throttled
                        << ",\"streams\": [";
                bool first = true;
                for (const auto& [stream, counts] : evidence.counts) {
                    if (!first)
                        metrics << ',';
                    first = false;
                    metrics << '[' << stream;
                    for (const auto count : counts)
                        metrics << ',' << count;
                    metrics << ']';
                }
                metrics << "]}\n";
                metrics.flush();
                if (!metrics || !events)
                    throw std::runtime_error("evidence output failed");
                next_sample = current + std::chrono::seconds(1);
                // The supervisor owns instrument/expiry/price-limit/session checks.
                // Consuming a uniquely numbered request before begin_order forbids
                // automatic replay or Add retry after uncertainty or a process crash.
                const auto command = output / "order.request";
                if (std::filesystem::exists(command)) {
                    std::ifstream input(command);
                    ch::ConnectorHostOrderRequest order;
                    std::string side, extra;
                    input >> side >> order.price >> order.base_contract_code;
                    const bool valid = input && !(input >> extra) && (side == "buy" || side == "sell");
                    std::filesystem::rename(command, output / ("order-consumed-" + std::to_string(ns()) + ".txt"));
                    order.side = side == "buy" ? tr::Plaza2TradeSide::Buy : tr::Plaza2TradeSide::Sell;
                    if (!valid || orders_blocked || !evidence.runtime_active() || !evidence.ref_online ||
                        !evidence.ref_valid || !zero_gate(host.snapshot()) ||
                        !terms_gate(evidence, q, host.snapshot(), order)) {
                        events << ns() << " ORDER_REFUSED_PRECONDITION\n";
                    } else {
                        const auto plan = host.plan_order(order);
                        if (!plan.ok)
                            events << ns() << " ORDER_REFUSED_PLAN\n";
                        else {
                            write_file(output / ("plan-" + std::to_string(ns()) + ".json"), plan.canonical_json);
                            if (host.begin_order(order, plan.canonical_json, plan.sha256))
                                events << ns() << " ORDER_REFUSED_BIND\n";
                            else {
                                events << ns() << " ADD_BEGIN " << plan.sha256 << '\n';
                                const auto sent = host.submit_order();
                                events << ns() << " ADD_END " << static_cast<unsigned>(sent.state) << ' '
                                       << sent.add_submission.post_invoked << '\n';
                                orders_blocked |= sent.state != tr::OrderLifecycleState::Posted;
                            }
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        write_file(output / "state_after.json", ch::render_snapshot(host.snapshot(), true));
        const bool stopped = !host.stop();
        evidence.flush(events);
        const bool evidence_failed = failed || !stopped || evidence.invalid_books || evidence.dropped ||
                                     evidence.callback_errors || evidence.owner_violation;
        write_file(output / "result.json",
                   std::string("{\"status\":\"") + (evidence_failed ? "FAIL" : "PARTIAL") +
                       "\",\"scope\":\"host observation; scenario verdict requires retained evidence review\"}\n");
        return evidence_failed ? 3 : 0;
    } catch (const std::exception& error) {
        std::cerr << "qualification: " << error.what() << '\n';
        return 2;
    }
}
