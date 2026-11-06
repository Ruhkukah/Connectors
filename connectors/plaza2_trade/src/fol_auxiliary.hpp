#pragma once

#include "moex/plaza2/cgate/plaza2_decimal.hpp"
#include "moex/plaza2/cgate/plaza2_runtime.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <functional>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace moex::plaza2_trade {
namespace fol_auxiliary {
namespace cg = moex::plaza2::cgate;
using Event = cg::Plaza2ListenerEvent;
using Kind = cg::Plaza2ListenerEventKind;

struct Field {
    std::string_view name, type;
    bool optional{};
};

struct Table {
    std::string_view name;
    std::vector<Field> fields;
    std::string_view account_field;
    bool selected_instrument{};
};

inline std::size_t field_size(std::string_view type) {
    if (type == "i1")
        return 1;
    if (type == "i4")
        return 4;
    if (type == "i8" || type == "u8")
        return 8;
    if (type == "t")
        return sizeof(plaza2::public_wire::Time);
    const auto parse = [](std::string_view digits, unsigned max_width) {
        unsigned out{};
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), out);
        if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size() || out > max_width)
            throw std::invalid_argument("unsupported auxiliary field width");
        return out;
    };
    if (type.starts_with('c'))
        return parse(type.substr(1), 255) + 1;
    if (type.starts_with('d')) {
        const auto dot = type.find('.');
        if (dot != std::string_view::npos) {
            const auto precision = parse(type.substr(1, dot - 1), 32);
            const auto scale = parse(type.substr(dot + 1), 32);
            if (precision > scale)
                return 2 + (precision - scale + 1) / 2 + (scale + 1) / 2;
        }
    }
    throw std::invalid_argument("unsupported auxiliary field type");
}

inline std::string decimal(std::span<const std::byte> data, std::string_view type) {
    const auto dot = type.find('.');
    const auto precision = std::to_integer<unsigned>(data[1]);
    const auto scale = std::to_integer<unsigned>(data[0]);
    if ("d" + std::to_string(precision) + "." + std::to_string(scale) != type || dot == std::string_view::npos ||
        precision <= scale || field_size(type) != data.size())
        throw std::invalid_argument("invalid auxiliary decimal header");
    const bool negative = (std::to_integer<unsigned>(data[2]) & 0x80) != 0;
    std::string digits;
    for (std::size_t i = 2; i < data.size(); ++i) {
        auto value = std::to_integer<unsigned>(data[i]);
        if (i == 2)
            value &= 0x7f;
        else if (value == 0x80)
            value = 0;
        const bool first_half = i == 2 && (precision - scale) % 2;
        const bool last_half = i + 1 == data.size() && scale % 2;
        if (value > (first_half ? 9u : 99u) || (last_half && value % 10))
            throw std::invalid_argument("invalid auxiliary decimal digits");
        if (!first_half)
            digits += static_cast<char>('0' + value / 10);
        if (!last_half)
            digits += static_cast<char>('0' + value % 10);
    }
    const auto dot_at = digits.size() - scale;
    const auto first = digits.find_first_not_of('0');
    auto out = digits.substr(std::min(dot_at - 1, first));
    if (scale)
        out.insert(out.size() - scale, 1, '.');
    if (negative && first != std::string::npos)
        out.insert(out.begin(), '-');
    return out;
}

class Projection final : public cg::Plaza2ListenerEventHandler {
  public:
    using Clock = std::chrono::steady_clock;
    using Values = std::map<std::string_view, std::string, std::less<>>;
    struct Row {
        std::int64_t revision{};
        Values values;
        std::uint64_t commit_sequence{};
    };
    using Rows = std::map<std::pair<std::size_t, std::int64_t>, Row>;

    Projection(std::vector<Table> tables, std::string account, std::vector<std::int64_t> instruments,
               std::function<Clock::time_point()> now = Clock::now)
        : tables_(std::move(tables)), account_(std::move(account)), instruments_(std::move(instruments)),
          now_(std::move(now)) {}

    bool wants_raw_replication(std::string_view) const noexcept override {
        return true;
    }
    void on_plaza2_listener_error(const cg::Plaza2Error&) noexcept override {
        reset();
    }
    cg::Plaza2Error on_plaza2_listener_event(const Event& event) noexcept override {
        try {
            switch (event.kind) {
            case Kind::Open:
                reset();
                bind(event.raw_tables);
                break;
            case Kind::Close:
                reset();
                break;
            case Kind::LifeNum:
                if (life_ && life_ != event.unsigned_value)
                    throw std::invalid_argument("auxiliary LifeNum changed; a new snapshot is required");
                life_ = event.unsigned_value;
                break;
            case Kind::TransactionBegin:
                if (transaction_)
                    throw std::invalid_argument("nested auxiliary transaction");
                transaction_ = true;
                break;
            case Kind::TransactionCommit:
                if (!transaction_)
                    throw std::invalid_argument("auxiliary commit without begin");
                apply();
                transaction_ = false;
                break;
            case Kind::Online:
                if (transaction_)
                    throw std::invalid_argument("auxiliary ONLINE during transaction");
                apply();
                if (bindings_.empty())
                    throw std::invalid_argument("auxiliary ONLINE before schema binding");
                online_ = true;
                break;
            case Kind::ReplState:
                if (online_ && !transaction_)
                    committed_at_ = now_();
                break;
            case Kind::StreamData:
                if (online_ && !transaction_)
                    throw std::invalid_argument("auxiliary row outside a transaction");
                row(event);
                break;
            case Kind::ClearDeleted:
                if (event.signed_value == INT64_MAX) {
                    if (online_)
                        throw std::invalid_argument("auxiliary table reset; a new snapshot is required");
                    if (const auto table = bindings_.find(event.table_index); table != bindings_.end()) {
                        const auto index = table->second.table;
                        std::erase_if(rows_, [&](const auto& item) { return item.first.first == index; });
                        std::erase_if(pending_, [&](const auto& item) { return item.key.first == index; });
                        std::erase_if(tombstones_, [&](const auto& item) { return item.first.first == index; });
                    }
                }
                break;
            default:
                break;
            }
            return {};
        } catch (const std::exception& error) {
            reset();
            return {.code = event.kind == Kind::Open ? cg::Plaza2ErrorCode::IncompatibleScheme
                                                     : cg::Plaza2ErrorCode::DecodeFailed,
                    .message = error.what()};
        }
    }
    bool online() const noexcept {
        return online_;
    }
    std::uint64_t life() const noexcept {
        return life_;
    }
    Clock::time_point committed_at() const noexcept {
        return committed_at_;
    }
    const Rows& rows() const noexcept {
        static const Rows empty;
        return online_ ? rows_ : empty;
    }
    std::string_view table_name(std::size_t index) const {
        return tables_.at(index).name;
    }

  private:
    struct BoundField {
        Field field;
        std::size_t offset{}, size{}, ordinal{};
    };
    struct BoundTable {
        std::size_t table{}, size{};
        std::vector<BoundField> fields;
    };
    struct Change {
        std::pair<std::size_t, std::int64_t> key;
        Row row;
        bool deleted{};
    };
    std::vector<Table> tables_;
    std::string account_;
    std::vector<std::int64_t> instruments_;
    std::function<Clock::time_point()> now_;
    std::map<std::size_t, BoundTable> bindings_;
    Rows rows_;
    std::map<std::pair<std::size_t, std::int64_t>, std::int64_t> tombstones_;
    std::vector<Change> pending_;
    Clock::time_point committed_at_{};
    std::uint64_t life_{}, commit_sequence_{};
    bool online_{}, transaction_{};

    void reset() noexcept {
        bindings_.clear();
        rows_.clear();
        tombstones_.clear();
        pending_.clear();
        committed_at_ = {};
        life_ = commit_sequence_ = 0;
        online_ = transaction_ = false;
    }
    void bind(std::span<const cg::Plaza2RawTableBinding> tables) {
        if (tables.size() != tables_.size())
            throw std::invalid_argument("auxiliary negotiated table set differs from allowlist");
        for (std::size_t i = 0; i < tables_.size(); ++i) {
            const auto& spec = tables_[i];
            const auto source = std::ranges::find(tables, spec.name, &cg::Plaza2RawTableBinding::name);
            if (source == tables.end())
                throw std::invalid_argument("required auxiliary table missing");
            auto fields = spec.fields;
            fields.insert(fields.end(), {{"replID", "i8"}, {"replRev", "i8"}, {"replAct", "i8"}});
            if (spec.selected_instrument)
                fields.push_back({"isin_id", "i4"});
            BoundTable target{.table = i, .size = source->row_size};
            for (const auto& field : fields) {
                const auto found = std::ranges::find(source->fields, field.name, &cg::Plaza2RawFieldBinding::name);
                if (found == source->fields.end() && field.optional)
                    continue;
                if (found == source->fields.end() || found->type_token != field.type ||
                    found->size != field_size(field.type) || found->offset > target.size ||
                    found->size > target.size - found->offset ||
                    std::ranges::count(source->fields, field.name, &cg::Plaza2RawFieldBinding::name) != 1)
                    throw std::invalid_argument("auxiliary field layout mismatch: " + std::string(field.name));
                target.fields.push_back({field, found->offset, found->size, found->index});
            }
            if (!bindings_.emplace(source->index, std::move(target)).second)
                throw std::invalid_argument("duplicate auxiliary table index");
        }
    }
    static std::string decode(std::span<const std::byte> data, std::string_view type) {
        using plaza2::public_wire::load;
        if (type.starts_with('d'))
            return decimal(data, type);
        if (type.starts_with('c')) {
            const auto bytes = std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
            return std::string(bytes.substr(0, bytes.find('\0')));
        }
        if (type == "i1")
            return std::to_string(load<std::int8_t>(data));
        if (type == "i4")
            return std::to_string(load<std::int32_t>(data));
        if (type == "i8")
            return std::to_string(load<std::int64_t>(data));
        if (type == "t") {
            const auto time = load<plaza2::public_wire::Time>(data);
            using namespace std::chrono;
            const auto date = year{time.year} / month{time.month} / day{time.day};
            if (!date.ok() || time.hour > 23 || time.minute > 59 || time.second > 59 || time.msec > 999)
                return {};
            const auto value =
                sys_days(date) + hours(time.hour) + minutes(time.minute) + seconds(time.second) - hours(3);
            return std::to_string(duration_cast<seconds>(value.time_since_epoch()).count());
        }
        return std::to_string(load<std::uint64_t>(data));
    }
    void row(const Event& event) {
        const auto target = bindings_.find(event.table_index);
        if (target == bindings_.end() || target->second.size != event.raw_payload.size())
            throw std::invalid_argument("auxiliary row layout mismatch");
        const auto& plan = target->second;
        const auto& spec = tables_[plan.table];
        const auto read = [&](const BoundField& f) {
            if (!event.raw_nulls.empty()) {
                if (f.ordinal >= event.raw_nulls.size())
                    throw std::invalid_argument("truncated auxiliary null map");
                if (event.raw_nulls[f.ordinal])
                    return std::string{};
            }
            return decode(event.raw_payload.subspan(f.offset, f.size), f.field.type);
        };
        const auto read_named = [&](std::string_view name) {
            const auto found = std::ranges::find_if(plan.fields, [&](const auto& f) { return f.field.name == name; });
            return found == plan.fields.end() ? std::string{} : read(*found);
        };
        const auto id = std::stoll(read_named("replID"));
        const auto revision = std::stoll(read_named("replRev"));
        const auto action = std::stoll(read_named("replAct"));
        if (id <= 0 || revision <= 0)
            throw std::invalid_argument("invalid auxiliary replication identity");
        const auto key = std::pair{plan.table, static_cast<std::int64_t>(id)};
        if (action || (!spec.account_field.empty() && read_named(spec.account_field) != account_) ||
            (spec.selected_instrument &&
             std::ranges::find(instruments_, std::stoll(read_named("isin_id"))) == instruments_.end())) {
            // A newer revision can move a previously selected row out of scope.
            if (rows_.contains(key) || tombstones_.contains(key) ||
                std::ranges::any_of(pending_, [&](const auto& item) { return item.key == key; }))
                append({key, {revision, {}}, true});
            return;
        }
        Values values;
        for (const auto& field : plan.fields) {
            const auto name = field.field.name;
            if (name == "replID" || name == "replRev" || name == "replAct" || name == spec.account_field)
                continue;
            if (auto value = read(field); !value.empty())
                values.emplace(name, std::move(value));
        }
        append({key, {revision, std::move(values)}, false});
    }
    void append(Change change) {
        if (pending_.size() >= 8192)
            throw std::invalid_argument("auxiliary transaction exceeds row budget");
        pending_.push_back(std::move(change));
    }
    void apply() {
        if (commit_sequence_ == UINT64_MAX)
            throw std::invalid_argument("auxiliary commit sequence exhausted");
        ++commit_sequence_;
        for (auto& change : pending_) {
            const auto deleted = tombstones_.find(change.key);
            if (deleted != tombstones_.end() && deleted->second >= change.row.revision)
                continue;
            const auto prior = rows_.find(change.key);
            if (prior != rows_.end() && prior->second.revision >= change.row.revision)
                continue;
            if (change.deleted) {
                rows_.erase(change.key);
                tombstones_.insert_or_assign(change.key, change.row.revision);
            } else {
                tombstones_.erase(change.key);
                change.row.commit_sequence = commit_sequence_;
                rows_.insert_or_assign(change.key, std::move(change.row));
            }
        }
        if (rows_.size() + tombstones_.size() > 8192)
            throw std::invalid_argument("auxiliary snapshot exceeds row budget");
        pending_.clear();
        committed_at_ = now_();
    }
};

inline std::string_view value(const Projection::Row& row, std::string_view name) {
    const auto it = row.values.find(name);
    return it == row.values.end() ? std::string_view{} : std::string_view(it->second);
}

inline std::int64_t integer_value(std::string_view text) {
    if (text.empty())
        return 0;
    std::int64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? result : 0;
}

// An unrecognized runtime scheme may be newer. Require the environment row
// unless the version explicitly predates its introduction in SPECTRA 9.12.
inline bool environment_row_required(std::string_view release) noexcept {
    constexpr std::string_view prefix = "SPECTRA";
    if (!release.starts_with(prefix))
        return true;
    release.remove_prefix(prefix.size());
    const auto major_end = release.find('.');
    if (major_end == std::string_view::npos)
        return true;
    const auto major_token = release.substr(0, major_end);
    release.remove_prefix(major_end + 1);
    const auto minor_token = release.substr(0, release.find('.'));
    if (minor_token.empty() || minor_token.find_first_not_of("0123456789") != std::string_view::npos)
        return true;
    const auto major = integer_value(major_token);
    const auto minor = integer_value(minor_token);
    return major <= 0 || major > 9 || (major == 9 && minor >= 12);
}

inline bool environment_matches(const Projection::Row& row, cg::Plaza2Environment expected) {
    const auto id = value(row, "environment_id");
    const auto name = value(row, "environment_name");
    switch (expected) {
    case cg::Plaza2Environment::Test:
        return id == "2" && name == "MXFOPREP";
    case cg::Plaza2Environment::Prod:
        return id == "1" && name == "MXFOPROD";
    }
    return false;
}

inline std::string_view login_cod_state(const Projection& projection, std::int64_t utc) {
    std::string_view state = "unknown";
    for (const auto& [key, row] : projection.rows()) {
        const auto start = integer_value(value(row, "start_date"));
        const auto end = integer_value(value(row, "end_date"));
        const auto flags = value(row, "sma_flags");
        if (!start || utc < start || (end && utc > end) || flags.empty())
            continue;
        if (state != "unknown")
            return "unknown";
        state = integer_value(flags) & 1 ? "enabled" : "disabled";
    }
    return state;
}

} // namespace fol_auxiliary
} // namespace moex::plaza2_trade
