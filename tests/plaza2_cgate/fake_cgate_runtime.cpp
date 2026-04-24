#include "plaza2_generated_metadata.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using moex::plaza2::generated::FieldCode;
using moex::plaza2::generated::FindFieldByCode;
using moex::plaza2::generated::FindStreamByCode;
using moex::plaza2::generated::FindTableByCode;
using moex::plaza2::generated::StreamCode;
using moex::plaza2::generated::StreamDescriptors;
using moex::plaza2::generated::TableCode;
using moex::plaza2::generated::ValueClass;

constexpr std::uint32_t kCgErrOk = 0;
constexpr std::uint32_t kCgRangeBegin = 131072;
constexpr std::uint32_t kCgErrInvalidArgument = kCgRangeBegin + 1;
constexpr std::uint32_t kCgErrTimeout = kCgRangeBegin + 3;
constexpr std::uint32_t kCgErrIncorrectState = kCgRangeBegin + 5;
constexpr std::uint32_t kCgErrBufferTooSmall = kCgRangeBegin + 7;

constexpr std::uint32_t kStateClosed = 0;
constexpr std::uint32_t kStateOpening = 1;
constexpr std::uint32_t kStateActive = 2;
constexpr std::uint32_t kStateError = 3;

constexpr std::uint32_t kCgMsgOpen = 0x100;
constexpr std::uint32_t kCgMsgStreamData = 0x120;
constexpr std::uint32_t kCgMsgTnBegin = 0x200;
constexpr std::uint32_t kCgMsgTnCommit = 0x210;
constexpr std::uint32_t kCgMsgP2replLifenum = 0x1110;
constexpr std::uint32_t kCgMsgP2replOnline = 0x1112;
constexpr std::uint32_t kCgMsgP2replReplState = 0x1115;

struct CgValuePair {
    CgValuePair* next;
    char* key;
    char* value;
};

struct CgFieldValueDesc {
    CgFieldValueDesc* next;
    char* name;
    char* desc;
    void* value;
    void* mask;
};

struct CgMessageDesc;

struct CgFieldDesc {
    CgFieldDesc* next;
    std::uint32_t id;
    char* name;
    char* desc;
    char* type;
    std::size_t size;
    std::size_t offset;
    void* def_value;
    std::size_t num_values;
    CgFieldValueDesc* values;
    CgValuePair* hints;
    std::size_t max_count;
    CgFieldDesc* count_field;
    CgMessageDesc* type_msg;
};

struct CgIndexFieldDesc {
    CgIndexFieldDesc* next;
    CgFieldDesc* field;
    std::uint32_t sort_order;
};

struct CgIndexDesc {
    CgIndexDesc* next;
    std::size_t num_fields;
    CgIndexFieldDesc* fields;
    char* name;
    char* desc;
    CgValuePair* hints;
};

struct CgMessageDesc {
    CgMessageDesc* next;
    std::size_t size;
    std::size_t num_fields;
    CgFieldDesc* fields;
    std::uint32_t id;
    char* name;
    char* desc;
    CgValuePair* hints;
    std::size_t num_indices;
    CgIndexDesc* indices;
    std::size_t align;
};

struct CgSchemeDesc {
    std::uint32_t scheme_type;
    std::uint32_t features;
    std::size_t num_messages;
    CgMessageDesc* messages;
    CgValuePair* hints;
};

struct CgMsg {
    std::uint32_t type;
    std::size_t data_size;
    void* data;
    std::int64_t owner_id;
};

struct CgMsgStreamData {
    std::uint32_t type;
    std::size_t data_size;
    void* data;
    std::int64_t owner_id;
    std::size_t msg_index;
    std::uint32_t msg_id;
    const char* msg_name;
    std::int64_t rev;
    std::size_t num_nulls;
    std::uint8_t* nulls;
    std::uint64_t user_id;
};

struct CgTime {
    std::uint16_t year;
    std::uint8_t month;
    std::uint8_t day;
    std::uint8_t hour;
    std::uint8_t minute;
    std::uint8_t second;
    std::uint16_t msec;
};

struct CgDataLifeNum {
    std::uint32_t life_number;
    std::uint32_t flags;
};

enum class FakeValueKind : std::uint8_t {
    SignedInteger = 0,
    UnsignedInteger = 1,
    Text = 2,
    Timestamp = 3,
};

struct FakeFieldValue {
    FieldCode field_code{};
    FakeValueKind kind{FakeValueKind::SignedInteger};
    std::int64_t signed_value{0};
    std::uint64_t unsigned_value{0};
    std::string text;
};

struct FakeMessageScript {
    TableCode table_code{};
    std::int64_t rev{0};
    std::vector<FakeFieldValue> fields;
};

struct OwnedField {
    CgFieldDesc desc{};
    std::string name;
    std::string type_token;
};

struct OwnedMessage {
    CgMessageDesc desc{};
    std::string name;
    std::vector<std::unique_ptr<OwnedField>> fields;
};

struct OwnedScheme {
    CgSchemeDesc desc{};
    std::vector<std::unique_ptr<OwnedMessage>> messages;
};

struct FieldPlan {
    FieldCode field_code{};
    ValueClass value_class{ValueClass::kSignedInteger};
    std::size_t offset{0};
    std::size_t size{0};
};

struct MessagePlan {
    std::size_t msg_index{0};
    TableCode table_code{};
    std::string message_name;
    std::vector<FieldPlan> fields;
    std::size_t row_size{0};
};

struct FakeConnection;

using CgListenerCallback = std::uint32_t (*)(void* conn, void* listener, void* msg, void* data);

struct FakeListener {
    std::uint32_t state{kStateClosed};
    std::string settings;
    FakeConnection* connection{nullptr};
    CgListenerCallback callback{nullptr};
    void* callback_data{nullptr};
    StreamCode stream_code{};
    std::unique_ptr<OwnedScheme> scheme;
    std::vector<MessagePlan> message_plans;
    bool script_emitted{false};
};

struct FakeConnection {
    std::uint32_t state{kStateClosed};
    std::string settings;
    std::vector<FakeListener*> listeners;
    bool script_emitted{false};
};

bool g_env_open = false;

std::string copy_c_string(const char* value, std::size_t size) {
    if (value == nullptr || size == 0) {
        return {};
    }
    const auto* end = static_cast<const char*>(std::memchr(value, '\0', size));
    const auto count = end == nullptr ? size : static_cast<std::size_t>(end - value);
    return std::string(value, count);
}

std::size_t size_for_value_class(ValueClass value_class) {
    switch (value_class) {
    case ValueClass::kSignedInteger:
    case ValueClass::kUnsignedInteger:
        return 8;
    case ValueClass::kFixedString:
    case ValueClass::kDecimal:
    case ValueClass::kFloatingPoint:
        return 32;
    case ValueClass::kTimestamp:
        return sizeof(CgTime);
    case ValueClass::kBinary:
        return 16;
    }
    return 8;
}

template <typename T> void write_scalar(std::byte* dest, T value, std::size_t size) {
    std::memset(dest, 0, size);
    std::memcpy(dest, &value, std::min(size, sizeof(T)));
}

CgTime make_cg_time(std::uint64_t unix_seconds) {
    const auto raw = static_cast<std::time_t>(unix_seconds);
    const auto* utc = std::gmtime(&raw);
    CgTime value{};
    if (utc == nullptr) {
        return value;
    }
    value.year = static_cast<std::uint16_t>(utc->tm_year + 1900);
    value.month = static_cast<std::uint8_t>(utc->tm_mon + 1);
    value.day = static_cast<std::uint8_t>(utc->tm_mday);
    value.hour = static_cast<std::uint8_t>(utc->tm_hour);
    value.minute = static_cast<std::uint8_t>(utc->tm_min);
    value.second = static_cast<std::uint8_t>(utc->tm_sec);
    value.msec = 0;
    return value;
}

std::vector<FakeMessageScript> script_for_stream(StreamCode stream_code) {
    using enum FakeValueKind;
    using enum StreamCode;
    using enum TableCode;

    switch (stream_code) {
    case kFortsRefdataRepl:
        return {
            {
                .table_code = kFortsRefdataReplSession,
                .rev = 1,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsRefdataReplSessionSessId,
                         .kind = SignedInteger,
                         .signed_value = 321},
                        {.field_code = FieldCode::kFortsRefdataReplSessionBegin,
                         .kind = SignedInteger,
                         .signed_value = 1700000000},
                        {.field_code = FieldCode::kFortsRefdataReplSessionEnd,
                         .kind = SignedInteger,
                         .signed_value = 1700003600},
                        {.field_code = FieldCode::kFortsRefdataReplSessionState,
                         .kind = SignedInteger,
                         .signed_value = 2},
                    },
            },
            {
                .table_code = kFortsRefdataReplFutInstruments,
                .rev = 2,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsRefdataReplFutInstrumentsIsinId,
                         .kind = SignedInteger,
                         .signed_value = 1001},
                        {.field_code = FieldCode::kFortsRefdataReplFutInstrumentsIsin,
                         .kind = Text,
                         .text = "RTS-6.26"},
                        {.field_code = FieldCode::kFortsRefdataReplFutInstrumentsShortIsin,
                         .kind = Text,
                         .text = "RIH6"},
                        {.field_code = FieldCode::kFortsRefdataReplFutInstrumentsName,
                         .kind = Text,
                         .text = "RTS Jun 2026"},
                        {.field_code = FieldCode::kFortsRefdataReplFutInstrumentsBaseContractCode,
                         .kind = Text,
                         .text = "RTS"},
                        {.field_code = FieldCode::kFortsRefdataReplFutInstrumentsInstTerm,
                         .kind = SignedInteger,
                         .signed_value = 3},
                        {.field_code = FieldCode::kFortsRefdataReplFutInstrumentsRoundto,
                         .kind = SignedInteger,
                         .signed_value = 2},
                        {.field_code = FieldCode::kFortsRefdataReplFutInstrumentsLotVolume,
                         .kind = SignedInteger,
                         .signed_value = 1},
                        {.field_code = FieldCode::kFortsRefdataReplFutInstrumentsTradeModeId,
                         .kind = SignedInteger,
                         .signed_value = 4},
                        {.field_code = FieldCode::kFortsRefdataReplFutInstrumentsMinStep, .kind = Text, .text = "10"},
                        {.field_code = FieldCode::kFortsRefdataReplFutInstrumentsStepPrice,
                         .kind = Text,
                         .text = "12.5"},
                        {.field_code = FieldCode::kFortsRefdataReplFutInstrumentsSettlementPrice,
                         .kind = Text,
                         .text = "105000.5"},
                    },
            },
            {
                .table_code = kFortsRefdataReplInstr2matchingMap,
                .rev = 3,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsRefdataReplInstr2matchingMapBaseContractId,
                         .kind = SignedInteger,
                         .signed_value = 500},
                        {.field_code = FieldCode::kFortsRefdataReplInstr2matchingMapMatchingId,
                         .kind = SignedInteger,
                         .signed_value = 3},
                    },
            },
        };
    case kFortsPartRepl:
        return {
            {
                .table_code = kFortsPartReplPart,
                .rev = 4,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsPartReplPartClientCode, .kind = Text, .text = "CL001"},
                        {.field_code = FieldCode::kFortsPartReplPartLimitsSet,
                         .kind = SignedInteger,
                         .signed_value = 1},
                        {.field_code = FieldCode::kFortsPartReplPartMoneyFree, .kind = Text, .text = "125000.50"},
                        {.field_code = FieldCode::kFortsPartReplPartMoneyBlocked, .kind = Text, .text = "1200.25"},
                    },
            },
        };
    case kFortsPosRepl:
        return {
            {
                .table_code = kFortsPosReplPosition,
                .rev = 5,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsPosReplPositionClientCode, .kind = Text, .text = "CL001"},
                        {.field_code = FieldCode::kFortsPosReplPositionIsinId,
                         .kind = SignedInteger,
                         .signed_value = 1001},
                        {.field_code = FieldCode::kFortsPosReplPositionAccountType,
                         .kind = SignedInteger,
                         .signed_value = 1},
                        {.field_code = FieldCode::kFortsPosReplPositionXpos, .kind = SignedInteger, .signed_value = 4},
                        {.field_code = FieldCode::kFortsPosReplPositionWaprice, .kind = Text, .text = "104950.25"},
                    },
            },
            {
                .table_code = kFortsPosReplInfo,
                .rev = 6,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsPosReplInfoTradesRev,
                         .kind = SignedInteger,
                         .signed_value = 44},
                        {.field_code = FieldCode::kFortsPosReplInfoTradesLifenum,
                         .kind = SignedInteger,
                         .signed_value = 7},
                        {.field_code = FieldCode::kFortsPosReplInfoServerTime,
                         .kind = SignedInteger,
                         .signed_value = 1700000001},
                    },
            },
        };
    case kFortsUserorderbookRepl:
        return {
            {
                .table_code = kFortsUserorderbookReplOrdersCurrentday,
                .rev = 7,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayPublicOrderId,
                         .kind = SignedInteger,
                         .signed_value = 10003},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrivateOrderId,
                         .kind = SignedInteger,
                         .signed_value = 20003},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayExtId,
                         .kind = SignedInteger,
                         .signed_value = 79},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayClientCode,
                         .kind = Text,
                         .text = "CL001"},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdaySessId,
                         .kind = SignedInteger,
                         .signed_value = 321},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayIsinId,
                         .kind = SignedInteger,
                         .signed_value = 1001},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayLoginFrom,
                         .kind = Text,
                         .text = "trader_a"},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayComment,
                         .kind = Text,
                         .text = "currentday-order"},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrice,
                         .kind = Text,
                         .text = "102250"},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayPublicAmount,
                         .kind = SignedInteger,
                         .signed_value = 7},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayPublicAmountRest,
                         .kind = SignedInteger,
                         .signed_value = 7},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrivateAmount,
                         .kind = SignedInteger,
                         .signed_value = 7},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrivateAmountRest,
                         .kind = SignedInteger,
                         .signed_value = 6},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayXstatus,
                         .kind = SignedInteger,
                         .signed_value = 13},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayXstatus2,
                         .kind = SignedInteger,
                         .signed_value = 130},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayDir,
                         .kind = SignedInteger,
                         .signed_value = 2},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayPublicAction,
                         .kind = SignedInteger,
                         .signed_value = 1},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrivateAction,
                         .kind = SignedInteger,
                         .signed_value = 1},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayMoment,
                         .kind = SignedInteger,
                         .signed_value = 1700000004},
                        {.field_code = FieldCode::kFortsUserorderbookReplOrdersCurrentdayMomentNs,
                         .kind = UnsignedInteger,
                         .unsigned_value = 11},
                    },
            },
            {
                .table_code = kFortsUserorderbookReplInfoCurrentday,
                .rev = 8,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsUserorderbookReplInfoCurrentdayPublicationState,
                         .kind = SignedInteger,
                         .signed_value = 1},
                        {.field_code = FieldCode::kFortsUserorderbookReplInfoCurrentdayTradesRev,
                         .kind = SignedInteger,
                         .signed_value = 1001},
                        {.field_code = FieldCode::kFortsUserorderbookReplInfoCurrentdayTradesLifenum,
                         .kind = SignedInteger,
                         .signed_value = 7},
                        {.field_code = FieldCode::kFortsUserorderbookReplInfoCurrentdayServerTime,
                         .kind = SignedInteger,
                         .signed_value = 1700000002},
                    },
            },
        };
    case kFortsTradeRepl:
        return {
            {
                .table_code = kFortsTradeReplOrdersLog,
                .rev = 9,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogPublicOrderId,
                         .kind = SignedInteger,
                         .signed_value = 10003},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogPrivateOrderId,
                         .kind = SignedInteger,
                         .signed_value = 20003},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogExtId,
                         .kind = SignedInteger,
                         .signed_value = 79},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogClientCode, .kind = Text, .text = "CL001"},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogSessId,
                         .kind = SignedInteger,
                         .signed_value = 321},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogIsinId,
                         .kind = SignedInteger,
                         .signed_value = 1001},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogLoginFrom, .kind = Text, .text = "trader_a"},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogComment, .kind = Text, .text = "trade-delta"},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogPrice, .kind = Text, .text = "102500"},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogPublicAmount,
                         .kind = SignedInteger,
                         .signed_value = 7},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogPublicAmountRest,
                         .kind = SignedInteger,
                         .signed_value = 5},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogPrivateAmount,
                         .kind = SignedInteger,
                         .signed_value = 7},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogPrivateAmountRest,
                         .kind = SignedInteger,
                         .signed_value = 4},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogIdDeal,
                         .kind = SignedInteger,
                         .signed_value = 9001},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogXstatus,
                         .kind = SignedInteger,
                         .signed_value = 21},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogXstatus2,
                         .kind = SignedInteger,
                         .signed_value = 210},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogDir,
                         .kind = SignedInteger,
                         .signed_value = 2},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogPublicAction,
                         .kind = SignedInteger,
                         .signed_value = 2},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogPrivateAction,
                         .kind = SignedInteger,
                         .signed_value = 2},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogMoment,
                         .kind = SignedInteger,
                         .signed_value = 1700000005},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogMomentNs,
                         .kind = UnsignedInteger,
                         .unsigned_value = 12},
                    },
            },
            {
                .table_code = kFortsTradeReplUserDeal,
                .rev = 10,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsTradeReplUserDealIdDeal,
                         .kind = SignedInteger,
                         .signed_value = 9001},
                        {.field_code = FieldCode::kFortsTradeReplUserDealSessId,
                         .kind = SignedInteger,
                         .signed_value = 321},
                        {.field_code = FieldCode::kFortsTradeReplUserDealIsinId,
                         .kind = SignedInteger,
                         .signed_value = 1001},
                        {.field_code = FieldCode::kFortsTradeReplUserDealPrice, .kind = Text, .text = "102500"},
                        {.field_code = FieldCode::kFortsTradeReplUserDealXamount,
                         .kind = SignedInteger,
                         .signed_value = 2},
                        {.field_code = FieldCode::kFortsTradeReplUserDealPublicOrderIdBuy,
                         .kind = SignedInteger,
                         .signed_value = 7777},
                        {.field_code = FieldCode::kFortsTradeReplUserDealPublicOrderIdSell,
                         .kind = SignedInteger,
                         .signed_value = 10003},
                        {.field_code = FieldCode::kFortsTradeReplUserDealPrivateOrderIdBuy,
                         .kind = SignedInteger,
                         .signed_value = 0},
                        {.field_code = FieldCode::kFortsTradeReplUserDealPrivateOrderIdSell,
                         .kind = SignedInteger,
                         .signed_value = 20003},
                        {.field_code = FieldCode::kFortsTradeReplUserDealCodeBuy, .kind = Text, .text = "MM001"},
                        {.field_code = FieldCode::kFortsTradeReplUserDealCodeSell, .kind = Text, .text = "CL001"},
                        {.field_code = FieldCode::kFortsTradeReplUserDealCommentBuy,
                         .kind = Text,
                         .text = "maker-fill"},
                        {.field_code = FieldCode::kFortsTradeReplUserDealCommentSell,
                         .kind = Text,
                         .text = "trade-delta"},
                        {.field_code = FieldCode::kFortsTradeReplUserDealLoginBuy, .kind = Text, .text = "mm_bot"},
                        {.field_code = FieldCode::kFortsTradeReplUserDealLoginSell, .kind = Text, .text = "trader_a"},
                        {.field_code = FieldCode::kFortsTradeReplUserDealMoment,
                         .kind = SignedInteger,
                         .signed_value = 1700000006},
                        {.field_code = FieldCode::kFortsTradeReplUserDealMomentNs,
                         .kind = UnsignedInteger,
                         .unsigned_value = 13},
                    },
            },
            {
                .table_code = kFortsTradeReplHeartbeat,
                .rev = 11,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsTradeReplHeartbeatServerTime,
                         .kind = SignedInteger,
                         .signed_value = 1700000007},
                    },
            },
        };
    default:
        return {};
    }
}

StreamCode stream_code_from_settings(std::string_view settings) {
    for (const auto& descriptor : StreamDescriptors()) {
        if (settings.find(descriptor.stream_name) != std::string_view::npos) {
            return descriptor.stream_code;
        }
    }
    return static_cast<StreamCode>(0);
}

bool relative_scheme_path_forbidden(std::string_view settings) {
    if (std::getenv("MOEX_FAKE_CGATE_REQUIRE_ABSOLUTE_SCHEME") == nullptr) {
        return false;
    }
    return settings.find("|FILE|scheme/forts_scheme.ini|") != std::string_view::npos;
}

std::unique_ptr<OwnedScheme> build_scheme_for_messages(const std::vector<FakeMessageScript>& script,
                                                       std::vector<MessagePlan>* plans) {
    auto scheme = std::make_unique<OwnedScheme>();
    plans->clear();
    plans->reserve(script.size());

    for (std::size_t index = 0; index < script.size(); ++index) {
        const auto& message_script = script[index];
        const auto* table = FindTableByCode(message_script.table_code);
        if (table == nullptr) {
            return {};
        }

        auto message = std::make_unique<OwnedMessage>();
        message->name = std::string(table->table_name);
        message->desc.id = static_cast<std::uint32_t>(table->table_code);
        message->desc.name = message->name.data();
        message->desc.align = 1;

        MessagePlan plan;
        plan.msg_index = index;
        plan.table_code = message_script.table_code;
        plan.message_name = message->name;

        std::size_t offset = 0;
        for (const auto& scripted_field : message_script.fields) {
            const auto* field = FindFieldByCode(scripted_field.field_code);
            if (field == nullptr) {
                return {};
            }

            auto owned_field = std::make_unique<OwnedField>();
            owned_field->name = std::string(field->field_name);
            owned_field->type_token = std::string(field->type_token);
            owned_field->desc.id = static_cast<std::uint32_t>(field->field_code);
            owned_field->desc.name = owned_field->name.data();
            owned_field->desc.type = owned_field->type_token.data();
            owned_field->desc.size = size_for_value_class(field->value_class);
            owned_field->desc.offset = offset;

            plan.fields.push_back({
                .field_code = scripted_field.field_code,
                .value_class = field->value_class,
                .offset = offset,
                .size = owned_field->desc.size,
            });

            offset += owned_field->desc.size;
            message->fields.push_back(std::move(owned_field));
        }

        for (std::size_t field_index = 0; field_index < message->fields.size(); ++field_index) {
            message->fields[field_index]->desc.next =
                field_index + 1 < message->fields.size() ? &message->fields[field_index + 1]->desc : nullptr;
        }

        message->desc.size = offset;
        message->desc.num_fields = message->fields.size();
        message->desc.fields = message->fields.empty() ? nullptr : &message->fields.front()->desc;
        plan.row_size = offset;

        plans->push_back(std::move(plan));
        scheme->messages.push_back(std::move(message));
    }

    for (std::size_t index = 0; index < scheme->messages.size(); ++index) {
        scheme->messages[index]->desc.next =
            index + 1 < scheme->messages.size() ? &scheme->messages[index + 1]->desc : nullptr;
    }

    scheme->desc.num_messages = scheme->messages.size();
    scheme->desc.messages = scheme->messages.empty() ? nullptr : &scheme->messages.front()->desc;
    return scheme;
}

const MessagePlan* find_message_plan(const FakeListener& listener, TableCode table_code) {
    for (const auto& plan : listener.message_plans) {
        if (plan.table_code == table_code) {
            return &plan;
        }
    }
    return nullptr;
}

void write_field_value(std::vector<std::byte>& buffer, const FieldPlan& plan, const FakeFieldValue& value) {
    auto* dest = buffer.data() + plan.offset;
    switch (plan.value_class) {
    case ValueClass::kSignedInteger:
        write_scalar(dest, value.signed_value, plan.size);
        break;
    case ValueClass::kUnsignedInteger:
        write_scalar(dest, value.unsigned_value, plan.size);
        break;
    case ValueClass::kFixedString:
    case ValueClass::kDecimal:
    case ValueClass::kFloatingPoint: {
        std::memset(dest, 0, plan.size);
        const auto copy_size = std::min(plan.size == 0 ? 0U : plan.size - 1, value.text.size());
        if (copy_size > 0) {
            std::memcpy(dest, value.text.data(), copy_size);
        }
        break;
    }
    case ValueClass::kTimestamp: {
        const auto timestamp = make_cg_time(value.unsigned_value);
        write_scalar(dest, timestamp, plan.size);
        break;
    }
    case ValueClass::kBinary:
        std::memset(dest, 0, plan.size);
        break;
    }
}

std::vector<std::byte> encode_message_payload(const MessagePlan& plan, const FakeMessageScript& message) {
    std::vector<std::byte> buffer(plan.row_size);
    std::memset(buffer.data(), 0, buffer.size());

    for (const auto& scripted_field : message.fields) {
        for (const auto& plan_field : plan.fields) {
            if (plan_field.field_code == scripted_field.field_code) {
                write_field_value(buffer, plan_field, scripted_field);
                break;
            }
        }
    }

    return buffer;
}

std::uint32_t emit_simple_message(FakeListener& listener, std::uint32_t type, void* data = nullptr,
                                  std::size_t data_size = 0) {
    CgMsg message{
        .type = type,
        .data_size = data_size,
        .data = data,
        .owner_id = 0,
    };
    return listener.callback(listener.connection, &listener, &message, listener.callback_data);
}

std::uint32_t emit_stream_message(FakeListener& listener, const FakeMessageScript& message) {
    const auto* plan = find_message_plan(listener, message.table_code);
    if (plan == nullptr) {
        return kCgErrIncorrectState;
    }

    auto payload = encode_message_payload(*plan, message);
    CgMsgStreamData stream_data{
        .type = kCgMsgStreamData,
        .data_size = payload.size(),
        .data = payload.data(),
        .owner_id = 0,
        .msg_index = plan->msg_index,
        .msg_id = static_cast<std::uint32_t>(message.table_code),
        .msg_name = plan->message_name.c_str(),
        .rev = message.rev,
        .num_nulls = 0,
        .nulls = nullptr,
        .user_id = 0,
    };
    return listener.callback(listener.connection, &listener, &stream_data, listener.callback_data);
}

std::uint32_t emit_script(FakeListener& listener) {
    const auto script = script_for_stream(listener.stream_code);
    if (script.empty()) {
        return kCgErrOk;
    }

    const std::string replstate = "lifenum=7;rev.private_state=1";
    if (const auto result = emit_simple_message(listener, kCgMsgP2replReplState, const_cast<char*>(replstate.c_str()),
                                                replstate.size() + 1);
        result != kCgErrOk) {
        return result;
    }

    CgDataLifeNum lifenum{
        .life_number = 7,
        .flags = 0,
    };
    if (const auto result = emit_simple_message(listener, kCgMsgP2replLifenum, &lifenum, sizeof(lifenum));
        result != kCgErrOk) {
        return result;
    }

    if (const auto result = emit_simple_message(listener, kCgMsgTnBegin); result != kCgErrOk) {
        return result;
    }
    for (const auto& message : script) {
        if (const auto result = emit_stream_message(listener, message); result != kCgErrOk) {
            return result;
        }
    }
    if (const auto result = emit_simple_message(listener, kCgMsgTnCommit); result != kCgErrOk) {
        return result;
    }
    return emit_simple_message(listener, kCgMsgP2replOnline);
}

void detach_listener(FakeConnection* connection, FakeListener* listener) {
    if (connection == nullptr) {
        return;
    }
    std::erase(connection->listeners, listener);
}

} // namespace

extern "C" {

std::uint32_t cg_env_open(const char* settings) {
    if (settings == nullptr || *settings == '\0') {
        return kCgErrInvalidArgument;
    }
    g_env_open = true;
    return kCgErrOk;
}

std::uint32_t cg_env_close() {
    if (!g_env_open) {
        return kCgErrIncorrectState;
    }
    g_env_open = false;
    return kCgErrOk;
}

std::uint32_t cg_conn_new(const char* settings, void** connptr) {
    if (!g_env_open || settings == nullptr || connptr == nullptr) {
        return !g_env_open ? kCgErrIncorrectState : kCgErrInvalidArgument;
    }
    auto* connection = new FakeConnection{};
    connection->settings = settings;
    *connptr = connection;
    return kCgErrOk;
}

std::uint32_t cg_conn_destroy(void* conn) {
    if (conn == nullptr) {
        return kCgErrInvalidArgument;
    }
    auto* connection = static_cast<FakeConnection*>(conn);
    for (auto* listener : connection->listeners) {
        if (listener != nullptr) {
            listener->connection = nullptr;
        }
    }
    delete connection;
    return kCgErrOk;
}

std::uint32_t cg_conn_open(void* conn, const char*) {
    if (!g_env_open || conn == nullptr) {
        return !g_env_open ? kCgErrIncorrectState : kCgErrInvalidArgument;
    }
    auto* connection = static_cast<FakeConnection*>(conn);
    if (connection->state == kStateActive) {
        connection->state = kStateError;
        return kCgErrIncorrectState;
    }
    connection->state = kStateActive;
    connection->script_emitted = false;
    return kCgErrOk;
}

std::uint32_t cg_conn_close(void* conn) {
    if (conn == nullptr) {
        return kCgErrInvalidArgument;
    }
    auto* connection = static_cast<FakeConnection*>(conn);
    connection->state = kStateClosed;
    return kCgErrOk;
}

std::uint32_t cg_conn_process(void* conn, std::uint32_t, void*) {
    if (conn == nullptr) {
        return kCgErrInvalidArgument;
    }
    auto* connection = static_cast<FakeConnection*>(conn);
    if (connection->state != kStateActive) {
        return kCgErrIncorrectState;
    }

    if (connection->script_emitted) {
        return kCgErrTimeout;
    }

    bool emitted_any = false;
    for (auto* listener : connection->listeners) {
        if (listener == nullptr || listener->state != kStateActive || listener->script_emitted ||
            listener->callback == nullptr || listener->callback_data == nullptr) {
            continue;
        }
        const auto result = emit_script(*listener);
        if (result != kCgErrOk) {
            return result;
        }
        listener->script_emitted = true;
        emitted_any = true;
    }

    if (!emitted_any) {
        return kCgErrTimeout;
    }

    connection->script_emitted = true;
    return kCgErrOk;
}

std::uint32_t cg_conn_getstate(void* conn, std::uint32_t* state) {
    if (conn == nullptr || state == nullptr) {
        return kCgErrInvalidArgument;
    }
    *state = static_cast<FakeConnection*>(conn)->state;
    return kCgErrOk;
}

std::uint32_t cg_lsn_new(void* conn, const char* settings, CgListenerCallback callback, void* data, void** lsnptr) {
    if (!g_env_open || conn == nullptr || settings == nullptr || callback == nullptr || lsnptr == nullptr) {
        return !g_env_open ? kCgErrIncorrectState : kCgErrInvalidArgument;
    }
    if (relative_scheme_path_forbidden(settings)) {
        return kCgErrInvalidArgument;
    }

    auto* connection = static_cast<FakeConnection*>(conn);
    auto* listener = new FakeListener{};
    listener->settings = settings;
    listener->connection = connection;
    listener->callback = callback;
    listener->callback_data = data;
    listener->stream_code = stream_code_from_settings(listener->settings);
    if (FindStreamByCode(listener->stream_code) == nullptr) {
        delete listener;
        return kCgErrInvalidArgument;
    }

    const auto script = script_for_stream(listener->stream_code);
    listener->scheme = build_scheme_for_messages(script, &listener->message_plans);
    if (!listener->scheme) {
        delete listener;
        return kCgErrIncorrectState;
    }

    connection->listeners.push_back(listener);
    *lsnptr = listener;
    return kCgErrOk;
}

std::uint32_t cg_lsn_destroy(void* listener) {
    if (listener == nullptr) {
        return kCgErrInvalidArgument;
    }
    auto* typed = static_cast<FakeListener*>(listener);
    detach_listener(typed->connection, typed);
    delete typed;
    return kCgErrOk;
}

std::uint32_t cg_lsn_open(void* listener, const char*) {
    if (listener == nullptr) {
        return kCgErrInvalidArgument;
    }
    auto* typed = static_cast<FakeListener*>(listener);
    typed->state = kStateActive;
    return emit_simple_message(*typed, kCgMsgOpen);
}

std::uint32_t cg_lsn_close(void* listener) {
    if (listener == nullptr) {
        return kCgErrInvalidArgument;
    }
    auto* typed = static_cast<FakeListener*>(listener);
    typed->state = kStateClosed;
    return kCgErrOk;
}

std::uint32_t cg_lsn_getstate(void* listener, std::uint32_t* state) {
    if (listener == nullptr || state == nullptr) {
        return kCgErrInvalidArgument;
    }
    *state = static_cast<FakeListener*>(listener)->state;
    return kCgErrOk;
}

std::uint32_t cg_lsn_getscheme(void* listener, void** schemeptr) {
    if (listener == nullptr || schemeptr == nullptr) {
        return kCgErrInvalidArgument;
    }
    auto* typed = static_cast<FakeListener*>(listener);
    *schemeptr = typed->scheme == nullptr ? nullptr : &typed->scheme->desc;
    return typed->scheme == nullptr ? kCgErrIncorrectState : kCgErrOk;
}

std::uint32_t cg_getstr(const char*, const void* data, char* buffer, std::size_t* buffer_size) {
    if (data == nullptr || buffer_size == nullptr) {
        return kCgErrInvalidArgument;
    }

    const auto text = copy_c_string(static_cast<const char*>(data), 32);
    const auto required_size = text.size() + 1;
    if (buffer == nullptr || *buffer_size < required_size) {
        *buffer_size = required_size;
        return kCgErrBufferTooSmall;
    }

    std::memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    *buffer_size = required_size;
    return kCgErrOk;
}

} // extern "C"
