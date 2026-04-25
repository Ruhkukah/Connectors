#pragma once

#include "moex/plaza2_trade/plaza2_trade_commands.hpp"
#include "moex/plaza2_trade/plaza2_trade_replies.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace moex::plaza2_trade {

enum class Plaza2TradeValidationCode {
    Ok,
    MissingRequiredField,
    StringTooLong,
    InvalidAscii,
    InvalidEnum,
    InvalidNumericRange,
    InvalidDecimalText,
    BufferTooSmall,
    UnknownMessage,
};

struct Plaza2TradeValidationResult {
    Plaza2TradeValidationCode code{Plaza2TradeValidationCode::Ok};
    std::string field_name;
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return code == Plaza2TradeValidationCode::Ok;
    }
};

struct Plaza2TradeEncodedCommand {
    Plaza2TradeCommandKind command_kind{Plaza2TradeCommandKind::AddOrder};
    std::string command_name;
    std::int32_t msgid{0};
    std::vector<std::byte> payload;
    Plaza2TradeValidationResult validation;
    bool offline_only{true};
};

class Plaza2TradeCodec {
  public:
    [[nodiscard]] Plaza2TradeValidationResult validate(const Plaza2TradeCommandRequest& request) const;
    [[nodiscard]] Plaza2TradeEncodedCommand encode(const Plaza2TradeCommandRequest& request) const;
    [[nodiscard]] Plaza2TradeDecodedReply decode_reply(std::int32_t msgid, std::span<const std::byte> payload,
                                                       Plaza2TradeValidationResult& validation) const;
};

[[nodiscard]] bool is_sendable(const Plaza2TradeEncodedCommand& command) noexcept;
[[nodiscard]] std::string bytes_to_hex(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> bytes_from_hex(std::string_view hex);

} // namespace moex::plaza2_trade
