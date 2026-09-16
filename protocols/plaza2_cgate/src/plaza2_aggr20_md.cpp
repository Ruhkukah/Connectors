#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
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

std::optional<std::string_view> fixed_point_text_field(std::span<const Plaza2DecodedFieldValue> fields,
                                                       FieldCode code) {
    for (const auto& field : fields) {
        if (field.field_code != code) {
            continue;
        }
        if (field.kind == Plaza2DecodedValueKind::String || field.kind == Plaza2DecodedValueKind::Decimal) {
            return field.text_value;
        }
        return std::nullopt;
    }
    return std::nullopt;
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

const Plaza2Aggr20AuthoritySnapshot Plaza2Aggr20ListenerBridge::authority_snapshot() const {
    Plaza2Aggr20AuthoritySnapshot out;
    out.transport_active = transport_active_;
    out.snapshot_complete = snapshot_complete_;
    out.session_data_ready = session_data_ready_;
    out.target_authoritative = target_authoritative_;
    out.stream_epoch = stream_epoch_;
    out.market_data_authority_epoch = market_data_authority_epoch_;
    out.last_sys_event = last_sys_event_;
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
    switch (event.kind) {
    case Plaza2ListenerEventKind::Open:
        invalidate(false, true);
        return {};
    case Plaza2ListenerEventKind::LifeNum:
        invalidate(false, transport_active_);
        has_lifenum_ = true;
        last_lifenum_ = event.unsigned_value;
        return {};
    case Plaza2ListenerEventKind::ClearDeleted:
        // CGate sends cleanup markers before the initial snapshot. Deleting from an already
        // empty projection needs no reopen, which would only request the same markers again.
        if (!reopen_required_ && !online_ && !snapshot_complete_ && !projector_.transaction_open() &&
            projector_.snapshot().row_count == 0 && projector_.snapshot().last_repl_rev == 0) {
            invalidate(false, transport_active_);
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
        return {};
    }
    case Plaza2ListenerEventKind::TransactionBegin:
        if (reopen_required_)
            return {};
        projector_.begin_transaction();
        return {};
    case Plaza2ListenerEventKind::TransactionCommit:
        if (reopen_required_)
            return {};
        return projector_.commit();
    case Plaza2ListenerEventKind::StreamData:
        if (reopen_required_)
            return {};
        if (event.table_code == generated::TableCode::kFortsAggrReplOrdersAggr)
            return projector_.on_row(event.fields);
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
            last_sys_event_ = sys_event;

            // A sys_events row carried in the bootstrap transaction is useful
            // provenance but cannot certify the current stream generation.
            if (!snapshot_complete_ || !online_)
                return {};
            if (sys_event.event_type == 1 && is_session_data_ready_message(sys_event.message) &&
                accepts_session(sys_event.sess_id)) {
                session_data_ready_ = true;
                target_authoritative_ = true;
            }
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
        return {};
    default:
        return {};
    }
}

Plaza2Error Plaza2Aggr20ListenerBridge::supervise(Plaza2Listener& listener, std::chrono::steady_clock::time_point now) {
    std::uint32_t state = 0;
    if (const auto error = listener.state(state); error) {
        last_recovery_error_ = error;
        recovery_failure_classification_ = "fatal_listener_state";
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
    return {};
}

Plaza2Aggr20BookProjector::Plaza2Aggr20BookProjector(NowFn now)
    : now_(now ? std::move(now) : NowFn{[] { return Clock::now(); }}) {}

void Plaza2Aggr20BookProjector::reset() {
    staged_rows_.clear();
    affected_isin_ids_.clear();
    committed_ = {};
    instrument_snapshots_.clear();
    transaction_open_ = false;
}

void Plaza2Aggr20BookProjector::begin_transaction() {
    staged_rows_.clear();
    affected_isin_ids_.clear();
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
    const auto price_text = fixed_point_text_field(fields, FieldCode::kFortsAggrReplOrdersAggrPrice);
    if (!price_text.has_value()) {
        return {
            .code = Plaza2ErrorCode::DecodeFailed,
            .runtime_code = 0,
            .message = "AGGR20 orders_aggr.price is missing or has an unsupported runtime value kind",
        };
    }
    const auto price_scaled = parse_fixed_point(*price_text, kPlaza2Aggr20FractionalDigits, false);
    if (!price_scaled.has_value()) {
        return {
            .code = Plaza2ErrorCode::DecodeFailed,
            .runtime_code = 0,
            .message = "AGGR20 orders_aggr.price is malformed, over-precise, or outside checked fixed-point range",
        };
    }
    level.price = std::string(*price_text);
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
    staged_rows_.push_back(std::move(level));
    return {};
}

Plaza2Error Plaza2Aggr20BookProjector::commit() {
    if (!transaction_open_) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = 0,
            .message = "AGGR20 TN_COMMIT arrived without TN_BEGIN",
        };
    }

    for (const auto& row : staged_rows_) {
        auto existing = std::find_if(committed_.levels.begin(), committed_.levels.end(), [&](const auto& level) {
            // AGGR rows are mutable replication slots, not immutable price levels.
            // A price, side, or instrument change replaces the previous slot.
            return level.repl_id == row.repl_id;
        });
        if (existing != committed_.levels.end()) {
            affected_isin_ids_.insert(existing->isin_id);
        }
        committed_.last_repl_id = std::max(committed_.last_repl_id, row.repl_id);
        committed_.last_repl_rev = std::max(committed_.last_repl_rev, row.repl_rev);
        if (row.volume <= 0) {
            if (existing != committed_.levels.end()) {
                committed_.levels.erase(existing);
            }
            continue;
        }
        if (existing == committed_.levels.end()) {
            committed_.levels.push_back(row);
        } else {
            *existing = row;
        }
    }

    committed_.row_count = committed_.levels.size();
    committed_.committed_at = now_();
    committed_.exchange_moment = 0;
    committed_.exchange_moment_ns = 0;
    committed_.bid_depth_levels = 0;
    committed_.ask_depth_levels = 0;
    committed_.top_bid.reset();
    committed_.top_ask.reset();
    // Rebuild the diagnostic/global view from the committed levels, but only
    // refresh instrument-scoped freshness for instruments touched by this
    // transaction.  An unrelated instrument update must not make the target
    // BBO look newer than its last local row.
    std::set<std::int64_t> instruments;
    for (const auto& isin_id : affected_isin_ids_) {
        const auto previous = instrument_snapshots_.find(isin_id);
        Plaza2Aggr20InstrumentSnapshot scoped;
        if (previous != instrument_snapshots_.end())
            scoped = previous->second;
        scoped.isin_id = isin_id;
        for (const auto& row : staged_rows_) {
            if (row.isin_id != isin_id) {
                continue;
            }
            // Keep the last relevant replication metadata even when this row
            // is a deletion and no level remains in the visible book.
            scoped.last_repl_id = std::max(scoped.last_repl_id, row.repl_id);
            scoped.last_repl_rev = std::max(scoped.last_repl_rev, row.repl_rev);
            scoped.exchange_moment = std::max(scoped.exchange_moment, row.moment);
            scoped.exchange_moment_ns = std::max(scoped.exchange_moment_ns, row.moment_ns);
        }
        scoped.row_count = 0;
        scoped.bid_depth_levels = 0;
        scoped.ask_depth_levels = 0;
        scoped.top_bid.reset();
        scoped.top_ask.reset();
        scoped.levels.clear();
        for (const auto& level : committed_.levels) {
            if (level.isin_id != isin_id) {
                continue;
            }
            scoped.levels.push_back(level);
            scoped.row_count += 1;
            if (level.dir == 1) {
                scoped.bid_depth_levels += 1;
                if (!scoped.top_bid.has_value() || level.price_scaled > scoped.top_bid->price_scaled) {
                    scoped.top_bid = level;
                }
            } else if (level.dir == 2) {
                scoped.ask_depth_levels += 1;
                if (!scoped.top_ask.has_value() || level.price_scaled < scoped.top_ask->price_scaled) {
                    scoped.top_ask = level;
                }
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
            scoped.source_snapshot_version = ++snapshot_version_counter_;
            scoped.committed_at = committed_.committed_at;
        }
        instrument_snapshots_[isin_id] = std::move(scoped);
    }
    for (const auto& level : committed_.levels) {
        instruments.insert(level.isin_id);
        committed_.exchange_moment = std::max(committed_.exchange_moment, level.moment);
        committed_.exchange_moment_ns = std::max(committed_.exchange_moment_ns, level.moment_ns);
        if (level.dir == 1) {
            committed_.bid_depth_levels += 1;
            if (!committed_.top_bid.has_value() || level.price_scaled > committed_.top_bid->price_scaled) {
                committed_.top_bid = level;
            }
        } else if (level.dir == 2) {
            committed_.ask_depth_levels += 1;
            if (!committed_.top_ask.has_value() || level.price_scaled < committed_.top_ask->price_scaled) {
                committed_.top_ask = level;
            }
        }
    }
    committed_.instrument_count = instruments.size();
    if (qualification_observer_)
        qualification_observer_->committed(committed_);
    staged_rows_.clear();
    affected_isin_ids_.clear();
    transaction_open_ = false;
    return {};
}

void Plaza2Aggr20BookProjector::rollback() {
    staged_rows_.clear();
    affected_isin_ids_.clear();
    transaction_open_ = false;
}

const Plaza2Aggr20Snapshot& Plaza2Aggr20BookProjector::snapshot() const noexcept {
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
        health.clock_evidence_ok =
            health.clock_evidence_present && plaza2_clock_evidence_passes(*config.clock_evidence);
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
        listener_bridge.reset();
        health.state = Plaza2Aggr20MdRunnerState::Failed;
        health.last_error = message;
        refresh_health();
        if (!bridge_classification.empty()) {
            health.failure_classification = bridge_classification;
        }
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
