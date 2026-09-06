#include "moex/plaza2_trade/plaza2_test_trade_transport.hpp"

#include "moex/plaza2/cgate/plaza2_private_state_bridge.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace moex::plaza2_trade {

namespace {

namespace plaza2 = ::moex::plaza2;
namespace cgate = ::moex::plaza2::cgate;
namespace private_state = ::moex::plaza2::private_state;
namespace fake = ::moex::plaza2::fake;
namespace generated = ::moex::plaza2::generated;

using cgate::Plaza2DecodedValueKind;
using cgate::Plaza2Error;
using cgate::Plaza2ErrorCode;
using cgate::Plaza2ListenerEvent;
using cgate::Plaza2ListenerEventHandler;
using cgate::Plaza2ListenerEventKind;
using cgate::Plaza2PublisherMessageResult;
using plaza2::fake::EngineState;
using plaza2::fake::EventKind;
using plaza2::fake::EventSpec;
using plaza2::fake::FieldValueSpec;
using plaza2::fake::RowSpec;
using plaza2::generated::StreamCode;

constexpr std::array<StreamCode, 5> kRequiredPrivateStreams = {
    StreamCode::kFortsTradeRepl, StreamCode::kFortsUserorderbookRepl, StreamCode::kFortsPosRepl,
    StreamCode::kFortsPartRepl,  StreamCode::kFortsRefdataRepl,
};

bool exact_required_private_streams(std::span<const Plaza2TestTradeStreamConfig> streams) {
    if (streams.size() != kRequiredPrivateStreams.size()) {
        return false;
    }
    for (const auto required : kRequiredPrivateStreams) {
        if (std::count_if(streams.begin(), streams.end(),
                          [&](const auto& stream) { return stream.stream_code == required; }) != 1) {
            return false;
        }
    }
    return true;
}

Plaza2TestTradeStreamConfig default_status_stream(StreamCode code, std::string_view name) {
    return {
        .stream_code = code,
        .settings = "p2repl://" + std::string(name) + ";scheme=|FILE|scheme/forts_scheme.ini|",
        .open_settings = {},
    };
}

constexpr std::string_view kCredentialToken = "${MOEX_PLAZA2_TEST_CREDENTIALS}";
constexpr std::string_view kLegacyCredentialToken = "${PLAZA2_TEST_CREDENTIALS}";
constexpr std::string_view kSoftwareKeyToken = "${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}";
constexpr std::string_view kRelativeSchemeToken = "|FILE|scheme/forts_scheme.ini|";
constexpr std::uint32_t kCgStateClosed = 0;
constexpr std::uint32_t kCgStateError = 1;
constexpr std::uint32_t kCgStateOpening = 2;
constexpr std::uint32_t kCgStateActive = 3;
constexpr auto kListenerReopenDelay = std::chrono::seconds(1);

Plaza2Error invalid(std::string message, Plaza2ErrorCode code = Plaza2ErrorCode::InvalidConfiguration) {
    return {.code = code, .runtime_code = 0, .message = std::move(message)};
}

void replace_all(std::string& value, std::string_view token, std::string_view replacement) {
    std::size_t position = 0;
    while ((position = value.find(token, position)) != std::string::npos) {
        value.replace(position, token.size(), replacement);
        position += replacement.size();
    }
}

std::optional<std::string> load_secret(const cgate::Plaza2CredentialConfig& config) {
    if (config.source == cgate::Plaza2CredentialSource::None) {
        return std::string{};
    }
    if (config.source == cgate::Plaza2CredentialSource::Env) {
        if (config.env_var.empty()) {
            return std::nullopt;
        }
        const auto* value = std::getenv(config.env_var.c_str());
        if (value == nullptr || *value == '\0') {
            return std::nullopt;
        }
        return std::string(value);
    }
    std::ifstream input(config.file_path);
    if (!input) {
        return std::nullopt;
    }
    std::string value;
    std::getline(input, value);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

std::string resolve_scheme(std::string value, const std::filesystem::path& scheme_path) {
    const auto replacement = std::string("|FILE|") + scheme_path.string() + "|";
    replace_all(value, kRelativeSchemeToken, replacement);
    return value;
}

std::string resolve_ini(std::string value, const std::filesystem::path& config_dir) {
    constexpr std::string_view prefix = "ini=";
    const auto begin = value.find(prefix);
    if (begin == std::string::npos) {
        return value;
    }
    const auto value_begin = begin + prefix.size();
    const auto value_end = value.find(';', value_begin);
    const auto value_size = (value_end == std::string::npos ? value.size() : value_end) - value_begin;
    const auto raw = std::filesystem::path(value.substr(value_begin, value_size));
    if (raw.is_absolute()) {
        return value;
    }
    auto resolved = config_dir / raw;
    if (!std::filesystem::exists(resolved) && raw.has_parent_path() && raw.begin() != raw.end() &&
        *raw.begin() == "config") {
        resolved = config_dir / raw.filename();
    }
    value.replace(value_begin, value_size, resolved.string());
    return value;
}

bool loopback_connection(std::string_view settings) {
    return settings.find("127.0.0.1") != std::string_view::npos ||
           settings.find("localhost") != std::string_view::npos || settings.find("::1") != std::string_view::npos;
}

std::optional<std::string> setting_value(std::string_view settings, std::string_view key) {
    std::size_t begin = 0;
    while (begin <= settings.size()) {
        const auto end = settings.find(';', begin);
        const auto token =
            settings.substr(begin, end == std::string_view::npos ? settings.size() - begin : end - begin);
        const auto equals = token.find('=');
        if (equals != std::string_view::npos && token.substr(0, equals) == key) {
            return std::string(token.substr(equals + 1));
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return std::nullopt;
}

Plaza2Error validate_publisher_reply_identity(const Plaza2TestSessionHostConfig& config,
                                              std::string_view reply_settings) {
    if (config.publisher_name.empty()) {
        return invalid("publisher_name must be provided explicitly");
    }
    const auto publisher_name = setting_value(config.publisher_settings, "name");
    if (config.mode != Plaza2TestSessionHostMode::OfflineFake &&
        (!publisher_name.has_value() || *publisher_name != config.publisher_name)) {
        return invalid("LiveTestPreSend publisher_settings must contain the configured unique name");
    }
    if (publisher_name.has_value() && *publisher_name != config.publisher_name) {
        return invalid("publisher_settings name contradicts publisher_name");
    }
    const auto reply_name = setting_value(reply_settings, "ref");
    if (!reply_name.has_value() || *reply_name != config.publisher_name) {
        return invalid("p2mqreply_settings ref must exactly match publisher_name");
    }
    return {};
}

std::string json_escape_local(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

bool valid_hex_sha256(std::string_view value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

// Reviewed PLAZA II 9.3/9.9 AddOrder semantics. State 0 is scheduled, 2 is
// suspended, and 4 is completed; all are cancellation-only or closed.
bool add_order_allowed_status(std::int32_t state) noexcept {
    return state == 1; // running: Add + Cancel allowed
}

std::string intent_fingerprint(const Plaza2AuthorizedOrderIntent& intent, bool broker) {
    const auto& explicit_hash = broker ? intent.broker_code_sha256 : intent.client_code_sha256;
    if (!explicit_hash.empty()) {
        return explicit_hash;
    }
    const auto& raw = broker ? intent.broker_code : intent.client_code;
    return raw.empty() ? std::string{} : cgate::plaza2_sha256_hex(raw);
}

std::string limit_row_fingerprint(const plaza2::private_state::LimitSnapshot& limit) {
    std::ostringstream value;
    value << static_cast<int>(limit.scope) << '|' << limit.account_code << '|' << limit.limits_set << '|'
          << limit.is_auto_update_limit << '|' << limit.money_free << '|' << limit.money_blocked << '|'
          << limit.vm_reserve << '|' << limit.fee << '|' << limit.money_old << '|' << limit.money_amount << '|'
          << limit.money_pledge_amount << '|' << limit.actual_amount_of_base_currency << '|' << limit.vm_intercl << '|'
          << limit.broker_fee << '|' << limit.penalty << '|' << limit.premium_intercl << '|' << limit.net_option_value;
    return cgate::plaza2_sha256_hex(value.str());
}

std::string position_row_fingerprint(const plaza2::private_state::PositionSnapshot& position) {
    std::ostringstream value;
    value << static_cast<int>(position.scope) << '|' << position.account_code << '|' << position.isin_id << '|'
          << static_cast<int>(position.account_type) << '|' << position.xpos << '|' << position.xbuys_qty << '|'
          << position.xsells_qty << '|' << position.xday_open_qty << '|' << position.xday_open_buys_qty << '|'
          << position.xday_open_sells_qty << '|' << position.xopen_qty << '|' << position.waprice << '|'
          << position.net_volume_rur << '|' << position.last_deal_id << '|' << position.last_quantity;
    return cgate::plaza2_sha256_hex(value.str());
}

std::optional<std::int64_t> parse_scaled_decimal(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto dot = text.find('.');
    if (dot != std::string_view::npos && text.find('.', dot + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto whole = dot == std::string_view::npos ? text : text.substr(0, dot);
    const auto fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (whole.empty() || fraction.size() > 6 ||
        !std::all_of(whole.begin(), whole.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }) ||
        !std::all_of(fraction.begin(), fraction.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    for (const auto ch : whole) {
        const auto digit = static_cast<std::int64_t>(ch - '0');
        if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    if (value > std::numeric_limits<std::int64_t>::max() / 1000000) {
        return std::nullopt;
    }
    value *= 1000000;
    std::int64_t fractional_value = 0;
    for (const auto ch : fraction) {
        fractional_value = fractional_value * 10 + static_cast<std::int64_t>(ch - '0');
    }
    for (std::size_t index = fraction.size(); index < 6; ++index) {
        fractional_value *= 10;
    }
    return value + fractional_value;
}

bool atomic_write_file(const std::filesystem::path& path, std::string_view contents, std::string& error) {
    std::error_code filesystem_error;
    if (!std::filesystem::create_directories(path.parent_path(), filesystem_error) && filesystem_error) {
        error = "failed to create execution-safety directory: " + filesystem_error.message();
        return false;
    }
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "failed to open execution-safety temporary file";
            return false;
        }
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output) {
            error = "failed to flush execution-safety temporary file";
            return false;
        }
    }
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        error = "failed to publish execution-safety receipt: " + filesystem_error.message();
        return false;
    }
    return true;
}

std::size_t stream_index(const EngineState& state, StreamCode code) {
    for (std::size_t index = 0; index < state.streams.size(); ++index) {
        if (state.streams[index].stream_code == code) {
            return index;
        }
    }
    return state.streams.size();
}

using PrivateProjectorBridge = cgate::Plaza2PrivateStateBridge;
class AggrProjectorBridge final : public Plaza2ListenerEventHandler {
  public:
    explicit AggrProjectorBridge(cgate::Plaza2Aggr20BookProjector& projector) : projector_(projector) {}

    [[nodiscard]] bool online() const noexcept {
        return online_;
    }

    [[nodiscard]] bool snapshot_complete() const noexcept {
        return snapshot_complete_;
    }

    void reset() noexcept {
        online_ = false;
        snapshot_complete_ = false;
    }

    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override {
        switch (event.kind) {
        case Plaza2ListenerEventKind::Open:
        case Plaza2ListenerEventKind::Timeout:
        case Plaza2ListenerEventKind::ReplState:
            return {};
        case Plaza2ListenerEventKind::LifeNum:
            online_ = false;
            snapshot_complete_ = false;
            projector_.reset();
            return {};
        case Plaza2ListenerEventKind::Close:
            online_ = false;
            snapshot_complete_ = false;
            projector_.reset();
            return {};
        case Plaza2ListenerEventKind::Online:
            online_ = true;
            snapshot_complete_ = true;
            return {};
        case Plaza2ListenerEventKind::TransactionBegin:
            projector_.begin_transaction();
            return {};
        case Plaza2ListenerEventKind::StreamData:
            if (event.table_code == plaza2::generated::TableCode::kFortsAggrReplOrdersAggr) {
                return projector_.on_row(event.fields);
            }
            return {};
        case Plaza2ListenerEventKind::TransactionCommit:
            return projector_.commit();
        case Plaza2ListenerEventKind::ClearDeleted:
            projector_.reset();
            snapshot_complete_ = false;
            return {};
        default:
            return {};
        }
    }

  private:
    cgate::Plaza2Aggr20BookProjector& projector_;
    bool online_{false};
    bool snapshot_complete_{false};
};

class ReplyBridge final : public Plaza2ListenerEventHandler {
  public:
    using Event = Plaza2TestSessionHost::ReplyEvent;

    void arm(std::uint32_t user_id, Plaza2TradeCommandKind kind) {
        active_[user_id] = kind;
    }

    void clear() {
        events_.clear();
        error_.clear();
        active_.clear();
        signatures_.clear();
    }

    [[nodiscard]] std::vector<Event> take() {
        auto events = std::move(events_);
        events_.clear();
        return events;
    }

    [[nodiscard]] const std::string& error() const noexcept {
        return error_;
    }

    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override {
        if (event.kind != Plaza2ListenerEventKind::StreamData && event.kind != Plaza2ListenerEventKind::Timeout) {
            return {};
        }
        const auto active = active_.find(event.user_id);
        if (active == active_.end()) {
            return fail("reply for an unknown active user_id");
        }
        if (event.kind == Plaza2ListenerEventKind::Timeout) {
            events_.push_back(
                {.user_id = event.user_id, .message_id = 0, .command_kind = active->second, .timed_out = true});
            return {};
        }
        const auto expected_message_id = active->second == Plaza2TradeCommandKind::AddOrder   ? 179
                                         : active->second == Plaza2TradeCommandKind::DelOrder ? 177
                                                                                              : 186;
        if (event.message_id != expected_message_id) {
            return fail("reply message family contradicts the active user_id command");
        }
        const auto signature = std::to_string(event.message_id) + ":" + cgate::plaza2_sha256_hex(event.raw_payload);
        const auto seen = signatures_.find(event.user_id);
        if (seen != signatures_.end() && seen->second != signature) {
            return fail("duplicate contradictory reply for an active user_id");
        }
        signatures_[event.user_id] = signature;
        events_.push_back({.user_id = event.user_id,
                           .message_id = event.message_id,
                           .command_kind = active->second,
                           .timed_out = false,
                           .raw_payload = std::vector<std::byte>(event.raw_payload.begin(), event.raw_payload.end())});
        return {};
    }

  private:
    Plaza2Error fail(std::string message) {
        if (error_.empty()) {
            error_ = std::move(message);
        }
        return invalid(error_, Plaza2ErrorCode::CallbackFailed);
    }

    std::map<std::uint32_t, Plaza2TradeCommandKind> active_;
    std::map<std::uint32_t, std::string> signatures_;
    std::vector<Event> events_;
    std::string error_;
};

struct DecodedAddPayload {
    std::string broker_code;
    std::int32_t isin_id{0};
    std::string client_code;
    Plaza2TradeSide side{Plaza2TradeSide::Buy};
    Plaza2TradeOrderType order_type{Plaza2TradeOrderType::Limit};
    std::int32_t amount{0};
    std::string price;
    std::int32_t ext_id{0};
};

template <typename T> std::optional<T> read_little_endian(std::span<const std::byte> payload, std::size_t offset) {
    if (offset + sizeof(T) > payload.size()) {
        return std::nullopt;
    }
    T value{};
    std::memcpy(&value, payload.data() + offset, sizeof(T));
    if constexpr (std::endian::native != std::endian::little) {
        using U = std::make_unsigned_t<T>;
        U raw{};
        std::memcpy(&raw, &value, sizeof(T));
        U swapped{};
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            swapped = static_cast<U>((swapped << 8U) | ((raw >> (index * 8U)) & 0xffU));
        }
        std::memcpy(&value, &swapped, sizeof(T));
    }
    return value;
}

std::string fixed_payload_string(std::span<const std::byte> payload, std::size_t offset, std::size_t width) {
    if (offset >= payload.size()) {
        return {};
    }
    const auto count = std::min(width, payload.size() - offset);
    std::string value;
    value.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto ch = static_cast<char>(std::to_integer<unsigned char>(payload[offset + index]));
        if (ch == '\0') {
            break;
        }
        value.push_back(ch);
    }
    return value;
}

std::optional<DecodedAddPayload> decode_add_payload(std::span<const std::byte> payload) {
    // Reviewed AddOrder(474) ABI: broker[4], isin i4, client[3], dir i4,
    // type i4, amount i4, price[17], comment[20], broker_to[20], ext_id i4.
    if (payload.size() < 112) {
        return std::nullopt;
    }
    const auto isin = read_little_endian<std::int32_t>(payload, 4);
    const auto side = read_little_endian<std::int32_t>(payload, 11);
    const auto type = read_little_endian<std::int32_t>(payload, 15);
    const auto amount = read_little_endian<std::int32_t>(payload, 19);
    const auto ext_id = read_little_endian<std::int32_t>(payload, 80);
    if (!isin || !side || !type || !amount || !ext_id ||
        (*side != static_cast<std::int32_t>(Plaza2TradeSide::Buy) &&
         *side != static_cast<std::int32_t>(Plaza2TradeSide::Sell)) ||
        (*type != static_cast<std::int32_t>(Plaza2TradeOrderType::Limit) &&
         *type != static_cast<std::int32_t>(Plaza2TradeOrderType::Market))) {
        return std::nullopt;
    }
    return DecodedAddPayload{
        .broker_code = fixed_payload_string(payload, 0, 4),
        .isin_id = *isin,
        .client_code = fixed_payload_string(payload, 8, 3),
        .side = static_cast<Plaza2TradeSide>(*side),
        .order_type = static_cast<Plaza2TradeOrderType>(*type),
        .amount = *amount,
        .price = fixed_payload_string(payload, 23, 17),
        .ext_id = *ext_id,
    };
}

struct DecodedRecoveryPayload {
    std::string broker_code;
    std::int32_t buy_sell{0};
    std::string client_code;
    std::string base_contract_code;
    std::int32_t ext_id{0};
    std::int32_t isin_id{0};
    std::int8_t instrument_mask{0};
};

std::optional<DecodedRecoveryPayload> decode_recovery_payload(std::span<const std::byte> payload) {
    if (payload.size() < 49) {
        return std::nullopt;
    }
    const auto buy_sell = read_little_endian<std::int32_t>(payload, 4);
    const auto ext_id = read_little_endian<std::int32_t>(payload, 40);
    const auto isin_id = read_little_endian<std::int32_t>(payload, 44);
    const auto mask = read_little_endian<std::int8_t>(payload, 48);
    if (!buy_sell || !ext_id || !isin_id || !mask) {
        return std::nullopt;
    }
    return DecodedRecoveryPayload{
        .broker_code = fixed_payload_string(payload, 0, 4),
        .buy_sell = *buy_sell,
        .client_code = fixed_payload_string(payload, 12, 3),
        .base_contract_code = fixed_payload_string(payload, 15, 25),
        .ext_id = *ext_id,
        .isin_id = *isin_id,
        .instrument_mask = *mask,
    };
}

} // namespace

std::string canonical_authorized_order_intent_json(const Plaza2AuthorizedOrderIntent& intent) {
    std::ostringstream json;
    json << "{\n"
         << "  \"schema\": \"moex.plaza2.authorized_order_intent.v1\",\n"
         << "  \"profile_id\": \"" << json_escape_local(intent.profile_id) << "\",\n"
         << "  \"profile_fingerprint\": \"" << intent.profile_fingerprint << "\",\n"
         << "  \"environment\": \"" << json_escape_local(intent.environment) << "\",\n"
         << "  \"command\": \"AddOrder\",\n"
         << "  \"message_id\": 474,\n"
         << "  \"payload_sha256\": \"" << intent.add_payload_sha256 << "\",\n"
         << "  \"recovery_message_id\": 466,\n"
         << "  \"recovery_payload_sha256\": \"" << intent.recovery_payload_sha256 << "\",\n"
         << "  \"add_user_id\": " << intent.add_user_id << ",\n"
         << "  \"cancel_user_id\": " << intent.cancel_user_id << ",\n"
         << "  \"recovery_user_id\": " << intent.recovery_user_id << ",\n"
         << "  \"ext_id\": " << intent.ext_id << ",\n"
         << "  \"isin_id\": " << intent.isin_id << ",\n"
         << "  \"base_contract_code\": \"" << json_escape_local(intent.base_contract_code) << "\",\n"
         << "  \"instrument_mask\": " << static_cast<int>(intent.instrument_mask) << ",\n"
         << "  \"side\": \"" << (intent.side == Plaza2TradeSide::Buy ? "buy" : "sell") << "\",\n"
         << "  \"order_type\": \"limit\",\n"
         << "  \"price\": \"" << json_escape_local(intent.price) << "\",\n"
         << "  \"quantity\": " << intent.quantity << ",\n"
         << "  \"client_code_sha256\": \"" << intent_fingerprint(intent, false) << "\",\n"
         << "  \"broker_code_sha256\": \"" << intent_fingerprint(intent, true) << "\",\n"
         << "  \"smoke_policy\": {\n"
         << "    \"version\": \"" << json_escape_local(intent.policy_version) << "\",\n"
         << "    \"sha256\": \"" << intent.policy_sha256 << "\",\n"
         << "    \"max_distance_ticks\": " << intent.max_distance_ticks << ",\n"
         << "    \"max_aggr20_age_ms\": " << intent.max_aggr20_age_ms << ",\n"
         << "    \"require_zero_starting_position\": " << (intent.require_zero_starting_position ? "true" : "false")
         << "\n"
         << "  }\n"
         << "}\n";
    return json.str();
}

std::string authorized_order_intent_sha256(const Plaza2AuthorizedOrderIntent& intent) {
    return cgate::plaza2_sha256_hex(canonical_authorized_order_intent_json(intent));
}

std::string_view position_evidence_class_name(PositionEvidenceClass value) noexcept {
    switch (value) {
    case PositionEvidenceClass::ExactZeroPosRow:
        return "EXACT_ZERO_POS_ROW";
    case PositionEvidenceClass::FlatByPosSnapshotAndTradeReplay:
        return "FLAT_BY_POS_SNAPSHOT_AND_TRADE_REPLAY";
    case PositionEvidenceClass::Unresolved:
        return "UNRESOLVED";
    }
    return "UNRESOLVED";
}

struct Plaza2TestSessionHost::Impl {
    struct ManagedPrivateListener {
        ManagedPrivateListener(StreamCode code, std::string settings)
            : stream_code(code), open_settings(std::move(settings)) {}

        StreamCode stream_code{};
        cgate::Plaza2Listener listener;
        std::string open_settings;
        std::chrono::steady_clock::time_point reopen_not_before{};
        bool reopen_pending{false};
        bool snapshot_completed_once{false};
    };

    explicit Impl(Plaza2TestSessionHostConfig initial)
        : config(std::move(initial)), private_bridge(private_projector), aggr_bridge(aggr_projector) {}

    Plaza2Error start() {
        if (started) {
            return invalid("TEST session host is already started", Plaza2ErrorCode::AdapterState);
        }
        if (config.runtime.environment != cgate::Plaza2Environment::Test) {
            return invalid("TEST session host refuses non-TEST environment");
        }
        if (config.mode != Plaza2TestSessionHostMode::OfflineFake &&
            config.mode != Plaza2TestSessionHostMode::LiveTestPreSend &&
            config.mode != Plaza2TestSessionHostMode::LiveTestAuthorizedSend) {
            return invalid("unknown TEST host mode");
        }
        if (config.mode == Plaza2TestSessionHostMode::LiveTestAuthorizedSend) {
            if (!config.trade_replay_from_pos_anchor) {
                return invalid("authorized TEST send requires POS-anchored TRADE replay");
            }
            if (!config.arm_state.test_order_send_armed || !config.arm_state.test_network_armed ||
                !config.arm_state.test_session_armed || !config.arm_state.test_plaza2_armed) {
                return invalid("authorized TEST send requires all TEST arms and test_order_send_armed");
            }
            // First-order scope: only the separately provisioned local T1 router.
            if (config.endpoint_host != "127.0.0.1" ||
                !config.connection_settings.starts_with("p2tcp://127.0.0.1:4101;")) {
                return invalid("authorized TEST send requires the explicit local T1 endpoint");
            }
        }
        const bool loopback = loopback_connection(config.connection_settings);
        if (config.mode == Plaza2TestSessionHostMode::OfflineFake && !loopback) {
            return invalid("OfflineFake session host requires a loopback connection setting");
        }
        if (config.mode != Plaza2TestSessionHostMode::OfflineFake) {
            if (config.endpoint_host.empty()) {
                return invalid("LiveTestPreSend requires endpoint_host for operator-gate validation");
            }
            const auto gate =
                cgate::Plaza2ManualOperatorGate::validate_session_start(config.endpoint_host, config.arm_state);
            if (!gate.allowed) {
                return {.code = gate.error_code, .runtime_code = 0, .message = gate.reason};
            }
        }
        if (!exact_required_private_streams(config.private_streams)) {
            return invalid("TEST session host requires the exact five private replication streams");
        }
        if (config.status_streams.empty()) {
            config.status_streams = {
                default_status_stream(StreamCode::kFortsSessionstateRepl, "FORTS_SESSIONSTATE_REPL"),
                default_status_stream(StreamCode::kFortsInstrumentstateRepl, "FORTS_INSTRUMENTSTATE_REPL"),
            };
        }
        if (config.runtime.runtime_root.empty() || config.connection_settings.empty() ||
            config.publisher_settings.empty() || config.private_streams.empty() ||
            config.aggr20_stream.settings.empty()) {
            return invalid("TEST session host requires runtime, connection, publisher, private, and AGGR20 settings");
        }
        if (config.mode != Plaza2TestSessionHostMode::OfflineFake && config.p2mqreply_settings.empty()) {
            return invalid("LiveTestPreSend requires explicit p2mqreply_settings");
        }
        const auto effective_reply_settings =
            config.p2mqreply_settings.empty() ? "p2mqreply://;ref=" + config.publisher_name : config.p2mqreply_settings;
        if (const auto identity = validate_publisher_reply_identity(config, effective_reply_settings); identity) {
            return identity;
        }
        if (config.runtime.env_open_settings.find(kCredentialToken) != std::string::npos ||
            config.runtime.env_open_settings.find(kLegacyCredentialToken) != std::string::npos) {
            return invalid("TEST session host env_open_settings must use the CGate software-key token");
        }
        probe = cgate::Plaza2RuntimeProbe::probe(config.runtime);
        probe_report = probe;
        if (probe.compatibility == cgate::Plaza2Compatibility::Incompatible ||
            probe.compatibility == cgate::Plaza2Compatibility::Unknown) {
            return invalid("TEST runtime probe is incompatible", Plaza2ErrorCode::ProbeIncompatible);
        }
        if (config.mode == Plaza2TestSessionHostMode::OfflineFake && !probe.fake_runtime_marker_present) {
            return invalid("OfflineFake TEST transport requires the fake CGate runtime marker",
                           Plaza2ErrorCode::ProbeIncompatible);
        }
        if (config.mode != Plaza2TestSessionHostMode::OfflineFake && !probe.trading_capable) {
            return invalid("LiveTestPreSend requires a trading-capable CGate runtime",
                           Plaza2ErrorCode::ProbeIncompatible);
        }
        const auto credentials = load_secret(config.credentials);
        const auto software_key = load_secret(config.software_key);
        if (!credentials.has_value() || !software_key.has_value()) {
            return invalid("TEST session host secret source is missing or empty");
        }
        credentials_value = credentials.value();
        software_key_value = software_key.value();
        const auto contains_token = [](std::string_view value, std::string_view token) {
            return value.find(token) != std::string_view::npos;
        };
        const auto requires_credentials = contains_token(config.runtime.env_open_settings, kCredentialToken) ||
                                          contains_token(config.runtime.env_open_settings, kLegacyCredentialToken) ||
                                          contains_token(config.connection_settings, kCredentialToken) ||
                                          contains_token(config.connection_settings, kLegacyCredentialToken) ||
                                          contains_token(config.publisher_settings, kCredentialToken) ||
                                          contains_token(config.publisher_settings, kLegacyCredentialToken) ||
                                          contains_token(effective_reply_settings, kCredentialToken) ||
                                          contains_token(effective_reply_settings, kLegacyCredentialToken);
        const auto requires_software_key = contains_token(config.runtime.env_open_settings, kSoftwareKeyToken) ||
                                           contains_token(config.connection_settings, kSoftwareKeyToken) ||
                                           contains_token(config.publisher_settings, kSoftwareKeyToken) ||
                                           contains_token(effective_reply_settings, kSoftwareKeyToken);
        if ((requires_credentials && credentials->empty()) || (requires_software_key && software_key->empty())) {
            return invalid("TEST session host settings require a configured secret source");
        }
        effective_runtime = config.runtime;
        render(effective_runtime.env_open_settings, credentials.value(), software_key.value());
        effective_runtime.env_open_settings = resolve_ini(effective_runtime.env_open_settings, probe.layout.config_dir);
        if (const auto error = env.open(effective_runtime); error) {
            return error;
        }
        if (const auto error = connection.create(
                env, render_copy(config.connection_settings, credentials.value(), software_key.value()));
            error) {
            return error;
        }
        if (const auto error = connection.open(config.connection_open_settings); error) {
            return error;
        }

        std::vector<Plaza2TestTradeStreamConfig> all_private_streams = config.private_streams;
        all_private_streams.insert(all_private_streams.end(), config.status_streams.begin(),
                                   config.status_streams.end());
        std::vector<StreamCode> bridge_stream_codes;
        bridge_stream_codes.reserve(all_private_streams.size());
        for (const auto& stream : all_private_streams) {
            bridge_stream_codes.push_back(stream.stream_code);
        }
        if (const auto bridge_error = private_bridge.reset(bridge_stream_codes); bridge_error) {
            return bridge_error;
        }
        if (const auto bridge_error = private_bridge.begin_run(); bridge_error) {
            return bridge_error;
        }
        bridge_started = true;
        reply_bridge.clear();
        aggr_projector.reset();
        aggr_bridge.reset();
        deferred_trade_stream.reset();
        trade_replay_anchor_is_ready = !config.trade_replay_from_pos_anchor;

        private_listeners.reserve(all_private_streams.size());
        for (const auto& stream : all_private_streams) {
            if (config.trade_replay_from_pos_anchor && stream.stream_code == StreamCode::kFortsTradeRepl) {
                deferred_trade_stream = stream;
                continue;
            }
            private_listeners.emplace_back(stream.stream_code, stream.open_settings);
            auto& managed = private_listeners.back();
            const auto settings = resolve_scheme(
                render_copy(stream.settings, credentials.value(), software_key.value()), probe.layout.scheme_path);
            if (const auto error = managed.listener.create(connection, stream.stream_code, settings, &private_bridge);
                error) {
                return error;
            }
            if (const auto error = managed.listener.open(managed.open_settings); error) {
                return error;
            }
        }

        const auto aggr_settings =
            resolve_scheme(render_copy(config.aggr20_stream.settings, credentials.value(), software_key.value()),
                           probe.layout.scheme_path);
        if (const auto error =
                aggr_listener.create(connection, config.aggr20_stream.stream_code, aggr_settings, &aggr_bridge);
            error) {
            return error;
        }
        if (const auto error = aggr_listener.open(config.aggr20_stream.open_settings); error) {
            return error;
        }
        if (const auto error = publisher.create(
                connection, render_copy(config.publisher_settings, credentials.value(), software_key.value()));
            error) {
            return error;
        }
        if (const auto error = publisher.open(config.publisher_open_settings); error) {
            return error;
        }
        publisher_is_open = true;
        if (const auto error = reply_listener.create(
                connection, cgate::kNoStreamCode,
                render_copy(effective_reply_settings, credentials.value(), software_key.value()), &reply_bridge);
            error) {
            return error;
        }
        if (const auto error = reply_listener.open(
                render_copy(config.p2mqreply_open_settings, credentials.value(), software_key.value()));
            error) {
            return error;
        }
        reply_listener_is_open = true;
        started = true;
        return {};
    }

    Plaza2Error open_deferred_trade_replay_if_anchored() {
        if (!deferred_trade_stream.has_value() || trade_listener_index.has_value()) {
            return {};
        }
        const auto health = private_projector.stream_health();
        const auto pos_health = std::find_if(health.begin(), health.end(), [](const auto& stream) {
            return stream.stream_code == StreamCode::kFortsPosRepl;
        });
        const bool fake_anchor_time = probe.fake_runtime_marker_present && (pos_health != health.end()) &&
                                      (pos_health->last_trades_rev != 0 || pos_health->last_trades_lifenum != 0);
        if (pos_health == health.end() || !pos_health->online || !pos_health->snapshot_complete ||
            (pos_health->last_server_time == 0 && !fake_anchor_time)) {
            return {};
        }

        std::string open_settings = deferred_trade_stream->open_settings;
        if (open_settings.empty()) {
            open_settings = "mode=snapshot+online";
        }
        replace_all(open_settings, "${POS_TRADES_REV}", std::to_string(pos_health->last_trades_rev));
        replace_all(open_settings, "${POS_TRADES_LIFENUM}", std::to_string(pos_health->last_trades_lifenum));
        const auto append_or_validate = [&](std::string_view key, std::string value) -> Plaza2Error {
            const auto existing = setting_value(open_settings, key);
            if (existing.has_value() && *existing != value) {
                return invalid("FORTS_TRADE_REPL open_settings contradicts the negotiated POS.info anchor");
            }
            if (!existing.has_value()) {
                open_settings += ";";
                open_settings += key;
                open_settings += "=";
                open_settings += value;
            }
            return {};
        };
        if (const auto error = append_or_validate("lifenum", std::to_string(pos_health->last_trades_lifenum)); error) {
            return error;
        }
        if (const auto error = append_or_validate("rev.deal", std::to_string(pos_health->last_trades_rev)); error) {
            return error;
        }
        if (const auto error = append_or_validate("rev.heart_beat", std::to_string(pos_health->last_trades_rev));
            error) {
            return error;
        }

        private_listeners.emplace_back(deferred_trade_stream->stream_code, open_settings);
        auto& managed = private_listeners.back();
        const auto settings =
            resolve_scheme(render_copy(deferred_trade_stream->settings, credentials_value, software_key_value),
                           probe.layout.scheme_path);
        if (const auto error =
                managed.listener.create(connection, deferred_trade_stream->stream_code, settings, &private_bridge);
            error) {
            return error;
        }
        if (const auto error = managed.listener.open(managed.open_settings); error) {
            return error;
        }
        trade_listener_index = private_listeners.size() - 1;
        trade_replay_anchor_used = Plaza2TradeReplayAnchor{
            .trades_rev = pos_health->last_trades_rev,
            .trades_lifenum = pos_health->last_trades_lifenum,
            .server_time = pos_health->last_server_time,
        };
        // cg_lsn_open only initiates an asynchronous open. The immutable POS
        // anchor is selected here, but replay readiness is established only
        // after TRADE is ACTIVE and its snapshot has reached ONLINE.
        trade_replay_anchor_is_ready = false;
        return {};
    }

    [[nodiscard]] const private_state::StreamHealthSnapshot* stream_health(StreamCode code) const noexcept {
        const auto health = private_projector.stream_health();
        const auto found =
            std::find_if(health.begin(), health.end(), [&](const auto& stream) { return stream.stream_code == code; });
        return found == health.end() ? nullptr : &*found;
    }

    [[nodiscard]] bool pos_anchor_matches_selected() const noexcept {
        if (!trade_replay_anchor_used.has_value()) {
            return false;
        }
        const auto* pos = stream_health(StreamCode::kFortsPosRepl);
        return pos != nullptr && pos->online && pos->snapshot_complete &&
               pos->last_trades_rev == trade_replay_anchor_used->trades_rev &&
               pos->last_trades_lifenum == trade_replay_anchor_used->trades_lifenum;
    }

    Plaza2Error supervise_initial_listener_opens() {
        const auto now = std::chrono::steady_clock::now();
        for (auto& managed : private_listeners) {
            const auto* health = stream_health(managed.stream_code);
            managed.snapshot_completed_once =
                managed.snapshot_completed_once || (health != nullptr && health->online && health->snapshot_complete);

            std::uint32_t state = kCgStateClosed;
            if (const auto error = managed.listener.state(state); error) {
                return error;
            }
            if (state == kCgStateActive || state == kCgStateOpening) {
                continue;
            }
            if (state == kCgStateError) {
                if (managed.snapshot_completed_once) {
                    return invalid("replication listener entered ERROR after completing its initial snapshot; "
                                   "mid-run recovery is intentionally out of scope",
                                   Plaza2ErrorCode::AdapterState);
                }
                if (managed.stream_code == StreamCode::kFortsTradeRepl && !pos_anchor_matches_selected()) {
                    trade_replay_anchor_is_ready = false;
                    return invalid("FORTS_TRADE_REPL entered ERROR and the immutable POS replay anchor changed",
                                   Plaza2ErrorCode::AdapterState);
                }
                if (const auto error = private_bridge.on_plaza2_listener_event(Plaza2ListenerEvent{
                        .kind = Plaza2ListenerEventKind::Close, .stream_code = managed.stream_code});
                    error) {
                    return error;
                }
                if (const auto error = managed.listener.close(); error) {
                    return error;
                }
                managed.reopen_pending = true;
                managed.reopen_not_before = now + kListenerReopenDelay;
                continue;
            }
            if (state == kCgStateClosed && managed.reopen_pending && now >= managed.reopen_not_before) {
                if (managed.stream_code == StreamCode::kFortsTradeRepl && !pos_anchor_matches_selected()) {
                    trade_replay_anchor_is_ready = false;
                    return invalid("FORTS_TRADE_REPL retry refused because the immutable POS replay anchor changed",
                                   Plaza2ErrorCode::AdapterState);
                }
                if (const auto error = managed.listener.open(managed.open_settings); error) {
                    managed.reopen_not_before = now + kListenerReopenDelay;
                    continue;
                }
                managed.reopen_pending = false;
            }
        }
        return {};
    }

    Plaza2Error update_trade_replay_readiness() {
        if (!config.trade_replay_from_pos_anchor) {
            trade_replay_anchor_is_ready = true;
            return {};
        }
        trade_replay_anchor_is_ready = false;
        if (!trade_listener_index.has_value() || !trade_replay_anchor_used.has_value()) {
            return {};
        }
        if (!pos_anchor_matches_selected()) {
            return invalid("FORTS_TRADE_REPL replay no longer matches its immutable POS.info anchor",
                           Plaza2ErrorCode::AdapterState);
        }
        std::uint32_t state = kCgStateClosed;
        if (const auto error = private_listeners[*trade_listener_index].listener.state(state); error) {
            return error;
        }
        const auto* trade = stream_health(StreamCode::kFortsTradeRepl);
        trade_replay_anchor_is_ready =
            state == kCgStateActive && trade != nullptr && trade->online && trade->snapshot_complete;
        return {};
    }

    Plaza2Error poll() {
        if (!started) {
            return invalid("TEST session host is not started", Plaza2ErrorCode::AdapterState);
        }
        std::uint32_t runtime_code = 0;
        const auto error = connection.process(config.process_timeout_ms, &runtime_code);
        if (error) {
            if (!reply_bridge.error().empty()) {
                return invalid(reply_bridge.error(), Plaza2ErrorCode::CallbackFailed);
            }
            if (!private_bridge.callback_error().empty()) {
                return invalid(private_bridge.callback_error(), Plaza2ErrorCode::CallbackFailed);
            }
            return error;
        }
        if (const auto replay_error = open_deferred_trade_replay_if_anchored(); replay_error) {
            return replay_error;
        }
        if (const auto listener_error = supervise_initial_listener_opens(); listener_error) {
            return listener_error;
        }
        if (const auto readiness_error = update_trade_replay_readiness(); readiness_error) {
            return readiness_error;
        }
        if (!reply_bridge.error().empty()) {
            return invalid(reply_bridge.error(), Plaza2ErrorCode::CallbackFailed);
        }
        if (!private_bridge.callback_error().empty()) {
            return invalid(private_bridge.callback_error(), Plaza2ErrorCode::CallbackFailed);
        }
        return {};
    }

    Plaza2Error stop() {
        const auto has_resources = started || env.is_open() || connection.is_created() || publisher.is_created() ||
                                   reply_listener.is_created() || aggr_listener.is_created() ||
                                   !private_listeners.empty();
        if (!has_resources) {
            return {};
        }
        // Reverse dependency order: publisher/listeners, connection, then Env.
        static_cast<void>(publisher.close());
        static_cast<void>(publisher.destroy());
        publisher_is_open = false;
        static_cast<void>(reply_listener.close());
        static_cast<void>(reply_listener.destroy());
        reply_listener_is_open = false;
        static_cast<void>(aggr_listener.close());
        static_cast<void>(aggr_listener.destroy());
        for (auto it = private_listeners.rbegin(); it != private_listeners.rend(); ++it) {
            static_cast<void>(it->listener.close());
            static_cast<void>(it->listener.destroy());
        }
        private_listeners.clear();
        deferred_trade_stream.reset();
        trade_listener_index.reset();
        trade_replay_anchor_used.reset();
        trade_replay_anchor_is_ready = false;
        static_cast<void>(connection.close());
        static_cast<void>(connection.destroy());
        static_cast<void>(env.close());
        if (bridge_started) {
            static_cast<void>(private_bridge.end_run());
            bridge_started = false;
        }
        started = false;
        return {};
    }

    static void render(std::string& value, const std::string& credentials, const std::string& software_key) {
        replace_all(value, kCredentialToken, credentials);
        replace_all(value, kLegacyCredentialToken, credentials);
        replace_all(value, kSoftwareKeyToken, software_key);
    }

    static std::string render_copy(std::string value, const std::string& credentials, const std::string& software_key) {
        render(value, credentials, software_key);
        return value;
    }

    Plaza2TestSessionHostConfig config;
    cgate::Plaza2RuntimeProbeReport probe_report;
    cgate::Plaza2RuntimeProbeReport probe;
    cgate::Plaza2Settings effective_runtime;
    cgate::Plaza2Env env;
    cgate::Plaza2Connection connection;
    cgate::Plaza2Publisher publisher;
    cgate::Plaza2Listener reply_listener;
    cgate::Plaza2Listener aggr_listener;
    std::vector<ManagedPrivateListener> private_listeners;
    private_state::Plaza2PrivateStateProjector private_projector;
    cgate::Plaza2Aggr20BookProjector aggr_projector;
    PrivateProjectorBridge private_bridge;
    AggrProjectorBridge aggr_bridge;
    ReplyBridge reply_bridge;
    std::optional<Plaza2TestTradeStreamConfig> deferred_trade_stream;
    std::optional<std::size_t> trade_listener_index;
    std::optional<Plaza2TradeReplayAnchor> trade_replay_anchor_used;
    std::string credentials_value;
    std::string software_key_value;
    bool reply_listener_is_open{false};
    bool publisher_is_open{false};
    bool trade_replay_anchor_is_ready{false};
    bool bridge_started{false};
    bool started{false};
};

Plaza2TestSessionHost::Plaza2TestSessionHost(Plaza2TestSessionHostConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Plaza2TestSessionHost::~Plaza2TestSessionHost() {
    if (impl_) {
        static_cast<void>(impl_->stop());
    }
}

Plaza2TestSessionHost::Plaza2TestSessionHost(Plaza2TestSessionHost&&) noexcept = default;
Plaza2TestSessionHost& Plaza2TestSessionHost::operator=(Plaza2TestSessionHost&&) noexcept = default;

Plaza2Error Plaza2TestSessionHost::start() {
    return impl_->start();
}
Plaza2Error Plaza2TestSessionHost::poll() {
    return impl_->poll();
}
Plaza2Error Plaza2TestSessionHost::stop() {
    return impl_->stop();
}
bool Plaza2TestSessionHost::started() const noexcept {
    return impl_->started;
}
const cgate::Plaza2RuntimeProbeReport& Plaza2TestSessionHost::probe_report() const noexcept {
    return impl_->probe_report;
}
const private_state::Plaza2PrivateStateProjector& Plaza2TestSessionHost::private_state() const noexcept {
    return impl_->private_projector;
}
const cgate::Plaza2Aggr20BookProjector& Plaza2TestSessionHost::aggr20_projector() const noexcept {
    return impl_->aggr_projector;
}
bool Plaza2TestSessionHost::p2mqreply_open() const noexcept {
    return impl_->reply_listener_is_open;
}
bool Plaza2TestSessionHost::publisher_open() const noexcept {
    return impl_->publisher_is_open;
}
cgate::Plaza2PublisherCallCounts Plaza2TestSessionHost::publisher_call_counts() const noexcept {
    return impl_->publisher.call_counts();
}
Plaza2TestSessionHostMode Plaza2TestSessionHost::mode() const noexcept {
    return impl_->config.mode;
}
bool Plaza2TestSessionHost::trade_replay_anchor_ready() const noexcept {
    return impl_->trade_replay_anchor_is_ready;
}
std::optional<Plaza2TradeReplayAnchor> Plaza2TestSessionHost::trade_replay_anchor_used() const noexcept {
    return impl_->trade_replay_anchor_used;
}
bool Plaza2TestSessionHost::aggr_online() const noexcept {
    return impl_->aggr_bridge.online();
}
bool Plaza2TestSessionHost::aggr_snapshot_complete() const noexcept {
    return impl_->aggr_bridge.snapshot_complete();
}
std::vector<Plaza2TestSessionHost::ReplyEvent> Plaza2TestSessionHost::take_reply_events() {
    return impl_->reply_bridge.take();
}
const std::string& Plaza2TestSessionHost::last_callback_error() const noexcept {
    if (!impl_->reply_bridge.error().empty()) {
        return impl_->reply_bridge.error();
    }
    return impl_->private_bridge.callback_error();
}
Plaza2PublisherMessageResult Plaza2TestSessionHost::post(std::string_view message_name,
                                                         std::span<const std::byte> payload, std::uint32_t user_id,
                                                         bool need_reply) {
    if (impl_->config.mode == Plaza2TestSessionHostMode::LiveTestAuthorizedSend) {
        Plaza2PublisherMessageResult result;
        result.validation_error = invalid("authorized TEST send requires the validated trade transport");
        return result;
    }
    return post_validated(message_name, payload, user_id, need_reply);
}

Plaza2PublisherMessageResult Plaza2TestSessionHost::post_validated(std::string_view message_name,
                                                                   std::span<const std::byte> payload,
                                                                   std::uint32_t user_id, bool need_reply) {
    if (impl_->config.mode == Plaza2TestSessionHostMode::LiveTestPreSend) {
        Plaza2PublisherMessageResult result;
        result.certainty = cgate::Plaza2SubmissionCertainty::DefinitelyNotSent;
        result.validation_error = {
            .code = cgate::Plaza2ErrorCode::SendDisabledPreSendPhase,
            .runtime_code = 0,
            .message = "SEND_DISABLED_PRE_SEND_PHASE",
        };
        result.post_invoked = false;
        return result;
    }
    if (!impl_->started || (impl_->config.mode == Plaza2TestSessionHostMode::LiveTestAuthorizedSend &&
                            !impl_->config.arm_state.test_order_send_armed)) {
        Plaza2PublisherMessageResult result;
        result.validation_error = invalid("publisher requires a started, explicitly armed TEST host");
        return result;
    }
    Plaza2TradeCommandKind kind = Plaza2TradeCommandKind::AddOrder;
    if (message_name == "DelOrder") {
        kind = Plaza2TradeCommandKind::DelOrder;
    } else if (message_name == "DelUserOrders") {
        kind = Plaza2TradeCommandKind::DelUserOrders;
    }
    impl_->reply_bridge.arm(user_id, kind);
    return impl_->publisher.post_by_message_name(message_name, payload, user_id, need_reply);
}

struct Plaza2TestTradeTransport::Impl {
    struct TargetRefdataProvenanceAssessment {
        std::uint64_t current_lifenum{0};
        std::optional<private_state::SourceRowProvenance> fut_instruments;
        std::optional<private_state::SourceRowProvenance> fut_sess_contents;
        std::optional<private_state::SourceRowProvenance> session;
        bool ready{false};
    };

    struct PositionEvidenceAssessment {
        PositionEvidenceClass classification{PositionEvidenceClass::Unresolved};
        bool zero_starting_position_proven{false};
        bool position_snapshot_complete{false};
        std::int64_t trades_rev{0};
        std::int64_t trades_lifenum{0};
        std::int64_t server_time{0};
        bool trade_replay_complete{false};
        std::size_t participant_user_deal_count{0};
        std::size_t participant_user_multileg_deal_count{0};
        std::int64_t reconstructed_target_xpos{0};
        std::size_t active_own_order_count{0};
        std::optional<Plaza2TradeReplayAnchor> trade_replay_anchor_used;
    };

    explicit Impl(Plaza2TestTradeTransportConfig initial) : config(std::move(initial)), host(config.host) {}

    [[nodiscard]] const Plaza2AuthorizedOrderIntent* intent() const noexcept {
        return config.authorized_intent.has_value() ? &*config.authorized_intent : nullptr;
    }

    [[nodiscard]] std::int64_t target_isin() const noexcept {
        return intent() != nullptr && intent()->isin_id != 0 ? intent()->isin_id : config.target_isin_id;
    }

    [[nodiscard]] Plaza2TradeSide target_side() const noexcept {
        return intent() != nullptr ? intent()->side : config.target_side;
    }

    [[nodiscard]] std::uint32_t target_distance_ticks() const noexcept {
        if (intent() == nullptr || config.target_max_distance_ticks == 0) {
            return intent() != nullptr ? intent()->max_distance_ticks : config.target_max_distance_ticks;
        }
        return std::min(intent()->max_distance_ticks, config.target_max_distance_ticks);
    }

    [[nodiscard]] std::chrono::milliseconds effective_max_aggr20_age() const noexcept {
        const auto* authorized = intent();
        if (authorized == nullptr) {
            return std::chrono::milliseconds::zero();
        }
        const auto authorized_age =
            std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(authorized->max_aggr20_age_ms));
        if (config.max_aggr20_age.count() == 0) {
            return authorized_age;
        }
        return std::min(config.max_aggr20_age, authorized_age);
    }

    [[nodiscard]] bool require_zero_starting_position() const noexcept {
        return intent() != nullptr &&
               (intent()->require_zero_starting_position || config.require_zero_starting_position);
    }

    [[nodiscard]] std::int8_t expected_position_account_type() const noexcept {
        // FORTS_POS_REPL uses account_type=1 for the brokerage-firm row and
        // account_type=2 for a normal client row. The locked BF form uses
        // client_code=000.
        return intent() != nullptr && intent()->client_code == "000" ? 1 : 2;
    }

    [[nodiscard]] std::string participant_code() const {
        if (intent() != nullptr && !intent()->broker_code.empty() && !intent()->client_code.empty()) {
            return intent()->broker_code + intent()->client_code;
        }
        return config.observation_client_code;
    }

    [[nodiscard]] const private_state::StreamHealthSnapshot* stream_health(StreamCode stream_code) const noexcept {
        const auto health = host.private_state().stream_health();
        const auto found = std::find_if(health.begin(), health.end(), [stream_code](const auto& stream) {
            return stream.stream_code == stream_code;
        });
        return found == health.end() ? nullptr : &*found;
    }

    [[nodiscard]] TargetRefdataProvenanceAssessment target_refdata_provenance() const {
        TargetRefdataProvenanceAssessment result;
        if (target_isin() <= 0 || target_isin() > std::numeric_limits<std::int32_t>::max()) {
            return result;
        }
        const auto& state = host.private_state();
        const auto lifenum = state.refdata_lifenum();
        result.fut_instruments = state.instrument_source_provenance(
            generated::TableCode::kFortsRefdataReplFutInstruments, static_cast<std::int32_t>(target_isin()));
        result.fut_sess_contents = state.instrument_source_provenance(
            generated::TableCode::kFortsRefdataReplFutSessContents, static_cast<std::int32_t>(target_isin()));
        result.session =
            state.session_source_provenance(generated::TableCode::kFortsRefdataReplSession, config.target_session_id);
        if (!lifenum.has_value()) {
            return result;
        }
        result.current_lifenum = *lifenum;
        const auto current = [&](const auto& provenance, generated::TableCode expected_table) {
            return provenance.has_value() && provenance->present &&
                   provenance->stream_code == StreamCode::kFortsRefdataRepl &&
                   provenance->table_code == expected_table && provenance->lifenum == result.current_lifenum;
        };
        result.ready = current(result.fut_instruments, generated::TableCode::kFortsRefdataReplFutInstruments) &&
                       current(result.fut_sess_contents, generated::TableCode::kFortsRefdataReplFutSessContents) &&
                       current(result.session, generated::TableCode::kFortsRefdataReplSession);
        return result;
    }

    [[nodiscard]] bool participant_trade(const private_state::OwnTradeSnapshot& trade) const {
        const auto participant = participant_code();
        if (participant.empty()) {
            return false;
        }
        return trade.code_buy == participant || trade.code_sell == participant || trade.login_buy == participant ||
               trade.login_sell == participant;
    }

    [[nodiscard]] bool active_participant_order(const private_state::OwnOrderSnapshot& order) const {
        if (order.isin_id != target_isin()) {
            return false;
        }
        // Trade-replication history alone is not an active USERORDERBOOK
        // order.  Only an order currently present in a USERORDERBOOK table
        // can block the pre-send flatness gate.
        if (!order.from_user_book && !order.from_current_day) {
            return false;
        }
        const auto participant = participant_code();
        if (!participant.empty() && order.client_code != participant && order.login_from != participant &&
            (intent() == nullptr || order.client_code != intent()->client_code)) {
            return false;
        }
        return order.public_amount_rest > 0 || order.private_amount_rest > 0;
    }

    [[nodiscard]] PositionEvidenceAssessment assess_position_evidence() const {
        PositionEvidenceAssessment assessment;
        const auto target = target_isin();
        const auto* position_health = stream_health(StreamCode::kFortsPosRepl);
        if (position_health != nullptr) {
            assessment.position_snapshot_complete = position_health->online && position_health->snapshot_complete;
            assessment.trades_rev = position_health->last_trades_rev;
            assessment.trades_lifenum = position_health->last_trades_lifenum;
            assessment.server_time = position_health->last_server_time;
        }

        for (const auto& trade : host.private_state().own_trades()) {
            if (trade.isin_id != target || !participant_trade(trade)) {
                continue;
            }
            if (trade.multileg) {
                ++assessment.participant_user_multileg_deal_count;
            } else {
                ++assessment.participant_user_deal_count;
            }
        }
        assessment.active_own_order_count = static_cast<std::size_t>(
            std::count_if(host.private_state().own_orders().begin(), host.private_state().own_orders().end(),
                          [&](const auto& order) { return active_participant_order(order); }));

        const auto expected_account_type = expected_position_account_type();
        const auto positions = host.private_state().positions();
        const auto matching_positions = std::count_if(positions.begin(), positions.end(), [&](const auto& position) {
            return position.scope == private_state::PositionScope::kClient && position.isin_id == target &&
                   position.account_code == participant_code() && position.account_type == expected_account_type;
        });
        if (matching_positions == 1) {
            const auto position = std::find_if(positions.begin(), positions.end(), [&](const auto& candidate) {
                return candidate.scope == private_state::PositionScope::kClient && candidate.isin_id == target &&
                       candidate.account_code == participant_code() && candidate.account_type == expected_account_type;
            });
            assessment.reconstructed_target_xpos = position->xpos;
            if (position->xpos == 0) {
                assessment.classification = PositionEvidenceClass::ExactZeroPosRow;
                assessment.zero_starting_position_proven = true;
                return assessment;
            }
            return assessment;
        }
        if (matching_positions != 0 || !config.host.trade_replay_from_pos_anchor || !host.trade_replay_anchor_ready() ||
            !host.trade_replay_anchor_used().has_value() || position_health == nullptr ||
            !assessment.position_snapshot_complete ||
            (assessment.server_time == 0 && !(host.probe_report().fake_runtime_marker_present &&
                                              (assessment.trades_rev != 0 || assessment.trades_lifenum != 0)))) {
            return assessment;
        }
        assessment.trade_replay_anchor_used = host.trade_replay_anchor_used();
        if (assessment.trades_rev != assessment.trade_replay_anchor_used->trades_rev ||
            assessment.trades_lifenum != assessment.trade_replay_anchor_used->trades_lifenum) {
            return assessment;
        }

        const auto* trade_health = stream_health(StreamCode::kFortsTradeRepl);
        const bool fake_trade_replay = host.probe_report().fake_runtime_marker_present && trade_health != nullptr &&
                                       trade_health->committed_row_count != 0;
        if (trade_health != nullptr && trade_health->online && trade_health->snapshot_complete &&
            (trade_health->last_server_time != 0 || fake_trade_replay) &&
            (assessment.server_time == 0 || trade_health->last_server_time >= assessment.server_time)) {
            assessment.trade_replay_complete = true;
        }
        if (assessment.trade_replay_complete && assessment.participant_user_deal_count == 0 &&
            assessment.participant_user_multileg_deal_count == 0 && assessment.active_own_order_count == 0) {
            assessment.classification = PositionEvidenceClass::FlatByPosSnapshotAndTradeReplay;
            assessment.zero_starting_position_proven = true;
            assessment.reconstructed_target_xpos = 0;
        }
        return assessment;
    }

    Plaza2Error validate_intent() const {
        const auto* authorized = intent();
        if (authorized == nullptr) {
            return invalid("AddOrder requires a validated authorized intent");
        }
        if (!valid_hex_sha256(authorized->sha256) || !valid_hex_sha256(authorized->profile_fingerprint) ||
            !valid_hex_sha256(authorized->add_payload_sha256) ||
            !valid_hex_sha256(authorized->recovery_payload_sha256) || !valid_hex_sha256(authorized->policy_sha256)) {
            return invalid("authorized intent hashes are not valid SHA-256 values");
        }
        if (authorized->max_aggr20_age_ms == 0 ||
            authorized->max_aggr20_age_ms >
                static_cast<std::uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max()) ||
            authorized->quantity != 1 || authorized->isin_id <= 0 || authorized->ext_id <= 0 ||
            authorized->add_user_id == 0 || authorized->cancel_user_id == 0 || authorized->recovery_user_id == 0 ||
            authorized->add_user_id == authorized->cancel_user_id ||
            authorized->add_user_id == authorized->recovery_user_id ||
            authorized->cancel_user_id == authorized->recovery_user_id || authorized->base_contract_code.empty() ||
            authorized->price.empty() || authorized->profile_id.empty() || authorized->profile_fingerprint.empty() ||
            authorized->environment != "test" ||
            (authorized->instrument_mask != 1 && authorized->instrument_mask != 2 &&
             authorized->instrument_mask != 4)) {
            return invalid("authorized intent is incomplete or not a one-contract TEST intent");
        }
        if (host.mode() == Plaza2TestSessionHostMode::LiveTestAuthorizedSend &&
            (!authorized->require_zero_starting_position || authorized->max_distance_ticks > 4 ||
             authorized->max_aggr20_age_ms > 5000)) {
            return invalid("authorized TEST send requires zero position, max four ticks and max 5000 ms");
        }
        const auto canonical = canonical_authorized_order_intent_json(*authorized);
        if (!authorized->canonical_json.empty() && authorized->canonical_json != canonical) {
            return invalid("authorized intent canonical JSON does not match its fields");
        }
        if (authorized_order_intent_sha256(*authorized) != authorized->sha256) {
            return invalid("authorized intent SHA-256 does not match canonical intent");
        }
        const auto broker_hash = intent_fingerprint(*authorized, true);
        const auto client_hash = intent_fingerprint(*authorized, false);
        if (!valid_hex_sha256(broker_hash) || !valid_hex_sha256(client_hash)) {
            return invalid("authorized intent lacks broker/client identity fingerprints");
        }
        if (authorized->broker_code.size() != 4) {
            return invalid("authorized broker_code must be the reviewed four-character value");
        }
        if (authorized->client_code.size() != 3) {
            return invalid("authorized client_code must be the reviewed three-character value");
        }
        if (config.target_isin_id != 0 && config.target_isin_id != authorized->isin_id) {
            return invalid("duplicated target_isin_id contradicts the authorized intent");
        }
        if (!config.target_price.empty() && config.target_price != authorized->price) {
            return invalid("duplicated target_price contradicts the authorized intent");
        }
        if (!config.target_price.empty() && config.target_side != authorized->side) {
            return invalid("duplicated target_side contradicts the authorized intent");
        }
        if (config.target_max_distance_ticks > authorized->max_distance_ticks) {
            return invalid("transport distance override cannot weaken authorized policy");
        }
        if (config.max_aggr20_age.count() > 0 &&
            config.max_aggr20_age >
                std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(authorized->max_aggr20_age_ms))) {
            return invalid("transport AGGR20 age override cannot weaken authorized policy");
        }
        if (config.observation_ext_id != 0 && config.observation_ext_id != authorized->ext_id) {
            return invalid("duplicated observation_ext_id contradicts the authorized intent");
        }
        if (config.observation_ext_id != 0 && config.observation_quantity != authorized->quantity) {
            return invalid("duplicated observation quantity contradicts the authorized intent");
        }
        if (config.observation_ext_id != 0 && config.observation_side != authorized->side) {
            return invalid("duplicated observation side contradicts the authorized intent");
        }
        if (!config.observation_client_code.empty() && config.observation_client_code != authorized->client_code &&
            config.observation_client_code != participant_code()) {
            return invalid("observation client identity cannot select a different participant");
        }
        return {};
    }

    Plaza2Error bind_authorized_plan(const PreSendPlan& plan) {
        if (!plan.ok || plan.sha256.empty() || plan.canonical_json.empty()) {
            return invalid("cannot bind an invalid pre-send plan");
        }
        if (const auto intent_error = validate_intent(); intent_error) {
            return intent_error;
        }
        const auto* authorized = intent();
        if (authorized == nullptr || plan.sha256 != authorized->sha256 ||
            plan.canonical_json != canonical_authorized_order_intent_json(*authorized) ||
            cgate::plaza2_sha256_hex(plan.add_command.payload) != authorized->add_payload_sha256 ||
            cgate::plaza2_sha256_hex(plan.exact_ext_id_recovery_command.payload) !=
                authorized->recovery_payload_sha256) {
            return invalid("pre-send plan is not the exact authorized order intent");
        }
        bound_authorized_plan_sha256 = plan.sha256;
        return {};
    }

    Plaza2Error install_authorized_intent(Plaza2AuthorizedOrderIntent candidate) {
        if (!bound_authorized_plan_sha256.empty()) {
            return invalid("authorized intent installation is closed after plan binding");
        }
        if (config.authorized_intent.has_value()) {
            return invalid("authorized intent is already installed and immutable");
        }
        if (!host.started()) {
            return invalid("authorized intent installation requires a started host");
        }

        // The slot is known to be empty above.  Roll it back on every
        // validation failure so a rejected candidate cannot poison a later
        // valid installation.
        config.authorized_intent = std::move(candidate);
        if (const auto validation = validate_intent(); validation) {
            config.authorized_intent.reset();
            return validation;
        }
        return {};
    }

    Plaza2Error validate_add_payload(const Plaza2TradeEncodedCommand& command, std::uint32_t user_id) const {
        const auto* authorized = intent();
        if (authorized == nullptr || command.command_kind != Plaza2TradeCommandKind::AddOrder || command.msgid != 474 ||
            user_id != authorized->add_user_id ||
            cgate::plaza2_sha256_hex(command.payload) != authorized->add_payload_sha256) {
            return invalid("AddOrder command is not bound to the authorized intent");
        }
        const auto decoded = decode_add_payload(command.payload);
        if (!decoded.has_value() || decoded->isin_id != authorized->isin_id || decoded->side != authorized->side ||
            decoded->order_type != Plaza2TradeOrderType::Limit || decoded->amount != authorized->quantity ||
            decoded->price != authorized->price || decoded->ext_id != authorized->ext_id ||
            cgate::plaza2_sha256_hex(decoded->broker_code) != intent_fingerprint(*authorized, true) ||
            cgate::plaza2_sha256_hex(decoded->client_code) != intent_fingerprint(*authorized, false)) {
            return invalid("encoded AddOrder semantics contradict the authorized intent");
        }
        if ((!authorized->broker_code.empty() && decoded->broker_code != authorized->broker_code) ||
            (!authorized->client_code.empty() && decoded->client_code != authorized->client_code)) {
            return invalid("encoded AddOrder participant identity contradicts the authorized intent");
        }
        return {};
    }

    Plaza2Error validate_recovery_payload(const Plaza2TradeEncodedCommand& command, std::uint32_t user_id) const {
        const auto* authorized = intent();
        if (authorized == nullptr || command.command_kind != Plaza2TradeCommandKind::DelUserOrders ||
            command.msgid != 466 || user_id != authorized->recovery_user_id ||
            cgate::plaza2_sha256_hex(command.payload) != authorized->recovery_payload_sha256) {
            return invalid("exact-ext recovery command is not bound to the authorized intent");
        }
        const auto decoded = decode_recovery_payload(command.payload);
        if (!decoded.has_value() || decoded->ext_id != authorized->ext_id || decoded->isin_id != authorized->isin_id ||
            decoded->base_contract_code != authorized->base_contract_code ||
            decoded->instrument_mask != authorized->instrument_mask ||
            decoded->buy_sell != static_cast<std::int32_t>(authorized->side) ||
            cgate::plaza2_sha256_hex(decoded->broker_code) != intent_fingerprint(*authorized, true) ||
            cgate::plaza2_sha256_hex(decoded->client_code) != intent_fingerprint(*authorized, false)) {
            return invalid("exact-ext recovery semantics contradict the authorized intent");
        }
        return {};
    }

    Plaza2Error preflight_target() {
        if (const auto intent_error = validate_intent(); intent_error) {
            return intent_error;
        }
        const auto target = target_isin();
        if (target == 0) {
            return invalid("AddOrder requires an exact target isin_id");
        }
        if (config.target_session_id == 0) {
            return invalid("AddOrder requires an exact target session id");
        }
        if (const auto error = host.poll(); error) {
            return error;
        }
        if (config.host.trade_replay_from_pos_anchor && host.trade_replay_anchor_used().has_value() &&
            !host.trade_replay_anchor_ready()) {
            // The first poll can select the POS anchor and only initiate the
            // asynchronous TRADE open. Give the newly created listener one
            // process turn; readiness still requires ACTIVE plus ONLINE.
            if (const auto error = host.poll(); error) {
                return error;
            }
        }
        if (config.host.trade_replay_from_pos_anchor && host.trade_replay_anchor_ready()) {
            const auto trade_health =
                std::find_if(host.private_state().stream_health().begin(), host.private_state().stream_health().end(),
                             [](const auto& stream) { return stream.stream_code == StreamCode::kFortsTradeRepl; });
            if (trade_health == host.private_state().stream_health().end() || !trade_health->online ||
                !trade_health->snapshot_complete) {
                if (const auto error = host.poll(); error) {
                    return error;
                }
            }
        }
        if (!host.aggr_online() || !host.aggr_snapshot_complete()) {
            return invalid("target AGGR20 replication is not online and snapshot-complete");
        }
        const auto scoped = host.aggr20_projector().snapshot_for_isin(target);
        if (!scoped.has_value() || !scoped->top_bid.has_value() || !scoped->top_ask.has_value()) {
            return invalid("target AGGR20 snapshot is missing or not two-sided");
        }
        if (scoped->top_bid->price_scaled >= scoped->top_ask->price_scaled) {
            return invalid("target AGGR20 BBO is crossed or locked");
        }
        if (std::chrono::steady_clock::now() - scoped->committed_at > effective_max_aggr20_age()) {
            return invalid("target AGGR20 snapshot is stale");
        }
        const auto& private_state = host.private_state();
        const auto instrument = std::find_if(private_state.instruments().begin(), private_state.instruments().end(),
                                             [&](const auto& candidate) { return candidate.isin_id == target; });
        if (instrument == private_state.instruments().end()) {
            return invalid("target instrument is absent from committed refdata");
        }
        if (!config.target_tick_size.empty()) {
            const auto configured_tick = parse_scaled_decimal(config.target_tick_size);
            const auto authoritative_tick = parse_scaled_decimal(instrument->min_step);
            if (!configured_tick.has_value() || !authoritative_tick.has_value() ||
                *configured_tick != *authoritative_tick) {
                return invalid("operator target tick size contradicts authoritative instrument min_step");
            }
        }
        if (instrument->kind != plaza2::private_state::InstrumentKind::kFuture || instrument->min_step.empty() ||
            instrument->trade_mode_id == 0 || !instrument->current_session_member ||
            instrument->sess_id != config.target_session_id ||
            (instrument->has_current_status && !add_order_allowed_status(instrument->current_status)) ||
            !instrument->has_current_status) {
            return invalid("target instrument is absent or not Add-capable in the current trading-day status");
        }
        if (config.target_session_id != 0) {
            const auto session =
                std::find_if(private_state.sessions().begin(), private_state.sessions().end(),
                             [&](const auto& candidate) { return candidate.sess_id == config.target_session_id; });
            const auto effective_state = session == private_state.sessions().end()
                                             ? 0
                                             : (session->has_current_status ? session->current_status : session->state);
            if (session == private_state.sessions().end() || !add_order_allowed_status(effective_state) ||
                !session->has_current_status) {
                return invalid("target trading session is missing or not tradable");
            }
        }
        const auto target_provenance = target_refdata_provenance();
        if (!target_provenance.ready) {
            return invalid("target REFDATA rows lack exact current-LifeNum provenance");
        }
        const auto matching_limit_count =
            std::count_if(private_state.limits().begin(), private_state.limits().end(), [&](const auto& candidate) {
                return candidate.scope == plaza2::private_state::PositionScope::kClient &&
                       candidate.account_code == participant_code() && candidate.limits_set;
            });
        const auto limit =
            std::find_if(private_state.limits().begin(), private_state.limits().end(), [&](const auto& candidate) {
                return candidate.scope == plaza2::private_state::PositionScope::kClient &&
                       candidate.account_code == participant_code() && candidate.limits_set;
            });
        if (limit == private_state.limits().end() || matching_limit_count != 1) {
            return invalid(matching_limit_count == 0 ? "applicable committed client limit row is missing or unset"
                                                     : "multiple applicable committed client limit rows are ambiguous");
        }
        const auto position_evidence = assess_position_evidence();
        if (require_zero_starting_position() && !position_evidence.zero_starting_position_proven) {
            const auto any_target_account = std::count_if(
                private_state.positions().begin(), private_state.positions().end(), [&](const auto& position) {
                    return position.scope == plaza2::private_state::PositionScope::kClient &&
                           position.isin_id == target && position.account_code == participant_code();
                });
            if (any_target_account != 0) {
                return invalid("target starting position row has the wrong account type or is not zero");
            }
            return invalid("target starting position evidence is unresolved (POS plus anchored TRADE replay required)");
        }
        if (position_evidence.active_own_order_count != 0 && host.mode() != Plaza2TestSessionHostMode::OfflineFake) {
            return invalid("target has active own orders in the current USERORDERBOOK state");
        }
        const auto private_ready =
            std::all_of(private_state.stream_health().begin(), private_state.stream_health().end(),
                        [](const auto& stream) { return stream.online && stream.snapshot_complete; });
        const auto exact_private_set_present =
            std::all_of(kRequiredPrivateStreams.begin(), kRequiredPrivateStreams.end(), [&](const auto required) {
                return std::count_if(private_state.stream_health().begin(), private_state.stream_health().end(),
                                     [&](const auto& stream) { return stream.stream_code == required; }) == 1;
            });
        const auto userorderbook = std::find_if(
            private_state.stream_health().begin(), private_state.stream_health().end(),
            [](const auto& stream) { return stream.stream_code == generated::StreamCode::kFortsUserorderbookRepl; });
        const bool userorderbook_periodic_consistent =
            userorderbook != private_state.stream_health().end() && userorderbook->periodic_snapshot_consistent;
        if (!private_ready || !exact_private_set_present || !userorderbook_periodic_consistent) {
            return invalid("private replication is not online and snapshot-complete for every stream");
        }
        if (!host.probe_report().trading_capable) {
            return invalid("TEST runtime is not trading-capable");
        }
        if (!host.p2mqreply_open()) {
            return invalid("p2mqreply listener is not open");
        }
        if (!host.publisher_open()) {
            return invalid("publisher is not open");
        }
        return {};
    }

    Plaza2Error persist_execution_safety_receipt() {
        if (const auto intent_error = validate_intent(); intent_error) {
            return intent_error;
        }
        if (config.execution_safety_receipt_path.empty()) {
            return invalid("AddOrder requires an execution-safety receipt path");
        }
        const auto target = target_isin();
        const auto scoped = host.aggr20_projector().snapshot_for_isin(target);
        if (!scoped.has_value() || !scoped->top_bid.has_value() || !scoped->top_ask.has_value()) {
            return invalid("cannot persist execution-safety receipt without a two-sided target BBO");
        }
        if (scoped->top_bid->price_scaled >= scoped->top_ask->price_scaled) {
            return invalid("cannot persist execution-safety receipt with a crossed or locked target BBO");
        }
        const auto& state = host.private_state();
        const auto target_provenance = target_refdata_provenance();
        const auto instrument = std::find_if(state.instruments().begin(), state.instruments().end(),
                                             [&](const auto& candidate) { return candidate.isin_id == target; });
        const auto session = std::find_if(state.sessions().begin(), state.sessions().end(), [&](const auto& candidate) {
            return candidate.sess_id == config.target_session_id;
        });
        const auto limit = std::find_if(state.limits().begin(), state.limits().end(), [&](const auto& candidate) {
            return candidate.scope == plaza2::private_state::PositionScope::kClient &&
                   candidate.account_code == participant_code() && candidate.limits_set;
        });
        const auto position_evidence = assess_position_evidence();
        const auto expected_account_type = expected_position_account_type();
        const auto position =
            std::find_if(state.positions().begin(), state.positions().end(), [&](const auto& candidate) {
                return candidate.scope == plaza2::private_state::PositionScope::kClient &&
                       candidate.isin_id == target && candidate.account_code == participant_code() &&
                       candidate.account_type == expected_account_type;
            });
        if (!target_provenance.ready || instrument == state.instruments().end() || session == state.sessions().end() ||
            limit == state.limits().end()) {
            return invalid("execution-safety receipt lacks target refdata/session/limit evidence");
        }
        if (require_zero_starting_position() && !position_evidence.zero_starting_position_proven) {
            return invalid("execution-safety receipt lacks the required starting position evidence");
        }

        Plaza2ExecutionSafetyReceipt receipt;
        receipt.authorized_intent_sha256 = config.authorized_intent->sha256;
        receipt.target_isin_id = target;
        receipt.target_refdata_lifenum = target_provenance.current_lifenum;
        receipt.target_fut_instruments_provenance = *target_provenance.fut_instruments;
        receipt.target_fut_sess_contents_provenance = *target_provenance.fut_sess_contents;
        receipt.target_session_provenance = *target_provenance.session;
        receipt.target_refdata_provenance_ready = true;
        receipt.aggr20 = scoped;
        receipt.instrument = *instrument;
        receipt.session = *session;
        receipt.limit = *limit;
        if (position != state.positions().end()) {
            receipt.position = *position;
        }
        receipt.position_evidence_class = position_evidence.classification;
        receipt.zero_starting_position_proven = position_evidence.zero_starting_position_proven;
        receipt.position_snapshot_complete = position_evidence.position_snapshot_complete;
        receipt.position_trades_rev = position_evidence.trades_rev;
        receipt.position_trades_lifenum = position_evidence.trades_lifenum;
        receipt.position_server_time = position_evidence.server_time;
        receipt.trade_replay_complete = position_evidence.trade_replay_complete;
        receipt.participant_user_deal_count = position_evidence.participant_user_deal_count;
        receipt.participant_user_multileg_deal_count = position_evidence.participant_user_multileg_deal_count;
        receipt.reconstructed_target_xpos = position_evidence.reconstructed_target_xpos;
        receipt.active_own_order_count = position_evidence.active_own_order_count;
        receipt.trade_replay_anchor_used = position_evidence.trade_replay_anchor_used;
        receipt.runtime_compatibility =
            std::string(cgate::plaza2_compatibility_name(host.probe_report().compatibility));
        receipt.runtime_scheme_sha256 = host.probe_report().scheme_drift.runtime_scheme_sha256;
        receipt.runtime_scheme_fatal_drift_count = host.probe_report().scheme_drift.fatal_drift_count;
        receipt.runtime_scheme_warning_drift_count = host.probe_report().scheme_drift.warning_drift_count;
        receipt.aggr_online = host.aggr_online();
        receipt.aggr_snapshot_complete = host.aggr_snapshot_complete();
        receipt.limit_fingerprint_sha256 = limit_row_fingerprint(*limit);
        for (const auto& stream : state.stream_health()) {
            if (stream.stream_code == generated::StreamCode::kFortsPartRepl) {
                receipt.limit_source_commit_sequence = stream.last_commit_sequence;
            }
            if (stream.stream_code == generated::StreamCode::kFortsPosRepl) {
                receipt.position_source_commit_sequence = stream.last_commit_sequence;
            }
        }
        if (receipt.position.has_value()) {
            receipt.position_fingerprint_sha256 = position_row_fingerprint(*receipt.position);
        }
        {
            std::ostringstream stream_json;
            stream_json << "[";
            bool first_stream = true;
            for (const auto& stream : state.stream_health()) {
                if (!first_stream) {
                    stream_json << ",";
                }
                first_stream = false;
                stream_json << "{\"code\":" << static_cast<std::uint32_t>(stream.stream_code)
                            << ",\"online\":" << (stream.online ? "true" : "false")
                            << ",\"snapshot_complete\":" << (stream.snapshot_complete ? "true" : "false")
                            << ",\"periodic_snapshot_consistent\":"
                            << (stream.periodic_snapshot_consistent ? "true" : "false")
                            << ",\"commit_sequence\":" << stream.last_commit_sequence << "}";
            }
            stream_json << "]";
            receipt.private_streams_json = stream_json.str();
        }
        receipt.local_age = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                  scoped->committed_at);
        receipt.authorized_max_aggr20_age_ms = config.authorized_intent->max_aggr20_age_ms;
        receipt.require_zero_starting_position = require_zero_starting_position();
        if (receipt.local_age > effective_max_aggr20_age()) {
            return invalid("execution-safety receipt observed a stale target AGGR20 snapshot");
        }
        receipt.quantity_one = config.authorized_intent->quantity == 1;
        receipt.target_aggr20_uncrossed = scoped->top_bid->price_scaled < scoped->top_ask->price_scaled;
        receipt.private_streams_ready =
            std::all_of(state.stream_health().begin(), state.stream_health().end(),
                        [](const auto& stream) { return stream.online && stream.snapshot_complete; });
        const auto userorderbook =
            std::find_if(state.stream_health().begin(), state.stream_health().end(), [](const auto& stream) {
                return stream.stream_code == generated::StreamCode::kFortsUserorderbookRepl;
            });
        receipt.userorderbook_periodic_snapshot_consistent =
            userorderbook != state.stream_health().end() && userorderbook->periodic_snapshot_consistent;
        receipt.private_streams_ready =
            receipt.private_streams_ready && receipt.userorderbook_periodic_snapshot_consistent;
        receipt.p2mqreply_open = host.p2mqreply_open();
        receipt.publisher_open = host.publisher_open();
        receipt.trading_capable = host.probe_report().trading_capable;
        receipt.test_order_send_armed = config.host.arm_state.test_order_send_armed;
        receipt.send_mode =
            host.mode() == Plaza2TestSessionHostMode::LiveTestAuthorizedSend ? "LiveTestAuthorizedSend" : "";

        const auto price = parse_scaled_decimal(config.authorized_intent->price);
        const auto tick = parse_scaled_decimal(instrument->min_step);
        if (price.has_value() && tick.has_value() && *tick > 0) {
            receipt.passive_non_marketable = target_side() == Plaza2TradeSide::Buy
                                                 ? *price < scoped->top_ask->price_scaled
                                                 : *price > scoped->top_bid->price_scaled;
            const auto distance = target_side() == Plaza2TradeSide::Buy
                                      ? std::max<std::int64_t>(0, scoped->top_bid->price_scaled - *price)
                                      : std::max<std::int64_t>(0, *price - scoped->top_ask->price_scaled);
            receipt.bbo_distance_allowed =
                distance % *tick == 0 && static_cast<std::uint64_t>(distance / *tick) <= target_distance_ticks();
        }

        std::ostringstream json;
        json << "{\n"
             << "  \"schema\": \"moex.plaza2.execution_safety.v3\",\n"
             << "  \"authorized_intent_sha256\": \"" << receipt.authorized_intent_sha256 << "\",\n"
             << "  \"target_isin_id\": " << receipt.target_isin_id << ",\n"
             << "  \"target_refdata_lifenum\": " << receipt.target_refdata_lifenum << ",\n"
             << "  \"target_fut_instruments_provenance\": {\"stream_code\":"
             << static_cast<std::uint32_t>(receipt.target_fut_instruments_provenance.stream_code)
             << ",\"table_code\":" << static_cast<std::uint32_t>(receipt.target_fut_instruments_provenance.table_code)
             << ",\"isin_id\":" << receipt.target_isin_id
             << ",\"repl_rev\":" << receipt.target_fut_instruments_provenance.repl_rev
             << ",\"lifenum\":" << receipt.target_fut_instruments_provenance.lifenum << ",\"present\":true},\n"
             << "  \"target_fut_sess_contents_provenance\": {\"stream_code\":"
             << static_cast<std::uint32_t>(receipt.target_fut_sess_contents_provenance.stream_code)
             << ",\"table_code\":" << static_cast<std::uint32_t>(receipt.target_fut_sess_contents_provenance.table_code)
             << ",\"isin_id\":" << receipt.target_isin_id
             << ",\"repl_rev\":" << receipt.target_fut_sess_contents_provenance.repl_rev
             << ",\"lifenum\":" << receipt.target_fut_sess_contents_provenance.lifenum << ",\"present\":true},\n"
             << "  \"target_session_provenance\": {\"stream_code\":"
             << static_cast<std::uint32_t>(receipt.target_session_provenance.stream_code)
             << ",\"table_code\":" << static_cast<std::uint32_t>(receipt.target_session_provenance.table_code)
             << ",\"sess_id\":" << session->sess_id << ",\"repl_rev\":" << receipt.target_session_provenance.repl_rev
             << ",\"lifenum\":" << receipt.target_session_provenance.lifenum << ",\"present\":true},\n"
             << "  \"top_bid\": " << scoped->top_bid->price_scaled << ",\n"
             << "  \"top_ask\": " << scoped->top_ask->price_scaled << ",\n"
             << "  \"aggr20_repl_id\": " << scoped->last_repl_id << ",\n"
             << "  \"aggr20_repl_rev\": " << scoped->last_repl_rev << ",\n"
             << "  \"local_age_ms\": " << receipt.local_age.count() << ",\n"
             << "  \"authorized_max_aggr20_age_ms\": " << receipt.authorized_max_aggr20_age_ms << ",\n"
             << "  \"require_zero_starting_position\": " << (receipt.require_zero_starting_position ? "true" : "false")
             << ",\n"
             << "  \"exchange_moment\": " << scoped->exchange_moment << ",\n"
             << "  \"exchange_moment_ns\": " << scoped->exchange_moment_ns << ",\n"
             << "  \"aggr_online\": " << (receipt.aggr_online ? "true" : "false") << ",\n"
             << "  \"aggr_snapshot_complete\": " << (receipt.aggr_snapshot_complete ? "true" : "false") << ",\n"
             << "  \"session_id\": " << session->sess_id << ",\n"
             << "  \"session_state\": " << session->state << ",\n"
             << "  \"session_current_status\": " << session->current_status << ",\n"
             << "  \"instrument_kind\": " << static_cast<int>(instrument->kind) << ",\n"
             << "  \"instrument_min_step\": \"" << instrument->min_step << "\",\n"
             << "  \"session_status_source\": \"FORTS_SESSIONSTATE_REPL\",\n"
             << "  \"instrument_current_status\": " << instrument->current_status << ",\n"
             << "  \"instrument_status_source\": \"FORTS_INSTRUMENTSTATE_REPL\",\n"
             << "  \"runtime_compatibility\": \"" << receipt.runtime_compatibility << "\",\n"
             << "  \"runtime_scheme_sha256\": \"" << receipt.runtime_scheme_sha256 << "\",\n"
             << "  \"runtime_scheme_fatal_drift_count\": " << receipt.runtime_scheme_fatal_drift_count << ",\n"
             << "  \"runtime_scheme_warning_drift_count\": " << receipt.runtime_scheme_warning_drift_count << ",\n"
             << "  \"limits_account_sha256\": \"" << cgate::plaza2_sha256_hex(limit->account_code) << "\",\n"
             << "  \"limit_row_fingerprint_sha256\": \"" << receipt.limit_fingerprint_sha256 << "\",\n"
             << "  \"limit_source_commit_sequence\": " << receipt.limit_source_commit_sequence << ",\n"
             << "  \"position_row_fingerprint_sha256\": \"" << receipt.position_fingerprint_sha256 << "\",\n"
             << "  \"position_source_commit_sequence\": " << receipt.position_source_commit_sequence << ",\n"
             << "  \"position_evidence_class\": \"" << position_evidence_class_name(receipt.position_evidence_class)
             << "\",\n"
             << "  \"zero_starting_position_proven\": " << (receipt.zero_starting_position_proven ? "true" : "false")
             << ",\n"
             << "  \"position_snapshot_complete\": " << (receipt.position_snapshot_complete ? "true" : "false") << ",\n"
             << "  \"position_trades_rev\": " << receipt.position_trades_rev << ",\n"
             << "  \"position_trades_lifenum\": " << receipt.position_trades_lifenum << ",\n"
             << "  \"position_server_time\": " << receipt.position_server_time << ",\n"
             << "  \"trade_replay_complete\": " << (receipt.trade_replay_complete ? "true" : "false") << ",\n"
             << "  \"participant_user_deal_count\": " << receipt.participant_user_deal_count << ",\n"
             << "  \"participant_user_multileg_deal_count\": " << receipt.participant_user_multileg_deal_count << ",\n"
             << "  \"reconstructed_target_xpos\": " << receipt.reconstructed_target_xpos << ",\n"
             << "  \"active_own_order_count\": " << receipt.active_own_order_count << ",\n"
             << "  \"trade_replay_anchor_used\": ";
        if (receipt.trade_replay_anchor_used.has_value()) {
            json << "{\"trades_rev\": " << receipt.trade_replay_anchor_used->trades_rev
                 << ", \"trades_lifenum\": " << receipt.trade_replay_anchor_used->trades_lifenum
                 << ", \"server_time\": " << receipt.trade_replay_anchor_used->server_time << "},\n";
        } else {
            json << "null,\n";
        }
        json << "  \"private_streams\": " << receipt.private_streams_json << ",\n"
             << "  \"target_refdata_provenance_ready\": "
             << (receipt.target_refdata_provenance_ready ? "true" : "false") << ",\n"
             << "  \"target_aggr20_uncrossed\": " << (receipt.target_aggr20_uncrossed ? "true" : "false") << ",\n"
             << "  \"passive_non_marketable\": " << (receipt.passive_non_marketable ? "true" : "false") << ",\n"
             << "  \"bbo_distance_allowed\": " << (receipt.bbo_distance_allowed ? "true" : "false") << ",\n"
             << "  \"quantity_one\": " << (receipt.quantity_one ? "true" : "false") << ",\n"
             << "  \"private_streams_ready\": " << (receipt.private_streams_ready ? "true" : "false") << ",\n"
             << "  \"userorderbook_periodic_snapshot_consistent\": "
             << (receipt.userorderbook_periodic_snapshot_consistent ? "true" : "false") << ",\n"
             << "  \"p2mqreply_open\": " << (receipt.p2mqreply_open ? "true" : "false") << ",\n"
             << "  \"publisher_open\": " << (receipt.publisher_open ? "true" : "false") << ",\n"
             << "  \"trading_capable\": " << (receipt.trading_capable ? "true" : "false") << "\n"
             << "}\n";
        auto serialized = json.str();
        if (host.mode() == Plaza2TestSessionHostMode::LiveTestAuthorizedSend) {
            const auto schema = serialized.find("moex.plaza2.execution_safety.v3");
            serialized.replace(schema, std::string("moex.plaza2.execution_safety.v3").size(),
                               "moex.plaza2.execution_safety.v4");
            serialized.insert(serialized.rfind("\n}"),
                              ",\n  \"send_mode\": \"LiveTestAuthorizedSend\",\n  \"test_order_send_armed\": true");
        }
        receipt.canonical_json = std::move(serialized);
        receipt.sha256 = cgate::plaza2_sha256_hex(receipt.canonical_json);
        if (!receipt.target_refdata_provenance_ready || !receipt.target_aggr20_uncrossed ||
            !receipt.passive_non_marketable || !receipt.bbo_distance_allowed || !receipt.quantity_one ||
            !receipt.private_streams_ready || !receipt.aggr_online || !receipt.aggr_snapshot_complete ||
            !receipt.p2mqreply_open || !receipt.publisher_open || !receipt.trading_capable ||
            (receipt.active_own_order_count != 0 && host.mode() != Plaza2TestSessionHostMode::OfflineFake) ||
            (receipt.require_zero_starting_position && !receipt.zero_starting_position_proven)) {
            return invalid("execution-safety receipt rejected the current target conditions");
        }
        std::string error;
        if (!atomic_write_file(config.execution_safety_receipt_path, receipt.canonical_json, error)) {
            return invalid(error, cgate::Plaza2ErrorCode::RuntimeCallFailed);
        }
        last_receipt = std::move(receipt);
        return {};
    }

    Plaza2PublisherMessageResult submit(const Plaza2TradeEncodedCommand& command, std::uint32_t user_id) {
        if (host.mode() == Plaza2TestSessionHostMode::LiveTestAuthorizedSend) {
            const auto refuse = [](std::string reason) {
                Plaza2PublisherMessageResult result;
                result.validation_error = invalid(std::move(reason));
                return result;
            };
            const bool add = command.command_kind == Plaza2TradeCommandKind::AddOrder;
            const bool cancel = command.command_kind == Plaza2TradeCommandKind::DelOrder;
            const bool recovery = command.command_kind == Plaza2TradeCommandKind::DelUserOrders;
            if ((!add && !cancel && !recovery) ||
                command.command_name != (add      ? "AddOrder"
                                         : cancel ? "DelOrder"
                                                  : "DelUserOrders") ||
                command.msgid != (add      ? 474
                                  : cancel ? 461
                                           : 466)) {
                return refuse("TEST command name/id/kind mismatch");
            }
            if (cancel) {
                if (!intent() || !cancel_order_id || cancel_identity_conflict) {
                    return refuse("cancel requires an unambiguous correlated Add reply order_id");
                }
                DelOrderRequest request;
                request.broker_code = intent()->broker_code;
                request.client_code = intent()->client_code;
                request.isin_id = intent()->isin_id;
                request.order_id = *cancel_order_id;
                const auto expected = Plaza2TradeCodec{}.encode(Plaza2TradeCommandRequest{request});
                if (!expected.validation.ok() || expected.payload != command.payload) {
                    return refuse("cancel payload differs from the correlated authorized order");
                }
            }
            bool* attempted = command.command_kind == Plaza2TradeCommandKind::AddOrder   ? &add_attempted
                              : command.command_kind == Plaza2TradeCommandKind::DelOrder ? &cancel_attempted
                                                                                         : &recovery_attempted;
            if (*attempted || (command.command_kind != Plaza2TradeCommandKind::AddOrder && !order_may_exist)) {
                Plaza2PublisherMessageResult result;
                result.validation_error =
                    invalid("one TEST Add/cancel/recovery attempt maximum; cleanup requires possible order");
                return result;
            }
            *attempted = true; // Never retry even a definitely-not-sent validation/allocation failure.
        }
        if (!command.validation.ok() || command.command_name.empty()) {
            Plaza2PublisherMessageResult result;
            result.validation_error = {.code = cgate::Plaza2ErrorCode::InvalidConfiguration,
                                       .message = "invalid trade command cannot be posted"};
            return result;
        }
        if (command.command_kind == Plaza2TradeCommandKind::AddOrder) {
            if (bound_authorized_plan_sha256.empty() || intent() == nullptr ||
                bound_authorized_plan_sha256 != intent()->sha256) {
                Plaza2PublisherMessageResult result;
                result.validation_error = invalid("AddOrder requires the exact authorized pre-send plan binding");
                return result;
            }
            if (const auto binding = validate_add_payload(command, user_id); binding) {
                Plaza2PublisherMessageResult result;
                result.validation_error = binding;
                return result;
            }
        } else if (command.command_kind == Plaza2TradeCommandKind::DelUserOrders) {
            if (const auto binding = validate_recovery_payload(command, user_id); binding) {
                Plaza2PublisherMessageResult result;
                result.validation_error = binding;
                return result;
            }
        } else if (command.command_kind == Plaza2TradeCommandKind::DelOrder &&
                   (intent() == nullptr || user_id != intent()->cancel_user_id)) {
            Plaza2PublisherMessageResult result;
            result.validation_error = invalid("DelOrder user_id does not match the authorized cancel user");
            return result;
        }
        if (!host.started()) {
            const auto start_error = host.start();
            if (start_error) {
                Plaza2PublisherMessageResult result;
                result.validation_error = start_error;
                return result;
            }
        }
        if (command.command_kind == Plaza2TradeCommandKind::AddOrder) {
            if (const auto preflight = preflight_target(); preflight) {
                Plaza2PublisherMessageResult result;
                result.validation_error = preflight;
                return result;
            }
            if (const auto receipt = persist_execution_safety_receipt(); receipt) {
                Plaza2PublisherMessageResult result;
                result.validation_error = receipt;
                return result;
            }
        }
        const auto result = host.post_validated(command.command_name, command.payload, user_id, true);
        if (command.command_kind == Plaza2TradeCommandKind::AddOrder) {
            order_may_exist = result.certainty != cgate::Plaza2SubmissionCertainty::DefinitelyNotSent;
        }
        return result;
    }

    OrderLifecyclePollResult collect(bool process) {
        OrderLifecyclePollResult result;
        if (!host.started()) {
            const auto start_error = host.start();
            if (start_error) {
                result.ok = false;
                result.error = start_error.message;
                return result;
            }
        }
        if (process) {
            const auto error = host.poll();
            if (error) {
                result.ok = false;
                result.error = error.message;
            }
        }
        const Plaza2TradeCodec codec;
        for (const auto& event : host.take_reply_events()) {
            OrderReplyObservation reply;
            reply.user_id = event.user_id;
            reply.timed_out = event.timed_out;
            if (event.message_id != 179 && event.message_id != 177 && event.message_id != 186 &&
                event.message_id != 0) {
                result.ok = false;
                result.error = "reply message family is not allowed for the active user_id";
                continue;
            }
            reply.command_kind = event.command_kind;
            if (!event.timed_out) {
                Plaza2TradeValidationResult validation;
                const auto decoded = codec.decode_reply(event.message_id, event.raw_payload, validation);
                if (!validation.ok()) {
                    result.ok = false;
                    result.error = "malformed PLAZA trade reply: " + validation.message;
                    continue;
                }
                reply.code = decoded.code;
                reply.accepted = decoded.status == Plaza2TradeReplyStatusCategory::Accepted;
                reply.order_id = decoded.order_id;
            }
            result.replies.push_back(reply);
            if (host.mode() == Plaza2TestSessionHostMode::LiveTestAuthorizedSend && intent() &&
                event.message_id == 179 && reply.user_id == intent()->add_user_id && reply.accepted &&
                !reply.timed_out && reply.order_id && *reply.order_id > 0) {
                if (cancel_order_id && *cancel_order_id != *reply.order_id) {
                    cancel_identity_conflict = true;
                }
                cancel_order_id = reply.order_id;
            }
        }
        if (config.observation_ext_id != 0 ||
            (host.mode() == Plaza2TestSessionHostMode::LiveTestAuthorizedSend && intent() != nullptr)) {
            const auto observation =
                observe_order(intent() != nullptr ? intent()->ext_id : config.observation_ext_id, participant_code(),
                              intent() != nullptr ? intent()->side : config.observation_side,
                              intent() != nullptr ? intent()->quantity : config.observation_quantity,
                              host.private_state().own_orders(), host.private_state().own_trades());
            if (observation.has_value()) {
                auto normalized = *observation;
                // Replication uses the seven-symbol participant identity
                // (broker_code + client_code); lifecycle correlation remains
                // keyed by the authorized three-symbol AddOrder client code.
                if (host.mode() == Plaza2TestSessionHostMode::LiveTestAuthorizedSend && intent()) {
                    normalized.client_code = intent()->client_code;
                } else if (!config.observation_client_code.empty()) {
                    normalized.client_code = config.observation_client_code;
                }
                result.observations.push_back(std::move(normalized));
            }
        }
        return result;
    }

    Plaza2TestTradeTransportConfig config;
    Plaza2TestSessionHost host;
    std::optional<Plaza2ExecutionSafetyReceipt> last_receipt;
    std::string bound_authorized_plan_sha256;
    bool add_attempted{false};
    bool cancel_attempted{false};
    bool recovery_attempted{false};
    bool order_may_exist{false};
    std::optional<std::int64_t> cancel_order_id;
    bool cancel_identity_conflict{false};
};

Plaza2TestTradeTransport::Plaza2TestTradeTransport(Plaza2TestTradeTransportConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
Plaza2TestTradeTransport::~Plaza2TestTradeTransport() = default;
Plaza2TestTradeTransport::Plaza2TestTradeTransport(Plaza2TestTradeTransport&&) noexcept = default;
Plaza2TestTradeTransport& Plaza2TestTradeTransport::operator=(Plaza2TestTradeTransport&&) noexcept = default;

Plaza2Error Plaza2TestTradeTransport::bind_authorized_plan(const PreSendPlan& plan) {
    return impl_->bind_authorized_plan(plan);
}

Plaza2Error Plaza2TestTradeTransport::install_authorized_intent(Plaza2AuthorizedOrderIntent intent) {
    return impl_->install_authorized_intent(std::move(intent));
}

Plaza2PublisherMessageResult Plaza2TestTradeTransport::post(const Plaza2TradeEncodedCommand& command,
                                                            std::uint32_t user_id) {
    if (command.command_kind == Plaza2TradeCommandKind::DelUserOrders) {
        Plaza2PublisherMessageResult result;
        result.validation_error = invalid("exact-ext recovery must use post_exact_ext_id_recovery");
        return result;
    }
    return impl_->submit(command, user_id);
}
Plaza2PublisherMessageResult
Plaza2TestTradeTransport::post_exact_ext_id_recovery(const Plaza2TradeEncodedCommand& command, std::uint32_t user_id) {
    if (command.command_kind != Plaza2TradeCommandKind::DelUserOrders) {
        Plaza2PublisherMessageResult result;
        result.validation_error = invalid("post_exact_ext_id_recovery requires a DelUserOrders command");
        return result;
    }
    return impl_->submit(command, user_id);
}
OrderLifecyclePollResult Plaza2TestTradeTransport::poll(std::chrono::steady_clock::time_point deadline) {
    if (std::chrono::steady_clock::now() >= deadline) {
        return {.ok = true, .deadline_reached = true};
    }
    return impl_->collect(true);
}
OrderLifecyclePollResult Plaza2TestTradeTransport::reconcile() {
    return impl_->collect(true);
}
const Plaza2TestSessionHost& Plaza2TestTradeTransport::host() const noexcept {
    return impl_->host;
}
Plaza2TestSessionHost& Plaza2TestTradeTransport::host() noexcept {
    return impl_->host;
}
const std::optional<Plaza2ExecutionSafetyReceipt>&
Plaza2TestTradeTransport::last_execution_safety_receipt() const noexcept {
    return impl_->last_receipt;
}

} // namespace moex::plaza2_trade
