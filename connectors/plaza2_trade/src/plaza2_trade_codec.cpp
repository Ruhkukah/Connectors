#include "moex/plaza2_trade/plaza2_trade_codec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <cstring>
#include <optional>
#include <sstream>
#include <type_traits>

namespace moex::plaza2_trade {

namespace {

constexpr std::size_t kReplyMessageLength = 255;

template <typename T> void append_le(std::vector<std::byte>& out, T value) {
    static_assert(std::is_integral_v<T>);
    using Unsigned = std::make_unsigned_t<T>;
    auto raw = static_cast<Unsigned>(value);
    if constexpr (std::endian::native != std::endian::little) {
        if constexpr (sizeof(T) == 2) {
            raw = static_cast<Unsigned>((raw >> 8U) | (raw << 8U));
        } else if constexpr (sizeof(T) == 4) {
            raw = static_cast<Unsigned>(((raw & 0x000000FFU) << 24U) | ((raw & 0x0000FF00U) << 8U) |
                                        ((raw & 0x00FF0000U) >> 8U) | ((raw & 0xFF000000U) >> 24U));
        } else if constexpr (sizeof(T) == 8) {
            raw = static_cast<Unsigned>(((raw & 0x00000000000000FFULL) << 56U) |
                                        ((raw & 0x000000000000FF00ULL) << 40U) |
                                        ((raw & 0x0000000000FF0000ULL) << 24U) |
                                        ((raw & 0x00000000FF000000ULL) << 8U) |
                                        ((raw & 0x000000FF00000000ULL) >> 8U) |
                                        ((raw & 0x0000FF0000000000ULL) >> 24U) |
                                        ((raw & 0x00FF000000000000ULL) >> 40U) |
                                        ((raw & 0xFF00000000000000ULL) >> 56U));
        }
    }
    const auto start = out.size();
    out.resize(start + sizeof(T));
    std::memcpy(out.data() + start, &raw, sizeof(T));
}

template <typename T> std::optional<T> load_le(std::span<const std::byte> bytes, std::size_t& offset) {
    static_assert(std::is_integral_v<T>);
    if (offset + sizeof(T) > bytes.size()) {
        return std::nullopt;
    }
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned raw{};
    std::memcpy(&raw, bytes.data() + offset, sizeof(T));
    offset += sizeof(T);
    if constexpr (std::endian::native != std::endian::little) {
        if constexpr (sizeof(T) == 2) {
            raw = static_cast<Unsigned>((raw >> 8U) | (raw << 8U));
        } else if constexpr (sizeof(T) == 4) {
            raw = static_cast<Unsigned>(((raw & 0x000000FFU) << 24U) | ((raw & 0x0000FF00U) << 8U) |
                                        ((raw & 0x00FF0000U) >> 8U) | ((raw & 0xFF000000U) >> 24U));
        } else if constexpr (sizeof(T) == 8) {
            raw = static_cast<Unsigned>(((raw & 0x00000000000000FFULL) << 56U) |
                                        ((raw & 0x000000000000FF00ULL) << 40U) |
                                        ((raw & 0x0000000000FF0000ULL) << 24U) |
                                        ((raw & 0x00000000FF000000ULL) << 8U) |
                                        ((raw & 0x000000FF00000000ULL) >> 8U) |
                                        ((raw & 0x0000FF0000000000ULL) >> 24U) |
                                        ((raw & 0x00FF000000000000ULL) >> 40U) |
                                        ((raw & 0xFF00000000000000ULL) >> 56U));
        }
    }
    return static_cast<T>(raw);
}

Plaza2TradeValidationResult ok() {
    return {};
}

Plaza2TradeValidationResult fail(Plaza2TradeValidationCode code, std::string field_name, std::string message) {
    return Plaza2TradeValidationResult{.code = code, .field_name = std::move(field_name), .message = std::move(message)};
}

bool is_ascii_printable(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return byte >= 0x20U && byte <= 0x7EU;
    });
}

bool is_decimal_text(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    std::size_t index = 0;
    if (value[index] == '-') {
        ++index;
    }
    bool digit = false;
    bool dot = false;
    for (; index < value.size(); ++index) {
        if (std::isdigit(static_cast<unsigned char>(value[index])) != 0) {
            digit = true;
            continue;
        }
        if (value[index] == '.' && !dot) {
            dot = true;
            continue;
        }
        return false;
    }
    return digit;
}

Plaza2TradeValidationResult validate_fixed_string(std::string_view field, const std::optional<std::string>& value,
                                                  std::size_t width, bool required) {
    if (!value) {
        return required ? fail(Plaza2TradeValidationCode::MissingRequiredField, std::string(field), "required field")
                        : ok();
    }
    if (value->size() > width) {
        return fail(Plaza2TradeValidationCode::StringTooLong, std::string(field), "fixed string field is too long");
    }
    if (!is_ascii_printable(*value)) {
        return fail(Plaza2TradeValidationCode::InvalidAscii, std::string(field), "fixed string must be printable ASCII");
    }
    return ok();
}

Plaza2TradeValidationResult validate_decimal(std::string_view field, const std::optional<std::string>& value,
                                             std::size_t width, bool required) {
    auto result = validate_fixed_string(field, value, width, required);
    if (!result.ok() || !value) {
        return result;
    }
    if (!is_decimal_text(*value)) {
        return fail(Plaza2TradeValidationCode::InvalidDecimalText, std::string(field), "invalid decimal text");
    }
    return ok();
}

template <typename T>
Plaza2TradeValidationResult validate_integer(std::string_view field, const std::optional<T>& value, bool required,
                                             std::optional<T> minimum = std::nullopt) {
    if (!value) {
        return required ? fail(Plaza2TradeValidationCode::MissingRequiredField, std::string(field), "required field")
                        : ok();
    }
    if (minimum && *value < *minimum) {
        return fail(Plaza2TradeValidationCode::InvalidNumericRange, std::string(field), "numeric field below minimum");
    }
    return ok();
}

template <typename T>
Plaza2TradeValidationResult validate_integer(std::string_view field, const std::optional<T>& value, bool required,
                                             T minimum) {
    return validate_integer(field, value, required, std::optional<T>{minimum});
}

Plaza2TradeValidationResult validate_side(const std::optional<Plaza2TradeSide>& value) {
    if (!value) {
        return fail(Plaza2TradeValidationCode::MissingRequiredField, "dir", "required field");
    }
    if (*value != Plaza2TradeSide::Buy && *value != Plaza2TradeSide::Sell) {
        return fail(Plaza2TradeValidationCode::InvalidEnum, "dir", "invalid side enum");
    }
    return ok();
}

Plaza2TradeValidationResult validate_order_type(const std::optional<Plaza2TradeOrderType>& value) {
    if (!value) {
        return fail(Plaza2TradeValidationCode::MissingRequiredField, "type", "required field");
    }
    if (*value != Plaza2TradeOrderType::Limit && *value != Plaza2TradeOrderType::Market) {
        return fail(Plaza2TradeValidationCode::InvalidEnum, "type", "invalid order type enum");
    }
    return ok();
}

void append_string(std::vector<std::byte>& out, const std::optional<std::string>& value, std::size_t width) {
    const auto start = out.size();
    out.resize(start + width, std::byte{0});
    if (value) {
        std::memcpy(out.data() + start, value->data(), value->size());
    }
}

void append_i4(std::vector<std::byte>& out, const std::optional<std::int32_t>& value) {
    append_le<std::int32_t>(out, value.value_or(0));
}

void append_i8(std::vector<std::byte>& out, const std::optional<std::int64_t>& value) {
    append_le<std::int64_t>(out, value.value_or(0));
}

void append_i1(std::vector<std::byte>& out, const std::optional<std::int8_t>& value) {
    append_le<std::int8_t>(out, value.value_or(0));
}

void append_side(std::vector<std::byte>& out, const std::optional<Plaza2TradeSide>& value) {
    append_le<std::int32_t>(out, value ? static_cast<std::int32_t>(*value) : 0);
}

void append_order_type(std::vector<std::byte>& out, const std::optional<Plaza2TradeOrderType>& value) {
    append_le<std::int32_t>(out, value ? static_cast<std::int32_t>(*value) : 0);
}

std::string read_string(std::span<const std::byte> bytes, std::size_t& offset, std::size_t width) {
    const auto available = offset + width <= bytes.size() ? width : bytes.size() - offset;
    std::string value;
    value.reserve(available);
    for (std::size_t index = 0; index < available; ++index) {
        const auto ch = static_cast<char>(std::to_integer<unsigned char>(bytes[offset + index]));
        if (ch == '\0') {
            break;
        }
        value.push_back(ch);
    }
    offset += available;
    return value;
}

Plaza2TradeValidationResult validate_add_order(const AddOrderRequest& request) {
    if (auto result = validate_fixed_string("broker_code", request.broker_code, 4, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("isin_id", request.isin_id, true, std::int32_t{1}); !result.ok()) {
        return result;
    }
    if (auto result = validate_fixed_string("client_code", request.client_code, 3, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_side(request.dir); !result.ok()) {
        return result;
    }
    if (auto result = validate_order_type(request.type); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("amount", request.amount, true, std::int32_t{1}); !result.ok()) {
        return result;
    }
    if (auto result = validate_decimal("price", request.price, 17, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_fixed_string("comment", request.comment, 20, false); !result.ok()) {
        return result;
    }
    if (auto result = validate_fixed_string("broker_to", request.broker_to, 20, false); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("ext_id", request.ext_id, false); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("is_check_limit", request.is_check_limit, false, std::int32_t{0});
        !result.ok()) {
        return result;
    }
    if (auto result = validate_fixed_string("date_exp", request.date_exp, 8, false); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("dont_check_money", request.dont_check_money, false, std::int32_t{0});
        !result.ok()) {
        return result;
    }
    if (auto result = validate_fixed_string("match_ref", request.match_ref, 10, false); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("ncc_request", request.ncc_request, false, std::int8_t{0}); !result.ok()) {
        return result;
    }
    return validate_fixed_string("compliance_id", request.compliance_id, 1, false);
}

Plaza2TradeValidationResult validate_iceberg_add_order(const IcebergAddOrderRequest& request) {
    AddOrderRequest base;
    base.broker_code = request.broker_code;
    base.isin_id = request.isin_id;
    base.client_code = request.client_code;
    base.dir = request.dir;
    base.type = request.type;
    base.amount = request.iceberg_amount;
    base.price = request.price;
    base.comment = request.comment;
    base.ext_id = request.ext_id;
    base.is_check_limit = request.is_check_limit;
    base.date_exp = request.date_exp;
    base.dont_check_money = request.dont_check_money;
    base.ncc_request = request.ncc_request;
    base.compliance_id = request.compliance_id;
    if (auto result = validate_add_order(base); !result.ok()) {
        return result.field_name == "amount" ? fail(result.code, "iceberg_amount", result.message) : result;
    }
    if (auto result = validate_integer("disclose_const_amount", request.disclose_const_amount, true, std::int32_t{1});
        !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("variance_amount", request.variance_amount, false, std::int32_t{0});
        !result.ok()) {
        return result;
    }
    return ok();
}

Plaza2TradeValidationResult validate_del_order(const DelOrderRequest& request) {
    if (auto result = validate_fixed_string("broker_code", request.broker_code, 4, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("order_id", request.order_id, true, std::int64_t{1}); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("ncc_request", request.ncc_request, false, std::int8_t{0}); !result.ok()) {
        return result;
    }
    if (auto result = validate_fixed_string("client_code", request.client_code, 3, true); !result.ok()) {
        return result;
    }
    return validate_integer("isin_id", request.isin_id, true, std::int32_t{1});
}

Plaza2TradeValidationResult validate_iceberg_del_order(const IcebergDelOrderRequest& request) {
    if (auto result = validate_fixed_string("broker_code", request.broker_code, 4, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("order_id", request.order_id, true, std::int64_t{1}); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("isin_id", request.isin_id, true, std::int32_t{1}); !result.ok()) {
        return result;
    }
    return validate_integer("ncc_request", request.ncc_request, false, std::int8_t{0});
}

Plaza2TradeValidationResult validate_move_order(const MoveOrderRequest& request) {
    if (auto result = validate_fixed_string("broker_code", request.broker_code, 4, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("regime", request.regime, true, std::int32_t{0}); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("order_id1", request.order_id1, true, std::int64_t{1}); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("amount1", request.amount1, true, std::int32_t{1}); !result.ok()) {
        return result;
    }
    if (auto result = validate_decimal("price1", request.price1, 17, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("ext_id1", request.ext_id1, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("order_id2", request.order_id2, true, std::int64_t{1}); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("amount2", request.amount2, true, std::int32_t{1}); !result.ok()) {
        return result;
    }
    if (auto result = validate_decimal("price2", request.price2, 17, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("ext_id2", request.ext_id2, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("is_check_limit", request.is_check_limit, false, std::int32_t{0});
        !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("ncc_request", request.ncc_request, false, std::int8_t{0}); !result.ok()) {
        return result;
    }
    if (auto result = validate_fixed_string("client_code", request.client_code, 3, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("isin_id", request.isin_id, true, std::int32_t{1}); !result.ok()) {
        return result;
    }
    return validate_fixed_string("compliance_id", request.compliance_id, 1, false);
}

Plaza2TradeValidationResult validate_iceberg_move_order(const IcebergMoveOrderRequest& request) {
    if (auto result = validate_fixed_string("broker_code", request.broker_code, 4, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("order_id", request.order_id, true, std::int64_t{1}); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("isin_id", request.isin_id, true, std::int32_t{1}); !result.ok()) {
        return result;
    }
    if (auto result = validate_decimal("price", request.price, 17, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("ext_id", request.ext_id, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("ncc_request", request.ncc_request, false, std::int8_t{0}); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("is_check_limit", request.is_check_limit, false, std::int32_t{0});
        !result.ok()) {
        return result;
    }
    return validate_fixed_string("compliance_id", request.compliance_id, 1, false);
}

Plaza2TradeValidationResult validate_del_user_orders(const DelUserOrdersRequest& request) {
    if (auto result = validate_fixed_string("broker_code", request.broker_code, 4, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("buy_sell", request.buy_sell, true, std::int32_t{0}); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("non_system", request.non_system, true, std::int32_t{0}); !result.ok()) {
        return result;
    }
    if (auto result = validate_fixed_string("code", request.code, 3, true); !result.ok()) {
        return result;
    }
    if (auto result = validate_fixed_string("base_contract_code", request.base_contract_code, 25, true);
        !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("ext_id", request.ext_id, false); !result.ok()) {
        return result;
    }
    if (auto result = validate_integer("isin_id", request.isin_id, true, std::int32_t{1}); !result.ok()) {
        return result;
    }
    return validate_integer("instrument_mask", request.instrument_mask, true, std::int8_t{0});
}

Plaza2TradeValidationResult validate_del_orders_by_bf_limit(const DelOrdersByBFLimitRequest& request) {
    return validate_fixed_string("broker_code", request.broker_code, 4, true);
}

Plaza2TradeValidationResult validate_cod_heartbeat(const CODHeartbeatRequest& request) {
    return validate_integer("seq_number", request.seq_number, false, std::int32_t{0});
}

std::vector<std::byte> encode_add_order_payload(const AddOrderRequest& request) {
    std::vector<std::byte> out;
    append_string(out, request.broker_code, 4);
    append_i4(out, request.isin_id);
    append_string(out, request.client_code, 3);
    append_side(out, request.dir);
    append_order_type(out, request.type);
    append_i4(out, request.amount);
    append_string(out, request.price, 17);
    append_string(out, request.comment, 20);
    append_string(out, request.broker_to, 20);
    append_i4(out, request.ext_id);
    append_i4(out, request.is_check_limit);
    append_string(out, request.date_exp, 8);
    append_i4(out, request.dont_check_money);
    append_string(out, request.match_ref, 10);
    append_i1(out, request.ncc_request);
    append_string(out, request.compliance_id, 1);
    return out;
}

std::vector<std::byte> encode_iceberg_add_order_payload(const IcebergAddOrderRequest& request) {
    std::vector<std::byte> out;
    append_string(out, request.broker_code, 4);
    append_i4(out, request.isin_id);
    append_string(out, request.client_code, 3);
    append_side(out, request.dir);
    append_order_type(out, request.type);
    append_i4(out, request.disclose_const_amount);
    append_i4(out, request.iceberg_amount);
    append_i4(out, request.variance_amount);
    append_string(out, request.price, 17);
    append_string(out, request.comment, 20);
    append_i4(out, request.ext_id);
    append_i4(out, request.is_check_limit);
    append_string(out, request.date_exp, 8);
    append_i4(out, request.dont_check_money);
    append_i1(out, request.ncc_request);
    append_string(out, request.compliance_id, 1);
    return out;
}

std::vector<std::byte> encode_del_order_payload(const DelOrderRequest& request) {
    std::vector<std::byte> out;
    append_string(out, request.broker_code, 4);
    append_i8(out, request.order_id);
    append_i1(out, request.ncc_request);
    append_string(out, request.client_code, 3);
    append_i4(out, request.isin_id);
    return out;
}

std::vector<std::byte> encode_iceberg_del_order_payload(const IcebergDelOrderRequest& request) {
    std::vector<std::byte> out;
    append_string(out, request.broker_code, 4);
    append_i8(out, request.order_id);
    append_i4(out, request.isin_id);
    append_i1(out, request.ncc_request);
    return out;
}

std::vector<std::byte> encode_move_order_payload(const MoveOrderRequest& request) {
    std::vector<std::byte> out;
    append_string(out, request.broker_code, 4);
    append_i4(out, request.regime);
    append_i8(out, request.order_id1);
    append_i4(out, request.amount1);
    append_string(out, request.price1, 17);
    append_i4(out, request.ext_id1);
    append_i8(out, request.order_id2);
    append_i4(out, request.amount2);
    append_string(out, request.price2, 17);
    append_i4(out, request.ext_id2);
    append_i4(out, request.is_check_limit);
    append_i1(out, request.ncc_request);
    append_string(out, request.client_code, 3);
    append_i4(out, request.isin_id);
    append_string(out, request.compliance_id, 1);
    return out;
}

std::vector<std::byte> encode_iceberg_move_order_payload(const IcebergMoveOrderRequest& request) {
    std::vector<std::byte> out;
    append_string(out, request.broker_code, 4);
    append_i8(out, request.order_id);
    append_i4(out, request.isin_id);
    append_string(out, request.price, 17);
    append_i4(out, request.ext_id);
    append_i1(out, request.ncc_request);
    append_i4(out, request.is_check_limit);
    append_string(out, request.compliance_id, 1);
    return out;
}

std::vector<std::byte> encode_del_user_orders_payload(const DelUserOrdersRequest& request) {
    std::vector<std::byte> out;
    append_string(out, request.broker_code, 4);
    append_i4(out, request.buy_sell);
    append_i4(out, request.non_system);
    append_string(out, request.code, 3);
    append_string(out, request.base_contract_code, 25);
    append_i4(out, request.ext_id);
    append_i4(out, request.isin_id);
    append_i1(out, request.instrument_mask);
    return out;
}

std::vector<std::byte> encode_del_orders_by_bf_limit_payload(const DelOrdersByBFLimitRequest& request) {
    std::vector<std::byte> out;
    append_string(out, request.broker_code, 4);
    return out;
}

std::vector<std::byte> encode_cod_heartbeat_payload(const CODHeartbeatRequest& request) {
    std::vector<std::byte> out;
    append_i4(out, request.seq_number);
    return out;
}

Plaza2TradeReplyStatusCategory status_from_code(std::int32_t code, bool error_surface) {
    if (error_surface) {
        return code == 0 ? Plaza2TradeReplyStatusCategory::Unknown : Plaza2TradeReplyStatusCategory::SystemError;
    }
    return code == 0 ? Plaza2TradeReplyStatusCategory::Accepted : Plaza2TradeReplyStatusCategory::Rejected;
}

} // namespace

Plaza2TradeCommandKind command_kind(const Plaza2TradeCommandRequest& request) {
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, AddOrderRequest>) {
                return Plaza2TradeCommandKind::AddOrder;
            } else if constexpr (std::is_same_v<T, IcebergAddOrderRequest>) {
                return Plaza2TradeCommandKind::IcebergAddOrder;
            } else if constexpr (std::is_same_v<T, DelOrderRequest>) {
                return Plaza2TradeCommandKind::DelOrder;
            } else if constexpr (std::is_same_v<T, IcebergDelOrderRequest>) {
                return Plaza2TradeCommandKind::IcebergDelOrder;
            } else if constexpr (std::is_same_v<T, MoveOrderRequest>) {
                return Plaza2TradeCommandKind::MoveOrder;
            } else if constexpr (std::is_same_v<T, IcebergMoveOrderRequest>) {
                return Plaza2TradeCommandKind::IcebergMoveOrder;
            } else if constexpr (std::is_same_v<T, DelUserOrdersRequest>) {
                return Plaza2TradeCommandKind::DelUserOrders;
            } else if constexpr (std::is_same_v<T, DelOrdersByBFLimitRequest>) {
                return Plaza2TradeCommandKind::DelOrdersByBFLimit;
            } else {
                return Plaza2TradeCommandKind::CODHeartbeat;
            }
        },
        request);
}

const char* command_name(Plaza2TradeCommandKind kind) noexcept {
    switch (kind) {
    case Plaza2TradeCommandKind::AddOrder:
        return "AddOrder";
    case Plaza2TradeCommandKind::IcebergAddOrder:
        return "IcebergAddOrder";
    case Plaza2TradeCommandKind::DelOrder:
        return "DelOrder";
    case Plaza2TradeCommandKind::IcebergDelOrder:
        return "IcebergDelOrder";
    case Plaza2TradeCommandKind::MoveOrder:
        return "MoveOrder";
    case Plaza2TradeCommandKind::IcebergMoveOrder:
        return "IcebergMoveOrder";
    case Plaza2TradeCommandKind::DelUserOrders:
        return "DelUserOrders";
    case Plaza2TradeCommandKind::DelOrdersByBFLimit:
        return "DelOrdersByBFLimit";
    case Plaza2TradeCommandKind::CODHeartbeat:
        return "CODHeartbeat";
    }
    return "Unknown";
}

std::int32_t command_msgid(Plaza2TradeCommandKind kind) noexcept {
    return static_cast<std::int32_t>(kind);
}

Plaza2TradeValidationResult Plaza2TradeCodec::validate(const Plaza2TradeCommandRequest& request) const {
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, AddOrderRequest>) {
                return validate_add_order(value);
            } else if constexpr (std::is_same_v<T, IcebergAddOrderRequest>) {
                return validate_iceberg_add_order(value);
            } else if constexpr (std::is_same_v<T, DelOrderRequest>) {
                return validate_del_order(value);
            } else if constexpr (std::is_same_v<T, IcebergDelOrderRequest>) {
                return validate_iceberg_del_order(value);
            } else if constexpr (std::is_same_v<T, MoveOrderRequest>) {
                return validate_move_order(value);
            } else if constexpr (std::is_same_v<T, IcebergMoveOrderRequest>) {
                return validate_iceberg_move_order(value);
            } else if constexpr (std::is_same_v<T, DelUserOrdersRequest>) {
                return validate_del_user_orders(value);
            } else if constexpr (std::is_same_v<T, DelOrdersByBFLimitRequest>) {
                return validate_del_orders_by_bf_limit(value);
            } else {
                return validate_cod_heartbeat(value);
            }
        },
        request);
}

Plaza2TradeEncodedCommand Plaza2TradeCodec::encode(const Plaza2TradeCommandRequest& request) const {
    const auto kind = command_kind(request);
    Plaza2TradeEncodedCommand encoded{
        .command_kind = kind,
        .command_name = command_name(kind),
        .msgid = command_msgid(kind),
        .payload = {},
        .validation = validate(request),
        .offline_only = true,
    };
    if (!encoded.validation.ok()) {
        return encoded;
    }
    encoded.payload = std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, AddOrderRequest>) {
                return encode_add_order_payload(value);
            } else if constexpr (std::is_same_v<T, IcebergAddOrderRequest>) {
                return encode_iceberg_add_order_payload(value);
            } else if constexpr (std::is_same_v<T, DelOrderRequest>) {
                return encode_del_order_payload(value);
            } else if constexpr (std::is_same_v<T, IcebergDelOrderRequest>) {
                return encode_iceberg_del_order_payload(value);
            } else if constexpr (std::is_same_v<T, MoveOrderRequest>) {
                return encode_move_order_payload(value);
            } else if constexpr (std::is_same_v<T, IcebergMoveOrderRequest>) {
                return encode_iceberg_move_order_payload(value);
            } else if constexpr (std::is_same_v<T, DelUserOrdersRequest>) {
                return encode_del_user_orders_payload(value);
            } else if constexpr (std::is_same_v<T, DelOrdersByBFLimitRequest>) {
                return encode_del_orders_by_bf_limit_payload(value);
            } else {
                return encode_cod_heartbeat_payload(value);
            }
        },
        request);
    return encoded;
}

Plaza2TradeDecodedReply Plaza2TradeCodec::decode_reply(std::int32_t msgid, std::span<const std::byte> payload,
                                                       Plaza2TradeValidationResult& validation) const {
    Plaza2TradeDecodedReply reply{.msgid = msgid};
    std::size_t offset = 0;
    const auto read_i4 = [&]() { return load_le<std::int32_t>(payload, offset); };
    const auto read_i8 = [&]() { return load_le<std::int64_t>(payload, offset); };
    const auto read_message = [&]() { return read_string(payload, offset, kReplyMessageLength); };
    validation = ok();

    switch (msgid) {
    case 176:
        reply.message_name = "FORTS_MSG176";
        if (auto code = read_i4(); code && offset + kReplyMessageLength + 16 <= payload.size()) {
            reply.code = *code;
            reply.message = read_message();
            reply.order_id1 = read_i8();
            reply.order_id2 = read_i8();
        } else {
            validation = fail(Plaza2TradeValidationCode::BufferTooSmall, "FORTS_MSG176", "reply payload too short");
        }
        break;
    case 177:
    case 182:
        reply.message_name = msgid == 177 ? "FORTS_MSG177" : "FORTS_MSG182";
        if (auto code = read_i4(); code && offset + kReplyMessageLength + 4 <= payload.size()) {
            reply.code = *code;
            reply.message = read_message();
            reply.amount = read_i4();
        } else {
            validation = fail(Plaza2TradeValidationCode::BufferTooSmall, reply.message_name, "reply payload too short");
        }
        break;
    case 179:
    case 181:
        reply.message_name = msgid == 179 ? "FORTS_MSG179" : "FORTS_MSG181";
        if (auto code = read_i4(); code && offset + kReplyMessageLength + 8 <= payload.size()) {
            reply.code = *code;
            reply.message = read_message();
            reply.order_id = read_i8();
        } else {
            validation = fail(Plaza2TradeValidationCode::BufferTooSmall, reply.message_name, "reply payload too short");
        }
        break;
    case 180:
        reply.message_name = "FORTS_MSG180";
        if (auto code = read_i4(); code && offset + kReplyMessageLength + 8 <= payload.size()) {
            reply.code = *code;
            reply.message = read_message();
            reply.iceberg_order_id = read_i8();
        } else {
            validation = fail(Plaza2TradeValidationCode::BufferTooSmall, "FORTS_MSG180", "reply payload too short");
        }
        break;
    case 186:
    case 172:
        reply.message_name = msgid == 186 ? "FORTS_MSG186" : "FORTS_MSG172";
        if (auto code = read_i4(); code && offset + kReplyMessageLength + 4 <= payload.size()) {
            reply.code = *code;
            reply.message = read_message();
            reply.num_orders = read_i4();
        } else {
            validation = fail(Plaza2TradeValidationCode::BufferTooSmall, reply.message_name, "reply payload too short");
        }
        break;
    case 99:
        reply.message_name = "FORTS_MSG99";
        if (auto queue = read_i4(); queue && offset + 4 + kReplyMessageLength <= payload.size()) {
            reply.queue_size = *queue;
            reply.penalty_remain = read_i4();
            reply.message = read_message();
            reply.status = Plaza2TradeReplyStatusCategory::BusinessRejection;
            return reply;
        }
        validation = fail(Plaza2TradeValidationCode::BufferTooSmall, "FORTS_MSG99", "reply payload too short");
        return reply;
    case 100:
        reply.message_name = "FORTS_MSG100";
        if (auto code = read_i4(); code && offset + kReplyMessageLength <= payload.size()) {
            reply.code = *code;
            reply.message = read_message();
            reply.status = status_from_code(reply.code, true);
            return reply;
        }
        validation = fail(Plaza2TradeValidationCode::BufferTooSmall, "FORTS_MSG100", "reply payload too short");
        return reply;
    default:
        validation = fail(Plaza2TradeValidationCode::UnknownMessage, "msgid", "unknown reply message id");
        return reply;
    }

    if (validation.ok()) {
        reply.status = status_from_code(reply.code, false);
    }
    return reply;
}

bool is_sendable(const Plaza2TradeEncodedCommand& command) noexcept {
    return !command.offline_only;
}

std::string bytes_to_hex(std::span<const std::byte> bytes) {
    constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                           '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned int>(byte);
        out.push_back(kHex[(value >> 4U) & 0x0FU]);
        out.push_back(kHex[value & 0x0FU]);
    }
    return out;
}

std::vector<std::byte> bytes_from_hex(std::string_view hex) {
    auto nibble = [](char ch) -> std::optional<unsigned int> {
        if (ch >= '0' && ch <= '9') {
            return static_cast<unsigned int>(ch - '0');
        }
        if (ch >= 'a' && ch <= 'f') {
            return static_cast<unsigned int>(ch - 'a' + 10);
        }
        if (ch >= 'A' && ch <= 'F') {
            return static_cast<unsigned int>(ch - 'A' + 10);
        }
        return std::nullopt;
    };
    std::vector<std::byte> out;
    if (hex.size() % 2 != 0) {
        return out;
    }
    out.reserve(hex.size() / 2);
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        const auto high = nibble(hex[index]);
        const auto low = nibble(hex[index + 1]);
        if (!high || !low) {
            return {};
        }
        out.push_back(static_cast<std::byte>((*high << 4U) | *low));
    }
    return out;
}

} // namespace moex::plaza2_trade
