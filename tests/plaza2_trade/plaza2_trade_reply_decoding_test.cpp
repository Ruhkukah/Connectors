#include "plaza2_trade_test_support.hpp"
#include "plaza2_trade_official_wire_test_support.hpp"

#include <cstring>
#include <iostream>
#include <limits>
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
    out.resize(start + 256, std::byte{0});
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
    flood.resize(sizeof(official_cgate99::FORTS_MSG99));

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

// CGate 9.12.0.2144 packaged p2gate_en.html Appendix C, matching the Russian edition.
// Archive SHA-256 73fb651459fdb1420f82f684e7c2fc3dbaa20dac085f9ff83fdc38fc35e16d61.
// forts_messages.ini declares an i4 code, with no per-command list of possible values.
// Injecting this global list tests the codec, independent of exchange applicability.
constexpr std::int32_t kAppendixCReturnCodes[] = {
    -1,    0,     1,     2,     3,     4,     5,     6,     7,     8,     9,     10,    12,    13,    14,    25,
    26,    27,    28,    31,    32,    33,    34,    35,    36,    37,    38,    39,    40,    41,    42,    43,
    44,    45,    46,    47,    48,    49,    50,    53,    54,    55,    56,    57,    58,    60,    61,    62,
    63,    64,    65,    66,    67,    68,    69,    70,    71,    72,    73,    74,    75,    76,    77,    78,
    79,    80,    81,    82,    85,    90,    95,    100,   105,   110,   115,   120,   125,   130,   135,   140,
    310,   311,   312,   314,   315,   316,   317,   318,   320,   331,   332,   333,   335,   336,   337,   338,
    339,   680,   681,   682,   3000,  3001,  3002,  3003,  4000,  4001,  4002,  4004,  4005,  4006,  4007,  4008,
    4009,  4010,  4011,  4012,  4013,  4014,  4015,  4016,  4017,  4018,  4021,  4022,  4023,  4024,  4025,  4026,
    4027,  4028,  4030,  4031,  4032,  4033,  4034,  4035,  4040,  4041,  4042,  4043,  4044,  4045,  4046,  4050,
    4051,  4052,  4055,  4060,  4061,  4062,  4063,  4064,  4065,  4066,  4067,  4069,  4070,  4090,  4091,  4092,
    4093,  4094,  4095,  4096,  4097,  4098,  4099,  4100,  4101,  4102,  4103,  4104,  4105,  4106,  4107,  4108,
    4109,  4110,  4111,  4112,  4115,  4116,  4117,  4118,  4119,  4122,  4123,  4124,  4125,  4126,  4127,  4128,
    4129,  4130,  4131,  4132,  4133,  4134,  4135,  4136,  4137,  4138,  4139,  4140,  4141,  4142,  4143,  4144,
    4145,  4146,  4147,  4148,  4149,  4150,  4151,  4152,  4153,  4154,  4155,  4160,  4161,  4162,  4163,  4164,
    4165,  4166,  4167,  4169,  4170,  4171,  4172,  4173,  4174,  4175,  4180,  4181,  4200,  4201,  4202,  4203,
    4204,  4205,  4206,  4207,  4208,  4220,  4221,  4222,  4223,  4224,  4225,  4226,  4227,  4230,  4231,  4232,
    4233,  4234,  4258,  4259,  4260,  4261,  4262,  4264,  4266,  4268,  4269,  4280,  4281,  4282,  4283,  4284,
    4285,  4300,  4301,  4302,  4303,  4304,  4305,  5052,  5055,  5061,  5062,  5063,  5064,  5065,  5069,  5071,
    5072,  5073,  5074,  10000, 10001, 10004, 10005, 10006, 20000, 20001, 20002, 20003, 20004, 20005, 20006, 20007,
    20008, 20009, 20010, 20011, 20012, 20013, 20014, 20015, 20016, 20017, 20018, 20019, 20101, 20102, 20103, 20104,
    20105, 20106, 20107, 20108, 20109, 20110, 20111, 20112, 20113, 20114, 20115, 20116, 20117, 20129, 20130, 30000,
    30001, 30002, 40000, 40001, 40002, 40003, 40004, 40005, 40006, 40007,
};

void test_appendix_c_command_replies() {
    using namespace moex::plaza2_trade::test_support;
    const Plaza2TradeCodec codec;
    const auto check = [&](std::int32_t msgid, const auto& wire) {
        Plaza2TradeValidationResult validation;
        const auto reply = codec.decode_reply(msgid, wire_bytes(wire), validation);
        const auto expected =
            wire.code == 0 ? Plaza2TradeReplyStatusCategory::Accepted : Plaza2TradeReplyStatusCategory::Rejected;
        if (!validation.ok() || reply.msgid != msgid || reply.code != wire.code || reply.status != expected ||
            reply.message != "reply code fixture")
            throw std::runtime_error("incorrect command reply for message " + std::to_string(msgid) + " and code " +
                                     std::to_string(wire.code));
    };
    const auto check_code = [&](std::int32_t code) {
        using namespace official_cgate99;
        check(FORTS_MSG179_msgid, FORTS_MSG179{.code = code, .message = "reply code fixture"});
        check(FORTS_MSG176_msgid, FORTS_MSG176{.code = code, .message = "reply code fixture"});
        check(FORTS_MSG177_msgid, FORTS_MSG177{.code = code, .message = "reply code fixture"});
        check(FORTS_MSG186_msgid, FORTS_MSG186{.code = code, .message = "reply code fixture"});
    };
    for (const auto code : kAppendixCReturnCodes)
        check_code(code);
    // Signed i4 is open-ended. Code 99 must also remain an ordinary command error.
    for (const auto code : {std::numeric_limits<std::int32_t>::min(), 99, std::numeric_limits<std::int32_t>::max()})
        check_code(code);
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

void test_excluded_command_replies_fail_closed() {
    const Plaza2TradeCodec codec;
    Plaza2TradeValidationResult validation;
    // Complete payloads must still be rejected when their command family is outside this certificate.
    const std::vector<std::byte> payload(276, std::byte{0});
    for (const int msgid : {172, 180, 181, 182, 10000}) {
        const auto reply = codec.decode_reply(msgid, payload, validation);
        require(validation.code == Plaza2TradeValidationCode::UnknownMessage &&
                    reply.status == Plaza2TradeReplyStatusCategory::Unknown,
                "excluded command reply was accepted by the first-certificate codec");
    }
}

void test_official_reply_layouts() {
    using namespace moex::plaza2_trade::test_support;
    const Plaza2TradeCodec codec;
    official_cgate99::FORTS_MSG179 add{};
    add.order_id = 0x102030405060708LL;
    std::memset(add.message, 'X', 255);
    Plaza2TradeValidationResult validation;
    const auto accepted = codec.decode_reply(179, wire_bytes(add), validation);
    require(validation.ok() && accepted.order_id == std::int64_t{add.order_id} && accepted.message.size() == 255,
            "official Add reply offset and full c255 text must decode");
    auto truncated = wire_bytes(add);
    truncated.pop_back();
    static_cast<void>(codec.decode_reply(179, truncated, validation));
    require(!validation.ok(), "old compact Add reply length must fail closed");
    official_cgate99::FORTS_MSG177 cancel{};
    cancel.amount = 12345;
    const auto cancelled = codec.decode_reply(177, wire_bytes(cancel), validation);
    require(validation.ok() && cancelled.amount == cancel.amount, "official cancel amount offset");
    official_cgate99::FORTS_MSG186 recovery{};
    recovery.num_orders = 6789;
    const auto recovered = codec.decode_reply(186, wire_bytes(recovery), validation);
    require(validation.ok() && recovered.num_orders == recovery.num_orders, "official recovery count offset");
    official_cgate99::FORTS_MSG99 flood{};
    flood.queue_size = 3;
    flood.penalty_remain = 10;
    std::memset(flood.message, 'Y', 128);
    const auto refused = codec.decode_reply(99, wire_bytes(flood), validation);
    require(validation.ok() && refused.queue_size == 3 && refused.penalty_remain == 10 && refused.message.size() == 128,
            "official flood reply c128 and padding");
}

} // namespace

int main() {
    try {
        test_add_order_reply_decoding();
        test_move_reply_decoding();
        test_error_decoding();
        test_appendix_c_command_replies();
        test_unknown_and_short_replies_fail_closed();
        test_excluded_command_replies_fail_closed();
        test_official_reply_layouts();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
