#pragma once
#include "moex/plaza2/cgate/plaza2_metadata.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace moex::plaza2::test::fake {
enum class Option : std::uint32_t {
    ActiveOrderAltExtId,
    AggrClearAfterReady,
    AggrClearOnBootstrap,
    AggrCloseAfterReady,
    AggrCrossed,
    AggrEmpty,
    AggrErrorAfterReady,
    AggrLifenumAfterReady,
    AggrMultiInstrument,
    AggrNegativeUnrelated,
    AggrOneSided,
    AggrReopenOpenNoService,
    AggrReopenOpenResult,
    AggrSnapshotReadyOnly,
    AggrUnrelatedUpdateAfterReady,
    AggrWrongSession,
    CallbackCorruption,
    CallbackProcessInternal,
    CancelledOrder,
    CancelAfterDel,
    CancelAfterRecovery,
    CgateClearDeletedInsideTransaction,
    CgateClearDeletedUnknownTable,
    CgateRequireAbsoluteScheme,
    ClientCode,
    ClientShapedUnmatched,
    CompletedSession,
    ConnectionClosed,
    ConnectionCreateResult,
    ConnectionError,
    ConnectionInternalLoss,
    ConnectionOpenFail,
    ConnectionOpenResult,
    ConnAsyncOpen,
    ConnDirectClosed,
    ConnGetstateInternal,
    ConnGetstateInternalOnce,
    ConnHoldOpening,
    ConnToOpening,
    Cp1251Text,
    DealsBadScheme,
    DecodeCorruption,
    DelayUserorderbook,
    DisablePublisher,
    DisableReplyListener,
    EnvOpenResult,
    ExitAfterMsgnew,
    ExitAfterPost,
    ExtId,
    FirstOrderBboShift,
    FirstOrderFilled,
    FlatTradeReplay,
    ForceTradeTerminal,
    FreshPosAnchor,
    FullFill,
    IdentityConflict,
    ListenerOpenNoService,
    ListenerOpenResult,
    LiveSessionSwitch,
    LsnErrorState,
    LsnErrorStateOnce,
    LsnGetstateInternal,
    LsnOpeningState,
    LsnOpeningStateOnce,
    MissingInstrument,
    MissingLimits,
    MissingOrder,
    MissingPosition,
    MissingSession,
    MissingTradeOrder,
    NontradableInstrument,
    NontradableSession,
    PersistentOrderSession,
    PosAnchorDrift,
    PrepublishNextSession,
    PrivateCloseAfterReady,
    PrivateErrorAfterReady,
    PrivateErrorStreamAfterReady,
    PrivateLifenumAfterReady,
    ProcessInternalActive,
    ProcessInvalidArgument,
    ProcessResult,
    ProcessTimeout,
    PublisherClosed,
    PublisherError,
    PublisherOpenResult,
    PubDuplicateReply,
    PubGetstateInternal,
    PubMsgfreeResult,
    PubMsgnewResult,
    PubPostResult,
    PubReplyCode,
    PubReplyFamily,
    PubReplyMalformed,
    PubReplyMode,
    PubReplyOrderId,
    PubReplyRejectDel,
    PubReplyRejectRecovery,
    PubReplyTimeoutAddOnly,
    PubReplyTimeoutDel,
    PubReplyTimeoutRecovery,
    RefdataOnlyNewGeneration,
    RefdataOpenErrorOnce,
    RegularRecoveredOrder,
    RemoveTargetAfterReady,
    ReplyBeforeReplication,
    ReplyError,
    RestartFilled,
    ScheduledSession,
    SchemeExtraTable,
    SchemeMissingPrice,
    SchemeRetypePrice,
    SessionId,
    SessionPriceRevision,
    SinglePrivateError,
    StatusBeforeRefdata,
    StatusRefreshOpenErrorOnce,
    StatusRefreshStall,
    SuspendedSession,
    SystemZeroCode,
    TimestampMilliseconds,
    TradeIdentityConflict,
    TradeOpenErrorOnce,
    TradeOpenErrorPosDrift,
    UserbookOnlyOrder,
    UserorderbookPeriodicRefresh,
    WrongLimitClient,
    WrongPositionAccountType,
    WrongSchemeOverride,
    ZeroPosition,
    Count
};
// Scenario controls are local to one explicitly loaded fake runtime, never process environment.
struct Scenario {
    bool continuous_input{false};
    bool suppress_auto_replies{false};
    bool suppress_initial_orders{false};
    bool zero_position{false};
    std::uint32_t heartbeat_replay_rows{0};
    bool simulate_idle_wait{false};
    // Zero follows the fake server's POS epoch; a positive value models an independent TRADE rollover.
    std::uint32_t trade_server_lifenum{0};
    std::uint32_t listener_create_result{0}, publisher_create_result{0};
    generated::StreamCode unrecognized_schema_stream{};
    generated::FieldCode mutated_schema_field{};
    generated::TableCode omitted_schema_table{};
    bool omit_schema_field{false};
    std::string schema_field_type;
    std::string client_code;
    std::int32_t session_id{0};
    std::array<std::string, static_cast<std::size_t>(Option::Count)> options;
};
enum class FieldKind : std::uint8_t { SignedInteger, UnsignedInteger, Text, Timestamp };
struct Field {
    generated::FieldCode field_code{};
    FieldKind kind{FieldKind::SignedInteger};
    std::int64_t signed_value{0};
    std::uint64_t unsigned_value{0};
    std::string text;
};
enum class EventKind : std::uint8_t {
    Begin,
    Row,
    Commit,
    Online,
    LifeNum,
    ClearDeleted,
    Close,
    Reply,
    ConnectionError
};
struct Event {
    EventKind kind{EventKind::Row};
    generated::StreamCode stream_code{};
    generated::TableCode table_code{};
    std::int64_t revision{0};
    std::uint64_t value{0};
    std::uint32_t flags{0};
    std::vector<Field> fields;
    std::int32_t message_id{0};
    std::uint32_t user_id{0};
    std::vector<std::byte> payload;
    bool timed_out{false};
};
struct PostedCommand {
    std::string name;
    std::uint32_t user_id{}, result{};
    std::vector<std::byte> payload;
};
class Control {
  public:
    explicit Control(const std::filesystem::path& library) {
        handle_ = ::dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_)
            throw std::runtime_error("load fake controls failed");
    }
    ~Control() {
        if (handle_)
            ::dlclose(handle_);
    }
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;
    void configure(const Scenario& scenario) const {
        call<void (*)(const Scenario*)>("moex_fake_scenario")(&scenario);
    }
    void set(Option option, std::string_view value = "1") const {
        const std::string owned(value);
        call<void (*)(Option, const char*)>("moex_fake_option")(option, owned.c_str());
    }
    void clear(Option option) const {
        set(option, {});
    }
    void enqueue(const Event& event) const {
        call<void (*)(const Event*)>("moex_fake_enqueue")(&event);
    }
    std::vector<PostedCommand> commands() const {
        std::vector<PostedCommand> result;
        call<void (*)(std::vector<PostedCommand>*)>("moex_fake_commands")(&result);
        return result;
    }
    std::uint64_t process_count() const {
        return call<std::uint64_t (*)()>("moex_fake_process_count")();
    }
    std::array<std::uint64_t, 4> successful_closes() const {
        std::array<std::uint64_t, 4> counts{};
        call<void (*)(std::array<std::uint64_t, 4>*)>("moex_fake_successful_closes")(&counts);
        return counts;
    }
    std::uint32_t last_process_timeout() const {
        return call<std::uint32_t (*)()>("moex_fake_last_process_timeout")();
    }
    std::int64_t last_post_started_steady_ns() const {
        return call<std::int64_t (*)()>("moex_fake_last_post_started_steady_ns")();
    }
    std::uint64_t opens(generated::StreamCode stream) const {
        return call<std::uint64_t (*)(generated::StreamCode)>("moex_fake_listener_opens")(stream);
    }
    std::string trade_open_settings() const {
        std::string result;
        call<void (*)(std::string*)>("moex_fake_trade_open_settings")(&result);
        return result;
    }

  private:
    template <class T> T call(const char* name) const {
        auto symbol = ::dlsym(handle_, name);
        if (!symbol)
            throw std::runtime_error(std::string("missing fake control: ") + name);
        return reinterpret_cast<T>(symbol);
    }
    void* handle_{};
};
} // namespace moex::plaza2::test::fake
