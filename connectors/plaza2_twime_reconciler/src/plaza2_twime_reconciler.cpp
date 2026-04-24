#include "moex/plaza2_twime_reconciler/plaza2_twime_reconciler.hpp"

#include "moex/twime_sbe/twime_codec.hpp"
#include "moex/twime_sbe/twime_schema.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace moex::plaza2_twime_reconciler {

namespace {

using moex::plaza2::private_state::OwnOrderSnapshot;
using moex::plaza2::private_state::OwnTradeSnapshot;
using moex::plaza2::private_state::Plaza2PrivateStateProjector;
using moex::plaza2::private_state::ResumeMarkersSnapshot;
using moex::plaza2::private_state::StreamHealthSnapshot;
using moex::twime_sbe::DecodedTwimeField;
using moex::twime_sbe::DecodedTwimeMessage;
using moex::twime_sbe::TwimeCodec;
using moex::twime_sbe::TwimeDecodeError;
using moex::twime_sbe::TwimeEncodeRequest;
using moex::twime_sbe::TwimeFieldValue;
using moex::twime_trade::TwimeJournalEntry;
using moex::twime_trade::TwimeSessionHealthSnapshot;
using moex::twime_trade::TwimeSessionMetrics;

constexpr std::uint16_t kTemplateNewOrderSingle = 6000;
constexpr std::uint16_t kTemplateNewOrderIceberg = 6008;
constexpr std::uint16_t kTemplateNewOrderIcebergX = 6011;
constexpr std::uint16_t kTemplateOrderCancelRequest = 6006;
constexpr std::uint16_t kTemplateOrderIcebergCancelRequest = 6009;
constexpr std::uint16_t kTemplateOrderReplaceRequest = 6007;
constexpr std::uint16_t kTemplateOrderIcebergReplaceRequest = 6010;
constexpr std::uint16_t kTemplateNewOrderSingleResponse = 7015;
constexpr std::uint16_t kTemplateNewOrderIcebergResponse = 7016;
constexpr std::uint16_t kTemplateOrderCancelResponse = 7017;
constexpr std::uint16_t kTemplateOrderReplaceResponse = 7018;
constexpr std::uint16_t kTemplateExecutionSingleReport = 7019;
constexpr std::uint16_t kTemplateExecutionMultilegReport = 7020;
constexpr std::uint16_t kTemplateBusinessMessageReject = 5009;

constexpr std::array<moex::plaza2::generated::StreamCode, 5> kRequiredPrivateStreams = {
    moex::plaza2::generated::StreamCode::kFortsTradeRepl,
    moex::plaza2::generated::StreamCode::kFortsUserorderbookRepl,
    moex::plaza2::generated::StreamCode::kFortsPosRepl,
    moex::plaza2::generated::StreamCode::kFortsPartRepl,
    moex::plaza2::generated::StreamCode::kFortsRefdataRepl,
};

const DecodedTwimeField* find_field(const DecodedTwimeMessage& message, std::string_view name) {
    for (const auto& field : message.fields) {
        if (field.metadata != nullptr && field.metadata->name == name) {
            return &field;
        }
    }
    return nullptr;
}

std::optional<std::uint64_t> field_unsigned(const DecodedTwimeMessage& message, std::string_view name) {
    const auto* field = find_field(message, name);
    if (field == nullptr) {
        return std::nullopt;
    }

    if (field->metadata->nullable && field->metadata->has_null_value &&
        field->value.unsigned_value == field->metadata->null_value) {
        return std::nullopt;
    }
    switch (field->value.kind) {
    case moex::twime_sbe::TwimeValueKind::Signed:
        return field->value.signed_value < 0
                   ? std::nullopt
                   : std::optional<std::uint64_t>(static_cast<std::uint64_t>(field->value.signed_value));
    case moex::twime_sbe::TwimeValueKind::Unsigned:
    case moex::twime_sbe::TwimeValueKind::TimeStamp:
    case moex::twime_sbe::TwimeValueKind::DeltaMillisecs:
        return field->value.unsigned_value;
    default:
        return std::nullopt;
    }
}

std::optional<std::int64_t> field_signed(const DecodedTwimeMessage& message, std::string_view name) {
    const auto* field = find_field(message, name);
    if (field == nullptr) {
        return std::nullopt;
    }

    if (field->metadata->nullable && field->metadata->has_null_value &&
        field->value.unsigned_value == field->metadata->null_value) {
        return std::nullopt;
    }
    switch (field->value.kind) {
    case moex::twime_sbe::TwimeValueKind::Signed:
        return field->value.signed_value;
    case moex::twime_sbe::TwimeValueKind::Unsigned:
    case moex::twime_sbe::TwimeValueKind::TimeStamp:
    case moex::twime_sbe::TwimeValueKind::DeltaMillisecs:
        return static_cast<std::int64_t>(field->value.unsigned_value);
    default:
        return std::nullopt;
    }
}

std::optional<std::string> field_string(const DecodedTwimeMessage& message, std::string_view name) {
    const auto* field = find_field(message, name);
    if (field == nullptr) {
        return std::nullopt;
    }
    const auto text = field->value.string_view();
    if (text.empty()) {
        return std::nullopt;
    }
    return std::string(text);
}

std::optional<std::int64_t> field_decimal5(const DecodedTwimeMessage& message, std::string_view name) {
    const auto* field = find_field(message, name);
    if (field == nullptr) {
        return std::nullopt;
    }
    if (field->value.kind != moex::twime_sbe::TwimeValueKind::Decimal5) {
        return std::nullopt;
    }
    return field->value.decimal5.mantissa;
}

Side parse_twime_side(std::string_view side_text) {
    if (side_text.empty()) {
        return Side::Unknown;
    }
    if (side_text == "Buy") {
        return Side::Buy;
    }
    if (side_text == "Sell") {
        return Side::Sell;
    }
    return Side::Unknown;
}

Side parse_twime_side(const std::optional<std::string>& side_text) {
    return side_text.has_value() ? parse_twime_side(std::string_view(*side_text)) : Side::Unknown;
}

Side parse_plaza_side(std::int8_t dir) {
    if (dir == 1) {
        return Side::Buy;
    }
    if (dir == 2) {
        return Side::Sell;
    }
    return Side::Unknown;
}

std::optional<std::int64_t> parse_plaza_decimal_to_mantissa(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }

    std::size_t position = 0;
    bool negative = false;
    if (value[position] == '-') {
        negative = true;
        ++position;
    }
    if (position == value.size()) {
        return std::nullopt;
    }

    std::int64_t whole = 0;
    bool saw_digit = false;
    while (position < value.size() && value[position] != '.') {
        if (!std::isdigit(static_cast<unsigned char>(value[position]))) {
            return std::nullopt;
        }
        saw_digit = true;
        whole = whole * 10 + static_cast<std::int64_t>(value[position] - '0');
        ++position;
    }
    if (!saw_digit) {
        return std::nullopt;
    }

    std::int64_t fraction = 0;
    std::int32_t fraction_digits = 0;
    if (position < value.size() && value[position] == '.') {
        ++position;
        while (position < value.size()) {
            if (!std::isdigit(static_cast<unsigned char>(value[position])) || fraction_digits >= 5) {
                return std::nullopt;
            }
            fraction = fraction * 10 + static_cast<std::int64_t>(value[position] - '0');
            ++fraction_digits;
            ++position;
        }
    }
    while (fraction_digits < 5) {
        fraction *= 10;
        ++fraction_digits;
    }

    std::int64_t mantissa = whole * 100000 + fraction;
    return negative ? -mantissa : mantissa;
}

bool order_has_terminal_fill(const PlazaOrderView& plaza, const TwimeTradeView* trade) {
    if (trade == nullptr || !trade->present || !trade->has_last_qty || !trade->has_order_qty) {
        return false;
    }
    if (trade->last_qty == 0 || trade->order_qty == 0 || trade->last_qty < trade->order_qty) {
        return false;
    }
    return !plaza.present || (plaza.private_amount_rest == 0 && plaza.public_amount_rest == 0);
}

std::int64_t plaza_order_qty_for_compare(const PlazaOrderView& plaza) {
    if (plaza.private_amount > 0) {
        return plaza.private_amount;
    }
    return plaza.public_amount;
}

bool exact_order_fallback_match(const TwimeOrderView& twime, const PlazaOrderView& plaza) {
    if (!twime.present || !plaza.present) {
        return false;
    }
    if (twime.account.empty() || plaza.client_code.empty()) {
        return false;
    }
    if (twime.account != plaza.client_code) {
        return false;
    }
    if (twime.trading_session_id == 0 || twime.security_id == 0) {
        return false;
    }
    if (twime.trading_session_id != plaza.sess_id || twime.security_id != plaza.isin_id) {
        return false;
    }
    if (twime.side == Side::Unknown || twime.side != plaza.side) {
        return false;
    }
    if (!twime.has_price || !plaza.has_price || twime.price_mantissa != plaza.price_mantissa) {
        return false;
    }
    if (!twime.has_order_qty) {
        return false;
    }
    const auto plaza_qty = plaza_order_qty_for_compare(plaza);
    return plaza_qty > 0 && static_cast<std::uint32_t>(plaza_qty) == twime.order_qty;
}

bool exact_trade_fallback_match(const TwimeTradeView& twime, const PlazaTradeView& plaza) {
    if (!twime.present || !plaza.present) {
        return false;
    }
    if (twime.order_id <= 0) {
        return false;
    }
    if (twime.trading_session_id == 0 || twime.security_id == 0) {
        return false;
    }
    if (twime.trading_session_id != plaza.sess_id || twime.security_id != plaza.isin_id) {
        return false;
    }
    if (twime.side == Side::Unknown) {
        return false;
    }
    const auto plaza_order_id = twime.side == Side::Buy ? plaza.private_order_id_buy : plaza.private_order_id_sell;
    if (plaza_order_id <= 0 || plaza_order_id != twime.order_id) {
        return false;
    }
    if (!twime.has_price || !plaza.has_price || twime.price_mantissa != plaza.price_mantissa) {
        return false;
    }
    if (!twime.has_last_qty || twime.last_qty == 0) {
        return false;
    }
    return plaza.amount == static_cast<std::int64_t>(twime.last_qty);
}

bool orders_materially_diverge(const TwimeOrderView& twime, const PlazaOrderView& plaza, std::string& reason) {
    if (!twime.present || !plaza.present) {
        return false;
    }
    if (twime.trading_session_id != 0 && plaza.sess_id != 0 && twime.trading_session_id != plaza.sess_id) {
        reason = "session_mismatch";
        return true;
    }
    if (twime.security_id != 0 && plaza.isin_id != 0 && twime.security_id != plaza.isin_id) {
        reason = "instrument_mismatch";
        return true;
    }
    if (twime.side != Side::Unknown && plaza.side != Side::Unknown && twime.side != plaza.side) {
        reason = "side_mismatch";
        return true;
    }
    if (twime.has_price && plaza.has_price && twime.price_mantissa != plaza.price_mantissa) {
        reason = "price_mismatch";
        return true;
    }
    if (twime.has_order_qty) {
        const auto plaza_qty = plaza_order_qty_for_compare(plaza);
        if (plaza_qty > 0 && static_cast<std::uint32_t>(plaza_qty) != twime.order_qty) {
            reason = "quantity_mismatch";
            return true;
        }
    }
    if (twime.terminal_reject) {
        reason = "twime_reject_vs_plaza_presence";
        return true;
    }
    return false;
}

bool trades_materially_diverge(const TwimeTradeView& twime, const PlazaTradeView& plaza, std::string& reason) {
    if (!twime.present || !plaza.present) {
        return false;
    }
    if (twime.trading_session_id != 0 && plaza.sess_id != 0 && twime.trading_session_id != plaza.sess_id) {
        reason = "session_mismatch";
        return true;
    }
    if (twime.security_id != 0 && plaza.isin_id != 0 && twime.security_id != plaza.isin_id) {
        reason = "instrument_mismatch";
        return true;
    }
    if (twime.has_price && plaza.has_price && twime.price_mantissa != plaza.price_mantissa) {
        reason = "price_mismatch";
        return true;
    }
    if (twime.has_last_qty && plaza.amount != 0 && static_cast<std::int64_t>(twime.last_qty) != plaza.amount) {
        reason = "quantity_mismatch";
        return true;
    }
    if (twime.side != Side::Unknown) {
        const auto plaza_order_id = twime.side == Side::Buy ? plaza.private_order_id_buy : plaza.private_order_id_sell;
        if (twime.order_id > 0 && plaza_order_id > 0 && twime.order_id != plaza_order_id) {
            reason = "order_id_mismatch";
            return true;
        }
    }
    return false;
}

PlazaOrderView make_plaza_order_view(const PlazaOrderInput& input, std::uint64_t logical_sequence) {
    return {
        .present = true,
        .multileg = input.multileg,
        .public_order_id = input.public_order_id,
        .private_order_id = input.private_order_id,
        .sess_id = input.sess_id,
        .isin_id = input.isin_id,
        .client_code = input.client_code,
        .login_from = input.login_from,
        .comment = input.comment,
        .price_text = input.price_text,
        .has_price = input.has_price,
        .price_mantissa = input.price_mantissa,
        .public_amount = input.public_amount,
        .public_amount_rest = input.public_amount_rest,
        .private_amount = input.private_amount,
        .private_amount_rest = input.private_amount_rest,
        .id_deal = input.id_deal,
        .xstatus = input.xstatus,
        .xstatus2 = input.xstatus2,
        .side = input.side,
        .public_action = input.public_action,
        .private_action = input.private_action,
        .moment = input.moment,
        .moment_ns = input.moment_ns,
        .ext_id = input.ext_id,
        .from_trade_repl = input.from_trade_repl,
        .from_user_book = input.from_user_book,
        .from_current_day = input.from_current_day,
        .last_logical_sequence = logical_sequence,
    };
}

PlazaTradeView make_plaza_trade_view(const PlazaTradeInput& input, std::uint64_t logical_sequence) {
    return {
        .present = true,
        .multileg = input.multileg,
        .trade_id = input.trade_id,
        .sess_id = input.sess_id,
        .isin_id = input.isin_id,
        .price_text = input.price_text,
        .has_price = input.has_price,
        .price_mantissa = input.price_mantissa,
        .amount = input.amount,
        .public_order_id_buy = input.public_order_id_buy,
        .public_order_id_sell = input.public_order_id_sell,
        .private_order_id_buy = input.private_order_id_buy,
        .private_order_id_sell = input.private_order_id_sell,
        .code_buy = input.code_buy,
        .code_sell = input.code_sell,
        .comment_buy = input.comment_buy,
        .comment_sell = input.comment_sell,
        .login_buy = input.login_buy,
        .login_sell = input.login_sell,
        .moment = input.moment,
        .moment_ns = input.moment_ns,
        .last_logical_sequence = logical_sequence,
    };
}

struct StoredTwimeOrder {
    TwimeOrderView view;
};

struct StoredTwimeTrade {
    TwimeTradeView view;
};

struct Pairing {
    std::optional<std::size_t> twime_index;
    std::optional<std::size_t> plaza_index;
    MatchMode mode{MatchMode::None};
    bool ambiguous{false};
    std::string fault_reason;
};

} // namespace

struct Plaza2TwimeReconciler::Impl {
    explicit Impl(std::uint64_t stale_after_steps) : stale_after_steps(stale_after_steps) {}

    void reset() {
        current_step = 0;
        stale_after_steps = std::max<std::uint64_t>(stale_after_steps, 1);
        twime_order_entries.clear();
        twime_trade_entries.clear();
        twime_order_by_cl_ord_id.clear();
        twime_order_by_order_id.clear();
        twime_trade_by_trade_id.clear();
        current_plaza_orders.clear();
        current_plaza_trades.clear();
        twime_source.reset();
        plaza_source = {};
        plaza_seen = false;
        reconciled_orders.clear();
        reconciled_trades.clear();
        health = {};
    }

    void set_stale_after(std::uint64_t value) {
        stale_after_steps = std::max<std::uint64_t>(value, 1);
        recompute();
    }

    void advance(std::uint64_t steps) {
        current_step += steps;
        recompute();
    }

    void update_twime_health(const TwimeSourceHealthInput& input) {
        twime_source = input;
        recompute();
    }

    void apply_twime_order(const TwimeOrderInput& input) {
        auto index = find_or_create_twime_order(input);
        auto& state = twime_order_entries[index].view;
        state.present = true;
        state.last_kind = input.kind;
        state.last_logical_sequence = input.logical_sequence;
        state.last_logical_step = current_step;
        if (input.cl_ord_id != 0) {
            state.cl_ord_id = input.cl_ord_id;
            twime_order_by_cl_ord_id[input.cl_ord_id] = index;
        }
        if (input.order_id > 0) {
            state.order_id = input.order_id;
            if (input.kind != TwimeOrderInputKind::ReplaceIntent) {
                twime_order_by_order_id[input.order_id] = index;
            }
        }
        if (input.prev_order_id > 0) {
            state.prev_order_id = input.prev_order_id;
        }
        if (input.trading_session_id != 0) {
            state.trading_session_id = input.trading_session_id;
        }
        if (input.security_id != 0) {
            state.security_id = input.security_id;
        }
        if (input.cl_ord_link_id != 0) {
            state.cl_ord_link_id = input.cl_ord_link_id;
        }
        if (!input.account.empty()) {
            state.account = input.account;
        }
        if (!input.compliance_id.empty()) {
            state.compliance_id = input.compliance_id;
        }
        if (input.side != Side::Unknown) {
            state.side = input.side;
        }
        state.multileg = input.multileg;
        if (input.has_price) {
            state.has_price = true;
            state.price_mantissa = input.price_mantissa;
        }
        if (input.has_order_qty) {
            state.has_order_qty = true;
            state.order_qty = input.order_qty;
        }
        if (input.kind == TwimeOrderInputKind::Rejected) {
            state.terminal_reject = true;
            state.reject_code = input.reject_code;
        }
        if (input.kind == TwimeOrderInputKind::CancelAccepted) {
            state.terminal_cancel = true;
        }
        if (input.kind == TwimeOrderInputKind::ReplaceAccepted && input.prev_order_id > 0) {
            const auto prev = twime_order_by_order_id.find(input.prev_order_id);
            if (prev != twime_order_by_order_id.end()) {
                auto& previous = twime_order_entries[prev->second].view;
                if (prev->second != index) {
                    previous.terminal_cancel = true;
                    previous.last_logical_sequence = input.logical_sequence;
                    previous.last_logical_step = current_step;
                }
            }
        }
        recompute();
    }

    void apply_twime_trade(const TwimeTradeInput& input) {
        auto index = find_or_create_twime_trade(input);
        auto& state = twime_trade_entries[index].view;
        state.present = true;
        state.last_kind = input.kind;
        state.last_logical_sequence = input.logical_sequence;
        state.last_logical_step = current_step;
        state.cl_ord_id = input.cl_ord_id;
        state.order_id = input.order_id;
        state.trade_id = input.trade_id;
        state.trading_session_id = input.trading_session_id;
        state.security_id = input.security_id;
        state.side = input.side;
        state.multileg = input.multileg;
        if (input.has_price) {
            state.has_price = true;
            state.price_mantissa = input.price_mantissa;
        }
        if (input.has_last_qty) {
            state.has_last_qty = true;
            state.last_qty = input.last_qty;
        }
        if (input.has_order_qty) {
            state.has_order_qty = true;
            state.order_qty = input.order_qty;
        }
        if (input.trade_id > 0) {
            twime_trade_by_trade_id[input.trade_id] = index;
        }
        recompute();
    }

    void apply_plaza(const PlazaCommittedSnapshotInput& snapshot) {
        plaza_source = snapshot.source_health;
        plaza_seen = true;
        if (snapshot.source_health.invalidated) {
            recompute();
            return;
        }

        current_plaza_orders.clear();
        current_plaza_orders.reserve(snapshot.orders.size());
        for (const auto& order : snapshot.orders) {
            current_plaza_orders.push_back(make_plaza_order_view(order, snapshot.logical_sequence));
        }

        current_plaza_trades.clear();
        current_plaza_trades.reserve(snapshot.trades.size());
        for (const auto& trade : snapshot.trades) {
            current_plaza_trades.push_back(make_plaza_trade_view(trade, snapshot.logical_sequence));
        }
        recompute();
    }

    std::size_t find_or_create_twime_order(const TwimeOrderInput& input) {
        switch (input.kind) {
        case TwimeOrderInputKind::ReplaceIntent:
        case TwimeOrderInputKind::ReplaceAccepted:
            if (input.cl_ord_id != 0) {
                const auto by_cl_ord = twime_order_by_cl_ord_id.find(input.cl_ord_id);
                if (by_cl_ord != twime_order_by_cl_ord_id.end()) {
                    return by_cl_ord->second;
                }
            }
            if (input.kind == TwimeOrderInputKind::ReplaceAccepted && input.order_id > 0) {
                const auto by_order = twime_order_by_order_id.find(input.order_id);
                if (by_order != twime_order_by_order_id.end()) {
                    return by_order->second;
                }
            }
            break;
        case TwimeOrderInputKind::CancelIntent:
        case TwimeOrderInputKind::CancelAccepted:
            if (input.order_id > 0) {
                const auto by_order = twime_order_by_order_id.find(input.order_id);
                if (by_order != twime_order_by_order_id.end()) {
                    return by_order->second;
                }
            }
            if (input.cl_ord_id != 0) {
                const auto by_cl_ord = twime_order_by_cl_ord_id.find(input.cl_ord_id);
                if (by_cl_ord != twime_order_by_cl_ord_id.end()) {
                    return by_cl_ord->second;
                }
            }
            break;
        case TwimeOrderInputKind::NewIntent:
        case TwimeOrderInputKind::NewAccepted:
        case TwimeOrderInputKind::Rejected:
            if (input.order_id > 0) {
                const auto by_order = twime_order_by_order_id.find(input.order_id);
                if (by_order != twime_order_by_order_id.end()) {
                    return by_order->second;
                }
            }
            if (input.cl_ord_id != 0) {
                const auto by_cl_ord = twime_order_by_cl_ord_id.find(input.cl_ord_id);
                if (by_cl_ord != twime_order_by_cl_ord_id.end()) {
                    return by_cl_ord->second;
                }
            }
            break;
        }
        twime_order_entries.push_back({});
        return twime_order_entries.size() - 1;
    }

    std::size_t find_or_create_twime_trade(const TwimeTradeInput& input) {
        if (input.trade_id > 0) {
            const auto by_trade = twime_trade_by_trade_id.find(input.trade_id);
            if (by_trade != twime_trade_by_trade_id.end()) {
                return by_trade->second;
            }
        }
        for (std::size_t index = 0; index < twime_trade_entries.size(); ++index) {
            const auto& existing = twime_trade_entries[index].view;
            if (existing.order_id == input.order_id && existing.trading_session_id == input.trading_session_id &&
                existing.security_id == input.security_id && existing.side == input.side &&
                existing.last_logical_sequence == input.logical_sequence) {
                return index;
            }
        }
        twime_trade_entries.push_back({});
        return twime_trade_entries.size() - 1;
    }

    void recompute() {
        reconciled_orders.clear();
        reconciled_trades.clear();
        health = {};
        health.logical_step = current_step;
        if (twime_source.has_value()) {
            health.twime.present = true;
            health.twime.session_state = twime_source->session_health.state;
            health.twime.transport_open = twime_source->session_health.transport_open;
            health.twime.session_active = twime_source->session_health.session_active;
            health.twime.reject_seen = twime_source->session_health.reject_seen;
            health.twime.last_reject_code = twime_source->session_health.last_reject_code;
            health.twime.next_expected_inbound_seq = twime_source->session_health.next_expected_inbound_seq;
            health.twime.next_outbound_seq = twime_source->session_health.next_outbound_seq;
            health.twime.reconnect_attempts = twime_source->session_metrics.reconnect_attempts;
            health.twime.faults = twime_source->session_health.faults;
            health.twime.remote_closes = twime_source->session_health.remote_closes;
            health.twime.last_transition_time_ms = twime_source->session_health.last_transition_time_ms;
        }

        health.plaza.present = plaza_seen;
        health.plaza.connector_open = plaza_source.connector_health.open;
        health.plaza.connector_online = plaza_source.connector_health.online;
        health.plaza.snapshot_active = plaza_source.connector_health.snapshot_active;
        health.plaza.required_private_streams_ready = plaza_source.required_private_streams_ready;
        health.plaza.invalidated = plaza_source.invalidated;
        health.plaza.last_lifenum = plaza_source.resume_markers.last_lifenum;
        health.plaza.last_replstate = plaza_source.resume_markers.last_replstate;
        health.plaza.last_invalidation_reason = plaza_source.invalidation_reason;
        health.plaza.required_stream_count = kRequiredPrivateStreams.size();
        for (const auto stream_code : kRequiredPrivateStreams) {
            for (const auto& stream : plaza_source.stream_health) {
                if (stream.stream_code == stream_code) {
                    if (stream.online) {
                        ++health.plaza.online_stream_count;
                    }
                    if (stream.snapshot_complete) {
                        ++health.plaza.snapshot_complete_stream_count;
                    }
                    break;
                }
            }
        }

        recompute_orders();
        recompute_trades();
    }

    void recompute_orders() {
        std::vector<Pairing> pairings;
        std::vector<bool> twime_used(twime_order_entries.size(), false);
        std::vector<bool> plaza_used(current_plaza_orders.size(), false);
        std::unordered_map<std::int64_t, std::vector<std::size_t>> twime_by_order_id;

        for (std::size_t index = 0; index < twime_order_entries.size(); ++index) {
            const auto& twime = twime_order_entries[index].view;
            if (twime.present && twime.order_id > 0) {
                twime_by_order_id[twime.order_id].push_back(index);
            }
        }

        for (std::size_t plaza_index = 0; plaza_index < current_plaza_orders.size(); ++plaza_index) {
            const auto& plaza = current_plaza_orders[plaza_index];
            auto it = twime_by_order_id.find(plaza.private_order_id);
            if (it == twime_by_order_id.end()) {
                continue;
            }
            if (it->second.size() == 1U) {
                const auto twime_index = it->second.front();
                pairings.push_back({
                    .twime_index = twime_index,
                    .plaza_index = plaza_index,
                    .mode = MatchMode::DirectIdentifier,
                });
                twime_used[twime_index] = true;
                plaza_used[plaza_index] = true;
            } else {
                for (const auto twime_index : it->second) {
                    pairings.push_back({
                        .twime_index = twime_index,
                        .plaza_index = plaza_index,
                        .mode = MatchMode::AmbiguousCandidates,
                        .ambiguous = true,
                        .fault_reason = "multiple_twime_direct_id_candidates",
                    });
                }
                plaza_used[plaza_index] = true;
            }
        }

        std::vector<std::vector<std::size_t>> twime_candidates(twime_order_entries.size());
        std::vector<std::vector<std::size_t>> plaza_candidates(current_plaza_orders.size());
        for (std::size_t twime_index = 0; twime_index < twime_order_entries.size(); ++twime_index) {
            if (twime_used[twime_index] || !twime_order_entries[twime_index].view.present) {
                continue;
            }
            for (std::size_t plaza_index = 0; plaza_index < current_plaza_orders.size(); ++plaza_index) {
                if (plaza_used[plaza_index]) {
                    continue;
                }
                if (exact_order_fallback_match(twime_order_entries[twime_index].view,
                                               current_plaza_orders[plaza_index])) {
                    twime_candidates[twime_index].push_back(plaza_index);
                    plaza_candidates[plaza_index].push_back(twime_index);
                }
            }
        }

        for (std::size_t twime_index = 0; twime_index < twime_candidates.size(); ++twime_index) {
            if (twime_used[twime_index] || twime_candidates[twime_index].empty()) {
                continue;
            }
            if (twime_candidates[twime_index].size() == 1U) {
                const auto plaza_index = twime_candidates[twime_index].front();
                if (plaza_candidates[plaza_index].size() == 1U && !plaza_used[plaza_index]) {
                    pairings.push_back({
                        .twime_index = twime_index,
                        .plaza_index = plaza_index,
                        .mode = MatchMode::ExactFallbackTuple,
                    });
                    twime_used[twime_index] = true;
                    plaza_used[plaza_index] = true;
                    continue;
                }
            }
            pairings.push_back({
                .twime_index = twime_index,
                .mode = MatchMode::AmbiguousCandidates,
                .ambiguous = true,
                .fault_reason = "multiple_plaza_fallback_candidates",
            });
            twime_used[twime_index] = true;
            for (const auto plaza_index : twime_candidates[twime_index]) {
                plaza_used[plaza_index] = true;
            }
        }

        for (std::size_t plaza_index = 0; plaza_index < plaza_candidates.size(); ++plaza_index) {
            if (plaza_used[plaza_index] || plaza_candidates[plaza_index].empty()) {
                continue;
            }
            pairings.push_back({
                .plaza_index = plaza_index,
                .mode = MatchMode::AmbiguousCandidates,
                .ambiguous = true,
                .fault_reason = "multiple_twime_fallback_candidates",
            });
            plaza_used[plaza_index] = true;
        }

        for (const auto& pairing : pairings) {
            ReconciledOrderSnapshot snapshot;
            snapshot.match_mode = pairing.mode;
            snapshot.twime =
                pairing.twime_index.has_value() ? twime_order_entries[*pairing.twime_index].view : TwimeOrderView{};
            snapshot.plaza =
                pairing.plaza_index.has_value() ? current_plaza_orders[*pairing.plaza_index] : PlazaOrderView{};
            snapshot.last_update_logical_sequence =
                std::max(snapshot.twime.last_logical_sequence, snapshot.plaza.last_logical_sequence);
            snapshot.last_update_logical_step = std::max(snapshot.twime.last_logical_step, current_step);
            snapshot.last_update_source = snapshot.plaza.last_logical_sequence >= snapshot.twime.last_logical_sequence
                                              ? ReconciliationSource::Plaza
                                              : ReconciliationSource::Twime;
            snapshot.plaza_revalidation_required =
                plaza_source.invalidated && (snapshot.plaza.present || snapshot.status == OrderStatus::Confirmed);
            if (pairing.ambiguous) {
                snapshot.status = OrderStatus::Ambiguous;
                snapshot.fault_reason = pairing.fault_reason;
            } else {
                update_order_status(snapshot);
            }
            reconciled_orders.push_back(std::move(snapshot));
        }

        for (std::size_t twime_index = 0; twime_index < twime_order_entries.size(); ++twime_index) {
            if (twime_used[twime_index] || !twime_order_entries[twime_index].view.present) {
                continue;
            }
            ReconciledOrderSnapshot snapshot;
            snapshot.twime = twime_order_entries[twime_index].view;
            snapshot.last_update_source = ReconciliationSource::Twime;
            snapshot.last_update_logical_sequence = snapshot.twime.last_logical_sequence;
            snapshot.last_update_logical_step = snapshot.twime.last_logical_step;
            update_order_status(snapshot);
            reconciled_orders.push_back(std::move(snapshot));
        }

        for (std::size_t plaza_index = 0; plaza_index < current_plaza_orders.size(); ++plaza_index) {
            if (plaza_used[plaza_index]) {
                continue;
            }
            ReconciledOrderSnapshot snapshot;
            snapshot.plaza = current_plaza_orders[plaza_index];
            snapshot.last_update_source = ReconciliationSource::Plaza;
            snapshot.last_update_logical_sequence = snapshot.plaza.last_logical_sequence;
            snapshot.last_update_logical_step = current_step;
            snapshot.status = OrderStatus::Unknown;
            reconciled_orders.push_back(std::move(snapshot));
        }

        std::sort(reconciled_orders.begin(), reconciled_orders.end(),
                  [](const ReconciledOrderSnapshot& lhs, const ReconciledOrderSnapshot& rhs) {
                      const auto lhs_key = std::tuple{
                          lhs.twime.order_id > 0 ? lhs.twime.order_id : lhs.plaza.private_order_id,
                          lhs.twime.cl_ord_id,
                          lhs.plaza.private_order_id,
                          lhs.plaza.public_order_id,
                      };
                      const auto rhs_key = std::tuple{
                          rhs.twime.order_id > 0 ? rhs.twime.order_id : rhs.plaza.private_order_id,
                          rhs.twime.cl_ord_id,
                          rhs.plaza.private_order_id,
                          rhs.plaza.public_order_id,
                      };
                      return lhs_key < rhs_key;
                  });

        for (const auto& order : reconciled_orders) {
            switch (order.status) {
            case OrderStatus::ProvisionalTwime:
                ++health.total_provisional_orders;
                break;
            case OrderStatus::Confirmed:
                ++health.total_confirmed_orders;
                break;
            case OrderStatus::Rejected:
                ++health.total_rejected_orders;
                break;
            case OrderStatus::Canceled:
                ++health.total_canceled_orders;
                break;
            case OrderStatus::Filled:
                ++health.total_filled_orders;
                break;
            case OrderStatus::Ambiguous:
                ++health.total_ambiguous_orders;
                break;
            case OrderStatus::Diverged:
                ++health.total_diverged_orders;
                break;
            case OrderStatus::Stale:
                ++health.total_stale_provisional_orders;
                break;
            case OrderStatus::Unknown:
                break;
            }
            if (order.twime.present && !order.plaza.present) {
                ++health.total_unmatched_twime_orders;
            }
            if (order.plaza.present && !order.twime.present) {
                ++health.total_unmatched_plaza_orders;
            }
            if (order.plaza_revalidation_required) {
                ++health.plaza_revalidation_pending_orders;
            }
        }
    }

    void update_order_status(ReconciledOrderSnapshot& snapshot) const {
        snapshot.plaza_revalidation_required = plaza_source.invalidated && snapshot.plaza.present;
        if (snapshot.twime.present && snapshot.plaza.present) {
            std::string reason;
            if (orders_materially_diverge(snapshot.twime, snapshot.plaza, reason)) {
                snapshot.status = OrderStatus::Diverged;
                snapshot.fault_reason = std::move(reason);
                return;
            }
            const auto matched_trade = find_trade_for_order(snapshot.twime.order_id, snapshot.twime.side);
            if (snapshot.twime.terminal_cancel && snapshot.plaza.private_amount_rest == 0 &&
                snapshot.plaza.public_amount_rest == 0) {
                snapshot.status = OrderStatus::Canceled;
                return;
            }
            if (order_has_terminal_fill(snapshot.plaza, matched_trade)) {
                snapshot.status = OrderStatus::Filled;
                return;
            }
            snapshot.status = OrderStatus::Confirmed;
            return;
        }

        if (snapshot.twime.present) {
            if (snapshot.twime.terminal_reject) {
                snapshot.status = OrderStatus::Rejected;
                return;
            }
            const auto matched_trade = find_trade_for_order(snapshot.twime.order_id, snapshot.twime.side);
            if (snapshot.twime.terminal_cancel) {
                snapshot.status = OrderStatus::Canceled;
                return;
            }
            if (matched_trade != nullptr && matched_trade->present && matched_trade->has_last_qty &&
                matched_trade->has_order_qty && matched_trade->last_qty >= matched_trade->order_qty) {
                snapshot.status = OrderStatus::Filled;
                return;
            }
            if (current_step - snapshot.twime.last_logical_step >= stale_after_steps) {
                snapshot.status = OrderStatus::Stale;
                return;
            }
            snapshot.status = OrderStatus::ProvisionalTwime;
            return;
        }

        snapshot.status = OrderStatus::Unknown;
    }

    const TwimeTradeView* find_trade_for_order(std::int64_t order_id, Side side) const {
        for (const auto& trade : twime_trade_entries) {
            if (!trade.view.present) {
                continue;
            }
            if (trade.view.order_id == order_id && (side == Side::Unknown || trade.view.side == side)) {
                return &trade.view;
            }
        }
        return nullptr;
    }

    void recompute_trades() {
        std::vector<Pairing> pairings;
        std::vector<bool> twime_used(twime_trade_entries.size(), false);
        std::vector<bool> plaza_used(current_plaza_trades.size(), false);
        std::unordered_map<std::int64_t, std::vector<std::size_t>> twime_by_trade_id;

        for (std::size_t index = 0; index < twime_trade_entries.size(); ++index) {
            const auto& trade = twime_trade_entries[index].view;
            if (trade.present && trade.trade_id > 0) {
                twime_by_trade_id[trade.trade_id].push_back(index);
            }
        }

        for (std::size_t plaza_index = 0; plaza_index < current_plaza_trades.size(); ++plaza_index) {
            const auto& plaza = current_plaza_trades[plaza_index];
            auto it = twime_by_trade_id.find(plaza.trade_id);
            if (it == twime_by_trade_id.end()) {
                continue;
            }
            if (it->second.size() == 1U) {
                const auto twime_index = it->second.front();
                pairings.push_back({
                    .twime_index = twime_index,
                    .plaza_index = plaza_index,
                    .mode = MatchMode::DirectIdentifier,
                });
                twime_used[twime_index] = true;
                plaza_used[plaza_index] = true;
            } else {
                for (const auto twime_index : it->second) {
                    pairings.push_back({
                        .twime_index = twime_index,
                        .plaza_index = plaza_index,
                        .mode = MatchMode::AmbiguousCandidates,
                        .ambiguous = true,
                        .fault_reason = "multiple_twime_trade_id_candidates",
                    });
                }
                plaza_used[plaza_index] = true;
            }
        }

        std::vector<std::vector<std::size_t>> twime_candidates(twime_trade_entries.size());
        std::vector<std::vector<std::size_t>> plaza_candidates(current_plaza_trades.size());
        for (std::size_t twime_index = 0; twime_index < twime_trade_entries.size(); ++twime_index) {
            if (twime_used[twime_index] || !twime_trade_entries[twime_index].view.present) {
                continue;
            }
            for (std::size_t plaza_index = 0; plaza_index < current_plaza_trades.size(); ++plaza_index) {
                if (plaza_used[plaza_index]) {
                    continue;
                }
                if (exact_trade_fallback_match(twime_trade_entries[twime_index].view,
                                               current_plaza_trades[plaza_index])) {
                    twime_candidates[twime_index].push_back(plaza_index);
                    plaza_candidates[plaza_index].push_back(twime_index);
                }
            }
        }

        for (std::size_t twime_index = 0; twime_index < twime_candidates.size(); ++twime_index) {
            if (twime_used[twime_index] || twime_candidates[twime_index].empty()) {
                continue;
            }
            if (twime_candidates[twime_index].size() == 1U) {
                const auto plaza_index = twime_candidates[twime_index].front();
                if (plaza_candidates[plaza_index].size() == 1U && !plaza_used[plaza_index]) {
                    pairings.push_back({
                        .twime_index = twime_index,
                        .plaza_index = plaza_index,
                        .mode = MatchMode::ExactFallbackTuple,
                    });
                    twime_used[twime_index] = true;
                    plaza_used[plaza_index] = true;
                    continue;
                }
            }
            pairings.push_back({
                .twime_index = twime_index,
                .mode = MatchMode::AmbiguousCandidates,
                .ambiguous = true,
                .fault_reason = "multiple_plaza_trade_fallback_candidates",
            });
            twime_used[twime_index] = true;
            for (const auto plaza_index : twime_candidates[twime_index]) {
                plaza_used[plaza_index] = true;
            }
        }

        for (std::size_t plaza_index = 0; plaza_index < plaza_candidates.size(); ++plaza_index) {
            if (plaza_used[plaza_index] || plaza_candidates[plaza_index].empty()) {
                continue;
            }
            pairings.push_back({
                .plaza_index = plaza_index,
                .mode = MatchMode::AmbiguousCandidates,
                .ambiguous = true,
                .fault_reason = "multiple_twime_trade_fallback_candidates",
            });
            plaza_used[plaza_index] = true;
        }

        for (const auto& pairing : pairings) {
            ReconciledTradeSnapshot snapshot;
            snapshot.match_mode = pairing.mode;
            snapshot.twime =
                pairing.twime_index.has_value() ? twime_trade_entries[*pairing.twime_index].view : TwimeTradeView{};
            snapshot.plaza =
                pairing.plaza_index.has_value() ? current_plaza_trades[*pairing.plaza_index] : PlazaTradeView{};
            snapshot.last_update_logical_sequence =
                std::max(snapshot.twime.last_logical_sequence, snapshot.plaza.last_logical_sequence);
            snapshot.last_update_logical_step = std::max(snapshot.twime.last_logical_step, current_step);
            snapshot.last_update_source = snapshot.plaza.last_logical_sequence >= snapshot.twime.last_logical_sequence
                                              ? ReconciliationSource::Plaza
                                              : ReconciliationSource::Twime;
            snapshot.plaza_revalidation_required = plaza_source.invalidated && snapshot.plaza.present;
            if (pairing.ambiguous) {
                snapshot.status = TradeStatus::Ambiguous;
                snapshot.fault_reason = pairing.fault_reason;
            } else {
                update_trade_status(snapshot);
            }
            reconciled_trades.push_back(std::move(snapshot));
        }

        for (std::size_t twime_index = 0; twime_index < twime_trade_entries.size(); ++twime_index) {
            if (twime_used[twime_index] || !twime_trade_entries[twime_index].view.present) {
                continue;
            }
            ReconciledTradeSnapshot snapshot;
            snapshot.twime = twime_trade_entries[twime_index].view;
            snapshot.last_update_source = ReconciliationSource::Twime;
            snapshot.last_update_logical_sequence = snapshot.twime.last_logical_sequence;
            snapshot.last_update_logical_step = snapshot.twime.last_logical_step;
            snapshot.status = TradeStatus::TwimeOnly;
            reconciled_trades.push_back(std::move(snapshot));
        }

        for (std::size_t plaza_index = 0; plaza_index < current_plaza_trades.size(); ++plaza_index) {
            if (plaza_used[plaza_index]) {
                continue;
            }
            ReconciledTradeSnapshot snapshot;
            snapshot.plaza = current_plaza_trades[plaza_index];
            snapshot.last_update_source = ReconciliationSource::Plaza;
            snapshot.last_update_logical_sequence = snapshot.plaza.last_logical_sequence;
            snapshot.last_update_logical_step = current_step;
            snapshot.status = TradeStatus::PlazaOnly;
            reconciled_trades.push_back(std::move(snapshot));
        }

        std::sort(
            reconciled_trades.begin(), reconciled_trades.end(),
            [](const ReconciledTradeSnapshot& lhs, const ReconciledTradeSnapshot& rhs) {
                const auto lhs_key = std::tuple{
                    lhs.twime.trade_id > 0 ? lhs.twime.trade_id : lhs.plaza.trade_id,
                    lhs.twime.order_id > 0 ? lhs.twime.order_id
                                           : std::max(lhs.plaza.private_order_id_buy, lhs.plaza.private_order_id_sell),
                    lhs.twime.cl_ord_id,
                };
                const auto rhs_key = std::tuple{
                    rhs.twime.trade_id > 0 ? rhs.twime.trade_id : rhs.plaza.trade_id,
                    rhs.twime.order_id > 0 ? rhs.twime.order_id
                                           : std::max(rhs.plaza.private_order_id_buy, rhs.plaza.private_order_id_sell),
                    rhs.twime.cl_ord_id,
                };
                return lhs_key < rhs_key;
            });

        for (const auto& trade : reconciled_trades) {
            switch (trade.status) {
            case TradeStatus::Matched:
                ++health.total_matched_trades;
                break;
            case TradeStatus::Diverged:
                ++health.total_diverged_trades;
                break;
            case TradeStatus::Ambiguous:
                ++health.total_ambiguous_trades;
                break;
            case TradeStatus::TwimeOnly:
                ++health.total_unmatched_twime_trades;
                break;
            case TradeStatus::PlazaOnly:
                ++health.total_unmatched_plaza_trades;
                break;
            }
            if (trade.plaza_revalidation_required) {
                ++health.plaza_revalidation_pending_trades;
            }
        }
    }

    void update_trade_status(ReconciledTradeSnapshot& snapshot) const {
        snapshot.plaza_revalidation_required = plaza_source.invalidated && snapshot.plaza.present;
        if (snapshot.twime.present && snapshot.plaza.present) {
            std::string reason;
            if (trades_materially_diverge(snapshot.twime, snapshot.plaza, reason)) {
                snapshot.status = TradeStatus::Diverged;
                snapshot.fault_reason = std::move(reason);
                return;
            }
            snapshot.status = TradeStatus::Matched;
            return;
        }
        snapshot.status = snapshot.twime.present ? TradeStatus::TwimeOnly : TradeStatus::PlazaOnly;
    }

    std::uint64_t current_step{0};
    std::uint64_t stale_after_steps{4};
    std::vector<StoredTwimeOrder> twime_order_entries;
    std::vector<StoredTwimeTrade> twime_trade_entries;
    std::unordered_map<std::uint64_t, std::size_t> twime_order_by_cl_ord_id;
    std::unordered_map<std::int64_t, std::size_t> twime_order_by_order_id;
    std::unordered_map<std::int64_t, std::size_t> twime_trade_by_trade_id;
    std::vector<PlazaOrderView> current_plaza_orders;
    std::vector<PlazaTradeView> current_plaza_trades;
    std::optional<TwimeSourceHealthInput> twime_source;
    PlazaSourceHealthInput plaza_source{};
    bool plaza_seen{false};
    std::vector<ReconciledOrderSnapshot> reconciled_orders;
    std::vector<ReconciledTradeSnapshot> reconciled_trades;
    Plaza2TwimeReconcilerHealthSnapshot health;
};

TwimeNormalizeResult normalize_twime_outbound_request(const TwimeEncodeRequest& request,
                                                      std::uint64_t logical_sequence) {
    TwimeNormalizeResult result{.ok = true};
    TwimeOrderInput input{
        .logical_sequence = logical_sequence,
    };

    switch (request.template_id) {
    case kTemplateNewOrderSingle:
    case kTemplateNewOrderIceberg:
    case kTemplateNewOrderIcebergX:
        input.kind = TwimeOrderInputKind::NewIntent;
        input.multileg = request.template_id == kTemplateNewOrderIcebergX;
        break;
    case kTemplateOrderCancelRequest:
    case kTemplateOrderIcebergCancelRequest:
        input.kind = TwimeOrderInputKind::CancelIntent;
        input.multileg = request.template_id == kTemplateOrderIcebergCancelRequest;
        break;
    case kTemplateOrderReplaceRequest:
    case kTemplateOrderIcebergReplaceRequest:
        input.kind = TwimeOrderInputKind::ReplaceIntent;
        input.multileg = request.template_id == kTemplateOrderIcebergReplaceRequest;
        break;
    default:
        return result;
    }

    for (const auto& field : request.fields) {
        if (field.name == "ClOrdID") {
            input.cl_ord_id = field.value.unsigned_value;
        } else if (field.name == "OrderID") {
            input.order_id = field.value.signed_value;
        } else if (field.name == "SecurityID") {
            input.security_id = static_cast<std::int32_t>(field.value.unsigned_value);
        } else if (field.name == "OrderQty") {
            input.has_order_qty = true;
            input.order_qty = static_cast<std::uint32_t>(field.value.unsigned_value);
        } else if (field.name == "Price") {
            input.has_price = true;
            input.price_mantissa = field.value.decimal5.mantissa;
        } else if (field.name == "Side") {
            input.side = parse_twime_side(field.value.string_view());
        } else if (field.name == "Account") {
            input.account = std::string(field.value.string_view());
        } else if (field.name == "ComplianceID") {
            input.compliance_id = std::string(field.value.string_view());
        } else if (field.name == "ClOrdLinkID") {
            input.cl_ord_link_id = static_cast<std::int32_t>(field.value.signed_value);
        }
    }

    if (input.kind == TwimeOrderInputKind::ReplaceIntent && input.order_id > 0) {
        input.prev_order_id = input.order_id;
        input.order_id = 0;
    }

    result.order_input = std::move(input);
    return result;
}

TwimeNormalizeResult normalize_twime_inbound_journal_entry(const TwimeJournalEntry& entry,
                                                           std::uint64_t logical_sequence) {
    TwimeCodec codec;
    DecodedTwimeMessage decoded;
    if (codec.decode_message(entry.bytes, decoded) != TwimeDecodeError::Ok) {
        return {
            .ok = false,
            .error = "failed to decode TWIME journal entry for reconciliation input",
        };
    }

    TwimeNormalizeResult result{.ok = true};
    switch (decoded.header.template_id) {
    case kTemplateNewOrderSingleResponse:
    case kTemplateNewOrderIcebergResponse: {
        TwimeOrderInput input{
            .kind = TwimeOrderInputKind::NewAccepted,
            .logical_sequence = logical_sequence,
            .multileg = decoded.header.template_id == kTemplateNewOrderIcebergResponse,
        };
        input.cl_ord_id = field_unsigned(decoded, "ClOrdID").value_or(0);
        input.order_id = field_signed(decoded, "OrderID").value_or(0);
        input.trading_session_id = static_cast<std::int32_t>(field_unsigned(decoded, "TradingSessionID").value_or(0));
        input.security_id = static_cast<std::int32_t>(field_unsigned(decoded, "SecurityID").value_or(0));
        input.cl_ord_link_id = static_cast<std::int32_t>(field_signed(decoded, "ClOrdLinkID").value_or(0));
        input.side = parse_twime_side(field_string(decoded, "Side"));
        input.compliance_id = field_string(decoded, "ComplianceID").value_or("");
        if (const auto price = field_decimal5(decoded, "Price"); price.has_value()) {
            input.has_price = true;
            input.price_mantissa = *price;
        }
        if (const auto qty = field_unsigned(decoded, "OrderQty"); qty.has_value()) {
            input.has_order_qty = true;
            input.order_qty = static_cast<std::uint32_t>(*qty);
        }
        result.order_input = std::move(input);
        return result;
    }
    case kTemplateOrderCancelResponse: {
        TwimeOrderInput input{
            .kind = TwimeOrderInputKind::CancelAccepted,
            .logical_sequence = logical_sequence,
        };
        input.cl_ord_id = field_unsigned(decoded, "ClOrdID").value_or(0);
        input.order_id = field_signed(decoded, "OrderID").value_or(0);
        input.trading_session_id = static_cast<std::int32_t>(field_unsigned(decoded, "TradingSessionID").value_or(0));
        if (const auto qty = field_unsigned(decoded, "OrderQty"); qty.has_value()) {
            input.has_order_qty = true;
            input.order_qty = static_cast<std::uint32_t>(*qty);
        }
        result.order_input = std::move(input);
        return result;
    }
    case kTemplateOrderReplaceResponse: {
        TwimeOrderInput input{
            .kind = TwimeOrderInputKind::ReplaceAccepted,
            .logical_sequence = logical_sequence,
        };
        input.cl_ord_id = field_unsigned(decoded, "ClOrdID").value_or(0);
        input.order_id = field_signed(decoded, "OrderID").value_or(0);
        input.prev_order_id = field_signed(decoded, "PrevOrderID").value_or(0);
        input.trading_session_id = static_cast<std::int32_t>(field_unsigned(decoded, "TradingSessionID").value_or(0));
        input.cl_ord_link_id = static_cast<std::int32_t>(field_signed(decoded, "ClOrdLinkID").value_or(0));
        input.compliance_id = field_string(decoded, "ComplianceID").value_or("");
        if (const auto price = field_decimal5(decoded, "Price"); price.has_value()) {
            input.has_price = true;
            input.price_mantissa = *price;
        }
        if (const auto qty = field_unsigned(decoded, "OrderQty"); qty.has_value()) {
            input.has_order_qty = true;
            input.order_qty = static_cast<std::uint32_t>(*qty);
        }
        result.order_input = std::move(input);
        return result;
    }
    case kTemplateBusinessMessageReject: {
        TwimeOrderInput input{
            .kind = TwimeOrderInputKind::Rejected,
            .logical_sequence = logical_sequence,
        };
        input.cl_ord_id = field_unsigned(decoded, "ClOrdID").value_or(0);
        input.reject_code = field_signed(decoded, "OrdRejReason").value_or(0);
        result.order_input = std::move(input);
        return result;
    }
    case kTemplateExecutionSingleReport:
    case kTemplateExecutionMultilegReport: {
        TwimeTradeInput input{
            .kind = TwimeTradeInputKind::Execution,
            .logical_sequence = logical_sequence,
            .multileg = decoded.header.template_id == kTemplateExecutionMultilegReport,
        };
        input.cl_ord_id = field_unsigned(decoded, "ClOrdID").value_or(0);
        input.order_id = field_signed(decoded, "OrderID").value_or(0);
        input.trade_id = field_signed(decoded, "TrdMatchID").value_or(0);
        input.trading_session_id = static_cast<std::int32_t>(field_unsigned(decoded, "TradingSessionID").value_or(0));
        input.security_id = static_cast<std::int32_t>(field_unsigned(decoded, "SecurityID").value_or(0));
        input.side = parse_twime_side(field_string(decoded, "Side"));
        if (const auto price = field_decimal5(decoded, "LastPx"); price.has_value()) {
            input.has_price = true;
            input.price_mantissa = *price;
        }
        if (const auto qty = field_unsigned(decoded, "LastQty"); qty.has_value()) {
            input.has_last_qty = true;
            input.last_qty = static_cast<std::uint32_t>(*qty);
        }
        if (const auto qty = field_unsigned(decoded, "OrderQty"); qty.has_value()) {
            input.has_order_qty = true;
            input.order_qty = static_cast<std::uint32_t>(*qty);
        }
        result.trade_input = std::move(input);
        return result;
    }
    default:
        return result;
    }
}

TwimeNormalizedInputBatch collect_twime_reconciliation_inputs(std::span<const TwimeEncodeRequest> outbound_requests,
                                                              std::span<const TwimeJournalEntry> inbound_journal,
                                                              const TwimeSessionHealthSnapshot* session_health,
                                                              const TwimeSessionMetrics* session_metrics,
                                                              std::uint64_t starting_sequence) {
    TwimeNormalizedInputBatch batch;
    if (session_health != nullptr && session_metrics != nullptr) {
        batch.source_health = make_twime_source_health_input(*session_health, *session_metrics);
    }

    auto logical_sequence = starting_sequence;
    for (const auto& request : outbound_requests) {
        const auto normalized = normalize_twime_outbound_request(request, logical_sequence++);
        if (!normalized.ok) {
            batch.ok = false;
            batch.error = normalized.error;
            return batch;
        }
        if (normalized.order_input.has_value()) {
            batch.order_inputs.push_back(*normalized.order_input);
        }
    }

    for (const auto& entry : inbound_journal) {
        const auto normalized = normalize_twime_inbound_journal_entry(entry, logical_sequence++);
        if (!normalized.ok) {
            batch.ok = false;
            batch.error = normalized.error;
            return batch;
        }
        if (normalized.order_input.has_value()) {
            batch.order_inputs.push_back(*normalized.order_input);
        }
        if (normalized.trade_input.has_value()) {
            batch.trade_inputs.push_back(*normalized.trade_input);
        }
    }
    return batch;
}

TwimeSourceHealthInput make_twime_source_health_input(const TwimeSessionHealthSnapshot& session_health,
                                                      const TwimeSessionMetrics& session_metrics) {
    return {
        .session_health = session_health,
        .session_metrics = session_metrics,
    };
}

PlazaCommittedSnapshotInput make_plaza_committed_snapshot(const Plaza2PrivateStateProjector& projector,
                                                          std::uint64_t logical_sequence) {
    PlazaCommittedSnapshotInput snapshot;
    snapshot.logical_sequence = logical_sequence;
    snapshot.source_health.connector_health = projector.connector_health();
    snapshot.source_health.resume_markers = projector.resume_markers();
    snapshot.source_health.stream_health.assign(projector.stream_health().begin(), projector.stream_health().end());

    for (const auto stream_code : kRequiredPrivateStreams) {
        bool ready = false;
        for (const auto& stream : snapshot.source_health.stream_health) {
            if (stream.stream_code == stream_code && stream.online && stream.snapshot_complete) {
                ready = true;
                break;
            }
        }
        if (!ready) {
            snapshot.source_health.required_private_streams_ready = false;
            snapshot.source_health.invalidated = true;
            snapshot.source_health.invalidation_reason = "private_stream_not_ready";
            break;
        }
        snapshot.source_health.required_private_streams_ready = true;
    }
    if (!snapshot.source_health.connector_health.online) {
        snapshot.source_health.invalidated = true;
        snapshot.source_health.required_private_streams_ready = false;
        snapshot.source_health.invalidation_reason = "connector_not_online";
    }

    snapshot.orders.reserve(projector.own_orders().size());
    for (const auto& order : projector.own_orders()) {
        snapshot.orders.push_back({
            .multileg = order.multileg,
            .public_order_id = order.public_order_id,
            .private_order_id = order.private_order_id,
            .sess_id = order.sess_id,
            .isin_id = order.isin_id,
            .client_code = order.client_code,
            .login_from = order.login_from,
            .comment = order.comment,
            .price_text = order.price,
            .has_price = !order.price.empty(),
            .price_mantissa = parse_plaza_decimal_to_mantissa(order.price).value_or(0),
            .public_amount = order.public_amount,
            .public_amount_rest = order.public_amount_rest,
            .private_amount = order.private_amount,
            .private_amount_rest = order.private_amount_rest,
            .id_deal = order.id_deal,
            .xstatus = order.xstatus,
            .xstatus2 = order.xstatus2,
            .side = parse_plaza_side(order.dir),
            .public_action = order.public_action,
            .private_action = order.private_action,
            .moment = order.moment,
            .moment_ns = order.moment_ns,
            .ext_id = order.ext_id,
            .from_trade_repl = order.from_trade_repl,
            .from_user_book = order.from_user_book,
            .from_current_day = order.from_current_day,
        });
    }

    snapshot.trades.reserve(projector.own_trades().size());
    for (const auto& trade : projector.own_trades()) {
        snapshot.trades.push_back({
            .multileg = trade.multileg,
            .trade_id = trade.id_deal,
            .sess_id = trade.sess_id,
            .isin_id = trade.isin_id,
            .price_text = trade.price,
            .has_price = !trade.price.empty(),
            .price_mantissa = parse_plaza_decimal_to_mantissa(trade.price).value_or(0),
            .amount = trade.amount,
            .public_order_id_buy = trade.public_order_id_buy,
            .public_order_id_sell = trade.public_order_id_sell,
            .private_order_id_buy = trade.private_order_id_buy,
            .private_order_id_sell = trade.private_order_id_sell,
            .code_buy = trade.code_buy,
            .code_sell = trade.code_sell,
            .comment_buy = trade.comment_buy,
            .comment_sell = trade.comment_sell,
            .login_buy = trade.login_buy,
            .login_sell = trade.login_sell,
            .moment = trade.moment,
            .moment_ns = trade.moment_ns,
        });
    }

    return snapshot;
}

Plaza2TwimeReconciler::Plaza2TwimeReconciler(std::uint64_t stale_after_steps)
    : impl_(std::make_unique<Impl>(stale_after_steps)) {}

Plaza2TwimeReconciler::~Plaza2TwimeReconciler() = default;

Plaza2TwimeReconciler::Plaza2TwimeReconciler(Plaza2TwimeReconciler&&) noexcept = default;

Plaza2TwimeReconciler& Plaza2TwimeReconciler::operator=(Plaza2TwimeReconciler&&) noexcept = default;

Plaza2TwimeReconciler Plaza2TwimeReconciler::clone() const {
    Plaza2TwimeReconciler copy;
    copy.impl_ = std::make_unique<Impl>(*impl_);
    return copy;
}

void Plaza2TwimeReconciler::reset() {
    impl_->reset();
}

void Plaza2TwimeReconciler::set_stale_after_steps(std::uint64_t stale_after_steps) {
    impl_->set_stale_after(stale_after_steps);
}

void Plaza2TwimeReconciler::advance_steps(std::uint64_t steps) {
    impl_->advance(steps);
}

void Plaza2TwimeReconciler::update_twime_source_health(const TwimeSourceHealthInput& input) {
    impl_->update_twime_health(input);
}

void Plaza2TwimeReconciler::apply_twime_order_input(const TwimeOrderInput& input) {
    impl_->apply_twime_order(input);
}

void Plaza2TwimeReconciler::apply_twime_trade_input(const TwimeTradeInput& input) {
    impl_->apply_twime_trade(input);
}

void Plaza2TwimeReconciler::apply_twime_inputs(const TwimeNormalizedInputBatch& batch) {
    if (batch.source_health.has_value()) {
        impl_->update_twime_health(*batch.source_health);
    }
    for (const auto& input : batch.order_inputs) {
        impl_->apply_twime_order(input);
    }
    for (const auto& input : batch.trade_inputs) {
        impl_->apply_twime_trade(input);
    }
}

void Plaza2TwimeReconciler::apply_plaza_snapshot(const PlazaCommittedSnapshotInput& snapshot) {
    impl_->apply_plaza(snapshot);
}

const Plaza2TwimeReconcilerHealthSnapshot& Plaza2TwimeReconciler::health() const noexcept {
    return impl_->health;
}

std::span<const ReconciledOrderSnapshot> Plaza2TwimeReconciler::orders() const noexcept {
    return impl_->reconciled_orders;
}

std::span<const ReconciledTradeSnapshot> Plaza2TwimeReconciler::trades() const noexcept {
    return impl_->reconciled_trades;
}

} // namespace moex::plaza2_twime_reconciler
