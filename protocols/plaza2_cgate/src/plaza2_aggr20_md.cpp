#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"
#include "moex/plaza2/cgate/plaza2_public_decode.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace moex::plaza2::cgate {

namespace {

using generated::FieldCode;
using generated::StreamCode;
using generated::TableCode;

constexpr std::string_view kCredentialToken = "${MOEX_PLAZA2_TEST_CREDENTIALS}";
constexpr std::string_view kLegacyCredentialToken = "${PLAZA2_TEST_CREDENTIALS}";
constexpr std::string_view kSoftwareKeyToken = "${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}";
constexpr std::string_view kRelativeSchemeToken = "|FILE|scheme/forts_scheme.ini|";
constexpr std::uint32_t kCgErrInternal = 131072;
constexpr std::uint32_t kCgErrInvalidArgument = 131073;
constexpr std::uint32_t kCgErrUnsupported = 131074;
constexpr std::uint32_t kCgErrServiceUnavailable = 36866;
constexpr std::size_t kTransactionLookupIndexThreshold = 256;
constexpr std::size_t kTransactionLookupStateThreshold = 256;

bool is_loopback_host(std::string_view host) {
    std::string normalized(host);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized == "127.0.0.1" || normalized == "::1" || normalized == "localhost";
}

Plaza2Error invalid_config(std::string message) {
    return {
        .code = Plaza2ErrorCode::InvalidConfiguration,
        .runtime_code = 0,
        .message = std::move(message),
    };
}

bool settings_need_credentials(std::string_view value) {
    return value.find(kCredentialToken) != std::string_view::npos ||
           value.find(kLegacyCredentialToken) != std::string_view::npos;
}

bool settings_need_software_key(std::string_view value) {
    return value.find(kSoftwareKeyToken) != std::string_view::npos;
}

void replace_all(std::string& rendered, std::string_view token, std::string_view replacement) {
    std::size_t position = 0;
    while ((position = rendered.find(token, position)) != std::string::npos) {
        rendered.replace(position, token.size(), replacement);
        position += replacement.size();
    }
}

std::string resolve_stream_scheme_path(std::string_view value, const std::filesystem::path& scheme_path) {
    std::string rendered(value);
    const auto replacement = std::string("|FILE|") + scheme_path.string() + "|";
    std::size_t position = 0;
    while ((position = rendered.find(kRelativeSchemeToken, position)) != std::string::npos) {
        rendered.replace(position, kRelativeSchemeToken.size(), replacement);
        position += replacement.size();
    }
    return rendered;
}

std::string resolve_env_open_ini_path(std::string_view value, const std::filesystem::path& config_dir) {
    std::string rendered(value);
    constexpr std::string_view kPrefix = "ini=";
    const auto begin = rendered.find(kPrefix);
    if (begin == std::string::npos) {
        return rendered;
    }
    const auto value_begin = begin + kPrefix.size();
    const auto value_end = rendered.find(';', value_begin);
    const auto value_size = (value_end == std::string::npos ? rendered.size() : value_end) - value_begin;
    const auto raw_path = std::filesystem::path(rendered.substr(value_begin, value_size));
    if (raw_path.is_absolute()) {
        return rendered;
    }
    auto resolved = config_dir / raw_path;
    if (!std::filesystem::exists(resolved) && raw_path.has_parent_path() && raw_path.begin() != raw_path.end() &&
        *raw_path.begin() == "config") {
        resolved = config_dir / raw_path.filename();
    }
    rendered.replace(value_begin, value_size, resolved.string());
    return rendered;
}

bool contains_forbidden_public_stream(std::string_view value) {
    return value.find("FORTS_ORDLOG_REPL") != std::string_view::npos ||
           value.find("FORTS_ORDBOOK_REPL") != std::string_view::npos ||
           value.find("FORTS_DEALS_REPL") != std::string_view::npos;
}

std::optional<std::int64_t> parse_i64(std::string_view value) {
    std::int64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<std::int64_t> signed_field(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code) {
    for (const auto& field : fields) {
        if (field.field_code != code) {
            continue;
        }
        if (field.kind == Plaza2DecodedValueKind::SignedInteger) {
            return field.signed_value;
        }
        if (field.kind == Plaza2DecodedValueKind::UnsignedInteger) {
            return static_cast<std::int64_t>(field.unsigned_value);
        }
        if (field.kind == Plaza2DecodedValueKind::String || field.kind == Plaza2DecodedValueKind::Decimal) {
            return parse_i64(field.text_value);
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> unsigned_field(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code) {
    for (const auto& field : fields) {
        if (field.field_code != code) {
            continue;
        }
        if (field.kind == Plaza2DecodedValueKind::UnsignedInteger || field.kind == Plaza2DecodedValueKind::Timestamp) {
            return field.unsigned_value;
        }
        if (field.kind == Plaza2DecodedValueKind::SignedInteger && field.signed_value >= 0) {
            return static_cast<std::uint64_t>(field.signed_value);
        }
    }
    return std::nullopt;
}

std::string text_field(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code) {
    for (const auto& field : fields) {
        if (field.field_code != code) {
            continue;
        }
        if (field.kind == Plaza2DecodedValueKind::String || field.kind == Plaza2DecodedValueKind::Decimal ||
            field.kind == Plaza2DecodedValueKind::FloatingPoint) {
            return std::string(field.text_value);
        }
        if (field.kind == Plaza2DecodedValueKind::SignedInteger) {
            return std::to_string(field.signed_value);
        }
        if (field.kind == Plaza2DecodedValueKind::UnsignedInteger) {
            return std::to_string(field.unsigned_value);
        }
    }
    return {};
}

const Plaza2DecodedFieldValue* find_decoded_field(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code) {
    for (const auto& field : fields) {
        if (field.field_code == code)
            return &field;
    }
    return nullptr;
}

std::string_view decoded_value_kind_name(Plaza2DecodedValueKind kind) noexcept {
    switch (kind) {
    case Plaza2DecodedValueKind::None:
        return "none";
    case Plaza2DecodedValueKind::SignedInteger:
        return "signed_integer";
    case Plaza2DecodedValueKind::UnsignedInteger:
        return "unsigned_integer";
    case Plaza2DecodedValueKind::Decimal:
        return "decimal";
    case Plaza2DecodedValueKind::FloatingPoint:
        return "floating_point";
    case Plaza2DecodedValueKind::String:
        return "string";
    case Plaza2DecodedValueKind::Timestamp:
        return "timestamp";
    }
    return "unknown";
}

std::string bytes_to_hex(std::span<const std::byte> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    const auto bounded_size = std::min<std::size_t>(bytes.size(), 64);
    std::string result;
    result.reserve(bounded_size * 2);
    for (const auto byte : bytes.first(bounded_size)) {
        const auto value = std::to_integer<std::uint8_t>(byte);
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 0x0fU]);
    }
    return result;
}

bool is_service_unavailable(const Plaza2Error& error) noexcept {
    return error.runtime_code == kCgErrServiceUnavailable ||
           error.message.find("SERV:NO_SERVICE") != std::string::npos ||
           error.message.find("NO_SERVICE") != std::string::npos;
}

bool is_transient_listener_open_error(const Plaza2Error& error) noexcept {
    // Service entitlement/upstream loss is explicitly retryable. CG_ERR_INTERNAL
    // during an asynchronous reopen is also a transport/runtime outage, while
    // invalid arguments, unsupported operations and unknown results are not.
    return is_service_unavailable(error) ||
           (error.code == Plaza2ErrorCode::RuntimeCallFailed && error.runtime_code == kCgErrInternal);
}

std::string_view classify_listener_open_error(const Plaza2Error& error) noexcept {
    if (is_service_unavailable(error)) {
        return "transient_service_unavailable";
    }
    if (error.code == Plaza2ErrorCode::RuntimeCallFailed && error.runtime_code == kCgErrInternal) {
        return "transient_runtime_failure";
    }
    if (error.code == Plaza2ErrorCode::InvalidConfiguration || error.code == Plaza2ErrorCode::ProbeIncompatible ||
        error.code == Plaza2ErrorCode::UnknownRuntimeResult || error.runtime_code == kCgErrInvalidArgument ||
        error.runtime_code == kCgErrUnsupported) {
        return "fatal_static_or_incompatible";
    }
    return "fatal_listener_reopen";
}

Plaza2Error first_fatal_issue(const Plaza2RuntimeProbeReport& report) {
    for (const auto& issue : report.issues) {
        if (issue.fatal) {
            return {
                .code = Plaza2ErrorCode::ProbeIncompatible,
                .runtime_code = 0,
                .message = issue.message,
            };
        }
    }
    return {
        .code = Plaza2ErrorCode::ProbeIncompatible,
        .runtime_code = 0,
        .message = "PLAZA II runtime probe reported an incompatible TEST runtime layout",
    };
}

class CanonicalHash {
  public:
    void append_byte(std::uint8_t value) noexcept {
        value_ ^= value;
        value_ *= 1099511628211ULL;
    }

    void append_u64(std::uint64_t value) noexcept {
        for (unsigned index = 0; index < 8; ++index) {
            append_byte(static_cast<std::uint8_t>(value >> (index * 8)));
        }
    }

    void append_i64(std::int64_t value) noexcept {
        append_u64(static_cast<std::uint64_t>(value));
    }

    [[nodiscard]] std::uint64_t value() const noexcept {
        return value_;
    }

  private:
    std::uint64_t value_{14695981039346656037ULL};
};

bool aggr_level_less(const Plaza2Aggr20Level& lhs, const Plaza2Aggr20Level& rhs) {
    const auto side_rank = [](std::int32_t dir) {
        if (dir == 1)
            return 0;
        if (dir == 2)
            return 1;
        return 2;
    };
    const auto lhs_rank = side_rank(lhs.dir);
    const auto rhs_rank = side_rank(rhs.dir);
    if (lhs_rank != rhs_rank)
        return lhs_rank < rhs_rank;
    if (lhs.dir == 1 && lhs.price_scaled != rhs.price_scaled)
        return lhs.price_scaled > rhs.price_scaled;
    if (lhs.dir == 2 && lhs.price_scaled != rhs.price_scaled)
        return lhs.price_scaled < rhs.price_scaled;
    if (lhs.price_scaled != rhs.price_scaled)
        return lhs.price_scaled < rhs.price_scaled;
    if (lhs.repl_id != rhs.repl_id)
        return lhs.repl_id < rhs.repl_id;
    if (lhs.repl_rev != rhs.repl_rev)
        return lhs.repl_rev < rhs.repl_rev;
    if (lhs.moment != rhs.moment)
        return lhs.moment < rhs.moment;
    return lhs.moment_ns < rhs.moment_ns;
}

std::uint64_t hash_instrument_snapshot(std::int64_t isin_id, std::uint64_t source_repl_id, std::int64_t source_repl_rev,
                                       std::uint64_t exchange_moment, std::uint64_t exchange_moment_ns,
                                       std::span<const Plaza2Aggr20Level> levels) noexcept {
    // Canonical form: fixed-width little-endian isin/source identity, then
    // level count and each sorted level's numeric identity. Decimal strings
    // are intentionally excluded because price_scaled is the normalized
    // fixed-point value used by the book and DTC boundary.
    CanonicalHash hash;
    hash.append_i64(isin_id);
    hash.append_u64(source_repl_id);
    hash.append_i64(source_repl_rev);
    hash.append_u64(exchange_moment);
    hash.append_u64(exchange_moment_ns);
    hash.append_u64(levels.size());
    for (const auto& level : levels) {
        hash.append_i64(level.isin_id);
        hash.append_i64(level.price_scaled);
        hash.append_i64(level.volume);
        hash.append_i64(level.dir);
        hash.append_u64(level.repl_id);
        hash.append_i64(level.repl_rev);
        hash.append_u64(level.moment);
        hash.append_u64(level.moment_ns);
    }
    return hash.value();
}

bool is_session_data_ready_message(std::string_view message) noexcept {
    if (message.size() != std::string_view{"session_data_ready"}.size())
        return false;
    for (std::size_t index = 0; index < message.size(); ++index) {
        const auto actual = static_cast<char>(std::tolower(static_cast<unsigned char>(message[index])));
        if (actual != std::string_view{"session_data_ready"}[index])
            return false;
    }
    return true;
}

std::string_view listener_event_kind_name(Plaza2ListenerEventKind kind) noexcept {
    switch (kind) {
    case Plaza2ListenerEventKind::Open:
        return "OPEN";
    case Plaza2ListenerEventKind::Close:
        return "CLOSE";
    case Plaza2ListenerEventKind::TransactionBegin:
        return "TN_BEGIN";
    case Plaza2ListenerEventKind::TransactionCommit:
        return "TN_COMMIT";
    case Plaza2ListenerEventKind::StreamData:
        return "STREAM_DATA";
    case Plaza2ListenerEventKind::Online:
        return "ONLINE";
    case Plaza2ListenerEventKind::LifeNum:
        return "LIFENUM";
    case Plaza2ListenerEventKind::ClearDeleted:
        return "CLEAR_DELETED";
    case Plaza2ListenerEventKind::ReplState:
        return "REPLSTATE";
    case Plaza2ListenerEventKind::Timeout:
        return "TIMEOUT";
    }
    return "UNKNOWN";
}

std::string_view authority_state_name(Plaza2Aggr20AuthorityState state) noexcept {
    switch (state) {
    case Plaza2Aggr20AuthorityState::WaitingForTransport:
        return "waiting_for_transport";
    case Plaza2Aggr20AuthorityState::WaitingForSnapshot:
        return "waiting_for_snapshot";
    case Plaza2Aggr20AuthorityState::WaitingForSessionDataReady:
        return "waiting_for_session_data_ready";
    case Plaza2Aggr20AuthorityState::Authoritative:
        return "authoritative";
    case Plaza2Aggr20AuthorityState::Recovering:
        return "recovering";
    }
    return "unknown";
}

std::uint64_t unix_now_ns() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

} // namespace

void Plaza2Aggr20ListenerBridge::invalidate(bool request_reopen, bool transport_active) noexcept {
    projector_.reset();
    ++stream_epoch_;
    ++market_data_authority_epoch_;
    transport_active_ = transport_active;
    online_ = false;
    snapshot_complete_ = false;
    session_data_ready_ = false;
    target_authoritative_ = false;
    reopen_required_ = request_reopen;
    last_sys_event_.reset();
    snapshot_ready_witness_.reset();
    online_ready_witness_.reset();
    pending_sys_events_.clear();
    retry_at_.reset();
    bootstrap_started_at_.reset();
    last_recovery_error_ = {};
    recovery_failure_classification_.clear();
}

void Plaza2Aggr20ListenerBridge::reset() noexcept {
    invalidate(false, false);
    has_lifenum_ = false;
    last_lifenum_ = 0;
}

bool Plaza2Aggr20ListenerBridge::accepts_session(std::int32_t sess_id) const noexcept {
    return expected_session_id_ > 0 ? sess_id == expected_session_id_ : sess_id > 0;
}

void Plaza2Aggr20ListenerBridge::trace_event(const Plaza2ListenerEvent& event,
                                             const Plaza2Aggr20AuthoritySnapshot& before, std::string detail) const {
    if (!event_trace_)
        return;
    const auto after = authority_snapshot();
    std::string line = "event=" + std::string(listener_event_kind_name(event.kind)) +
                       " local_unix_ns=" + std::to_string(unix_now_ns()) +
                       " authority_before=" + std::string(authority_state_name(before.state)) +
                       " authority_after=" + std::string(authority_state_name(after.state));
    if (before.state != after.state || before.target_authoritative != after.target_authoritative ||
        before.session_data_ready != after.session_data_ready) {
        line += " authority_transition=true";
    } else {
        line += " authority_transition=false";
    }
    if (!detail.empty()) {
        line += " " + std::move(detail);
    }
    event_trace_(std::move(line));
}

const Plaza2Aggr20AuthoritySnapshot Plaza2Aggr20ListenerBridge::authority_snapshot() const {
    Plaza2Aggr20AuthoritySnapshot out;
    out.transport_active = transport_active_;
    out.snapshot_complete = snapshot_complete_;
    out.session_data_ready = session_data_ready_;
    out.aggr_online = online_;
    out.snapshot_ready_witness = snapshot_ready_witness_;
    out.online_ready_witness = online_ready_witness_;
    out.target_authoritative = target_authoritative_;
    out.stream_epoch = stream_epoch_;
    out.market_data_authority_epoch = market_data_authority_epoch_;
    out.last_sys_event = last_sys_event_;
    out.recovery_service = recovery_service_;
    out.reopen_retry_count = reopen_retry_count_;
    out.first_recovery_error = first_recovery_error_;
    out.current_recovery_error = current_recovery_error_;
    if (recovering()) {
        out.state = Plaza2Aggr20AuthorityState::Recovering;
    } else if (!transport_active_) {
        out.state = Plaza2Aggr20AuthorityState::WaitingForTransport;
    } else if (!online_ || !snapshot_complete_) {
        out.state = Plaza2Aggr20AuthorityState::WaitingForSnapshot;
    } else if (!session_data_ready_) {
        out.state = Plaza2Aggr20AuthorityState::WaitingForSessionDataReady;
    } else {
        out.state = Plaza2Aggr20AuthorityState::Authoritative;
    }
    return out;
}

Plaza2Error Plaza2Aggr20ListenerBridge::on_plaza2_listener_event(const Plaza2ListenerEvent& event) {
    const auto before = authority_snapshot();
    switch (event.kind) {
    case Plaza2ListenerEventKind::Open:
        first_decimal_rejection_.reset();
        invalidate(false, true);
        reopen_retry_count_ = 0;
        first_recovery_error_.reset();
        current_recovery_error_.reset();
        trace_event(event, before);
        return {};
    case Plaza2ListenerEventKind::LifeNum:
        invalidate(false, transport_active_);
        has_lifenum_ = true;
        last_lifenum_ = event.unsigned_value;
        trace_event(event, before, "lifenum=" + std::to_string(last_lifenum_));
        return {};
    case Plaza2ListenerEventKind::ClearDeleted:
        // CGate sends cleanup markers before the initial snapshot. Deleting from an already
        // empty projection needs no reopen, which would only request the same markers again.
        if (!reopen_required_ && !online_ && !snapshot_complete_ && !projector_.transaction_open() &&
            projector_.snapshot().row_count == 0 && projector_.snapshot().last_repl_rev == 0) {
            invalidate(false, transport_active_);
            trace_event(event, before);
            return {};
        }
        [[fallthrough]];
    case Plaza2ListenerEventKind::Close: {
        // Conservatively invalidate and request a fresh full snapshot. No partial book is advertised.
        const bool had_visible_state = online_ || snapshot_complete_ || projector_.snapshot().row_count != 0;
        invalidate(true, false);
        if (had_visible_state) {
            last_recovery_error_ = {
                .code = Plaza2ErrorCode::AdapterState,
                .runtime_code = 0,
                .message = "AGGR20 listener delivered CLOSE; visible book invalidated pending a fresh snapshot",
            };
            recovery_failure_classification_ = "transient_listener_close";
        }
        trace_event(event, before);
        return {};
    }
    case Plaza2ListenerEventKind::TransactionBegin:
        if (reopen_required_) {
            trace_event(event, before, "ignored=true recovering=true");
            return {};
        }
        pending_sys_events_.clear();
        projector_.begin_transaction();
        trace_event(event, before);
        return {};
    case Plaza2ListenerEventKind::TransactionCommit: {
        if (reopen_required_)
            return {};
        if (const auto error = projector_.commit(); error) {
            pending_sys_events_.clear();
            projector_.rollback();
            trace_event(event, before, "commit_error=" + error.message);
            return error;
        }
        auto committed_events = std::move(pending_sys_events_);
        pending_sys_events_.clear();
        for (const auto& committed_event : committed_events) {
            last_sys_event_ = committed_event;
            // A replacement or tombstone for the retained source row revokes
            // its evidence. A later valid row may establish a new witness.
            const auto replaces = [&](const auto& witness) {
                return witness &&
                       (witness->source_repl_id == committed_event.source_repl_id ||
                        (committed_event.source_repl_act > 0 &&
                         witness->source_repl_id == static_cast<std::uint64_t>(committed_event.source_repl_act)));
            };
            if (replaces(snapshot_ready_witness_))
                snapshot_ready_witness_.reset();
            if (replaces(online_ready_witness_)) {
                online_ready_witness_.reset();
                session_data_ready_ = false;
                target_authoritative_ = false;
            }
            if (committed_event.source_repl_act == 0 && committed_event.event_type == 1 &&
                is_session_data_ready_message(committed_event.message) && accepts_session(committed_event.sess_id)) {
                if (committed_event.seen_during_snapshot)
                    snapshot_ready_witness_ = committed_event;
            }
            // A sys_events row can certify only after its containing source
            // transaction has committed and after ONLINE has fenced the
            // bootstrap generation from the current session generation.
            if (!committed_event.seen_during_snapshot && snapshot_complete_ && online_ &&
                committed_event.source_repl_act == 0 && committed_event.event_type == 1 &&
                is_session_data_ready_message(committed_event.message) && accepts_session(committed_event.sess_id)) {
                session_data_ready_ = true;
                target_authoritative_ = true;
                online_ready_witness_ = committed_event;
            }
        }
        trace_event(event, before);
        return {};
    }
    case Plaza2ListenerEventKind::StreamData:
        if (reopen_required_)
            return {};
        if (event.table_code == generated::TableCode::kFortsAggrReplOrdersAggr) {
            const auto error = projector_.on_row(event.fields);
            if (error && error.code == Plaza2ErrorCode::DecodeFailed &&
                error.message.find("AGGR20 orders_aggr.price") != std::string::npos) {
                capture_decimal_rejection(event);
            }
            return error;
        }
        if (event.table_code == generated::TableCode::kFortsAggrReplSysEvents) {
            Plaza2Aggr20SysEventSnapshot sys_event;
            sys_event.source_repl_id =
                unsigned_field(event.fields, generated::FieldCode::kFortsAggrReplSysEventsReplId).value_or(0);
            sys_event.source_repl_rev =
                signed_field(event.fields, generated::FieldCode::kFortsAggrReplSysEventsReplRev).value_or(0);
            sys_event.source_repl_act =
                signed_field(event.fields, generated::FieldCode::kFortsAggrReplSysEventsReplAct).value_or(0);
            sys_event.event_type = static_cast<std::int32_t>(
                signed_field(event.fields, generated::FieldCode::kFortsAggrReplSysEventsEventType).value_or(0));
            sys_event.event_id =
                signed_field(event.fields, generated::FieldCode::kFortsAggrReplSysEventsEventId).value_or(0);
            sys_event.sess_id = static_cast<std::int32_t>(
                signed_field(event.fields, generated::FieldCode::kFortsAggrReplSysEventsSessId).value_or(0));
            sys_event.message = text_field(event.fields, generated::FieldCode::kFortsAggrReplSysEventsMessage);
            sys_event.server_time =
                unsigned_field(event.fields, generated::FieldCode::kFortsAggrReplSysEventsServerTime).value_or(0);
            sys_event.seen_during_snapshot = !snapshot_complete_;
            const auto detail =
                "source_repl_id=" + std::to_string(sys_event.source_repl_id) +
                " source_repl_rev=" + std::to_string(sys_event.source_repl_rev) +
                " source_repl_act=" + std::to_string(sys_event.source_repl_act) +
                " event_type=" + std::to_string(sys_event.event_type) +
                " event_id=" + std::to_string(sys_event.event_id) + " sess_id=" + std::to_string(sys_event.sess_id) +
                " exchange_server_time=" + std::to_string(sys_event.server_time) + " message=" + sys_event.message +
                " seen_during_snapshot=" + (sys_event.seen_during_snapshot ? "true" : "false");
            pending_sys_events_.push_back(std::move(sys_event));
            trace_event(event, before, detail);
        }
        return {};
    case Plaza2ListenerEventKind::Online:
        if (!reopen_required_ && !projector_.transaction_open()) {
            transport_active_ = true;
            online_ = true;
            snapshot_complete_ = true;
            session_data_ready_ = false;
            target_authoritative_ = false;
        }
        trace_event(event, before);
        return {};
    default:
        return {};
    }
}

void Plaza2Aggr20ListenerBridge::capture_decimal_rejection(const Plaza2ListenerEvent& event) noexcept {
    if (first_decimal_rejection_.has_value())
        return;

    try {
        Plaza2Aggr20DecimalRejection receipt;
        const auto* price = find_decoded_field(event.fields, FieldCode::kFortsAggrReplOrdersAggrPrice);
        receipt.repl_id = signed_field(event.fields, FieldCode::kFortsAggrReplOrdersAggrReplId).value_or(0);
        receipt.repl_rev = signed_field(event.fields, FieldCode::kFortsAggrReplOrdersAggrReplRev).value_or(0);
        receipt.repl_act = signed_field(event.fields, FieldCode::kFortsAggrReplOrdersAggrReplAct).value_or(0);
        receipt.isin_id = signed_field(event.fields, FieldCode::kFortsAggrReplOrdersAggrIsinId).value_or(0);
        receipt.dir = signed_field(event.fields, FieldCode::kFortsAggrReplOrdersAggrDir).value_or(0);
        receipt.volume = signed_field(event.fields, FieldCode::kFortsAggrReplOrdersAggrVolume).value_or(0);
        if (price != nullptr) {
            receipt.decoded_field_kind = std::string(decoded_value_kind_name(price->kind));
            receipt.cg_getstr_text = std::string(price->text_value);
            receipt.text_byte_length = price->text_value.size();
            receipt.raw_bcd_bytes_hex = bytes_to_hex(price->raw_value);
            if (price->raw_value.size() == sizeof(public_wire::Bcd16_5)) {
                const auto bcd = public_wire::load<public_wire::Bcd16_5>(price->raw_value);
                if (const auto decoded = public_wire::decimal_value(bcd); decoded.has_value()) {
                    receipt.bcd_mantissa = decoded->mantissa;
                    receipt.bcd_scale = decoded->scale;
                }
            }
        }
        first_decimal_rejection_ = std::move(receipt);
    } catch (...) {
        // A diagnostic allocation must never turn a bounded decode failure
        // into an unbounded callback failure.
    }
}

Plaza2Error Plaza2Aggr20ListenerBridge::supervise(Plaza2Listener& listener, std::chrono::steady_clock::time_point now) {
    std::uint32_t state = 0;
    if (const auto error = listener.state(state); error) {
        last_recovery_error_ = error;
        recovery_failure_classification_ = "fatal_listener_state";
        first_recovery_error_ = first_recovery_error_.value_or(error);
        current_recovery_error_ = error;
        return error;
    }
    if (!reopen_required_ && !retry_at_ && state != 0 && state != 1) {
        if (!snapshot_complete_) {
            if (!bootstrap_started_at_.has_value())
                bootstrap_started_at_ = now;
            if (now >= *bootstrap_started_at_ && now - *bootstrap_started_at_ >= bootstrap_watchdog_) {
                if (const auto error = listener.close(); error) {
                    last_recovery_error_ = error;
                    recovery_failure_classification_ = "fatal_listener_close";
                    first_recovery_error_ = first_recovery_error_.value_or(error);
                    current_recovery_error_ = error;
                    return error;
                }
                invalidate(true, false);
                retry_at_ = now + std::chrono::seconds(1);
                last_recovery_error_ = {
                    .code = Plaza2ErrorCode::AdapterState,
                    .runtime_code = 0,
                    .message = "AGGR20 listener bootstrap watchdog expired before ONLINE",
                };
                recovery_failure_classification_ = "transient_bootstrap_watchdog";
            }
        }
        return {};
    }
    if (!retry_at_) {
        if (const auto error = listener.close(); error) {
            last_recovery_error_ = error;
            recovery_failure_classification_ = "fatal_listener_close";
            if (!first_recovery_error_.has_value())
                first_recovery_error_ = error;
            current_recovery_error_ = error;
            ++reopen_retry_count_;
            return error;
        }
        invalidate(true, false);
        retry_at_ = now + std::chrono::seconds(1);
        if (state == 1) {
            last_recovery_error_ = {
                .code = Plaza2ErrorCode::AdapterState,
                .runtime_code = 0,
                .message = "AGGR20 listener entered ERROR; visible book invalidated pending a fresh snapshot",
            };
            recovery_failure_classification_ = "transient_listener_error";
        }
        return {};
    }
    if (now < *retry_at_)
        return {};
    // A cleared projection cannot resume after an opaque cursor; bootstrap the whole snapshot again.
    const auto error = listener.open("mode=snapshot+online");
    if (error) {
        if (!first_recovery_error_.has_value())
            first_recovery_error_ = error;
        current_recovery_error_ = error;
        ++reopen_retry_count_;
        reopen_required_ = true;
        retry_at_ = now + std::chrono::seconds(1);
        last_recovery_error_ = error;
        recovery_failure_classification_ = std::string(classify_listener_open_error(error));
        if (!is_transient_listener_open_error(error)) {
            reopen_required_ = false;
            retry_at_.reset();
            return error;
        }
        return {};
    }
    reopen_required_ = false;
    retry_at_.reset();
    bootstrap_started_at_ = now;
    last_recovery_error_ = {};
    recovery_failure_classification_.clear();
    reopen_retry_count_ = 0;
    first_recovery_error_.reset();
    current_recovery_error_.reset();
    return {};
}

Plaza2Aggr20BookProjector::Plaza2Aggr20BookProjector(NowFn now)
    : now_(now ? std::move(now) : NowFn{[] { return Clock::now(); }}) {}

void Plaza2Aggr20BookProjector::reset() {
    staged_rows_.clear();
    affected_isin_ids_.clear();
    staged_metadata_.clear();
    slot_rows_.clear();
    active_slot_order_.clear();
    slot_order_position_.clear();
    free_slot_indices_.clear();
    slot_index_by_repl_id_.clear();
    instrument_slot_ownership_.clear();
    active_row_count_ = 0;
    active_instrument_count_ = 0;
    committed_ = {};
    instrument_snapshots_.clear();
    global_diagnostics_dirty_ = false;
    transaction_open_ = false;
}

void Plaza2Aggr20BookProjector::begin_transaction() {
    staged_rows_.clear();
    affected_isin_ids_.clear();
    staged_metadata_.clear();
    transaction_open_ = true;
}

Plaza2Error Plaza2Aggr20BookProjector::on_row(std::span<const Plaza2DecodedFieldValue> fields) {
    if (!transaction_open_) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = 0,
            .message = "AGGR20 row arrived outside TN_BEGIN/TN_COMMIT",
        };
    }

    Plaza2Aggr20Level level;
    level.isin_id = signed_field(fields, FieldCode::kFortsAggrReplOrdersAggrIsinId).value_or(0);
    const auto* price_field = find_decoded_field(fields, FieldCode::kFortsAggrReplOrdersAggrPrice);
    if (price_field == nullptr ||
        (price_field->kind != Plaza2DecodedValueKind::String && price_field->kind != Plaza2DecodedValueKind::Decimal)) {
        return {
            .code = Plaza2ErrorCode::DecodeFailed,
            .runtime_code = 0,
            .message = "AGGR20 orders_aggr.price is missing or has an unsupported runtime value kind",
        };
    }

    std::optional<std::int64_t> price_scaled;
    if (price_field->decimal_exact &&
        price_field->decimal_scale == static_cast<std::int32_t>(kPlaza2D16_5FractionalDigits)) {
        // The runtime already decoded the native d16.5 BCD. Keep this hot
        // path as BCD -> exact mantissa; do not round-trip through text.
        price_scaled = price_field->decimal_mantissa;
    } else if (price_field->raw_value.empty() && price_field->type_token.empty()) {
        // Unit-level callers may provide a decoded text value without a
        // runtime field view. Keep that test seam exact and signed too.
        price_scaled = parse_fixed_point(price_field->text_value, kPlaza2D16_5FractionalDigits, true,
                                         kPlaza2D16_5DecimalPrecision);
    }
    if (!price_scaled.has_value()) {
        return {
            .code = Plaza2ErrorCode::DecodeFailed,
            .runtime_code = 0,
            .message = "AGGR20 orders_aggr.price is malformed, over-precise, or outside checked fixed-point range",
        };
    }
    level.price = std::string(price_field->text_value);
    level.price_scaled = *price_scaled;
    level.volume = signed_field(fields, FieldCode::kFortsAggrReplOrdersAggrVolume).value_or(0);
    if (signed_field(fields, FieldCode::kFortsAggrReplOrdersAggrReplAct).value_or(0) != 0) {
        level.volume = 0;
    }
    level.dir = static_cast<std::int32_t>(signed_field(fields, FieldCode::kFortsAggrReplOrdersAggrDir).value_or(0));
    level.repl_id = unsigned_field(fields, FieldCode::kFortsAggrReplOrdersAggrReplId).value_or(0);
    level.repl_rev = signed_field(fields, FieldCode::kFortsAggrReplOrdersAggrReplRev).value_or(0);
    level.moment = unsigned_field(fields, FieldCode::kFortsAggrReplOrdersAggrMoment).value_or(0);
    level.moment_ns = unsigned_field(fields, FieldCode::kFortsAggrReplOrdersAggrMomentNs).value_or(0);
    level.synth_volume = text_field(fields, FieldCode::kFortsAggrReplOrdersAggrSynthVolume);
    affected_isin_ids_.insert(level.isin_id);
    auto& metadata = staged_metadata_[level.isin_id];
    metadata.last_repl_id = std::max(metadata.last_repl_id, level.repl_id);
    metadata.last_repl_rev = std::max(metadata.last_repl_rev, level.repl_rev);
    metadata.exchange_moment = std::max(metadata.exchange_moment, level.moment);
    metadata.exchange_moment_ns = std::max(metadata.exchange_moment_ns, level.moment_ns);
    staged_rows_.push_back(std::move(level));
    return {};
}

void Plaza2Aggr20BookProjector::add_slot_to_instrument(std::int64_t isin_id, std::size_t slot_index) {
    auto [it, inserted] = instrument_slot_ownership_.try_emplace(isin_id);
    if (inserted) {
        ++active_instrument_count_;
    }
    auto& slots = it->second;
    const auto position = slot_order_position_[slot_index];
    const auto insertion = std::lower_bound(slots.begin(), slots.end(), position,
                                            [this](std::size_t existing_slot, std::size_t wanted_position) {
                                                return slot_order_position_[existing_slot] < wanted_position;
                                            });
    slots.insert(insertion, slot_index);
}

void Plaza2Aggr20BookProjector::remove_slot_from_instrument(std::int64_t isin_id, std::size_t slot_index) {
    const auto it = instrument_slot_ownership_.find(isin_id);
    if (it == instrument_slot_ownership_.end()) {
        return;
    }
    auto& slots = it->second;
    const auto slot = std::find(slots.begin(), slots.end(), slot_index);
    if (slot == slots.end()) {
        return;
    }
    slots.erase(slot);
    if (slots.empty()) {
        instrument_slot_ownership_.erase(it);
        --active_instrument_count_;
    }
}

void Plaza2Aggr20BookProjector::compact_slot_order() noexcept {
    const auto hole_count = active_slot_order_.size() - active_row_count_;
    if (hole_count < kCompactionHoleThreshold) {
        return;
    }

    const auto new_end = std::remove(active_slot_order_.begin(), active_slot_order_.end(), kNoSlotIndex);
    active_slot_order_.erase(new_end, active_slot_order_.end());
    std::fill(slot_order_position_.begin(), slot_order_position_.end(), kNoSlotIndex);
    for (std::size_t position = 0; position < active_slot_order_.size(); ++position) {
        slot_order_position_[active_slot_order_[position]] = position;
    }
}

Plaza2Error Plaza2Aggr20BookProjector::commit() {
    if (!transaction_open_) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = 0,
            .message = "AGGR20 TN_COMMIT arrived without TN_BEGIN",
        };
    }

    // Everything through the reserve/resize block below is a preflight. It
    // may allocate, but it does not mutate the published projector state.
    // The publish block only uses reserved vectors, node handles, and
    // noexcept moves/swaps, so a bad_alloc cannot leave a half-applied row.
    const auto committed_at = now_();
    const auto staged_row_count = staged_rows_.size();
    std::uint64_t planned_last_repl_id = committed_.last_repl_id;
    std::int64_t planned_last_repl_rev = committed_.last_repl_rev;
    for (const auto& row : staged_rows_) {
        planned_last_repl_id = std::max(planned_last_repl_id, row.repl_id);
        planned_last_repl_rev = std::max(planned_last_repl_rev, row.repl_rev);
    }

    enum class AllocationSource : std::uint8_t { None, TransactionFree, ExistingFree, Append };
    enum class OperationKind : std::uint8_t { Noop, Delete, Insert, Update };
    struct PlannedRepl {
        std::uint64_t repl_id{0};
        bool initially_active{false};
        bool active{false};
        std::size_t slot_index{kNoSlotIndex};
        std::size_t order_position{kNoSlotIndex};
        std::size_t final_row_index{kNoSlotIndex};
        std::int64_t isin_id{0};
    };
    struct PlannedOperation {
        OperationKind kind{OperationKind::Noop};
        AllocationSource allocation_source{AllocationSource::None};
        std::size_t row_index{0};
        std::size_t old_slot{kNoSlotIndex};
        std::size_t new_slot{kNoSlotIndex};
        std::size_t old_order{kNoSlotIndex};
        std::size_t new_order{kNoSlotIndex};
        std::int64_t old_isin{0};
        std::int64_t new_isin{0};
    };
    struct PlannedOwnership {
        std::int64_t isin_id{0};
        std::vector<std::size_t> slots;
    };

    std::vector<PlannedRepl> planned_repls;
    planned_repls.reserve(staged_row_count);
    std::vector<PlannedOperation> planned_operations;
    planned_operations.reserve(staged_row_count);
    std::vector<std::size_t> transaction_free_slots;
    transaction_free_slots.reserve(staged_row_count);
    std::vector<std::int64_t> affected_isins;
    affected_isins.reserve(affected_isin_ids_.size() + staged_row_count);
    const bool use_transaction_lookup_index =
        staged_row_count >= kTransactionLookupIndexThreshold && slot_rows_.size() >= kTransactionLookupStateThreshold;
    std::vector<std::pair<std::uint64_t, std::size_t>> planned_repl_indices;
    std::vector<std::size_t> planned_slot_indices;
    std::vector<bool> deleted_slot_indices;
    if (use_transaction_lookup_index) {
        // These are transaction-local preflight structures. They are never
        // published and are fully built before the no-allocation publish
        // block, so allocation failure preserves the existing projector.
        planned_repl_indices.reserve(staged_row_count);
        for (const auto& row : staged_rows_)
            planned_repl_indices.emplace_back(row.repl_id, kNoSlotIndex);
        std::sort(planned_repl_indices.begin(), planned_repl_indices.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
        planned_repl_indices.erase(std::unique(planned_repl_indices.begin(), planned_repl_indices.end(),
                                               [](const auto& lhs, const auto& rhs) { return lhs.first == rhs.first; }),
                                   planned_repl_indices.end());
        const auto lookup_capacity = slot_rows_.size() + staged_row_count;
        planned_slot_indices.assign(lookup_capacity, kNoSlotIndex);
        deleted_slot_indices.assign(lookup_capacity, false);
    }

    const auto mark_affected = [&](std::int64_t isin_id) {
        // This is transaction-local staging state, not published projector
        // state. Preserve the original insertion history so source-version
        // numbering remains byte-for-byte compatible with the legacy loop.
        affected_isin_ids_.insert(isin_id);
    };
    const auto find_planned_repl = [&](std::uint64_t repl_id) -> PlannedRepl* {
        if (use_transaction_lookup_index) {
            const auto indexed = std::lower_bound(
                planned_repl_indices.begin(), planned_repl_indices.end(), repl_id,
                [](const auto& entry, std::uint64_t wanted_repl_id) { return entry.first < wanted_repl_id; });
            if (indexed == planned_repl_indices.end() || indexed->first != repl_id || indexed->second == kNoSlotIndex)
                return nullptr;
            return &planned_repls[indexed->second];
        }
        const auto it = std::find_if(planned_repls.begin(), planned_repls.end(),
                                     [repl_id](const PlannedRepl& planned) { return planned.repl_id == repl_id; });
        return it == planned_repls.end() ? nullptr : &*it;
    };
    const auto register_planned_repl = [&](std::uint64_t repl_id, std::size_t planned_index) {
        if (use_transaction_lookup_index) {
            const auto indexed = std::lower_bound(
                planned_repl_indices.begin(), planned_repl_indices.end(), repl_id,
                [](const auto& entry, std::uint64_t wanted_repl_id) { return entry.first < wanted_repl_id; });
            indexed->second = planned_index;
        }
    };

    std::size_t free_cursor = free_slot_indices_.size();
    std::size_t appended_slot_count = 0;
    std::size_t appended_order_count = 0;
    std::size_t planned_active_row_count = active_row_count_;
    const auto allocate_slot = [&]() {
        if (!transaction_free_slots.empty()) {
            const auto slot_index = transaction_free_slots.back();
            transaction_free_slots.pop_back();
            return std::pair{slot_index, AllocationSource::TransactionFree};
        }
        if (free_cursor != 0) {
            const auto slot_index = free_slot_indices_[--free_cursor];
            return std::pair{slot_index, AllocationSource::ExistingFree};
        }
        return std::pair{slot_rows_.size() + appended_slot_count++, AllocationSource::Append};
    };

    for (std::size_t row_index = 0; row_index < staged_rows_.size(); ++row_index) {
        const auto& row = staged_rows_[row_index];
        auto* planned = find_planned_repl(row.repl_id);
        if (planned == nullptr) {
            const auto existing = slot_index_by_repl_id_.find(row.repl_id);
            if (existing != slot_index_by_repl_id_.end()) {
                const auto slot_index = existing->second;
                const auto& old_row = *slot_rows_[slot_index];
                planned_repls.push_back({.repl_id = row.repl_id,
                                         .initially_active = true,
                                         .active = true,
                                         .slot_index = slot_index,
                                         .order_position = slot_order_position_[slot_index],
                                         .final_row_index = kNoSlotIndex,
                                         .isin_id = old_row.isin_id});
                register_planned_repl(row.repl_id, planned_repls.size() - 1);
                planned = &planned_repls.back();
                mark_affected(old_row.isin_id);
            }
        }

        PlannedOperation operation;
        operation.row_index = row_index;
        if (row.volume <= 0) {
            if (planned != nullptr && planned->active) {
                operation.kind = OperationKind::Delete;
                operation.old_slot = planned->slot_index;
                operation.old_order = planned->order_position;
                operation.old_isin = planned->isin_id;
                mark_affected(operation.old_isin);
                transaction_free_slots.push_back(planned->slot_index);
                if (use_transaction_lookup_index) {
                    planned_slot_indices[planned->slot_index] = kNoSlotIndex;
                    deleted_slot_indices[planned->slot_index] = true;
                }
                planned->active = false;
                planned->slot_index = kNoSlotIndex;
                planned->order_position = kNoSlotIndex;
                planned->final_row_index = kNoSlotIndex;
                --planned_active_row_count;
            }
            planned_operations.push_back(operation);
            continue;
        }

        if (planned == nullptr) {
            planned_repls.push_back({.repl_id = row.repl_id});
            register_planned_repl(row.repl_id, planned_repls.size() - 1);
            planned = &planned_repls.back();
        }
        mark_affected(row.isin_id);
        if (planned->active) {
            operation.kind = OperationKind::Update;
            operation.old_slot = planned->slot_index;
            operation.new_slot = planned->slot_index;
            operation.old_order = planned->order_position;
            operation.new_order = planned->order_position;
            operation.old_isin = planned->isin_id;
            operation.new_isin = row.isin_id;
            mark_affected(operation.old_isin);
            planned->isin_id = row.isin_id;
            planned->final_row_index = row_index;
            if (use_transaction_lookup_index)
                planned_slot_indices[planned->slot_index] = planned - planned_repls.data();
        } else {
            const auto [slot_index, allocation_source] = allocate_slot();
            operation.kind = OperationKind::Insert;
            operation.allocation_source = allocation_source;
            operation.new_slot = slot_index;
            operation.new_order = active_slot_order_.size() + appended_order_count++;
            operation.new_isin = row.isin_id;
            planned->active = true;
            planned->slot_index = slot_index;
            planned->order_position = operation.new_order;
            planned->final_row_index = row_index;
            planned->isin_id = row.isin_id;
            if (use_transaction_lookup_index)
                planned_slot_indices[slot_index] = planned - planned_repls.data();
            ++planned_active_row_count;
        }
        planned_operations.push_back(operation);
    }

    // Keep the old observable version-assignment order: the original
    // projector iterated this unordered affected set after old-ISIN
    // migrations had been inserted into it.
    affected_isins.clear();
    affected_isins.reserve(affected_isin_ids_.size());
    for (const auto isin_id : affected_isin_ids_)
        affected_isins.push_back(isin_id);

    std::vector<PlannedOwnership> planned_ownership;
    planned_ownership.reserve(affected_isins.size());
    for (const auto isin_id : affected_isins) {
        PlannedOwnership ownership{.isin_id = isin_id};
        const auto existing = instrument_slot_ownership_.find(isin_id);
        if (existing != instrument_slot_ownership_.end())
            ownership.slots = existing->second;
        planned_ownership.push_back(std::move(ownership));
    }

    const auto find_ownership_plan = [&](std::int64_t isin_id) -> PlannedOwnership* {
        const auto it = std::find_if(affected_isins.begin(), affected_isins.end(),
                                     [isin_id](std::int64_t candidate) { return candidate == isin_id; });
        if (it == affected_isins.end())
            return nullptr;
        const auto index = static_cast<std::size_t>(std::distance(affected_isins.begin(), it));
        return &planned_ownership[index];
    };

    const auto position_for_slot = [&](std::size_t slot_index) {
        for (auto operation = planned_operations.rbegin(); operation != planned_operations.rend(); ++operation) {
            if (operation->kind == OperationKind::Insert && operation->new_slot == slot_index)
                return operation->new_order;
        }
        return slot_order_position_[slot_index];
    };
    const auto remove_owned_slot = [](PlannedOwnership& ownership, std::size_t slot_index) {
        const auto it = std::find(ownership.slots.begin(), ownership.slots.end(), slot_index);
        if (it != ownership.slots.end())
            ownership.slots.erase(it);
    };
    const auto add_owned_slot = [&](PlannedOwnership& ownership, std::size_t slot_index, std::size_t order_position) {
        const auto insertion = std::lower_bound(ownership.slots.begin(), ownership.slots.end(), order_position,
                                                [&](std::size_t existing_slot, std::size_t wanted_position) {
                                                    return position_for_slot(existing_slot) < wanted_position;
                                                });
        ownership.slots.insert(insertion, slot_index);
    };
    for (const auto& operation : planned_operations) {
        if (operation.kind == OperationKind::Delete) {
            remove_owned_slot(*find_ownership_plan(operation.old_isin), operation.old_slot);
        } else if (operation.kind == OperationKind::Insert) {
            add_owned_slot(*find_ownership_plan(operation.new_isin), operation.new_slot, operation.new_order);
        } else if (operation.kind == OperationKind::Update && operation.old_isin != operation.new_isin) {
            remove_owned_slot(*find_ownership_plan(operation.old_isin), operation.old_slot);
            add_owned_slot(*find_ownership_plan(operation.new_isin), operation.new_slot, operation.new_order);
        }
    }

    std::size_t planned_active_instrument_count = active_instrument_count_;
    for (const auto& ownership : planned_ownership) {
        const bool was_active = instrument_slot_ownership_.find(ownership.isin_id) != instrument_slot_ownership_.end();
        const bool is_active = !ownership.slots.empty();
        if (was_active != is_active)
            is_active ? ++planned_active_instrument_count : --planned_active_instrument_count;
    }

    const auto final_repl_for_slot = [&](std::size_t slot_index) -> const PlannedRepl* {
        if (use_transaction_lookup_index) {
            const auto planned_index = planned_slot_indices[slot_index];
            if (planned_index == kNoSlotIndex || !planned_repls[planned_index].active)
                return nullptr;
            return &planned_repls[planned_index];
        }
        for (const auto& planned : planned_repls) {
            if (planned.active && planned.slot_index == slot_index)
                return &planned;
        }
        return nullptr;
    };
    const auto slot_was_deleted = [&](std::size_t slot_index) -> bool {
        if (use_transaction_lookup_index)
            return deleted_slot_indices[slot_index];
        return std::any_of(planned_operations.begin(), planned_operations.end(),
                           [slot_index](const PlannedOperation& operation) {
                               return operation.kind == OperationKind::Delete && operation.old_slot == slot_index;
                           });
    };
    const auto final_level_for_slot = [&](std::size_t slot_index) -> const Plaza2Aggr20Level& {
        if (const auto* planned = final_repl_for_slot(slot_index);
            planned != nullptr && planned->final_row_index != kNoSlotIndex) {
            return staged_rows_[planned->final_row_index];
        }
        return *slot_rows_[slot_index];
    };

    std::unordered_map<std::int64_t, Plaza2Aggr20InstrumentSnapshot> planned_snapshots;
    planned_snapshots.reserve(affected_isins.size());
    std::uint64_t planned_snapshot_version_counter = snapshot_version_counter_;
    for (const auto isin_id : affected_isins) {
        const auto previous = instrument_snapshots_.find(isin_id);
        Plaza2Aggr20InstrumentSnapshot scoped;
        if (previous != instrument_snapshots_.end()) {
            scoped.isin_id = previous->second.isin_id;
            scoped.last_repl_id = previous->second.last_repl_id;
            scoped.last_repl_rev = previous->second.last_repl_rev;
            scoped.exchange_moment = previous->second.exchange_moment;
            scoped.exchange_moment_ns = previous->second.exchange_moment_ns;
            scoped.source_snapshot_version = previous->second.source_snapshot_version;
            scoped.source_snapshot_hash = previous->second.source_snapshot_hash;
            scoped.committed_at = previous->second.committed_at;
        }
        scoped.isin_id = isin_id;
        if (const auto metadata = staged_metadata_.find(isin_id); metadata != staged_metadata_.end()) {
            // Keep the last relevant replication metadata even when this row
            // is a deletion and no level remains in the visible book.
            scoped.last_repl_id = std::max(scoped.last_repl_id, metadata->second.last_repl_id);
            scoped.last_repl_rev = std::max(scoped.last_repl_rev, metadata->second.last_repl_rev);
            scoped.exchange_moment = std::max(scoped.exchange_moment, metadata->second.exchange_moment);
            scoped.exchange_moment_ns = std::max(scoped.exchange_moment_ns, metadata->second.exchange_moment_ns);
        }
        const auto& ownership = *find_ownership_plan(isin_id);
        scoped.levels.reserve(ownership.slots.size());
        for (const auto slot_index : ownership.slots)
            scoped.levels.push_back(final_level_for_slot(slot_index));
        scoped.row_count = scoped.levels.size();
        scoped.bid_depth_levels = 0;
        scoped.ask_depth_levels = 0;
        scoped.top_bid.reset();
        scoped.top_ask.reset();
        for (const auto& level : scoped.levels) {
            if (level.dir == 1) {
                ++scoped.bid_depth_levels;
                if (!scoped.top_bid.has_value() || level.price_scaled > scoped.top_bid->price_scaled)
                    scoped.top_bid = level;
            } else if (level.dir == 2) {
                ++scoped.ask_depth_levels;
                if (!scoped.top_ask.has_value() || level.price_scaled < scoped.top_ask->price_scaled)
                    scoped.top_ask = level;
            }
        }
        std::ranges::sort(scoped.levels, aggr_level_less);
        const auto source_hash =
            hash_instrument_snapshot(scoped.isin_id, scoped.last_repl_id, scoped.last_repl_rev, scoped.exchange_moment,
                                     scoped.exchange_moment_ns, std::span<const Plaza2Aggr20Level>(scoped.levels));
        const bool source_changed =
            previous == instrument_snapshots_.end() || scoped.source_snapshot_hash != source_hash;
        scoped.source_snapshot_hash = source_hash;
        if (source_changed) {
            scoped.source_snapshot_version = ++planned_snapshot_version_counter;
            scoped.committed_at = committed_at;
        }
        planned_snapshots.emplace(isin_id, std::move(scoped));
    }

    std::unordered_map<std::uint64_t, std::size_t> planned_slot_nodes;
    planned_slot_nodes.reserve(planned_repls.size());
    for (const auto& planned : planned_repls) {
        if (planned.active && !planned.initially_active)
            planned_slot_nodes.emplace(planned.repl_id, planned.slot_index);
    }

    std::optional<Plaza2Aggr20Snapshot> planned_global;
    if (qualification_observer_ != nullptr && (global_diagnostics_dirty_ || !staged_rows_.empty())) {
        Plaza2Aggr20Snapshot global;
        global.row_count = planned_active_row_count;
        global.instrument_count = planned_active_instrument_count;
        global.last_repl_id = planned_last_repl_id;
        global.last_repl_rev = planned_last_repl_rev;
        global.committed_at = committed_at;
        global.levels.reserve(planned_active_row_count);
        const auto append_global_level = [&](const Plaza2Aggr20Level& level) {
            global.levels.push_back(level);
            global.exchange_moment = std::max(global.exchange_moment, level.moment);
            global.exchange_moment_ns = std::max(global.exchange_moment_ns, level.moment_ns);
            if (level.dir == 1) {
                ++global.bid_depth_levels;
                if (!global.top_bid.has_value() || level.price_scaled > global.top_bid->price_scaled)
                    global.top_bid = level;
            } else if (level.dir == 2) {
                ++global.ask_depth_levels;
                if (!global.top_ask.has_value() || level.price_scaled < global.top_ask->price_scaled)
                    global.top_ask = level;
            }
        };
        for (std::size_t position = 0; position < active_slot_order_.size(); ++position) {
            const auto slot_index = active_slot_order_[position];
            if (slot_index == kNoSlotIndex)
                continue;
            const auto* planned = final_repl_for_slot(slot_index);
            if (planned != nullptr) {
                if (planned->order_position != position)
                    continue;
                append_global_level(staged_rows_[planned->final_row_index]);
            } else if (slot_was_deleted(slot_index)) {
                continue;
            } else {
                append_global_level(*slot_rows_[slot_index]);
            }
        }
        std::vector<const PlannedRepl*> appended_repls;
        appended_repls.reserve(planned_repls.size());
        for (const auto& planned : planned_repls) {
            if (planned.active && planned.order_position >= active_slot_order_.size())
                appended_repls.push_back(&planned);
        }
        std::ranges::sort(appended_repls,
                          [](const auto* lhs, const auto* rhs) { return lhs->order_position < rhs->order_position; });
        for (const auto* planned : appended_repls)
            append_global_level(staged_rows_[planned->final_row_index]);
        planned_global.emplace(std::move(global));
    }

    static_assert(
        noexcept(std::declval<Plaza2Aggr20InstrumentSnapshot&>() = std::declval<Plaza2Aggr20InstrumentSnapshot&&>()));

    std::size_t missing_ownership_nodes = 0;
    for (const auto& ownership : planned_ownership) {
        if (!ownership.slots.empty() &&
            instrument_slot_ownership_.find(ownership.isin_id) == instrument_slot_ownership_.end())
            ++missing_ownership_nodes;
    }
    std::size_t missing_snapshot_nodes = 0;
    for (const auto isin_id : affected_isins) {
        if (instrument_snapshots_.find(isin_id) == instrument_snapshots_.end())
            ++missing_snapshot_nodes;
    }
    // Build and allocate every ownership node before touching canonical slot,
    // order, free-list, or map state. The moved-from plan remains local and
    // is discarded safely if any later reserve/resize fails.
    std::unordered_map<std::int64_t, std::vector<std::size_t>> ownership_nodes;
    ownership_nodes.reserve(affected_isins.size());
    for (auto& ownership : planned_ownership) {
        if (!ownership.slots.empty())
            ownership_nodes.emplace(ownership.isin_id, std::move(ownership.slots));
    }
    slot_rows_.reserve(slot_rows_.size() + appended_slot_count);
    slot_order_position_.reserve(slot_order_position_.size() + appended_slot_count);
    active_slot_order_.reserve(active_slot_order_.size() + appended_order_count);
    free_slot_indices_.reserve(free_slot_indices_.size() + staged_row_count);
    slot_index_by_repl_id_.reserve(slot_index_by_repl_id_.size() + planned_slot_nodes.size());
    instrument_slot_ownership_.reserve(instrument_slot_ownership_.size() + missing_ownership_nodes);
    instrument_snapshots_.reserve(instrument_snapshots_.size() + missing_snapshot_nodes);
    slot_rows_.resize(slot_rows_.size() + appended_slot_count);
    slot_order_position_.resize(slot_order_position_.size() + appended_slot_count, kNoSlotIndex);

    // No allocation is permitted below this line. Replacing a vector value,
    // extracting/inserting a prepared node, and the reserved push/pop/reset
    // operations are all noexcept for these concrete types.
    for (const auto& operation : planned_operations) {
        if (operation.kind == OperationKind::Delete) {
            slot_rows_[operation.old_slot].reset();
            slot_order_position_[operation.old_slot] = kNoSlotIndex;
            active_slot_order_[operation.old_order] = kNoSlotIndex;
            free_slot_indices_.push_back(operation.old_slot);
        } else if (operation.kind == OperationKind::Insert) {
            if (operation.allocation_source == AllocationSource::TransactionFree ||
                operation.allocation_source == AllocationSource::ExistingFree)
                free_slot_indices_.pop_back();
            slot_rows_[operation.new_slot] = std::move(staged_rows_[operation.row_index]);
            active_slot_order_.push_back(operation.new_slot);
            slot_order_position_[operation.new_slot] = operation.new_order;
        } else if (operation.kind == OperationKind::Update) {
            slot_rows_[operation.new_slot] = std::move(staged_rows_[operation.row_index]);
        }
    }

    for (const auto& planned : planned_repls) {
        const auto existing = slot_index_by_repl_id_.find(planned.repl_id);
        if (planned.initially_active) {
            if (planned.active) {
                if (existing != slot_index_by_repl_id_.end())
                    existing->second = planned.slot_index;
            } else if (existing != slot_index_by_repl_id_.end()) {
                slot_index_by_repl_id_.erase(existing);
            }
        } else if (planned.active) {
            auto node = planned_slot_nodes.extract(planned.repl_id);
            slot_index_by_repl_id_.insert(std::move(node));
        }
    }

    for (const auto& ownership : planned_ownership) {
        const auto existing = instrument_slot_ownership_.find(ownership.isin_id);
        const bool final_active = ownership_nodes.find(ownership.isin_id) != ownership_nodes.end();
        if (!final_active) {
            if (existing != instrument_slot_ownership_.end())
                instrument_slot_ownership_.erase(existing);
            continue;
        }
        auto node = ownership_nodes.extract(ownership.isin_id);
        if (existing == instrument_slot_ownership_.end()) {
            instrument_slot_ownership_.insert(std::move(node));
        } else {
            existing->second.swap(node.mapped());
        }
    }

    for (const auto isin_id : affected_isins) {
        auto node = planned_snapshots.extract(isin_id);
        const auto existing = instrument_snapshots_.find(isin_id);
        if (existing == instrument_snapshots_.end())
            instrument_snapshots_.insert(std::move(node));
        else
            existing->second = std::move(node.mapped());
    }

    active_row_count_ = planned_active_row_count;
    active_instrument_count_ = planned_active_instrument_count;
    snapshot_version_counter_ = planned_snapshot_version_counter;
    committed_.row_count = active_row_count_;
    committed_.instrument_count = active_instrument_count_;
    committed_.last_repl_id = planned_last_repl_id;
    committed_.last_repl_rev = planned_last_repl_rev;
    committed_.committed_at = committed_at;
    global_diagnostics_dirty_ = global_diagnostics_dirty_ || !staged_rows_.empty();
    if (planned_global.has_value()) {
        committed_.levels.swap(planned_global->levels);
        committed_.bid_depth_levels = planned_global->bid_depth_levels;
        committed_.ask_depth_levels = planned_global->ask_depth_levels;
        committed_.top_bid = std::move(planned_global->top_bid);
        committed_.top_ask = std::move(planned_global->top_ask);
        committed_.exchange_moment = planned_global->exchange_moment;
        committed_.exchange_moment_ns = planned_global->exchange_moment_ns;
        global_diagnostics_dirty_ = false;
    }
    compact_slot_order();
    if (qualification_observer_)
        qualification_observer_->committed(committed_);
    staged_rows_.clear();
    affected_isin_ids_.clear();
    staged_metadata_.clear();
    transaction_open_ = false;
    return {};
}

void Plaza2Aggr20BookProjector::rollback() {
    staged_rows_.clear();
    affected_isin_ids_.clear();
    staged_metadata_.clear();
    transaction_open_ = false;
}

void Plaza2Aggr20BookProjector::ensure_global_diagnostics() const {
    if (!global_diagnostics_dirty_) {
        return;
    }

    Plaza2Aggr20Snapshot rebuilt;
    rebuilt.row_count = active_row_count_;
    rebuilt.instrument_count = active_instrument_count_;
    rebuilt.last_repl_id = committed_.last_repl_id;
    rebuilt.last_repl_rev = committed_.last_repl_rev;
    rebuilt.committed_at = committed_.committed_at;
    rebuilt.levels.reserve(active_row_count_);
    const auto append_level = [&](const Plaza2Aggr20Level& level) {
        rebuilt.levels.push_back(level);
        rebuilt.exchange_moment = std::max(rebuilt.exchange_moment, level.moment);
        rebuilt.exchange_moment_ns = std::max(rebuilt.exchange_moment_ns, level.moment_ns);
        if (level.dir == 1) {
            ++rebuilt.bid_depth_levels;
            if (!rebuilt.top_bid.has_value() || level.price_scaled > rebuilt.top_bid->price_scaled)
                rebuilt.top_bid = level;
        } else if (level.dir == 2) {
            ++rebuilt.ask_depth_levels;
            if (!rebuilt.top_ask.has_value() || level.price_scaled < rebuilt.top_ask->price_scaled)
                rebuilt.top_ask = level;
        }
    };
    for (const auto slot_index : active_slot_order_) {
        if (slot_index != kNoSlotIndex)
            append_level(*slot_rows_[slot_index]);
    }

    // Publish only after the complete diagnostic copy succeeded. If reserve
    // or any level copy throws, the old committed snapshot and dirty flag are
    // untouched and a later snapshot() can retry safely.
    committed_.levels.swap(rebuilt.levels);
    committed_.bid_depth_levels = rebuilt.bid_depth_levels;
    committed_.ask_depth_levels = rebuilt.ask_depth_levels;
    committed_.top_bid = std::move(rebuilt.top_bid);
    committed_.top_ask = std::move(rebuilt.top_ask);
    committed_.exchange_moment = rebuilt.exchange_moment;
    committed_.exchange_moment_ns = rebuilt.exchange_moment_ns;
    committed_.row_count = rebuilt.row_count;
    committed_.instrument_count = rebuilt.instrument_count;
    global_diagnostics_dirty_ = false;
}

const Plaza2Aggr20Snapshot& Plaza2Aggr20BookProjector::snapshot() const {
    ensure_global_diagnostics();
    return committed_;
}

std::optional<Plaza2Aggr20InstrumentSnapshot> Plaza2Aggr20BookProjector::snapshot_for_isin(std::int64_t isin_id) const {
    const auto it = instrument_snapshots_.find(isin_id);
    if (it == instrument_snapshots_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool Plaza2Aggr20BookProjector::transaction_open() const noexcept {
    return transaction_open_;
}

std::string_view plaza2_aggr20_md_runner_state_name(Plaza2Aggr20MdRunnerState state) noexcept {
    switch (state) {
    case Plaza2Aggr20MdRunnerState::Created:
        return "Created";
    case Plaza2Aggr20MdRunnerState::Validated:
        return "Validated";
    case Plaza2Aggr20MdRunnerState::Started:
        return "Started";
    case Plaza2Aggr20MdRunnerState::Ready:
        return "Ready";
    case Plaza2Aggr20MdRunnerState::Stopped:
        return "Stopped";
    case Plaza2Aggr20MdRunnerState::Failed:
        return "Failed";
    case Plaza2Aggr20MdRunnerState::Recovering:
        return "Recovering";
    }
    return "Unknown";
}

std::string classify_plaza2_aggr20_failure(const Plaza2Aggr20MdHealthSnapshot& health) {
    if (!health.runtime_probe_ok) {
        return "runtime_probe_failed";
    }
    if (!health.scheme_drift_ok) {
        return "schema_mismatch";
    }
    if (!health.clock_evidence_present) {
        return "clock_evidence_missing";
    }
    if (!health.clock_evidence_ok) {
        return "clock_evidence_invalid";
    }
    if (!health.stream_created || !health.stream_opened) {
        return "stream_open_failed";
    }
    if (!health.stream_online) {
        return "stream_not_online";
    }
    if (!health.stream_snapshot_complete) {
        return "snapshot_incomplete";
    }
    if (!health.session_data_ready || !health.target_authoritative) {
        return "session_data_not_authoritative";
    }
    if (health.snapshot.row_count == 0) {
        return "zero_rows_observed";
    }
    return {};
}

Plaza2Error validate_plaza2_aggr20_md_config(const Plaza2Aggr20MdConfig& config) {
    if (config.profile_id.empty()) {
        return invalid_config("profile_id must be set for AGGR20 TEST bring-up");
    }
    if (config.endpoint_host.empty()) {
        return invalid_config("endpoint_host must be set for AGGR20 TEST bring-up");
    }
    if (config.runtime.environment != Plaza2Environment::Test) {
        return invalid_config("Phase 5D AGGR20 bring-up is TEST-only");
    }
    if (!config.arm_state.test_network_armed || !config.arm_state.test_session_armed ||
        !config.arm_state.test_plaza2_armed || !config.test_market_data_armed) {
        return invalid_config("AGGR20 TEST bring-up requires --armed-test-network, --armed-test-session, "
                              "--armed-test-plaza2, and --armed-test-market-data");
    }
    if (const auto runtime_error = validate_plaza2_settings(config.runtime); runtime_error) {
        return runtime_error;
    }
    if (config.runtime.env_open_settings.empty()) {
        return invalid_config("runtime.env_open_settings must be provided explicitly");
    }
    if (config.runtime.env_open_settings.find(kCredentialToken) != std::string::npos ||
        config.runtime.env_open_settings.find(kLegacyCredentialToken) != std::string::npos) {
        return invalid_config("Phase 5D env_open_settings must use ${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}, not the "
                              "exchange credential variable");
    }
    if (config.connection_settings.empty()) {
        return invalid_config("connection_settings must be provided explicitly");
    }
    if (config.stream.stream_code != StreamCode::kFortsAggrRepl) {
        return invalid_config("Phase 5D only allows FORTS_AGGR20_REPL");
    }
    if (config.listener_bootstrap_watchdog.count() <= 0) {
        return invalid_config("listener_bootstrap_watchdog must be positive");
    }
    if (config.clock_evidence_freshness_window_ns == 0) {
        return invalid_config("clock_evidence_freshness_window_ns must be positive");
    }
    if (config.stream.settings.find("FORTS_AGGR20_REPL") == std::string::npos) {
        return invalid_config("Phase 5D stream settings must explicitly use FORTS_AGGR20_REPL");
    }
    if (contains_forbidden_public_stream(config.stream.settings)) {
        return invalid_config("Phase 5D rejects FORTS_ORDLOG_REPL, FORTS_ORDBOOK_REPL, and FORTS_DEALS_REPL");
    }
    return {};
}

struct Plaza2Aggr20MdRunner::Impl {
    explicit Impl(Plaza2Aggr20MdConfig initial_config)
        : config(std::move(initial_config)), projector(config.now), listener_bridge(projector) {
        listener_bridge.set_bootstrap_watchdog(config.listener_bootstrap_watchdog);
        listener_bridge.set_recovery_service("FORTS_AGGR20_REPL");
        listener_bridge.set_event_trace([this](std::string line) { append_operator_log(std::move(line)); });
    }

    Plaza2Aggr20MdRunResult start() {
        if (started) {
            return fail("AGGR20 TEST runner already started");
        }

        append_operator_log("profile=" + config.profile_id);
        append_operator_log("endpoint=" + config.endpoint_host + ":" + std::to_string(config.endpoint_port));
        if (const auto validation_error = validate_plaza2_aggr20_md_config(config); validation_error) {
            return fail(validation_error.message);
        }
        health.clock_evidence_present = config.clock_evidence.has_value();
        const auto current_wall_ns = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
        health.clock_evidence_ok =
            health.clock_evidence_present && plaza2_clock_evidence_passes_at(*config.clock_evidence, current_wall_ns,
                                                                             config.clock_evidence_freshness_window_ns);
        if (!health.clock_evidence_present) {
            return fail("AGGR20 TEST launch requires current paired clock evidence");
        }
        if (!health.clock_evidence_ok) {
            return fail("AGGR20 TEST launch clock evidence is missing, stale, inconsistent, or unsafe");
        }
        if (const auto transport_gate =
                Plaza2ManualOperatorGate::validate_transport_connect(config.endpoint_host, config.arm_state);
            !transport_gate.allowed) {
            return fail(transport_gate.reason);
        }
        if (const auto session_gate =
                Plaza2ManualOperatorGate::validate_session_start(config.endpoint_host, config.arm_state);
            !session_gate.allowed) {
            return fail(session_gate.reason);
        }
        if (!is_loopback_host(config.endpoint_host) && !config.test_market_data_armed) {
            return fail("external AGGR20 TEST market data requires --armed-test-market-data");
        }
        if (const auto secrets = load_secrets_if_needed(); !secrets.ok) {
            return secrets;
        }

        effective_runtime = config.runtime;
        effective_runtime.env_open_settings = render_setting(config.runtime.env_open_settings);
        effective_connection_settings = render_setting(config.connection_settings);
        effective_connection_open_settings = render_setting(config.connection_open_settings);

        health.state = Plaza2Aggr20MdRunnerState::Validated;
        probe_report = Plaza2RuntimeProbe::probe(effective_runtime);
        health.compatibility = probe_report.compatibility;
        health.runtime_probe_ok = probe_report.runtime_library_loadable;
        health.scheme_drift_status = probe_report.scheme_drift.compatibility;
        health.scheme_drift_ok = probe_report.scheme_drift.compatibility == Plaza2Compatibility::Compatible ||
                                 probe_report.scheme_drift.compatibility == Plaza2Compatibility::CompatibleWithWarnings;
        health.scheme_drift_warning_count = probe_report.scheme_drift.warning_drift_count;
        health.scheme_drift_fatal_count = probe_report.scheme_drift.fatal_drift_count;
        if (probe_report.compatibility == Plaza2Compatibility::Incompatible ||
            probe_report.compatibility == Plaza2Compatibility::Unknown) {
            const auto probe_error = first_fatal_issue(probe_report);
            return fail(probe_error.message);
        }
        append_operator_log("runtime_probe=" + std::string(plaza2_compatibility_name(probe_report.compatibility)));
        append_operator_log("scheme_drift=" +
                            std::string(plaza2_compatibility_name(probe_report.scheme_drift.compatibility)));

        effective_runtime.env_open_settings =
            resolve_env_open_ini_path(effective_runtime.env_open_settings, probe_report.layout.config_dir);
        effective_stream = config.stream;
        effective_stream.settings =
            resolve_stream_scheme_path(render_setting(config.stream.settings), probe_report.layout.scheme_path);
        effective_stream.open_settings = render_setting(config.stream.open_settings);

        if (const auto env_error = env.open(effective_runtime); env_error) {
            return fail(env_error.message);
        }
        append_operator_log("env=open");

        if (const auto connection_error = connection.create(env, effective_connection_settings); connection_error) {
            return fail(connection_error.message);
        }
        append_operator_log("connection=create");

        if (const auto open_error = connection.open(effective_connection_open_settings); open_error) {
            return fail(open_error.message);
        }
        append_operator_log("connection=open");

        if (const auto create_error =
                listener.create(connection, effective_stream.stream_code, effective_stream.settings, &listener_bridge);
            create_error) {
            return fail(create_error.message);
        }
        health.stream_created = true;
        append_operator_log("listener=create stream=FORTS_AGGR20_REPL");

        if (const auto open_error = listener.open(effective_stream.open_settings); open_error) {
            return fail(open_error.message);
        }
        health.stream_opened = true;
        append_operator_log("listener=open stream=FORTS_AGGR20_REPL");

        started = true;
        health.state = Plaza2Aggr20MdRunnerState::Started;
        refresh_health();
        return {
            .ok = true,
            .message = "AGGR20 TEST runner started",
        };
    }

    Plaza2Aggr20MdRunResult poll_once() {
        if (!started) {
            return fail("AGGR20 TEST runner is not started");
        }
        const auto now = config.now ? config.now() : std::chrono::steady_clock::now();
        if (const auto recovery_error = listener_bridge.supervise(listener, now); recovery_error) {
            return fail(recovery_error.message);
        }
        std::uint32_t runtime_code = 0;
        const auto process_error = connection.process(config.process_timeout_ms, &runtime_code);
        health.last_process_runtime_code = runtime_code;
        if (process_error) {
            return fail(process_error.message);
        }
        if (const auto recovery_error = listener_bridge.supervise(listener, now); recovery_error) {
            return fail(recovery_error.message);
        }
        refresh_health();
        health.state =
            listener_bridge.recovering() ? Plaza2Aggr20MdRunnerState::Recovering : Plaza2Aggr20MdRunnerState::Started;
        if (health.ready) {
            append_operator_log("state=ready");
            health.state = Plaza2Aggr20MdRunnerState::Ready;
        }
        return {
            .ok = true,
            .message = "AGGR20 TEST runner poll completed",
        };
    }

    Plaza2Aggr20MdRunResult stop() {
        if (!started && health.state != Plaza2Aggr20MdRunnerState::Failed) {
            health.state = Plaza2Aggr20MdRunnerState::Stopped;
            return {
                .ok = true,
                .message = "AGGR20 TEST runner already stopped",
            };
        }

        static_cast<void>(listener.close());
        static_cast<void>(listener.destroy());
        static_cast<void>(connection.close());
        static_cast<void>(connection.destroy());
        static_cast<void>(env.close());
        started = false;
        listener_bridge.reset();
        refresh_health();
        health.state = Plaza2Aggr20MdRunnerState::Stopped;
        append_operator_log("runner=stopped");
        return {
            .ok = true,
            .message = "AGGR20 TEST runner stopped",
        };
    }

    Plaza2Aggr20MdRunResult load_secrets_if_needed() {
        const bool needs_credentials = settings_need_credentials(config.runtime.env_open_settings) ||
                                       settings_need_credentials(config.connection_settings) ||
                                       settings_need_credentials(config.connection_open_settings) ||
                                       settings_need_credentials(config.stream.settings) ||
                                       settings_need_credentials(config.stream.open_settings);
        const bool needs_software_key = settings_need_software_key(config.runtime.env_open_settings) ||
                                        settings_need_software_key(config.connection_settings) ||
                                        settings_need_software_key(config.connection_open_settings) ||
                                        settings_need_software_key(config.stream.settings) ||
                                        settings_need_software_key(config.stream.open_settings);

        if (config.credentials.source == Plaza2CredentialSource::None) {
            if (needs_credentials) {
                return fail("AGGR20 TEST settings require ${MOEX_PLAZA2_TEST_CREDENTIALS}, but no credential source "
                            "was configured");
            }
        } else {
            loaded_credentials = load_plaza2_credentials(config.credentials);
            if (!loaded_credentials.has_value()) {
                return fail(config.credentials.source == Plaza2CredentialSource::Env
                                ? "AGGR20 TEST credential env var is missing or empty"
                                : "AGGR20 TEST credential file is missing or empty");
            }
            append_operator_log("credentials=" + redact_plaza2_credentials(loaded_credentials->value));
        }

        if (config.software_key.source == Plaza2CredentialSource::None) {
            if (needs_software_key) {
                return fail("AGGR20 TEST settings require ${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}, but no software-key "
                            "source was configured");
            }
            return {
                .ok = true,
                .message = "AGGR20 TEST secrets loaded",
            };
        }

        loaded_software_key = load_plaza2_credentials(config.software_key);
        if (!loaded_software_key.has_value()) {
            return fail(config.software_key.source == Plaza2CredentialSource::Env
                            ? "AGGR20 TEST software-key env var is missing or empty"
                            : "AGGR20 TEST software-key file is missing or empty");
        }
        append_operator_log("software_key=" + redact_plaza2_credentials(loaded_software_key->value));
        return {
            .ok = true,
            .message = "AGGR20 TEST secrets loaded",
        };
    }

    std::string render_setting(std::string_view value) const {
        std::string rendered(value);
        if (loaded_credentials.has_value()) {
            replace_all(rendered, kCredentialToken, loaded_credentials->value);
            replace_all(rendered, kLegacyCredentialToken, loaded_credentials->value);
        }
        if (loaded_software_key.has_value()) {
            replace_all(rendered, kSoftwareKeyToken, loaded_software_key->value);
        }
        return rendered;
    }

    void refresh_health() {
        health.authority = listener_bridge.authority_snapshot();
        health.transport_active = health.authority.transport_active;
        health.stream_online = listener_bridge.online();
        health.stream_snapshot_complete = listener_bridge.snapshot_complete();
        health.session_data_ready = listener_bridge.session_data_ready();
        health.target_authoritative = listener_bridge.authoritative();
        health.recovery_service = health.authority.recovery_service;
        health.reopen_retry_count = health.authority.reopen_retry_count;
        health.first_recovery_error = health.authority.first_recovery_error;
        health.current_recovery_error = health.authority.current_recovery_error;
        health.snapshot = projector.snapshot();
        health.ready = health.runtime_probe_ok && health.scheme_drift_ok && health.stream_created &&
                       health.stream_opened && health.clock_evidence_ok && health.transport_active &&
                       health.stream_online && health.stream_snapshot_complete && health.session_data_ready &&
                       health.target_authoritative && health.snapshot.row_count > 0;
        if (const auto& recovery_error = listener_bridge.last_recovery_error(); recovery_error) {
            health.last_error = recovery_error.message;
            health.failure_classification = std::string(listener_bridge.recovery_failure_classification());
        } else {
            if (health.state != Plaza2Aggr20MdRunnerState::Failed)
                health.last_error.clear();
            health.failure_classification = classify_plaza2_aggr20_failure(health);
        }
    }

    Plaza2Aggr20MdRunResult fail(std::string message) {
        const auto bridge_classification = std::string(listener_bridge.recovery_failure_classification());
        const auto recovery_diagnostics = listener_bridge.authority_snapshot();
        listener_bridge.reset();
        health.state = Plaza2Aggr20MdRunnerState::Failed;
        health.last_error = message;
        refresh_health();
        if (!bridge_classification.empty()) {
            health.failure_classification = bridge_classification;
        }
        // reset() correctly fences the source, but must not erase the causal
        // recovery evidence that caused this terminal result.
        health.recovery_service = recovery_diagnostics.recovery_service;
        health.reopen_retry_count = recovery_diagnostics.reopen_retry_count;
        health.first_recovery_error = recovery_diagnostics.first_recovery_error;
        health.current_recovery_error = recovery_diagnostics.current_recovery_error;
        append_operator_log("error=" + message);
        return {
            .ok = false,
            .message = std::move(message),
        };
    }

    void append_operator_log(std::string line) {
        if (operator_log_lines.empty() || operator_log_lines.back() != line) {
            operator_log_lines.push_back(std::move(line));
        }
    }

    Plaza2Aggr20MdConfig config;
    Plaza2Aggr20MdHealthSnapshot health;
    Plaza2RuntimeProbeReport probe_report;
    Plaza2Aggr20BookProjector projector;
    Plaza2Aggr20ListenerBridge listener_bridge;
    Plaza2Env env;
    Plaza2Connection connection;
    Plaza2Listener listener;
    Plaza2Aggr20MdStreamConfig effective_stream;
    std::vector<std::string> operator_log_lines;
    std::optional<Plaza2Credentials> loaded_credentials;
    std::optional<Plaza2Credentials> loaded_software_key;
    Plaza2Settings effective_runtime;
    std::string effective_connection_settings;
    std::string effective_connection_open_settings;
    bool started{false};
};

Plaza2Aggr20MdRunner::Plaza2Aggr20MdRunner(Plaza2Aggr20MdConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Plaza2Aggr20MdRunner::~Plaza2Aggr20MdRunner() {
    if (impl_ != nullptr) {
        static_cast<void>(impl_->stop());
    }
}

Plaza2Aggr20MdRunner::Plaza2Aggr20MdRunner(Plaza2Aggr20MdRunner&&) noexcept = default;

Plaza2Aggr20MdRunner& Plaza2Aggr20MdRunner::operator=(Plaza2Aggr20MdRunner&&) noexcept = default;

Plaza2Aggr20MdRunResult Plaza2Aggr20MdRunner::start() {
    return impl_->start();
}

Plaza2Aggr20MdRunResult Plaza2Aggr20MdRunner::poll_once() {
    return impl_->poll_once();
}

Plaza2Aggr20MdRunResult Plaza2Aggr20MdRunner::stop() {
    return impl_->stop();
}

const Plaza2Aggr20MdHealthSnapshot& Plaza2Aggr20MdRunner::health_snapshot() const noexcept {
    return impl_->health;
}

const Plaza2RuntimeProbeReport& Plaza2Aggr20MdRunner::probe_report() const noexcept {
    return impl_->probe_report;
}

const Plaza2Aggr20BookProjector& Plaza2Aggr20MdRunner::projector() const noexcept {
    return impl_->projector;
}

const std::vector<std::string>& Plaza2Aggr20MdRunner::operator_log_lines() const noexcept {
    return impl_->operator_log_lines;
}

} // namespace moex::plaza2::cgate
