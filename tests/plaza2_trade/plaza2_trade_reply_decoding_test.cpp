#include "plaza2_trade_test_support.hpp"

#include <cstring>
#include <iostream>
#include <vector>

namespace {

using moex::plaza2_trade::Plaza2TradeCodec;
using moex::plaza2_trade::Plaza2TradeReplyStatusCategory;
using moex::plaza2_trade::Plaza2TradeValidationCode;
using moex::plaza2_trade::Plaza2TradeValidationResult;
using moex::plaza2_trade::test_support::require;

template <typename T> void append_le(std::vector<std::byte>& out, T value) {
    const auto start = out.size();
    out.resize(start + sizeof(T));
    std::memcpy(out.data() + start, &value, sizeof(T));
}

void append_message(std::vector<std::byte>& out, std::string_view value) {
    const auto start = out.size();
    out.resize(start + 255, std::byte{0});
    std::memcpy(out.data() + start, value.data(), value.size());
}

void test_add_order_reply_decoding() {
    std::vector<std::byte> payload;
    append_le<std::int32_t>(payload, 0);
    append_message(payload, "accepted");
    append_le<std::int64_t>(payload, 9001);

    const Plaza2TradeCodec codec;
    Plaza2TradeValidationResult validation;
    const auto decoded = codec.decode_reply(179, payload, validation);
    require(validation.ok(), "FORTS_MSG179 should decode");
    require(decoded.message_name == "FORTS_MSG179", "reply name should be preserved");
    require(decoded.status == Plaza2TradeReplyStatusCategory::Accepted, "zero reply code should be accepted");
    require(decoded.order_id && *decoded.order_id == 9001, "reply order id should decode");
}

void test_move_reply_decoding() {
    std::vector<std::byte> payload;
    append_le<std::int32_t>(payload, 0);
    append_message(payload, "moved");
    append_le<std::int64_t>(payload, 9001);
    append_le<std::int64_t>(payload, 9002);

    const Plaza2TradeCodec codec;
    Plaza2TradeValidationResult validation;
    const auto decoded = codec.decode_reply(176, payload, validation);
    require(validation.ok(), "FORTS_MSG176 should decode");
    require(decoded.order_id1 && *decoded.order_id1 == 9001, "first moved order id should decode");
    require(decoded.order_id2 && *decoded.order_id2 == 9002, "second moved order id should decode");
}

void test_error_decoding() {
    std::vector<std::byte> flood;
    append_le<std::int32_t>(flood, 3);
    append_le<std::int32_t>(flood, 10);
    append_message(flood, "flood");

    const Plaza2TradeCodec codec;
    Plaza2TradeValidationResult validation;
    const auto flood_decoded = codec.decode_reply(99, flood, validation);
    require(validation.ok(), "FORTS_MSG99 should decode");
    require(flood_decoded.status == Plaza2TradeReplyStatusCategory::BusinessRejection,
            "flood-control reply should be classified as business rejection");
    require(flood_decoded.queue_size && *flood_decoded.queue_size == 3, "queue size should decode");

    std::vector<std::byte> system;
    append_le<std::int32_t>(system, -17);
    append_message(system, "system");
    const auto system_decoded = codec.decode_reply(100, system, validation);
    require(validation.ok(), "FORTS_MSG100 should decode");
    require(system_decoded.status == Plaza2TradeReplyStatusCategory::SystemError,
            "nonzero system error code should be classified as system error");
}

void test_unknown_and_short_replies_fail_closed() {
    const Plaza2TradeCodec codec;
    Plaza2TradeValidationResult validation;
    const std::vector<std::byte> empty;
    const auto unknown = codec.decode_reply(9999, empty, validation);
    static_cast<void>(unknown);
    require(validation.code == Plaza2TradeValidationCode::UnknownMessage, "unknown msgid should fail closed");

    const auto short_reply = codec.decode_reply(179, empty, validation);
    static_cast<void>(short_reply);
    require(validation.code == Plaza2TradeValidationCode::BufferTooSmall, "short reply should fail closed");
}

} // namespace

int main() {
    try {
        test_add_order_reply_decoding();
        test_move_reply_decoding();
        test_error_decoding();
        test_unknown_and_short_replies_fail_closed();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
