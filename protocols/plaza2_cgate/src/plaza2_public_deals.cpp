#include "moex/plaza2/cgate/plaza2_public_deals.hpp"
#include "moex/plaza2/cgate/plaza2_field_read_audit.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace moex::plaza2::cgate {
namespace {

using FieldCode = generated::FieldCode;
using Kind = Plaza2ListenerEventKind;
using TableCode = generated::TableCode;
constexpr auto kDealsStream = generated::StreamCode::kFortsDealsRepl;
constexpr auto kActiveSide = std::uint64_t{0x20000000000};
constexpr auto kPassiveSide = std::uint64_t{0x40000000000};
constexpr auto kMaxD16_5Magnitude = std::int64_t{9'999'999'999'999'999};

const Plaza2DecodedFieldValue* find_field(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code) noexcept {
#ifdef MOEX_CGATE_FIELD_READ_AUDIT
    generated::AuditFieldRead(code);
#endif
    for (const auto& field : fields) {
        if (field.field_code == code) {
            return &field;
        }
    }
    return nullptr;
}

std::optional<std::int64_t> parse_i64(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint64_t> parse_u64(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> signed_value(const Plaza2DecodedFieldValue* field) noexcept {
    if (field == nullptr) {
        return std::nullopt;
    }
    switch (field->kind) {
    case Plaza2DecodedValueKind::SignedInteger:
        return field->signed_value;
    case Plaza2DecodedValueKind::UnsignedInteger:
        if (field->unsigned_value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(field->unsigned_value);
        }
        return std::nullopt;
    case Plaza2DecodedValueKind::String:
    case Plaza2DecodedValueKind::Decimal:
        return parse_i64(field->text_value);
    default:
        return std::nullopt;
    }
}

std::optional<std::uint64_t> unsigned_value(const Plaza2DecodedFieldValue* field) noexcept {
    if (field == nullptr) {
        return std::nullopt;
    }
    switch (field->kind) {
    case Plaza2DecodedValueKind::UnsignedInteger:
    case Plaza2DecodedValueKind::Timestamp:
        return field->unsigned_value;
    case Plaza2DecodedValueKind::SignedInteger:
        if (field->signed_value >= 0) {
            return static_cast<std::uint64_t>(field->signed_value);
        }
        return std::nullopt;
    case Plaza2DecodedValueKind::String:
        return parse_u64(field->text_value);
    default:
        return std::nullopt;
    }
}

std::optional<std::uint64_t> optional_status(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code,
                                             bool& malformed) noexcept {
    const auto* field = find_field(fields, code);
    if (field == nullptr) {
        return std::uint64_t{0};
    }
    auto value = unsigned_value(field);
    malformed = !value.has_value();
    return value;
}

std::string format_d16_5(std::int64_t scaled) {
    const bool negative = scaled < 0;
    const auto magnitude =
        negative ? static_cast<std::uint64_t>(-(scaled + 1)) + 1U : static_cast<std::uint64_t>(scaled);
    const auto whole = magnitude / static_cast<std::uint64_t>(kPlaza2D16_5PriceScale);
    auto fractional = magnitude % static_cast<std::uint64_t>(kPlaza2D16_5PriceScale);
    std::string result = negative ? "-" : "";
    result += std::to_string(whole);
    result.push_back('.');
    std::string digits = std::to_string(fractional);
    result.append(5U - digits.size(), '0');
    result += digits;
    while (result.back() == '0') {
        result.pop_back();
    }
    if (result.back() == '.') {
        result.push_back('0');
    }
    return result;
}

std::optional<std::int64_t> decoded_price(const Plaza2DecodedFieldValue* field) noexcept {
    if (field == nullptr ||
        (field->kind != Plaza2DecodedValueKind::Decimal && field->kind != Plaza2DecodedValueKind::String)) {
        return std::nullopt;
    }
    if (!field->type_token.empty() && field->type_token != "d16.5") {
        return std::nullopt;
    }
    if (field->decimal_exact) {
        if (field->decimal_scale != static_cast<std::int32_t>(kPlaza2D16_5FractionalDigits)) {
            return std::nullopt;
        }
        if (field->decimal_mantissa < -kMaxD16_5Magnitude || field->decimal_mantissa > kMaxD16_5Magnitude) {
            return std::nullopt;
        }
        if (!field->text_value.empty()) {
            const auto text_scaled =
                parse_fixed_point(field->text_value, kPlaza2D16_5FractionalDigits, true, kPlaza2D16_5DecimalPrecision);
            if (!text_scaled || *text_scaled != field->decimal_mantissa) {
                return std::nullopt;
            }
        }
        return field->decimal_mantissa;
    }
    // A runtime-owned d16.5 BCD that failed exact decoding is malformed even if
    // a generic string conversion happened to produce printable text.
    if (!field->raw_value.empty()) {
        return std::nullopt;
    }
    return parse_fixed_point(field->text_value, kPlaza2D16_5FractionalDigits, true, kPlaza2D16_5DecimalPrecision);
}

std::uint64_t receipt_unix_ns() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return count > 0 ? static_cast<std::uint64_t>(count) : 0;
}

bool known_deals_table(TableCode table) noexcept {
    return table == TableCode::kFortsDealsReplDeal || table == TableCode::kFortsDealsReplMultilegDeal ||
           table == TableCode::kFortsDealsReplHeartbeat || table == TableCode::kFortsDealsReplSysEvents;
}

// MOEX SPECTRA CGate 9.9 documents ActiveSide and PassiveSide in xstatus:
// https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/prod/docs/p2gate_ru.html
// The active/passive flags describe the aggressor/resting order side. Public
// order IDs are deliberately not used to infer aggressor direction.
std::int32_t aggressor_side(std::uint64_t buy, std::uint64_t sell) noexcept {
    const bool buy_active = (buy & kActiveSide) != 0;
    const bool buy_passive = (buy & kPassiveSide) != 0;
    const bool sell_active = (sell & kActiveSide) != 0;
    const bool sell_passive = (sell & kPassiveSide) != 0;
    if (buy_active && !buy_passive && sell_passive && !sell_active) {
        return 2;
    }
    if (buy_passive && !buy_active && sell_active && !sell_passive) {
        return 1;
    }
    return 0;
}

} // namespace

Plaza2PublicDealsBridge::Plaza2PublicDealsBridge(std::int64_t isin_id, std::int32_t sess_id, std::size_t max_trades)
    : target_isin_id_(isin_id), target_session_id_(sess_id),
      capacity_(isin_id > 0 && sess_id > 0 ? (max_trades == 0 ? 1 : max_trades) : 0), ring_(capacity_),
      dedup_history_(capacity_) {
    if (capacity_ == 0) {
        return;
    }
    pending_.reserve(capacity_);
    pending_by_repl_id_.reserve(capacity_);
    pending_by_deal_id_.reserve(capacity_);
    seen_by_repl_id_.reserve(capacity_);
    seen_by_deal_id_.reserve(capacity_);
}

bool Plaza2PublicDealsBridge::same_identity(const Identity& a, const Identity& b) noexcept {
    return a.repl_id == b.repl_id && a.repl_rev == b.repl_rev && a.repl_act == b.repl_act && a.deal_id == b.deal_id &&
           a.isin_id == b.isin_id && a.session_id == b.session_id && a.price_scaled == b.price_scaled &&
           a.quantity == b.quantity && a.moment == b.moment && a.moment_ns == b.moment_ns &&
           a.xstatus_buy == b.xstatus_buy && a.xstatus_sell == b.xstatus_sell;
}

bool Plaza2PublicDealsBridge::same_economic_trade(const Identity& a, const Identity& b) noexcept {
    return a.deal_id == b.deal_id && a.isin_id == b.isin_id && a.session_id == b.session_id &&
           a.price_scaled == b.price_scaled && a.quantity == b.quantity && a.moment == b.moment &&
           a.moment_ns == b.moment_ns && a.xstatus_buy == b.xstatus_buy && a.xstatus_sell == b.xstatus_sell;
}

Plaza2Error Plaza2PublicDealsBridge::fail(Plaza2ErrorCode code, const char* message) {
    online_ = false;
    valid_ = false;
    transaction_open_ = false;
    pending_.clear();
    pending_by_repl_id_.clear();
    pending_by_deal_id_.clear();
    error_ = message;
    return {.code = code, .message = error_};
}

void Plaza2PublicDealsBridge::clear_generation() noexcept {
    ring_begin_ = 0;
    ring_size_ = 0;
    dedup_begin_ = 0;
    dedup_size_ = 0;
    seen_by_repl_id_.clear();
    seen_by_deal_id_.clear();
    pending_.clear();
    pending_by_repl_id_.clear();
    pending_by_deal_id_.clear();
    transaction_open_ = false;
    online_ = false;
    last_repl_rev_ = -1;
}

void Plaza2PublicDealsBridge::reset() noexcept {
    clear_generation();
    lifenum_ = 0;
    has_lifenum_ = false;
    opened_ = false;
    valid_ = false;
    error_.clear();
}

void Plaza2PublicDealsBridge::on_plaza2_listener_error(const Plaza2Error& error) noexcept {
    clear_generation();
    valid_ = false;
    try {
        error_ = error.message.empty() ? "public DEALS listener callback failed" : error.message;
    } catch (...) {
        error_.clear();
    }
}

Plaza2Error Plaza2PublicDealsBridge::begin_transaction() {
    if (transaction_open_) {
        return fail(Plaza2ErrorCode::AdapterState, "public DEALS received nested TN_BEGIN");
    }
    transaction_open_ = true;
    pending_.clear();
    pending_by_repl_id_.clear();
    pending_by_deal_id_.clear();
    return {};
}

bool Plaza2PublicDealsBridge::push_committed(Plaza2PublicDeal&& deal) noexcept {
    if (capacity_ == 0 || ring_.size() != capacity_) {
        return false;
    }
    if (ring_size_ < capacity_) {
        const auto index = (ring_begin_ + ring_size_) % capacity_;
        ring_[index] = std::move(deal);
        ++ring_size_;
    } else {
        ring_[ring_begin_] = std::move(deal);
        ring_begin_ = (ring_begin_ + 1) % capacity_;
    }
    return true;
}

void Plaza2PublicDealsBridge::remember(const Identity& identity) {
    if (dedup_size_ == capacity_) {
        const auto& oldest = dedup_history_[dedup_begin_];
        const auto existing = seen_by_repl_id_.find(oldest.repl_id);
        const bool indexed_as_trade = existing != seen_by_repl_id_.end() && existing->second.repl_act == 0;
        seen_by_repl_id_.erase(oldest.repl_id);
        if (indexed_as_trade) {
            const auto by_deal = seen_by_deal_id_.find(oldest.deal_id);
            if (by_deal != seen_by_deal_id_.end()) {
                auto& aliases = by_deal->second;
                aliases.erase(std::remove(aliases.begin(), aliases.end(), oldest.repl_id), aliases.end());
                if (aliases.empty()) {
                    seen_by_deal_id_.erase(by_deal);
                }
            }
        }
        dedup_begin_ = (dedup_begin_ + 1) % capacity_;
        --dedup_size_;
    }

    seen_by_repl_id_[identity.repl_id] = identity;
    if (identity.repl_act == 0) {
        auto& aliases = seen_by_deal_id_[identity.deal_id];
        if (std::find(aliases.begin(), aliases.end(), identity.repl_id) == aliases.end()) {
            aliases.push_back(identity.repl_id);
        }
    }
    const auto index = (dedup_begin_ + dedup_size_) % capacity_;
    dedup_history_[index] = {.repl_id = identity.repl_id, .deal_id = identity.deal_id};
    ++dedup_size_;
}

Plaza2Error Plaza2PublicDealsBridge::commit_transaction() {
    if (!transaction_open_) {
        return fail(Plaza2ErrorCode::AdapterState, "public DEALS received TN_COMMIT without TN_BEGIN");
    }

    std::vector<PendingDeal> committed;
    committed.reserve(pending_.size());
    std::unordered_map<std::uint64_t, Identity> planned_by_repl;
    std::unordered_map<std::int64_t, Identity> planned_by_deal;
    planned_by_repl.reserve(pending_.size());
    planned_by_deal.reserve(pending_.size());
    auto planned_revision = last_repl_rev_;
    std::size_t emitted_count = 0;

    for (const auto& item : pending_) {
        const auto& identity = item.identity;
        if (const auto existing = seen_by_repl_id_.find(identity.repl_id); existing != seen_by_repl_id_.end()) {
            if (!same_identity(existing->second, identity)) {
                return fail(Plaza2ErrorCode::DecodeFailed, "public DEALS repl_id was reused with conflicting identity");
            }
            continue;
        }
        if (const auto local = planned_by_repl.find(identity.repl_id); local != planned_by_repl.end()) {
            if (!same_identity(local->second, identity)) {
                return fail(Plaza2ErrorCode::DecodeFailed,
                            "public DEALS transaction reuses repl_id with conflicting identity");
            }
            continue;
        }

        bool duplicate_trade = false;
        if (const auto aliases = seen_by_deal_id_.find(identity.deal_id); aliases != seen_by_deal_id_.end()) {
            for (const auto alias : aliases->second) {
                const auto previous = seen_by_repl_id_.find(alias);
                if (previous != seen_by_repl_id_.end() && !same_economic_trade(previous->second, identity)) {
                    return fail(Plaza2ErrorCode::DecodeFailed,
                                "public DEALS deal_id was reused with conflicting economic fields");
                }
                duplicate_trade = true;
            }
        }
        if (const auto local = planned_by_deal.find(identity.deal_id); local != planned_by_deal.end()) {
            if (!same_economic_trade(local->second, identity)) {
                return fail(Plaza2ErrorCode::DecodeFailed,
                            "public DEALS transaction reuses deal_id with conflicting economic fields");
            }
            duplicate_trade = true;
        }

        if (identity.repl_rev < planned_revision) {
            continue; // Historical replay; revisions are ordered, not required to be consecutive.
        }
        planned_revision = std::max(planned_revision, identity.repl_rev);
        if (!planned_by_deal.contains(identity.deal_id)) {
            planned_by_deal.emplace(identity.deal_id, identity);
        }

        PendingDeal accepted = item;
        accepted.emit_candidate = accepted.emit_candidate && !duplicate_trade;
        emitted_count += accepted.emit_candidate ? 1U : 0U;
        planned_by_repl.emplace(identity.repl_id, identity);
        committed.push_back(std::move(accepted));
    }

    if (emitted_count > std::numeric_limits<std::uint64_t>::max() - last_sequence_) {
        return fail(Plaza2ErrorCode::AdapterState, "public DEALS sequence exhausted");
    }

    transaction_open_ = false;
    for (auto& item : committed) {
        remember(item.identity);
        last_repl_rev_ = std::max(last_repl_rev_, item.identity.repl_rev);
        if (!item.emit_candidate) {
            continue;
        }
        item.deal.sequence = ++last_sequence_;
        item.deal.stream_epoch = stream_epoch_;
        item.deal.lifenum = lifenum_;
        if (!push_committed(std::move(item.deal))) {
            return fail(Plaza2ErrorCode::AdapterState, "public DEALS ring capacity invariant failed");
        }
    }
    pending_.clear();
    pending_by_repl_id_.clear();
    pending_by_deal_id_.clear();
    return {};
}

Plaza2Error Plaza2PublicDealsBridge::stage_deal(const Plaza2ListenerEvent& event) {
    const auto& fields = event.fields;
    const auto sess = signed_value(find_field(fields, FieldCode::kFortsDealsReplDealSessId));
    const auto isin = signed_value(find_field(fields, FieldCode::kFortsDealsReplDealIsinId));
    const auto nosystem = signed_value(find_field(fields, FieldCode::kFortsDealsReplDealNosystem));
    if (!sess || !isin || !nosystem) {
        return fail(Plaza2ErrorCode::DecodeFailed, "public DEALS row is missing session, ISIN, or nosystem");
    }
    if (*sess != target_session_id_ || *isin != target_isin_id_ || *nosystem != 0) {
        return {};
    }
    if (!has_lifenum_ || lifenum_ == 0) {
        return fail(Plaza2ErrorCode::AdapterState, "public DEALS target row arrived before a nonzero LifeNum");
    }

    const auto repl_id = signed_value(find_field(fields, FieldCode::kFortsDealsReplDealReplId));
    const auto repl_rev = signed_value(find_field(fields, FieldCode::kFortsDealsReplDealReplRev));
    const auto repl_act = signed_value(find_field(fields, FieldCode::kFortsDealsReplDealReplAct));
    const auto deal_id = signed_value(find_field(fields, FieldCode::kFortsDealsReplDealIdDeal));
    const auto quantity = signed_value(find_field(fields, FieldCode::kFortsDealsReplDealXamount));
    const auto moment = unsigned_value(find_field(fields, FieldCode::kFortsDealsReplDealMoment));
    const auto moment_ns = unsigned_value(find_field(fields, FieldCode::kFortsDealsReplDealMomentNs));
    const auto price_field = find_field(fields, FieldCode::kFortsDealsReplDealPrice);
    const auto price_scaled = decoded_price(price_field);
    if (!repl_id || *repl_id <= 0 || !repl_rev || *repl_rev <= 0 || !repl_act || *repl_act < 0 || !deal_id ||
        *deal_id <= 0 || !quantity || *quantity <= 0 || !moment || !moment_ns || *moment_ns == 0 ||
        *moment_ns > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) || !price_scaled ||
        *price_scaled <= 0) {
        return fail(Plaza2ErrorCode::DecodeFailed,
                    "public DEALS target row has invalid identity, quantity, timestamp, or d16.5 price");
    }

    bool malformed_buy = false;
    bool malformed_sell = false;
    const auto xstatus_buy = optional_status(fields, FieldCode::kFortsDealsReplDealXstatusBuy, malformed_buy);
    const auto xstatus_sell = optional_status(fields, FieldCode::kFortsDealsReplDealXstatusSell, malformed_sell);
    if (malformed_buy || malformed_sell || !xstatus_buy || !xstatus_sell) {
        return fail(Plaza2ErrorCode::DecodeFailed, "public DEALS xstatus flags are malformed");
    }

    const auto session_id = static_cast<std::int32_t>(*sess);
    if (static_cast<std::int64_t>(session_id) != *sess) {
        return fail(Plaza2ErrorCode::DecodeFailed, "public DEALS session ID is outside int32 range");
    }
    Identity identity{.repl_id = static_cast<std::uint64_t>(*repl_id),
                      .repl_rev = *repl_rev,
                      .repl_act = *repl_act,
                      .deal_id = *deal_id,
                      .isin_id = *isin,
                      .session_id = session_id,
                      .price_scaled = *price_scaled,
                      .quantity = *quantity,
                      .moment = *moment,
                      .moment_ns = *moment_ns,
                      .xstatus_buy = *xstatus_buy,
                      .xstatus_sell = *xstatus_sell};

    if (const auto existing = seen_by_repl_id_.find(identity.repl_id); existing != seen_by_repl_id_.end()) {
        if (!same_identity(existing->second, identity)) {
            return fail(Plaza2ErrorCode::DecodeFailed, "public DEALS repl_id was replayed with conflicting identity");
        }
        return {};
    }
    if (const auto staged = pending_by_repl_id_.find(identity.repl_id); staged != pending_by_repl_id_.end()) {
        if (!same_identity(pending_[staged->second].identity, identity)) {
            return fail(Plaza2ErrorCode::DecodeFailed,
                        "public DEALS transaction reuses repl_id with conflicting identity");
        }
        return {};
    }
    if (pending_.size() >= capacity_) {
        return fail(Plaza2ErrorCode::AdapterState, "public DEALS pending transaction exceeded configured bound");
    }

    bool duplicate_trade = false;
    if (const auto aliases = seen_by_deal_id_.find(identity.deal_id); aliases != seen_by_deal_id_.end()) {
        for (const auto alias : aliases->second) {
            const auto previous = seen_by_repl_id_.find(alias);
            if (previous != seen_by_repl_id_.end() && !same_economic_trade(previous->second, identity)) {
                return fail(Plaza2ErrorCode::DecodeFailed,
                            "public DEALS deal_id was replayed with conflicting economic fields");
            }
            duplicate_trade = true;
        }
    }
    if (const auto staged = pending_by_deal_id_.find(identity.deal_id); staged != pending_by_deal_id_.end()) {
        if (!same_economic_trade(staged->second, identity)) {
            return fail(Plaza2ErrorCode::DecodeFailed,
                        "public DEALS transaction reuses deal_id with conflicting economic fields");
        }
        duplicate_trade = true;
    } else {
        pending_by_deal_id_.emplace(identity.deal_id, identity);
    }

    auto price = price_field->text_value.empty() ? format_d16_5(*price_scaled) : std::string(price_field->text_value);
    const auto at_bid_or_ask = aggressor_side(*xstatus_buy, *xstatus_sell);
    const auto received_at_unix_ns = receipt_unix_ns();
    if (received_at_unix_ns == 0 ||
        received_at_unix_ns > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return fail(Plaza2ErrorCode::AdapterState,
                    "public DEALS local receive clock is outside signed nanosecond range");
    }
    Plaza2PublicDeal deal{.stream_epoch = stream_epoch_,
                          .lifenum = lifenum_,
                          .repl_id = identity.repl_id,
                          .repl_rev = identity.repl_rev,
                          .deal_id = identity.deal_id,
                          .isin_id = identity.isin_id,
                          .session_id = identity.session_id,
                          .price_scaled = identity.price_scaled,
                          .quantity = identity.quantity,
                          .price = std::move(price),
                          .moment = identity.moment,
                          .moment_ns = identity.moment_ns,
                          .received_at_unix_ns = received_at_unix_ns,
                          .xstatus_buy = identity.xstatus_buy,
                          .xstatus_sell = identity.xstatus_sell,
                          .at_bid_or_ask = at_bid_or_ask};
    const auto index = pending_.size();
    pending_by_repl_id_.emplace(identity.repl_id, index);
    pending_.push_back({.identity = identity,
                        .deal = std::move(deal),
                        .emit_candidate = online_ && *repl_act == 0 && !duplicate_trade});
    return {};
}

Plaza2Error Plaza2PublicDealsBridge::on_plaza2_listener_event(const Plaza2ListenerEvent& event) {
    return on_message(event);
}

Plaza2Error Plaza2PublicDealsBridge::on_message(const Plaza2ListenerEvent& event) {
    if (event.stream_code != kDealsStream) {
        return fail(Plaza2ErrorCode::DecodeFailed, "public DEALS bridge received an unexpected stream");
    }
    if (!valid_ && event.kind != Kind::Open && event.kind != Kind::LifeNum && event.kind != Kind::Close) {
        return {.code = Plaza2ErrorCode::AdapterState,
                .message = error_.empty() ? "public DEALS requires a fresh listener generation" : error_};
    }

    switch (event.kind) {
    case Kind::Open:
        if (target_isin_id_ <= 0 || target_session_id_ <= 0) {
            return fail(Plaza2ErrorCode::InvalidConfiguration, "public DEALS target IDs must be positive");
        }
        if (stream_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
            return fail(Plaza2ErrorCode::AdapterState, "public DEALS stream epoch exhausted");
        }
        clear_generation();
        ++stream_epoch_;
        lifenum_ = 0;
        has_lifenum_ = false;
        opened_ = true;
        valid_ = true;
        error_.clear();
        return {};
    case Kind::LifeNum: {
        if (!opened_) {
            return fail(Plaza2ErrorCode::AdapterState, "public DEALS LifeNum arrived before listener Open");
        }
        if (event.unsigned_value == 0) {
            return fail(Plaza2ErrorCode::AdapterState, "public DEALS requires a nonzero LifeNum");
        }
        if (transaction_open_) {
            return fail(Plaza2ErrorCode::AdapterState, "public DEALS LifeNum changed inside a transaction");
        }
        if (has_lifenum_ && lifenum_ == event.unsigned_value) {
            return {};
        }
        const bool changed_generation = has_lifenum_ || online_ || ring_size_ != 0;
        if (changed_generation && stream_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
            return fail(Plaza2ErrorCode::AdapterState, "public DEALS stream epoch exhausted");
        }
        clear_generation();
        if (changed_generation) {
            ++stream_epoch_;
        }
        lifenum_ = event.unsigned_value;
        has_lifenum_ = true;
        valid_ = true;
        error_.clear();
        return {};
    }
    case Kind::Close:
        clear_generation();
        opened_ = false;
        valid_ = false;
        error_ = "public DEALS listener closed";
        return {};
    case Kind::TransactionBegin:
        if (!opened_) {
            return fail(Plaza2ErrorCode::AdapterState, "public DEALS TN_BEGIN arrived before listener Open");
        }
        return begin_transaction();
    case Kind::TransactionCommit:
        return commit_transaction();
    case Kind::StreamData:
        if (!transaction_open_) {
            return fail(Plaza2ErrorCode::AdapterState, "public DEALS row arrived outside TN_BEGIN/TN_COMMIT");
        }
        if (!known_deals_table(event.table_code)) {
            return fail(Plaza2ErrorCode::DecodeFailed, "public DEALS received an unknown table code");
        }
        if (event.table_code != TableCode::kFortsDealsReplDeal) {
            return {};
        }
        return stage_deal(event);
    case Kind::Online:
        if (!opened_ || transaction_open_ || !has_lifenum_ || lifenum_ == 0) {
            return fail(Plaza2ErrorCode::AdapterState,
                        "public DEALS ONLINE requires Open, a committed transaction boundary, and nonzero LifeNum");
        }
        online_ = true;
        valid_ = true;
        error_.clear();
        return {};
    case Kind::ClearDeleted:
        if (!known_deals_table(event.table_code)) {
            return fail(Plaza2ErrorCode::DecodeFailed, "public DEALS clear_deleted referenced an unknown table");
        }
        // clear_deleted is a table-state control marker, not a public trade or retraction.
        return {};
    default:
        return {};
    }
}

Plaza2PublicDealsSnapshot Plaza2PublicDealsBridge::snapshot(std::uint64_t after_sequence) const {
    Plaza2PublicDealsSnapshot result{.online = online_,
                                     .valid = valid_,
                                     .stream_epoch = stream_epoch_,
                                     .lifenum = lifenum_,
                                     .last_sequence = last_sequence_,
                                     .first_sequence = 0,
                                     .error = error_};
    if (ring_size_ == 0) {
        return result;
    }
    result.first_sequence = ring_[ring_begin_].sequence;
    if (after_sequence >= last_sequence_) {
        return result;
    }

    // Logical ring order is chronological even when the physical storage wraps.
    // Binary search the first sequence greater than the caller's cursor.
    std::size_t low = 0;
    std::size_t high = ring_size_;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        const auto& deal = ring_[(ring_begin_ + middle) % capacity_];
        if (deal.sequence <= after_sequence) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    result.trades.reserve(ring_size_ - low);
    for (std::size_t offset = low; offset < ring_size_; ++offset) {
        result.trades.push_back(ring_[(ring_begin_ + offset) % capacity_]);
    }
    return result;
}

} // namespace moex::plaza2::cgate
