#include "plaza2_generated_metadata.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
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
constexpr std::uint32_t kStateError = 1;
constexpr std::uint32_t kStateOpening = 2;
constexpr std::uint32_t kStateActive = 3;

constexpr std::uint32_t kCgMsgOpen = 0x100;
constexpr std::uint32_t kCgMsgClose = 0x101;
constexpr std::uint32_t kCgMsgData = 0x110;
constexpr std::uint32_t kCgMsgStreamData = 0x120;
constexpr std::uint32_t kCgMsgTnBegin = 0x200;
constexpr std::uint32_t kCgMsgTnCommit = 0x210;
constexpr std::uint32_t kCgMsgP2MqTimeout = 0x1001;
constexpr std::uint32_t kCgMsgP2replLifenum = 0x1110;
constexpr std::uint32_t kCgMsgP2replClearDeleted = 0x1111;
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

struct CgMsgData {
    std::uint32_t type;
    std::size_t data_size;
    void* data;
    std::int64_t owner_id;
    std::size_t msg_index;
    std::uint32_t msg_id;
    const char* msg_name;
    std::uint32_t user_id;
    const char* addr;
    CgMsgData* ref_msg;
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

struct FakeReply {
    std::uint32_t message_id{0};
    std::string message_name;
    std::uint32_t user_id{0};
    std::vector<std::byte> payload;
    bool timed_out{false};
};

using CgListenerCallback = std::uint32_t (*)(void* conn, void* listener, void* msg, void* data);

struct FakeListener {
    std::uint32_t state{kStateClosed};
    std::string settings;
    FakeConnection* connection{nullptr};
    CgListenerCallback callback{nullptr};
    void* callback_data{nullptr};
    StreamCode stream_code{};
    bool reply_listener{false};
    std::unique_ptr<OwnedScheme> scheme;
    std::vector<MessagePlan> message_plans;
    bool script_emitted{false};
    std::uint32_t open_attempt_count{0};
};

struct FakeConnection {
    std::uint32_t state{kStateClosed};
    std::string settings;
    std::vector<FakeListener*> listeners;
    std::vector<FakeReply> pending_replies;
    bool script_emitted{false};
    bool liveness_event_emitted{false};
    bool userbook_periodic_clear_emitted{false};
    bool userbook_periodic_info_emitted{false};
    bool pos_anchor_drift_emitted{false};
    bool trade_open_error_seen{false};
};

struct FakePublisher {
    std::uint32_t state{kStateClosed};
    FakeConnection* connection{nullptr};
    std::string settings;
};

struct FakePublisherMessage {
    CgMsgData message{};
    std::string name;
    std::vector<std::byte> payload;
};

bool g_env_open = false;
bool g_cancel_after_cleanup = false;
std::unordered_map<void*, FakePublisherMessage*> g_publisher_messages;

std::uint32_t configured_result(const char* variable) {
    const auto* value = std::getenv(variable);
    if (value == nullptr || *value == '\0' || std::string_view(value) == "ok") {
        return kCgErrOk;
    }
    if (std::string_view(value) == "timeout") {
        return kCgErrTimeout;
    }
    if (std::string_view(value) == "invalid") {
        return kCgErrInvalidArgument;
    }
    return kCgRangeBegin;
}

bool configured_reply_timeout() {
    const auto* value = std::getenv("MOEX_FAKE_PUB_REPLY_MODE");
    return value != nullptr && std::string_view(value) == "timeout";
}

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

std::size_t publisher_payload_size(std::string_view message_name) {
    // Sizes are the reviewed generated TEST command layouts used by the
    // offline trade codec golden fixtures. The fake runtime only models these
    // three command families; it never accepts a live broker payload.
    if (message_name == "AddOrder") {
        return 112;
    }
    if (message_name == "DelOrder") {
        return 20;
    }
    if (message_name == "DelUserOrders") {
        return 49;
    }
    return 8;
}

template <typename T> void write_reply_scalar(std::vector<std::byte>& payload, std::size_t offset, T value) {
    if (offset + sizeof(T) <= payload.size()) {
        std::memcpy(payload.data() + offset, &value, sizeof(T));
    }
}

std::vector<std::byte> make_trade_reply(std::string_view message_name) {
    const bool add = message_name == "AddOrder";
    const bool recovery = message_name == "DelUserOrders";
    std::vector<std::byte> payload(4 + 255 + (add ? 8 : 4));
    const auto* code_text = std::getenv("MOEX_FAKE_PUB_REPLY_CODE");
    const auto code = code_text != nullptr && std::string_view(code_text) == "reject" ? 1 : 0;
    write_reply_scalar(payload, 0, static_cast<std::int32_t>(code));
    const std::string message = code == 0 ? "OK" : "REJECTED";
    std::memcpy(payload.data() + 4, message.data(), std::min<std::size_t>(message.size(), 254));
    if (add) {
        const auto* id_text = std::getenv("MOEX_FAKE_PUB_REPLY_ORDER_ID");
        const auto order_id = id_text == nullptr ? std::int64_t{20003} : std::strtoll(id_text, nullptr, 10);
        write_reply_scalar(payload, 4 + 255, order_id);
    } else {
        write_reply_scalar(payload, 4 + 255, static_cast<std::int32_t>(recovery ? 1 : 0));
    }
    return payload;
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

std::vector<FakeMessageScript> base_script_for_stream(StreamCode stream_code) {
    using enum FakeValueKind;
    using enum StreamCode;
    using enum TableCode;

    switch (stream_code) {
    case kFortsAggrRepl:
        return {
            {
                .table_code = kFortsAggrReplOrdersAggr,
                .rev = 21,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrReplId,
                         .kind = SignedInteger,
                         .signed_value = 2101},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrReplRev,
                         .kind = SignedInteger,
                         .signed_value = 21},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrReplAct,
                         .kind = SignedInteger,
                         .signed_value = 0},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrIsinId,
                         .kind = SignedInteger,
                         .signed_value = 1001},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrPrice, .kind = Text, .text = "102500"},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrVolume,
                         .kind = SignedInteger,
                         .signed_value = 7},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrMoment,
                         .kind = SignedInteger,
                         .signed_value = 1700000010},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrMomentNs,
                         .kind = UnsignedInteger,
                         .unsigned_value = 101},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrDir,
                         .kind = SignedInteger,
                         .signed_value = 1},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrSynthVolume, .kind = Text, .text = "0"},
                    },
            },
            {
                .table_code = kFortsAggrReplOrdersAggr,
                .rev = 22,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrReplId,
                         .kind = SignedInteger,
                         .signed_value = 2102},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrReplRev,
                         .kind = SignedInteger,
                         .signed_value = 22},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrReplAct,
                         .kind = SignedInteger,
                         .signed_value = 0},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrIsinId,
                         .kind = SignedInteger,
                         .signed_value = 1001},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrPrice, .kind = Text, .text = "102750"},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrVolume,
                         .kind = SignedInteger,
                         .signed_value = 5},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrMoment,
                         .kind = SignedInteger,
                         .signed_value = 1700000011},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrMomentNs,
                         .kind = UnsignedInteger,
                         .unsigned_value = 102},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrDir,
                         .kind = SignedInteger,
                         .signed_value = 2},
                        {.field_code = FieldCode::kFortsAggrReplOrdersAggrSynthVolume, .kind = Text, .text = "0"},
                    },
            },
        };
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
                         .signed_value = 1},
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
                        {.field_code = FieldCode::kFortsRefdataReplFutInstrumentsMinStep, .kind = Text, .text = "250"},
                        {.field_code = FieldCode::kFortsRefdataReplFutInstrumentsStepPrice,
                         .kind = Text,
                         .text = "12.5"},
                        {.field_code = FieldCode::kFortsRefdataReplFutInstrumentsSettlementPrice,
                         .kind = Text,
                         .text = "105000.5"},
                    },
            },
            {
                .table_code = kFortsRefdataReplFutSessContents,
                .rev = 3,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsReplId,
                         .kind = UnsignedInteger,
                         .unsigned_value = 3101},
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsReplRev,
                         .kind = SignedInteger,
                         .signed_value = 3},
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsReplAct,
                         .kind = SignedInteger,
                         .signed_value = 0},
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsSessId,
                         .kind = SignedInteger,
                         .signed_value = 321},
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsIsinId,
                         .kind = SignedInteger,
                         .signed_value = 1001},
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsShortIsin,
                         .kind = Text,
                         .text = "RIH6"},
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsIsin,
                         .kind = Text,
                         .text = "RTS-6.26"},
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsName,
                         .kind = Text,
                         .text = "RTS Jun 2026"},
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsBaseContractCode,
                         .kind = Text,
                         .text = "RTS"},
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsRoundto,
                         .kind = SignedInteger,
                         .signed_value = 2},
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsMinStep, .kind = Text, .text = "250"},
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsLotVolume,
                         .kind = SignedInteger,
                         .signed_value = 1},
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsTradeModeId,
                         .kind = SignedInteger,
                         .signed_value = 4},
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsState,
                         .kind = SignedInteger,
                         .signed_value = 1},
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsGroupMask,
                         .kind = SignedInteger,
                         .signed_value = 1},
                        {.field_code = FieldCode::kFortsRefdataReplFutSessContentsTradePeriodAccess,
                         .kind = SignedInteger,
                         .signed_value = 1},
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
    case kFortsSessionstateRepl:
        return {
            {
                .table_code = kFortsSessionstateReplSessionState,
                .rev = 1,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsSessionstateReplSessionStateReplId,
                         .kind = UnsignedInteger,
                         .unsigned_value = 4101},
                        {.field_code = FieldCode::kFortsSessionstateReplSessionStateReplRev,
                         .kind = SignedInteger,
                         .signed_value = 1},
                        {.field_code = FieldCode::kFortsSessionstateReplSessionStateReplAct,
                         .kind = SignedInteger,
                         .signed_value = 0},
                        {.field_code = FieldCode::kFortsSessionstateReplSessionStateSessId,
                         .kind = SignedInteger,
                         .signed_value = 321},
                        {.field_code = FieldCode::kFortsSessionstateReplSessionStatePublicState,
                         .kind = SignedInteger,
                         .signed_value = 1},
                    },
            },
        };
    case kFortsInstrumentstateRepl:
        return {
            {
                .table_code = kFortsInstrumentstateReplInstrumentState,
                .rev = 1,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsInstrumentstateReplInstrumentStateReplId,
                         .kind = UnsignedInteger,
                         .unsigned_value = 4201},
                        {.field_code = FieldCode::kFortsInstrumentstateReplInstrumentStateReplRev,
                         .kind = SignedInteger,
                         .signed_value = 1},
                        {.field_code = FieldCode::kFortsInstrumentstateReplInstrumentStateReplAct,
                         .kind = SignedInteger,
                         .signed_value = 0},
                        {.field_code = FieldCode::kFortsInstrumentstateReplInstrumentStateIsinId,
                         .kind = SignedInteger,
                         .signed_value = 1001},
                        {.field_code = FieldCode::kFortsInstrumentstateReplInstrumentStatePublicState,
                         .kind = SignedInteger,
                         .signed_value = 1},
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
                         .signed_value = 2},
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
                         .kind = Timestamp,
                         .unsigned_value = 1700000001},
                    },
            },
        };
    case kFortsUserorderbookRepl:
        return {
            {
                .table_code = kFortsUserorderbookReplInfo,
                .rev = 6,
                .fields =
                    {
                        {.field_code = FieldCode::kFortsUserorderbookReplInfoPublicationState,
                         .kind = SignedInteger,
                         .signed_value = 1},
                        {.field_code = FieldCode::kFortsUserorderbookReplInfoTradesRev,
                         .kind = SignedInteger,
                         .signed_value = 1001},
                        {.field_code = FieldCode::kFortsUserorderbookReplInfoTradesLifenum,
                         .kind = SignedInteger,
                         .signed_value = 7},
                        {.field_code = FieldCode::kFortsUserorderbookReplInfoMoment,
                         .kind = Timestamp,
                         .unsigned_value = 1700000002},
                    },
            },
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
                         .kind = Timestamp,
                         .unsigned_value = 1700000007},
                    },
            },
        };
    default:
        return {};
    }
}

const FakeFieldValue* find_field(const FakeMessageScript& message, FieldCode code) {
    for (const auto& field : message.fields) {
        if (field.field_code == code) {
            return &field;
        }
    }
    return nullptr;
}

FakeFieldValue* find_field(FakeMessageScript& message, FieldCode code) {
    for (auto& field : message.fields) {
        if (field.field_code == code) {
            return &field;
        }
    }
    return nullptr;
}

bool fake_flag(const char* name) {
    const auto* value = std::getenv(name);
    return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

std::vector<FakeMessageScript> script_for_stream(StreamCode stream_code) {
    auto script = base_script_for_stream(stream_code);
    using enum FieldCode;
    using enum TableCode;

    if (stream_code == StreamCode::kFortsTradeRepl) {
        if (fake_flag("MOEX_FAKE_FLAT_TRADE_REPLAY")) {
            std::erase_if(script, [](const auto& message) {
                return message.table_code == kFortsTradeReplUserDeal ||
                       message.table_code == kFortsTradeReplUserMultilegDeal;
            });
        }
        // Lifecycle evidence is sourced from TRADE orders_log. Keep the
        // concrete fake's fill/cancel transitions on that surface; the
        // USERORDERBOOK flags below independently exercise the TEST census.
        if (fake_flag("MOEX_FAKE_FULL_FILL") || fake_flag("MOEX_FAKE_CANCELLED_ORDER") || g_cancel_after_cleanup) {
            for (auto& message : script) {
                if (message.table_code != kFortsTradeReplOrdersLog &&
                    message.table_code != kFortsTradeReplMultilegOrdersLog) {
                    continue;
                }
                const auto public_rest_field = message.table_code == kFortsTradeReplMultilegOrdersLog
                                                   ? kFortsTradeReplMultilegOrdersLogPublicAmountRest
                                                   : kFortsTradeReplOrdersLogPublicAmountRest;
                const auto private_rest_field = message.table_code == kFortsTradeReplMultilegOrdersLog
                                                    ? kFortsTradeReplMultilegOrdersLogPrivateAmountRest
                                                    : kFortsTradeReplOrdersLogPrivateAmountRest;
                const auto public_action_field = message.table_code == kFortsTradeReplMultilegOrdersLog
                                                     ? kFortsTradeReplMultilegOrdersLogPublicAction
                                                     : kFortsTradeReplOrdersLogPublicAction;
                const auto private_action_field = message.table_code == kFortsTradeReplMultilegOrdersLog
                                                      ? kFortsTradeReplMultilegOrdersLogPrivateAction
                                                      : kFortsTradeReplOrdersLogPrivateAction;
                if (auto* public_rest = find_field(message, public_rest_field)) {
                    public_rest->signed_value = 0;
                }
                if (auto* private_rest = find_field(message, private_rest_field)) {
                    private_rest->signed_value = 0;
                }
                const auto action = fake_flag("MOEX_FAKE_FULL_FILL") ? 2 : 0;
                if (auto* public_action = find_field(message, public_action_field)) {
                    public_action->signed_value = action;
                }
                if (auto* private_action = find_field(message, private_action_field)) {
                    private_action->signed_value = action;
                }
            }
        }
        if (fake_flag("MOEX_FAKE_TRADE_IDENTITY_CONFLICT")) {
            for (std::size_t index = 0; index < script.size(); ++index) {
                const auto source = script[index];
                if (source.table_code != kFortsTradeReplOrdersLog &&
                    source.table_code != kFortsTradeReplMultilegOrdersLog) {
                    continue;
                }
                auto conflicting = source;
                const auto private_id_field = source.table_code == kFortsTradeReplMultilegOrdersLog
                                                  ? kFortsTradeReplMultilegOrdersLogPrivateOrderId
                                                  : kFortsTradeReplOrdersLogPrivateOrderId;
                if (auto* private_id = find_field(conflicting, private_id_field)) {
                    private_id->signed_value = 29999;
                }
                script.push_back(std::move(conflicting));
                break;
            }
        }
    }

    if (stream_code == StreamCode::kFortsAggrRepl) {
        if (fake_flag("MOEX_FAKE_AGGR_ONE_SIDED")) {
            std::erase_if(script, [](const auto& message) {
                const auto* direction = find_field(message, kFortsAggrReplOrdersAggrDir);
                return direction != nullptr && direction->signed_value == 2;
            });
        }
        if (fake_flag("MOEX_FAKE_AGGR_MULTI_INSTRUMENT")) {
            const auto original = script;
            for (const auto& source : original) {
                FakeMessageScript other = source;
                if (auto* isin = find_field(other, kFortsAggrReplOrdersAggrIsinId)) {
                    isin->signed_value = 2002;
                }
                if (auto* price = find_field(other, kFortsAggrReplOrdersAggrPrice)) {
                    price->text =
                        find_field(source, kFortsAggrReplOrdersAggrDir)->signed_value == 1 ? "10000000" : "10001000";
                }
                if (auto* repl = find_field(other, kFortsAggrReplOrdersAggrReplId)) {
                    repl->unsigned_value += 100;
                }
                script.push_back(std::move(other));
            }
        }
    } else if (stream_code == StreamCode::kFortsRefdataRepl) {
        if (fake_flag("MOEX_FAKE_MISSING_INSTRUMENT")) {
            std::erase_if(script, [](const auto& message) {
                return message.table_code == kFortsRefdataReplFutInstruments ||
                       message.table_code == kFortsRefdataReplFutSessContents;
            });
        }
    } else if (stream_code == StreamCode::kFortsSessionstateRepl) {
        if (fake_flag("MOEX_FAKE_MISSING_SESSION")) {
            for (auto& message : script) {
                if (auto* sess = find_field(message, kFortsSessionstateReplSessionStateSessId)) {
                    sess->signed_value = 999;
                }
            }
        }
        if (fake_flag("MOEX_FAKE_SCHEDULED_SESSION") || fake_flag("MOEX_FAKE_NONTRADABLE_SESSION")) {
            for (auto& message : script) {
                if (auto* state = find_field(message, kFortsSessionstateReplSessionStatePublicState)) {
                    state->signed_value = 0;
                }
            }
        } else if (fake_flag("MOEX_FAKE_SUSPENDED_SESSION")) {
            for (auto& message : script) {
                if (auto* state = find_field(message, kFortsSessionstateReplSessionStatePublicState)) {
                    state->signed_value = 2;
                }
            }
        } else if (fake_flag("MOEX_FAKE_COMPLETED_SESSION")) {
            for (auto& message : script) {
                if (auto* state = find_field(message, kFortsSessionstateReplSessionStatePublicState)) {
                    state->signed_value = 4;
                }
            }
        }
    } else if (stream_code == StreamCode::kFortsInstrumentstateRepl) {
        if (fake_flag("MOEX_FAKE_MISSING_INSTRUMENT")) {
            for (auto& message : script) {
                if (auto* isin = find_field(message, kFortsInstrumentstateReplInstrumentStateIsinId)) {
                    isin->signed_value = 999999;
                }
            }
        }
        if (fake_flag("MOEX_FAKE_NONTRADABLE_INSTRUMENT")) {
            for (auto& message : script) {
                if (auto* state = find_field(message, kFortsInstrumentstateReplInstrumentStatePublicState)) {
                    state->signed_value = 0;
                }
            }
        }
    } else if (stream_code == StreamCode::kFortsPartRepl) {
        if (fake_flag("MOEX_FAKE_MISSING_LIMITS")) {
            for (auto& message : script) {
                if (auto* limits_set = find_field(message, kFortsPartReplPartLimitsSet)) {
                    limits_set->signed_value = 0;
                }
            }
        }
        if (fake_flag("MOEX_FAKE_WRONG_LIMIT_CLIENT")) {
            for (auto& message : script) {
                if (auto* client = find_field(message, kFortsPartReplPartClientCode)) {
                    client->text = "OTHER";
                }
            }
        }
    } else if (stream_code == StreamCode::kFortsPosRepl) {
        if (fake_flag("MOEX_FAKE_MISSING_POSITION")) {
            std::erase_if(script, [](const auto& message) { return message.table_code == kFortsPosReplPosition; });
        } else if (fake_flag("MOEX_FAKE_ZERO_POSITION")) {
            for (auto& message : script) {
                if (auto* position = find_field(message, kFortsPosReplPositionXpos)) {
                    position->signed_value = 0;
                }
            }
        }
        if (fake_flag("MOEX_FAKE_WRONG_POSITION_ACCOUNT_TYPE")) {
            for (auto& message : script) {
                if (auto* account_type = find_field(message, kFortsPosReplPositionAccountType)) {
                    account_type->signed_value = 1;
                }
            }
        }
    } else if (stream_code == StreamCode::kFortsUserorderbookRepl) {
        if (fake_flag("MOEX_FAKE_MISSING_ORDER")) {
            std::erase_if(script, [](const auto& message) {
                return message.table_code == kFortsUserorderbookReplOrdersCurrentday;
            });
        }
        if (fake_flag("MOEX_FAKE_ACTIVE_ORDER_ALT_EXT_ID")) {
            for (auto& message : script) {
                if (message.table_code != kFortsUserorderbookReplOrdersCurrentday) {
                    continue;
                }
                if (auto* ext_id = find_field(message, kFortsUserorderbookReplOrdersCurrentdayExtId)) {
                    ext_id->signed_value = 877;
                }
            }
        }
        if (fake_flag("MOEX_FAKE_FULL_FILL") || fake_flag("MOEX_FAKE_CANCELLED_ORDER") || g_cancel_after_cleanup) {
            for (auto& message : script) {
                if (message.table_code != kFortsUserorderbookReplOrdersCurrentday) {
                    continue;
                }
                if (auto* public_rest = find_field(message, kFortsUserorderbookReplOrdersCurrentdayPublicAmountRest)) {
                    public_rest->signed_value = 0;
                }
                if (auto* private_rest =
                        find_field(message, kFortsUserorderbookReplOrdersCurrentdayPrivateAmountRest)) {
                    private_rest->signed_value = 0;
                }
                const auto action = fake_flag("MOEX_FAKE_FULL_FILL") ? 2 : 0;
                if (auto* public_action = find_field(message, kFortsUserorderbookReplOrdersCurrentdayPublicAction)) {
                    public_action->signed_value = action;
                }
                if (auto* private_action = find_field(message, kFortsUserorderbookReplOrdersCurrentdayPrivateAction)) {
                    private_action->signed_value = action;
                }
            }
        }
        if (fake_flag("MOEX_FAKE_IDENTITY_CONFLICT")) {
            for (std::size_t index = 0; index < script.size(); ++index) {
                const auto source = script[index];
                if (source.table_code != kFortsUserorderbookReplOrdersCurrentday) {
                    continue;
                }
                auto conflicting = source;
                if (auto* private_id = find_field(conflicting, kFortsUserorderbookReplOrdersCurrentdayPrivateOrderId)) {
                    private_id->signed_value = 29999;
                }
                script.push_back(std::move(conflicting));
                break;
            }
        }
        if (fake_flag("MOEX_FAKE_USERORDERBOOK_PERIODIC_REFRESH")) {
            // Keep regular table descriptors in the negotiated scheme so the
            // refresh can address them by table index without adding initial
            // rows to the ordinary fixture replay.
            script.push_back({.table_code = kFortsUserorderbookReplOrders, .rev = 9});
            script.push_back({.table_code = kFortsUserorderbookReplMultilegOrders, .rev = 10});
        }
    }
    if (const auto* client_override = std::getenv("MOEX_FAKE_CLIENT_CODE"); client_override != nullptr) {
        const std::string replacement(client_override);
        const std::array client_fields = {
            kFortsPartReplPartClientCode,
            kFortsPosReplPositionClientCode,
            kFortsUserorderbookReplOrdersCurrentdayClientCode,
            kFortsTradeReplOrdersLogClientCode,
            kFortsTradeReplUserDealCodeBuy,
            kFortsTradeReplUserDealCodeSell,
        };
        for (auto& message : script) {
            for (const auto field_code : client_fields) {
                if (auto* field = find_field(message, field_code)) {
                    if (fake_flag("MOEX_FAKE_WRONG_LIMIT_CLIENT") && field_code == kFortsPartReplPartClientCode) {
                        continue;
                    }
                    field->text = replacement;
                }
            }
        }
    }
    return script;
}

StreamCode stream_code_from_settings(std::string_view settings) {
    if (settings.find(std::string_view{"FORTS_AGGR5_REPL"}) != std::string_view::npos ||
        settings.find(std::string_view{"FORTS_AGGR20_REPL"}) != std::string_view::npos ||
        settings.find(std::string_view{"FORTS_AGGR50_REPL"}) != std::string_view::npos) {
        return StreamCode::kFortsAggrRepl;
    }
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
    return settings.find(std::string_view{"|FILE|scheme/forts_scheme.ini|"}) != std::string_view::npos;
}

bool emit_clear_deleted_inside_transaction() {
    const auto* value = std::getenv("MOEX_FAKE_CGATE_CLEAR_DELETED_INSIDE_TRANSACTION");
    return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

bool emit_unknown_clear_deleted_table() {
    const auto* value = std::getenv("MOEX_FAKE_CGATE_CLEAR_DELETED_UNKNOWN_TABLE");
    return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

std::array<std::byte, sizeof(std::uint32_t) + sizeof(std::int64_t) + sizeof(std::uint32_t)>
make_clear_deleted_payload(std::uint32_t table_idx, std::int64_t table_rev, std::uint32_t flags) {
    std::array<std::byte, sizeof(std::uint32_t) + sizeof(std::int64_t) + sizeof(std::uint32_t)> payload{};
    std::memcpy(payload.data(), &table_idx, sizeof(table_idx));
    std::memcpy(payload.data() + sizeof(table_idx), &table_rev, sizeof(table_rev));
    std::memcpy(payload.data() + sizeof(table_idx) + sizeof(table_rev), &flags, sizeof(flags));
    return payload;
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

std::uint32_t emit_reply_message(FakeListener& listener, const FakeReply& reply) {
    CgMsgData message{
        .type = reply.timed_out ? kCgMsgP2MqTimeout : kCgMsgData,
        .data_size = reply.payload.size(),
        .data = const_cast<std::byte*>(reply.payload.data()),
        .owner_id = 0,
        .msg_index = 0,
        .msg_id = reply.message_id,
        .msg_name = reply.message_name.c_str(),
        .user_id = reply.user_id,
        .addr = nullptr,
        .ref_msg = nullptr,
    };
    return listener.callback(listener.connection, &listener, &message, listener.callback_data);
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
    if (emit_clear_deleted_inside_transaction() && !listener.message_plans.empty()) {
        auto clear_deleted =
            make_clear_deleted_payload(emit_unknown_clear_deleted_table() ? 999U : 0U, script.front().rev, 0);
        if (const auto result =
                emit_simple_message(listener, kCgMsgP2replClearDeleted, clear_deleted.data(), clear_deleted.size());
            result != kCgErrOk) {
            return result;
        }
    }
    for (const auto& message : script) {
        if (fake_flag("MOEX_FAKE_USERORDERBOOK_PERIODIC_REFRESH") &&
            (message.table_code == TableCode::kFortsUserorderbookReplOrders ||
             message.table_code == TableCode::kFortsUserorderbookReplMultilegOrders)) {
            continue;
        }
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

const char* moex_fake_cgate_runtime_v1() {
    return "moex_fake_cgate_runtime_v1";
}

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
    g_cancel_after_cleanup = false;
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
    connection->liveness_event_emitted = false;
    connection->userbook_periodic_clear_emitted = false;
    connection->userbook_periodic_info_emitted = false;
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

    if (fake_flag("MOEX_FAKE_TRADE_OPEN_ERROR_POS_DRIFT") && !connection->pos_anchor_drift_emitted) {
        FakeListener* pos_listener = nullptr;
        bool trade_open_failed = connection->trade_open_error_seen;
        for (auto* listener : connection->listeners) {
            if (listener == nullptr || listener->reply_listener) {
                continue;
            }
            if (listener->stream_code == StreamCode::kFortsPosRepl && listener->state == kStateActive) {
                pos_listener = listener;
            } else if (listener->stream_code == StreamCode::kFortsTradeRepl && listener->state == kStateError) {
                trade_open_failed = true;
            }
        }
        if (pos_listener != nullptr && trade_open_failed) {
            const auto script = script_for_stream(StreamCode::kFortsPosRepl);
            const auto info = std::ranges::find_if(
                script, [](const auto& message) { return message.table_code == TableCode::kFortsPosReplInfo; });
            if (info == script.end()) {
                return kCgErrIncorrectState;
            }
            auto drifted = *info;
            if (auto* rev = find_field(drifted, FieldCode::kFortsPosReplInfoTradesRev)) {
                rev->signed_value = 45;
            }
            if (const auto result = emit_simple_message(*pos_listener, kCgMsgTnBegin); result != kCgErrOk) {
                return result;
            }
            if (const auto result = emit_stream_message(*pos_listener, drifted); result != kCgErrOk) {
                return result;
            }
            if (const auto result = emit_simple_message(*pos_listener, kCgMsgTnCommit); result != kCgErrOk) {
                return result;
            }
            connection->pos_anchor_drift_emitted = true;
            return kCgErrOk;
        }
    }

    if (connection->script_emitted && connection->pending_replies.empty() &&
        fake_flag("MOEX_FAKE_USERORDERBOOK_PERIODIC_REFRESH")) {
        FakeListener* userbook = nullptr;
        for (auto* listener : connection->listeners) {
            if (listener != nullptr && !listener->reply_listener && listener->state == kStateActive &&
                listener->stream_code == StreamCode::kFortsUserorderbookRepl) {
                userbook = listener;
                break;
            }
        }
        if (userbook == nullptr) {
            return kCgErrIncorrectState;
        }
        if (!connection->userbook_periodic_clear_emitted) {
            for (const auto table_code :
                 {TableCode::kFortsUserorderbookReplOrders, TableCode::kFortsUserorderbookReplMultilegOrders}) {
                const auto* plan = find_message_plan(*userbook, table_code);
                if (plan == nullptr) {
                    return kCgErrIncorrectState;
                }
                auto clear_deleted = make_clear_deleted_payload(static_cast<std::uint32_t>(plan->msg_index), 9, 0);
                if (const auto result = emit_simple_message(*userbook, kCgMsgP2replClearDeleted, clear_deleted.data(),
                                                            clear_deleted.size());
                    result != kCgErrOk) {
                    return result;
                }
            }
            connection->userbook_periodic_clear_emitted = true;
            return kCgErrOk;
        }
        if (!connection->userbook_periodic_info_emitted) {
            const auto script = script_for_stream(StreamCode::kFortsUserorderbookRepl);
            const auto info = std::ranges::find_if(script, [](const auto& message) {
                return message.table_code == TableCode::kFortsUserorderbookReplInfo;
            });
            if (info == script.end()) {
                return kCgErrIncorrectState;
            }
            if (const auto result = emit_simple_message(*userbook, kCgMsgTnBegin); result != kCgErrOk) {
                return result;
            }
            if (const auto result = emit_stream_message(*userbook, *info); result != kCgErrOk) {
                return result;
            }
            if (const auto result = emit_simple_message(*userbook, kCgMsgTnCommit); result != kCgErrOk) {
                return result;
            }
            connection->userbook_periodic_info_emitted = true;
            connection->liveness_event_emitted = true;
            return kCgErrOk;
        }
    }

    if (connection->script_emitted && connection->pending_replies.empty() && !connection->liveness_event_emitted) {
        if (fake_flag("MOEX_FAKE_REMOVE_TARGET_AFTER_READY")) {
            for (auto* listener : connection->listeners) {
                if (listener == nullptr || listener->reply_listener || listener->state != kStateActive ||
                    listener->stream_code != StreamCode::kFortsRefdataRepl) {
                    continue;
                }
                const auto* plan = find_message_plan(*listener, TableCode::kFortsRefdataReplFutSessContents);
                if (plan == nullptr) {
                    return kCgErrIncorrectState;
                }
                auto clear_deleted = make_clear_deleted_payload(static_cast<std::uint32_t>(plan->msg_index),
                                                                std::numeric_limits<std::int64_t>::max(), 0);
                if (const auto result = emit_simple_message(*listener, kCgMsgP2replClearDeleted, clear_deleted.data(),
                                                            clear_deleted.size());
                    result != kCgErrOk) {
                    return result;
                }
                if (const auto result = emit_simple_message(*listener, kCgMsgTnBegin); result != kCgErrOk) {
                    return result;
                }
                if (const auto result = emit_simple_message(*listener, kCgMsgTnCommit); result != kCgErrOk) {
                    return result;
                }
            }
            connection->liveness_event_emitted = true;
            return kCgErrOk;
        }
        const bool private_liveness =
            fake_flag("MOEX_FAKE_PRIVATE_CLOSE_AFTER_READY") || fake_flag("MOEX_FAKE_PRIVATE_LIFENUM_AFTER_READY");
        const bool aggr_liveness =
            fake_flag("MOEX_FAKE_AGGR_CLOSE_AFTER_READY") || fake_flag("MOEX_FAKE_AGGR_LIFENUM_AFTER_READY");
        if (private_liveness || aggr_liveness) {
            for (auto* listener : connection->listeners) {
                if (listener == nullptr || listener->reply_listener || listener->state != kStateActive) {
                    continue;
                }
                const bool is_aggr = listener->stream_code == StreamCode::kFortsAggrRepl;
                if ((is_aggr && !aggr_liveness) || (!is_aggr && !private_liveness)) {
                    continue;
                }
                if ((is_aggr && fake_flag("MOEX_FAKE_AGGR_CLOSE_AFTER_READY")) ||
                    (!is_aggr && fake_flag("MOEX_FAKE_PRIVATE_CLOSE_AFTER_READY"))) {
                    if (const auto result = emit_simple_message(*listener, kCgMsgClose); result != kCgErrOk) {
                        return result;
                    }
                } else {
                    CgDataLifeNum life_num{.life_number = 8, .flags = 0};
                    if (const auto result =
                            emit_simple_message(*listener, kCgMsgP2replLifenum, &life_num, sizeof(life_num));
                        result != kCgErrOk) {
                        return result;
                    }
                }
            }
            connection->liveness_event_emitted = true;
            return kCgErrOk;
        }
    }
    if (connection->script_emitted && connection->pending_replies.empty()) {
        const bool pending_new_listener =
            std::any_of(connection->listeners.begin(), connection->listeners.end(), [](const auto* listener) {
                return listener != nullptr && !listener->reply_listener && listener->state == kStateActive &&
                       !listener->script_emitted;
            });
        if (!pending_new_listener) {
            return kCgErrTimeout;
        }
    }

    bool emitted_any = false;
    const auto emit_listener = [&](FakeListener* listener) -> std::uint32_t {
        if (listener == nullptr || listener->state != kStateActive ||
            (!listener->reply_listener && listener->script_emitted) || listener->callback == nullptr ||
            listener->callback_data == nullptr) {
            return kCgErrOk;
        }
        if (listener->reply_listener) {
            for (const auto& reply : connection->pending_replies) {
                if (const auto result = emit_reply_message(*listener, reply); result != kCgErrOk) {
                    return result;
                }
                emitted_any = true;
            }
            return kCgErrOk;
        }
        const auto result = emit_script(*listener);
        if (result != kCgErrOk) {
            return result;
        }
        listener->script_emitted = true;
        emitted_any = true;
        return kCgErrOk;
    };
    const auto reply_first = fake_flag("MOEX_FAKE_REPLY_BEFORE_REPLICATION");
    for (const auto pass_reply : {reply_first, !reply_first}) {
        for (auto* listener : connection->listeners) {
            if (listener != nullptr && listener->reply_listener == pass_reply) {
                if (const auto result = emit_listener(listener); result != kCgErrOk) {
                    return result;
                }
            }
        }
    }

    if (fake_flag("MOEX_FAKE_POS_ANCHOR_DRIFT") && !connection->pos_anchor_drift_emitted) {
        FakeListener* pos_listener = nullptr;
        bool trade_replay_emitted = false;
        for (auto* listener : connection->listeners) {
            if (listener == nullptr || listener->reply_listener || listener->state != kStateActive) {
                continue;
            }
            if (listener->stream_code == StreamCode::kFortsPosRepl) {
                pos_listener = listener;
            } else if (listener->stream_code == StreamCode::kFortsTradeRepl && listener->script_emitted) {
                trade_replay_emitted = true;
            }
        }
        if (pos_listener != nullptr && trade_replay_emitted) {
            const auto script = script_for_stream(StreamCode::kFortsPosRepl);
            const auto info = std::ranges::find_if(
                script, [](const auto& message) { return message.table_code == TableCode::kFortsPosReplInfo; });
            if (info == script.end()) {
                return kCgErrIncorrectState;
            }
            auto drifted = *info;
            const auto* drift_kind = std::getenv("MOEX_FAKE_POS_ANCHOR_DRIFT");
            if (drift_kind != nullptr && std::string_view(drift_kind) == "lifenum") {
                if (auto* lifenum = find_field(drifted, FieldCode::kFortsPosReplInfoTradesLifenum)) {
                    lifenum->signed_value = 8;
                }
            } else if (auto* rev = find_field(drifted, FieldCode::kFortsPosReplInfoTradesRev)) {
                rev->signed_value = 45;
            }
            if (const auto result = emit_simple_message(*pos_listener, kCgMsgTnBegin); result != kCgErrOk) {
                return result;
            }
            if (const auto result = emit_stream_message(*pos_listener, drifted); result != kCgErrOk) {
                return result;
            }
            if (const auto result = emit_simple_message(*pos_listener, kCgMsgTnCommit); result != kCgErrOk) {
                return result;
            }
            connection->pos_anchor_drift_emitted = true;
            emitted_any = true;
        }
    }

    if (!emitted_any) {
        return kCgErrTimeout;
    }

    connection->pending_replies.clear();
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
    if (std::string_view(settings).find(std::string_view{"p2mqreply://"}) != std::string_view::npos &&
        fake_flag("MOEX_FAKE_DISABLE_REPLY_LISTENER")) {
        return kCgErrInvalidArgument;
    }

    auto* connection = static_cast<FakeConnection*>(conn);
    auto* listener = new FakeListener{};
    listener->settings = settings;
    listener->connection = connection;
    listener->callback = callback;
    listener->callback_data = data;
    listener->reply_listener = listener->settings.find("p2mqreply://") != std::string::npos;
    if (listener->reply_listener) {
        listener->stream_code = static_cast<StreamCode>(0);
        connection->listeners.push_back(listener);
        *lsnptr = listener;
        return kCgErrOk;
    }
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
    if (fake_flag("MOEX_FAKE_WRONG_SCHEME_OVERRIDE") && listener->settings.find("WRONG_SCHEME") != std::string::npos) {
        listener->scheme = std::make_unique<OwnedScheme>();
        listener->message_plans.clear();
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
    ++typed->open_attempt_count;
    const bool refdata_error_once =
        typed->stream_code == StreamCode::kFortsRefdataRepl && fake_flag("MOEX_FAKE_REFDATA_OPEN_ERROR_ONCE");
    const bool trade_error_once =
        typed->stream_code == StreamCode::kFortsTradeRepl &&
        (fake_flag("MOEX_FAKE_TRADE_OPEN_ERROR_ONCE") || fake_flag("MOEX_FAKE_TRADE_OPEN_ERROR_POS_DRIFT"));
    if (typed->open_attempt_count == 1 && (refdata_error_once || trade_error_once)) {
        typed->state = kStateError;
        if (trade_error_once && typed->connection != nullptr) {
            typed->connection->trade_open_error_seen = true;
        }
        return kCgErrOk;
    }
    if (fake_flag("MOEX_FAKE_LSN_OPENING_STATE")) {
        typed->state = kStateOpening;
        return kCgErrOk;
    }
    if (fake_flag("MOEX_FAKE_LSN_ERROR_STATE")) {
        typed->state = kStateError;
        return kCgErrOk;
    }
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

std::uint32_t cg_pub_new(void* conn, const char* settings, void** pubptr) {
    if (conn == nullptr || settings == nullptr || pubptr == nullptr) {
        return kCgErrInvalidArgument;
    }
    if (fake_flag("MOEX_FAKE_DISABLE_PUBLISHER")) {
        return kCgErrInvalidArgument;
    }
    auto* publisher = new FakePublisher{};
    publisher->connection = static_cast<FakeConnection*>(conn);
    publisher->settings = settings;
    *pubptr = publisher;
    return kCgErrOk;
}

std::uint32_t cg_pub_open(void* publisher, const char*) {
    if (publisher == nullptr) {
        return kCgErrInvalidArgument;
    }
    static_cast<FakePublisher*>(publisher)->state = kStateActive;
    return kCgErrOk;
}

std::uint32_t cg_pub_close(void* publisher) {
    if (publisher == nullptr) {
        return kCgErrInvalidArgument;
    }
    static_cast<FakePublisher*>(publisher)->state = kStateClosed;
    return kCgErrOk;
}

std::uint32_t cg_pub_destroy(void* publisher) {
    if (publisher == nullptr) {
        return kCgErrInvalidArgument;
    }
    delete static_cast<FakePublisher*>(publisher);
    return kCgErrOk;
}

std::uint32_t cg_pub_getstate(void* publisher, std::uint32_t* state) {
    if (publisher == nullptr || state == nullptr) {
        return kCgErrInvalidArgument;
    }
    *state = static_cast<FakePublisher*>(publisher)->state;
    return kCgErrOk;
}

std::uint32_t cg_pub_msgnew(void* publisher, std::uint32_t, const void* id, void** msgptr) {
    if (publisher == nullptr || id == nullptr || msgptr == nullptr) {
        return kCgErrInvalidArgument;
    }
    if (const auto result = configured_result("MOEX_FAKE_PUB_MSGNEW_RESULT"); result != kCgErrOk) {
        return result;
    }
    auto* owned = new FakePublisherMessage{};
    owned->name = static_cast<const char*>(id);
    owned->payload.resize(publisher_payload_size(owned->name));
    owned->message.type = kCgMsgData;
    owned->message.data_size = owned->payload.size();
    owned->message.data = owned->payload.data();
    owned->message.msg_name = owned->name.c_str();
    *msgptr = &owned->message;
    g_publisher_messages.emplace(*msgptr, owned);
    return kCgErrOk;
}

std::uint32_t cg_pub_post(void* publisher, void* message, std::uint32_t flags) {
    if (publisher == nullptr || message == nullptr) {
        return kCgErrInvalidArgument;
    }
    const auto result = configured_result("MOEX_FAKE_PUB_POST_RESULT");
    if (result == kCgErrOk && (flags & 1U) != 0U) {
        const auto* typed_publisher = static_cast<FakePublisher*>(publisher);
        const auto* typed_message = static_cast<CgMsgData*>(message);
        const std::string_view message_name =
            typed_message->msg_name == nullptr ? std::string_view{} : typed_message->msg_name;
        const bool add = message_name == "AddOrder";
        const bool recovery = message_name == "DelUserOrders";
        if ((recovery && fake_flag("MOEX_FAKE_CANCEL_AFTER_RECOVERY")) ||
            (message_name == "DelOrder" && fake_flag("MOEX_FAKE_CANCEL_AFTER_DEL"))) {
            g_cancel_after_cleanup = true;
            for (auto* listener : typed_publisher->connection->listeners) {
                if (listener != nullptr && !listener->reply_listener) {
                    listener->script_emitted = false;
                }
            }
        }
        auto reply_message_id = add ? 179U : recovery ? 186U : 177U;
        const auto* family = std::getenv("MOEX_FAKE_PUB_REPLY_FAMILY");
        if (family != nullptr) {
            if (std::string_view(family) == "add") {
                reply_message_id = 179U;
            } else if (std::string_view(family) == "del") {
                reply_message_id = 177U;
            } else if (std::string_view(family) == "recovery") {
                reply_message_id = 186U;
            } else if (std::string_view(family) == "wrong") {
                reply_message_id = add ? 177U : 179U;
            }
        }
        auto reply_payload = make_trade_reply(message_name);
        if (fake_flag("MOEX_FAKE_PUB_REPLY_MALFORMED") && !reply_payload.empty()) {
            reply_payload.resize(3);
        }
        typed_publisher->connection->pending_replies.push_back({
            .message_id = reply_message_id,
            .message_name = add        ? "AddOrderReply"
                            : recovery ? "DelUserOrdersReply"
                                       : "DelOrderReply",
            .user_id = typed_message->user_id,
            .payload = reply_payload,
            .timed_out = configured_reply_timeout() || (add && fake_flag("MOEX_FAKE_PUB_REPLY_TIMEOUT_ADD_ONLY")),
        });
        if (fake_flag("MOEX_FAKE_PUB_DUPLICATE_REPLY")) {
            auto contradictory = reply_payload;
            if (add && contradictory.size() >= 4 + 255 + sizeof(std::int64_t)) {
                std::int64_t order_id = 0;
                std::memcpy(&order_id, contradictory.data() + 4 + 255, sizeof(order_id));
                ++order_id;
                std::memcpy(contradictory.data() + 4 + 255, &order_id, sizeof(order_id));
            }
            typed_publisher->connection->pending_replies.push_back({
                .message_id = reply_message_id,
                .message_name = add        ? "AddOrderReply"
                                : recovery ? "DelUserOrdersReply"
                                           : "DelOrderReply",
                .user_id = typed_message->user_id,
                .payload = std::move(contradictory),
                .timed_out = configured_reply_timeout() || (add && fake_flag("MOEX_FAKE_PUB_REPLY_TIMEOUT_ADD_ONLY")),
            });
        }
    }
    return result;
}

std::uint32_t cg_pub_msgfree(void*, void* message) {
    if (message == nullptr) {
        return kCgErrInvalidArgument;
    }
    const auto found = g_publisher_messages.find(message);
    if (found == g_publisher_messages.end()) {
        return kCgErrInvalidArgument;
    }
    auto* owned = found->second;
    g_publisher_messages.erase(found);
    const auto result = configured_result("MOEX_FAKE_PUB_MSGFREE_RESULT");
    delete owned;
    return result;
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
