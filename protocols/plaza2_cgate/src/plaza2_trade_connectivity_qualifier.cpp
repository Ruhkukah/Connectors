#include "moex/plaza2/cgate/plaza2_trade_connectivity_qualifier.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace moex::plaza2::cgate {

namespace {

using private_state::InstrumentKind;
using private_state::PositionScope;

bool parse_isin_id(std::string_view value, std::int32_t& out) {
    if (value.empty()) {
        return false;
    }
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    std::int32_t parsed = 0;
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return false;
    }
    out = parsed;
    return true;
}

class QualificationAggr20Bridge final : public Plaza2ListenerEventHandler {
  public:
    explicit QualificationAggr20Bridge(Plaza2Aggr20BookProjector& projector) : projector_(projector) {}

    [[nodiscard]] bool online() const noexcept {
        return online_;
    }

    [[nodiscard]] bool snapshot_complete() const noexcept {
        return snapshot_complete_;
    }

    [[nodiscard]] bool has_lifenum() const noexcept {
        return has_lifenum_;
    }

    [[nodiscard]] std::uint64_t last_lifenum() const noexcept {
        return last_lifenum_;
    }

    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override {
        switch (event.kind) {
        case Plaza2ListenerEventKind::Open:
        case Plaza2ListenerEventKind::Timeout:
        case Plaza2ListenerEventKind::ReplState:
            return {};
        case Plaza2ListenerEventKind::LifeNum:
            if (has_lifenum_ && last_lifenum_ != event.unsigned_value) {
                online_ = false;
                snapshot_complete_ = false;
                projector_.reset();
            }
            has_lifenum_ = true;
            last_lifenum_ = event.unsigned_value;
            return {};
        case Plaza2ListenerEventKind::Close:
            online_ = false;
            snapshot_complete_ = false;
            projector_.reset();
            return {};
        case Plaza2ListenerEventKind::TransactionBegin:
            projector_.begin_transaction();
            return {};
        case Plaza2ListenerEventKind::TransactionCommit:
            return projector_.commit();
        case Plaza2ListenerEventKind::StreamData:
            if (event.table_code != generated::TableCode::kFortsAggrReplOrdersAggr) {
                return {};
            }
            return projector_.on_row(event.fields);
        case Plaza2ListenerEventKind::Online:
            online_ = true;
            snapshot_complete_ = true;
            return {};
        case Plaza2ListenerEventKind::ClearDeleted:
            online_ = false;
            projector_.reset();
            snapshot_complete_ = false;
            return {};
        }
        return {};
    }

  private:
    Plaza2Aggr20BookProjector& projector_;
    bool online_{false};
    bool snapshot_complete_{false};
    bool has_lifenum_{false};
    std::uint64_t last_lifenum_{0};
};

bool is_stream_ready(const Plaza2LiveStreamStatus& stream) {
    const bool initial_ready = !stream.required_online || (stream.online && stream.snapshot_complete);
    const bool periodic_ready =
        stream.stream_code != generated::StreamCode::kFortsUserorderbookRepl || stream.periodic_snapshot_consistent;
    return stream.created && stream.opened && initial_ready && periodic_ready;
}

const Plaza2LiveStreamStatus* find_stream(std::span<const Plaza2LiveStreamStatus> streams, std::string_view label) {
    for (const auto& stream : streams) {
        if (stream.stream_name == label) {
            return &stream;
        }
    }
    return nullptr;
}

bool has_all_private_streams(std::span<const Plaza2LiveStreamStatus> streams) {
    constexpr std::array<std::string_view, 5> required = {
        "FORTS_TRADE_REPL", "FORTS_USERORDERBOOK_REPL", "FORTS_POS_REPL", "FORTS_PART_REPL", "FORTS_REFDATA_REPL",
    };
    return std::ranges::all_of(required, [&](std::string_view label) {
        const auto* stream = find_stream(streams, label);
        return stream != nullptr && is_stream_ready(*stream);
    });
}

bool has_all_status_streams(std::span<const Plaza2LiveStreamStatus> streams) {
    constexpr std::array<std::string_view, 2> required = {
        "FORTS_SESSIONSTATE_REPL",
        "FORTS_INSTRUMENTSTATE_REPL",
    };
    return std::ranges::all_of(required, [&](std::string_view label) {
        const auto* stream = find_stream(streams, label);
        return stream != nullptr && is_stream_ready(*stream);
    });
}

bool has_p2mqreply(std::span<const Plaza2LiveStreamStatus> streams) {
    for (const auto& stream : streams) {
        if (stream.stream_name == "p2mqreply" || stream.stream_name == "P2MQREPLY") {
            return stream.created && stream.opened;
        }
    }
    return false;
}

const private_state::InstrumentSnapshot* find_target(std::span<const private_state::InstrumentSnapshot> instruments,
                                                     std::string_view target) {
    std::int32_t target_id = 0;
    const bool target_is_numeric = parse_isin_id(target, target_id);
    for (const auto& instrument : instruments) {
        if (!instrument.current_session_member) {
            continue;
        }
        if ((target_is_numeric && instrument.isin_id == target_id) || instrument.isin == target ||
            instrument.short_isin == target) {
            return &instrument;
        }
    }
    return nullptr;
}

void add_failure(Plaza2QualificationSnapshot& snapshot, bool condition, std::string reason) {
    if (!condition && std::find(snapshot.failure_reasons.begin(), snapshot.failure_reasons.end(), reason) ==
                          snapshot.failure_reasons.end()) {
        snapshot.failure_reasons.push_back(std::move(reason));
    }
}

} // namespace

struct Plaza2TradeConnectivityQualifier::Impl {
    explicit Impl(Plaza2TradeConnectivityQualifierConfig initial_config)
        : config(std::move(initial_config)), aggr_bridge(aggr_projector) {
        for (auto& stream : config.session.streams) {
            if (stream.stream_code == generated::StreamCode::kFortsAggrRepl) {
                stream.require_online = false;
                stream.handler = &aggr_bridge;
            }
        }
        private_runner = std::make_unique<Plaza2LiveSessionRunner>(config.session);
    }

    Plaza2TradeConnectivityQualificationResult start() {
        if (started) {
            return fail("PLAZA II qualification is already started");
        }
        if (const auto validation = validate_config(); !validation.ok) {
            return validation;
        }
        snapshot.state = Plaza2QualificationState::Validated;
        snapshot.terminal = Plaza2QualificationTerminal::NotReady;

        if (const auto result = private_runner->start(); !result.ok) {
            refresh();
            return fail(result.message);
        }
        started = true;
        snapshot.state = Plaza2QualificationState::Started;
        refresh();
        return {.ok = true, .message = "PLAZA II TEST connectivity qualification started"};
    }

    Plaza2TradeConnectivityQualificationResult poll_once() {
        if (!started) {
            return fail("PLAZA II qualification is not started");
        }
        if (const auto result = private_runner->poll_once(); !result.ok) {
            return fail(result.message);
        }
        refresh();
        if (snapshot.add_order_qualified && snapshot.state != Plaza2QualificationState::Stopped) {
            snapshot.state = Plaza2QualificationState::Ready;
            snapshot.terminal = Plaza2QualificationTerminal::Ready;
        } else {
            snapshot.terminal = Plaza2QualificationTerminal::NotReady;
        }
        return {.ok = true, .message = "PLAZA II TEST connectivity qualification poll completed"};
    }

    Plaza2TradeConnectivityQualificationResult stop() {
        if (private_runner != nullptr) {
            const auto result = private_runner->stop();
            started = false;
            snapshot.state = Plaza2QualificationState::Stopped;
            refresh();
            snapshot.terminal = snapshot.add_order_qualified ? Plaza2QualificationTerminal::Ready
                                                             : Plaza2QualificationTerminal::NotReady;
            if (!result.ok) {
                return {.ok = false, .message = result.message};
            }
        }
        return {.ok = true, .message = "PLAZA II TEST connectivity qualification stopped"};
    }

    Plaza2TradeConnectivityQualificationResult validate_config() {
        if (config.target.isin.empty()) {
            return fail("qualification target isin must be provided");
        }
        if (config.target.participant.empty()) {
            return fail("qualification participant must be provided");
        }
        if (config.max_aggr20_age_ms == 0) {
            return fail("max_aggr20_age_ms must be greater than zero");
        }
        if (!config.test_market_data_armed) {
            return fail("qualification requires --armed-test-market-data");
        }
        if (!config.session.open_publisher || config.session.publisher_settings.empty()) {
            return fail("qualification requires publisher open-only mode");
        }
        const auto& streams = config.session.streams;
        const auto has_stream_code = [&](generated::StreamCode code) {
            return std::ranges::any_of(
                streams, [&](const Plaza2LiveStreamConfig& stream) { return stream.stream_code == code; });
        };
        if (!has_stream_code(generated::StreamCode::kFortsSessionstateRepl) ||
            !has_stream_code(generated::StreamCode::kFortsInstrumentstateRepl)) {
            return fail("qualification requires SESSIONSTATE and INSTRUMENTSTATE listeners");
        }
        if (!has_stream_code(generated::StreamCode::kFortsAggrRepl)) {
            return fail("qualification requires the FORTS_AGGR_REPL listener");
        }
        if (!std::ranges::any_of(streams, [](const Plaza2LiveStreamConfig& stream) {
                return stream.stream_code == kNoStreamCode &&
                       (stream.label == "p2mqreply" || stream.label == "P2MQREPLY");
            })) {
            return fail("qualification requires an untyped p2mqreply listener");
        }
        return {.ok = true, .message = "PLAZA II qualification config validated"};
    }

    void refresh() {
        if (private_runner == nullptr) {
            return;
        }
        const auto& health = private_runner->health_snapshot();
        const auto& probe = private_runner->probe_report();
        const auto streams = std::span<const Plaza2LiveStreamStatus>(health.streams);

        snapshot.private_streams_ready = has_all_private_streams(streams);
        snapshot.status_streams_ready = has_all_status_streams(streams);
        snapshot.p2mqreply_open = has_p2mqreply(streams);
        snapshot.publisher_open = health.publisher_opened;
        snapshot.runtime_trading_capable = probe.trading_capable;
        snapshot.connectivity_ready = health.runtime_probe_ok && health.scheme_drift_ok &&
                                      snapshot.private_streams_ready && snapshot.status_streams_ready &&
                                      snapshot.p2mqreply_open;
        snapshot.target_isin_id = 0;
        snapshot.target_sess_id = 0;
        snapshot.target_found = false;
        snapshot.target_current_session_member = false;
        snapshot.target_min_step.clear();
        snapshot.target_trade_mode_id = 0;
        snapshot.target_session_status_available = false;
        snapshot.target_session_status = 0;
        snapshot.target_session_add_capable = false;
        snapshot.target_instrument_status_available = false;
        snapshot.target_instrument_status = 0;
        snapshot.target_instrument_add_capable = false;
        snapshot.target_refdata_present = false;
        snapshot.target_aggr20_two_sided = false;
        snapshot.target_aggr20_age_ms = 0;
        snapshot.target_aggr20_repl_id = 0;
        snapshot.target_aggr20_repl_rev = 0;
        snapshot.aggr20_has_lifenum = aggr_bridge.has_lifenum();
        snapshot.aggr20_lifenum = aggr_bridge.last_lifenum();
        snapshot.aggr20_row_count = aggr_projector.snapshot().row_count;
        snapshot.participant_limit_unique = false;
        snapshot.participant_limits_set = false;
        snapshot.applicable_position_count = 0;
        snapshot.position_identity_exact = false;
        snapshot.position_account_type = 0;
        snapshot.position_xpos = 0;
        snapshot.failure_reasons.clear();

        const auto& projector = private_runner->projector();
        const auto* target = find_target(projector.instruments(), config.target.isin);
        if (target != nullptr) {
            snapshot.target_found = true;
            snapshot.target_isin_id = target->isin_id;
            snapshot.target_sess_id = target->sess_id;
            snapshot.target_current_session_member = target->current_session_member;
            snapshot.target_instrument_status_available = target->has_current_status;
            snapshot.target_instrument_status = target->current_status;
            snapshot.target_instrument_add_capable =
                snapshot.target_instrument_status_available && snapshot.target_instrument_status == 1;
            snapshot.target_min_step = target->min_step;
            snapshot.target_trade_mode_id = target->trade_mode_id;
            snapshot.target_refdata_present = target->kind == InstrumentKind::kFuture && target->sess_id != 0 &&
                                              target->current_session_member && !target->min_step.empty() &&
                                              target->trade_mode_id != 0;

            for (const auto& session : projector.sessions()) {
                if (session.sess_id == target->sess_id) {
                    snapshot.target_session_status_available = session.has_current_status;
                    snapshot.target_session_status = session.current_status;
                    snapshot.target_session_add_capable =
                        snapshot.target_session_status_available && snapshot.target_session_status == 1;
                    break;
                }
            }

            const auto target_book = aggr_projector.snapshot_for_isin(target->isin_id);
            if (target_book.has_value()) {
                snapshot.target_aggr20_two_sided = target_book->top_bid.has_value() && target_book->top_ask.has_value();
                snapshot.target_aggr20_repl_id = target_book->last_repl_id;
                snapshot.target_aggr20_repl_rev = target_book->last_repl_rev;
                if (target_book->committed_at != Plaza2Aggr20BookProjector::Clock::time_point{}) {
                    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                        Plaza2Aggr20BookProjector::Clock::now() - target_book->committed_at);
                    snapshot.target_aggr20_age_ms = age.count() < 0 ? 0U : static_cast<std::uint32_t>(age.count());
                }
            }
        }

        std::size_t limit_matches = 0;
        for (const auto& limit : projector.limits()) {
            if (limit.scope == PositionScope::kClient && limit.account_code == config.target.participant) {
                ++limit_matches;
                snapshot.participant_limits_set = limit.limits_set;
            }
        }
        snapshot.participant_limit_unique = limit_matches == 1;

        for (const auto& position : projector.positions()) {
            if (position.scope == PositionScope::kClient && position.account_code == config.target.participant &&
                position.isin_id == snapshot.target_isin_id) {
                ++snapshot.applicable_position_count;
                snapshot.position_account_type = position.account_type;
                snapshot.position_xpos = position.xpos;
            }
        }
        snapshot.position_identity_exact =
            snapshot.applicable_position_count == 1 &&
            snapshot.position_account_type == config.target.expected_position_account_type;

        const bool aggr_age_ok = snapshot.target_aggr20_age_ms <= config.max_aggr20_age_ms &&
                                 snapshot.target_aggr20_repl_id != 0 && snapshot.target_aggr20_repl_rev != 0 &&
                                 aggr_bridge.online() && aggr_bridge.snapshot_complete();
        snapshot.market_state_ready = health.runtime_probe_ok && health.scheme_drift_ok &&
                                      snapshot.target_refdata_present && snapshot.target_session_add_capable &&
                                      snapshot.target_instrument_add_capable && snapshot.target_aggr20_two_sided &&
                                      aggr_age_ok;
        snapshot.account_state_ready =
            snapshot.participant_limit_unique && snapshot.participant_limits_set && snapshot.position_identity_exact;
        snapshot.publisher_ready = snapshot.publisher_open && snapshot.runtime_trading_capable;
        snapshot.add_order_qualified = snapshot.connectivity_ready && snapshot.market_state_ready &&
                                       snapshot.account_state_ready && snapshot.publisher_ready;

        add_failure(snapshot, health.runtime_probe_ok, "runtime probe failed");
        add_failure(snapshot, health.scheme_drift_ok, "scheme drift is not compatible");
        add_failure(snapshot, snapshot.private_streams_ready, "private replication stream incomplete");
        add_failure(snapshot, snapshot.status_streams_ready, "SESSIONSTATE or INSTRUMENTSTATE incomplete");
        add_failure(snapshot, snapshot.p2mqreply_open, "p2mqreply listener is not open");
        add_failure(snapshot, snapshot.publisher_open, "publisher is not open");
        add_failure(snapshot, snapshot.target_found, "qualification target is not present in refdata");
        add_failure(snapshot, snapshot.target_refdata_present, "target refdata is incomplete");
        add_failure(snapshot, snapshot.target_session_status_available, "target SESSIONSTATE is unavailable");
        add_failure(snapshot, snapshot.target_instrument_status_available, "target INSTRUMENTSTATE is unavailable");
        add_failure(snapshot, snapshot.target_session_add_capable,
                    "target SESSIONSTATE public_state is not add-capable (1)");
        add_failure(snapshot, snapshot.target_instrument_add_capable,
                    "target INSTRUMENTSTATE public_state is not add-capable (1)");
        add_failure(snapshot, snapshot.target_aggr20_two_sided, "target AGGR20 BBO is not two-sided");
        add_failure(snapshot, aggr_age_ok, "target AGGR20 evidence is missing, unversioned, stale, or offline");
        add_failure(snapshot, snapshot.participant_limit_unique && snapshot.participant_limits_set,
                    "participant limit identity is missing, ambiguous, or limits_set=false");
        add_failure(snapshot, snapshot.position_identity_exact,
                    "position identity is missing, ambiguous, or wrong account_type");
        add_failure(snapshot, snapshot.runtime_trading_capable, "runtime trading capability symbols are incomplete");
        if (snapshot.add_order_qualified && snapshot.state != Plaza2QualificationState::Stopped) {
            snapshot.state = Plaza2QualificationState::Ready;
            snapshot.terminal = Plaza2QualificationTerminal::Ready;
        } else if (snapshot.state != Plaza2QualificationState::Failed) {
            snapshot.terminal = Plaza2QualificationTerminal::NotReady;
        }
    }

    Plaza2TradeConnectivityQualificationResult fail(std::string message) {
        snapshot.state = Plaza2QualificationState::Failed;
        snapshot.terminal = Plaza2QualificationTerminal::Error;
        snapshot.failure_reasons.push_back(message);
        return {.ok = false, .message = std::move(message)};
    }

    Plaza2TradeConnectivityQualifierConfig config;
    Plaza2Aggr20BookProjector aggr_projector;
    QualificationAggr20Bridge aggr_bridge;
    std::unique_ptr<Plaza2LiveSessionRunner> private_runner;
    Plaza2QualificationSnapshot snapshot;
    bool started{false};
};

std::string_view plaza2_qualification_state_name(Plaza2QualificationState state) noexcept {
    switch (state) {
    case Plaza2QualificationState::Created:
        return "Created";
    case Plaza2QualificationState::Validated:
        return "Validated";
    case Plaza2QualificationState::Started:
        return "Started";
    case Plaza2QualificationState::Ready:
        return "Ready";
    case Plaza2QualificationState::Stopped:
        return "Stopped";
    case Plaza2QualificationState::Failed:
        return "Failed";
    }
    return "Unknown";
}

std::string_view plaza2_qualification_terminal_name(Plaza2QualificationTerminal terminal) noexcept {
    switch (terminal) {
    case Plaza2QualificationTerminal::Ready:
        return "READY";
    case Plaza2QualificationTerminal::NotReady:
        return "NOT_READY";
    case Plaza2QualificationTerminal::Error:
        return "ERROR";
    }
    return "ERROR";
}

Plaza2TradeConnectivityQualifier::Plaza2TradeConnectivityQualifier(Plaza2TradeConnectivityQualifierConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Plaza2TradeConnectivityQualifier::~Plaza2TradeConnectivityQualifier() {
    if (impl_ != nullptr) {
        static_cast<void>(impl_->stop());
    }
}

Plaza2TradeConnectivityQualifier::Plaza2TradeConnectivityQualifier(Plaza2TradeConnectivityQualifier&&) noexcept =
    default;

Plaza2TradeConnectivityQualifier&
Plaza2TradeConnectivityQualifier::operator=(Plaza2TradeConnectivityQualifier&&) noexcept = default;

Plaza2TradeConnectivityQualificationResult Plaza2TradeConnectivityQualifier::start() {
    return impl_->start();
}

Plaza2TradeConnectivityQualificationResult Plaza2TradeConnectivityQualifier::poll_once() {
    return impl_->poll_once();
}

Plaza2TradeConnectivityQualificationResult Plaza2TradeConnectivityQualifier::stop() {
    return impl_->stop();
}

const Plaza2QualificationSnapshot& Plaza2TradeConnectivityQualifier::qualification() const noexcept {
    return impl_->snapshot;
}

const Plaza2LiveSessionRunner& Plaza2TradeConnectivityQualifier::private_session() const noexcept {
    return *impl_->private_runner;
}

const Plaza2Aggr20BookProjector& Plaza2TradeConnectivityQualifier::aggr20_projector() const noexcept {
    return impl_->aggr_projector;
}

} // namespace moex::plaza2::cgate
