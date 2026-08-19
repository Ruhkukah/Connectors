#include "moex/plaza2_trade/plaza2_order_lifecycle.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace moex::plaza2_trade {

namespace {

namespace cgate = plaza2::cgate;
namespace private_state = plaza2::private_state;

constexpr std::int64_t kDecimalScale = 100000;

struct ParsedDecimal {
    std::int64_t scaled{0};
};

std::optional<ParsedDecimal> parse_nonnegative_decimal(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto dot = text.find('.');
    if (dot != std::string_view::npos && text.find('.', dot + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto whole = dot == std::string_view::npos ? text : text.substr(0, dot);
    const auto fractional = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (whole.empty() || fractional.size() > 5) {
        return std::nullopt;
    }
    const auto digits_only = [](std::string_view value) {
        return std::all_of(value.begin(), value.end(),
                           [](unsigned char character) { return std::isdigit(character) != 0; });
    };
    if (!digits_only(whole) || !digits_only(fractional)) {
        return std::nullopt;
    }

    std::int64_t whole_value = 0;
    for (const auto character : whole) {
        const auto digit = static_cast<std::int64_t>(character - '0');
        if (whole_value > (std::numeric_limits<std::int64_t>::max() / 10 - digit)) {
            return std::nullopt;
        }
        whole_value = whole_value * 10 + digit;
    }
    if (whole_value > std::numeric_limits<std::int64_t>::max() / kDecimalScale) {
        return std::nullopt;
    }

    std::int64_t fractional_value = 0;
    for (const auto character : fractional) {
        fractional_value = fractional_value * 10 + static_cast<std::int64_t>(character - '0');
    }
    for (std::size_t index = fractional.size(); index < 5; ++index) {
        fractional_value *= 10;
    }
    return ParsedDecimal{.scaled = whole_value * kDecimalScale + fractional_value};
}

bool valid_sha256(std::string_view text) {
    return text.size() == 64 &&
           std::all_of(text.begin(), text.end(), [](unsigned char character) { return std::isxdigit(character) != 0; });
}

bool valid_run_token(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '-' || character == '_';
    });
}

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const auto character : text) {
        switch (character) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) >= 0x20U) {
                out.push_back(character);
            }
            break;
        }
    }
    return out;
}

bool atomic_write(const std::filesystem::path& path, std::string_view contents, std::string& error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "failed to create output directory: " + filesystem_error.message();
        return false;
    }
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "failed to open temporary journal file";
            return false;
        }
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output) {
            error = "failed to flush temporary journal file";
            return false;
        }
    }
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(path, filesystem_error);
        filesystem_error.clear();
        std::filesystem::rename(temporary, path, filesystem_error);
    }
    if (filesystem_error) {
        error = "failed to publish journal file: " + filesystem_error.message();
        return false;
    }
    return true;
}

PreSendPlan fail_plan(PreSendFailure failure, std::string message) {
    return {
        .ok = false,
        .failure = failure,
        .message = std::move(message),
    };
}

std::filesystem::path ext_lock_path(const OrderLifecycleConfig& config) {
    return config.journal_root / "active" / ("ext_" + std::to_string(config.ext_id));
}

std::filesystem::path user_lock_path(const OrderLifecycleConfig& config, std::uint32_t user_id) {
    return config.journal_root / "active" / ("user_" + std::to_string(user_id));
}

bool has_unfinished_identifier(const OrderLifecycleConfig& config) {
    return std::filesystem::exists(ext_lock_path(config)) ||
           std::filesystem::exists(user_lock_path(config, config.add_user_id)) ||
           std::filesystem::exists(user_lock_path(config, config.cancel_user_id));
}

std::string submission_certainty_name(cgate::Plaza2SubmissionCertainty certainty) {
    switch (certainty) {
    case cgate::Plaza2SubmissionCertainty::DefinitelyNotSent:
        return "definitely_not_sent";
    case cgate::Plaza2SubmissionCertainty::PossiblySent:
        return "possibly_sent";
    case cgate::Plaza2SubmissionCertainty::Posted:
        return "posted";
    }
    return "definitely_not_sent";
}

void append_aliases(std::vector<std::int64_t>& target, std::span<const std::int64_t> source) {
    for (const auto value : source) {
        if (value != 0 && std::find(target.begin(), target.end(), value) == target.end()) {
            target.push_back(value);
        }
    }
    std::sort(target.begin(), target.end());
}

bool contains_id(std::span<const std::int64_t> aliases, std::int64_t value) {
    return value != 0 && std::find(aliases.begin(), aliases.end(), value) != aliases.end();
}

class RunJournal {
  public:
    bool begin(const OrderLifecycleConfig& config, std::string payload_hash, std::string& error) {
        config_ = &config;
        payload_hash_ = std::move(payload_hash);
        if (!valid_run_token(config.run_id) || config.journal_root.empty()) {
            error = "journal requires a safe unique run_id and explicit journal_root";
            return false;
        }

        std::error_code filesystem_error;
        std::filesystem::create_directories(config.journal_root / "active", filesystem_error);
        if (filesystem_error) {
            error = "failed to create active journal directory: " + filesystem_error.message();
            return false;
        }
        run_directory_ = config.journal_root / config.run_id;
        if (!std::filesystem::create_directory(run_directory_, filesystem_error)) {
            error = filesystem_error ? "failed to create run journal: " + filesystem_error.message()
                                     : "run_id already exists";
            return false;
        }

        const std::array locks = {
            ext_lock_path(config),
            user_lock_path(config, config.add_user_id),
            user_lock_path(config, config.cancel_user_id),
        };
        for (const auto& lock : locks) {
            filesystem_error.clear();
            if (!std::filesystem::create_directory(lock, filesystem_error)) {
                for (const auto& created : active_locks_) {
                    std::filesystem::remove(created, filesystem_error);
                }
                active_locks_.clear();
                error = filesystem_error ? "failed to reserve order identifier: " + filesystem_error.message()
                                         : "ext_id or user_id belongs to an unfinished run";
                return false;
            }
            active_locks_.push_back(lock);
        }
        path_ = run_directory_ / "journal.json";
        return persist(error);
    }

    void record_add_submission(const cgate::Plaza2PublisherMessageResult& submission) {
        add_submission_ = submission;
        persist_ignoring_error();
    }

    void record_cancel_submission(const cgate::Plaza2PublisherMessageResult& submission) {
        cancel_submission_ = submission;
        persist_ignoring_error();
    }

    void record_add_reply(const OrderReplyObservation& reply) {
        add_reply_ = reply;
        persist_ignoring_error();
    }

    void record_cancel_reply(const OrderReplyObservation& reply) {
        cancel_reply_ = reply;
        persist_ignoring_error();
    }

    void record_observation(const OrderObservation& observation) {
        observation_ = observation;
        record_state(observation.state);
    }

    void record_state(OrderLifecycleState state) {
        if (states_.empty() || states_.back() != state) {
            states_.push_back(state);
        }
        persist_ignoring_error();
    }

    bool finish(OrderLifecycleState state, bool finished, bool orphan, std::string& error) {
        if (states_.empty() || states_.back() != state) {
            states_.push_back(state);
        }
        final_state_ = state;
        finished_ = finished;
        orphan_incident_ = orphan;
        if (!persist(error)) {
            return false;
        }
        if (finished) {
            std::error_code filesystem_error;
            for (const auto& lock : active_locks_) {
                std::filesystem::remove(lock, filesystem_error);
            }
            active_locks_.clear();
        }
        return true;
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

    const std::vector<OrderLifecycleState>& states() const noexcept {
        return states_;
    }

  private:
    std::string json() const {
        std::ostringstream out;
        out << "{\n";
        out << "  \"schema\": \"moex.plaza2.order_run_journal.v1\",\n";
        out << "  \"run_id\": \"" << json_escape(config_->run_id) << "\",\n";
        out << "  \"profile_id\": \"" << json_escape(config_->profile_id) << "\",\n";
        out << "  \"profile_fingerprint\": \"" << json_escape(config_->profile_fingerprint) << "\",\n";
        out << "  \"add_command\": \"AddOrder\",\n";
        out << "  \"cancel_command\": \"DelOrder\",\n";
        out << "  \"ext_id\": " << config_->ext_id << ",\n";
        out << "  \"add_user_id\": " << config_->add_user_id << ",\n";
        out << "  \"cancel_user_id\": " << config_->cancel_user_id << ",\n";
        out << "  \"payload_sha256\": \"" << payload_hash_ << "\",\n";
        out << "  \"add_submission_certainty\": \"" << submission_certainty_name(add_submission_.certainty) << "\",\n";
        out << "  \"add_post_invoked\": " << (add_submission_.post_invoked ? "true" : "false") << ",\n";
        out << "  \"add_reply_observed\": " << (add_reply_.has_value() ? "true" : "false") << ",\n";
        out << "  \"add_reply_accepted\": " << (add_reply_.has_value() && add_reply_->accepted ? "true" : "false")
            << ",\n";
        out << "  \"add_reply_order_id\": "
            << (add_reply_.has_value() && add_reply_->order_id.has_value() ? *add_reply_->order_id : 0) << ",\n";
        out << "  \"cancel_submission_certainty\": \"" << submission_certainty_name(cancel_submission_.certainty)
            << "\",\n";
        out << "  \"cancel_post_invoked\": " << (cancel_submission_.post_invoked ? "true" : "false") << ",\n";
        out << "  \"cancel_reply_observed\": " << (cancel_reply_.has_value() ? "true" : "false") << ",\n";
        out << "  \"replication_observed\": " << (observation_.has_value() ? "true" : "false") << ",\n";
        out << "  \"trade_repl_provenance\": "
            << (observation_.has_value() && observation_->from_trade_replication ? "true" : "false") << ",\n";
        out << "  \"user_orderbook_provenance\": "
            << (observation_.has_value() && observation_->from_user_orderbook ? "true" : "false") << ",\n";
        out << "  \"own_trade_provenance\": "
            << (observation_.has_value() && observation_->from_own_trades ? "true" : "false") << ",\n";
        out << "  \"public_order_id\": " << (observation_.has_value() ? observation_->public_order_id : 0) << ",\n";
        out << "  \"private_order_id\": " << (observation_.has_value() ? observation_->private_order_id : 0) << ",\n";
        out << "  \"original_quantity\": " << (observation_.has_value() ? observation_->original_quantity : 0) << ",\n";
        out << "  \"remaining_quantity\": " << (observation_.has_value() ? observation_->remaining_quantity : 0)
            << ",\n";
        out << "  \"executed_quantity\": " << (observation_.has_value() ? observation_->executed_quantity : 0) << ",\n";
        out << "  \"state_transitions\": [";
        for (std::size_t index = 0; index < states_.size(); ++index) {
            if (index != 0) {
                out << ", ";
            }
            out << "\"" << order_lifecycle_state_name(states_[index]) << "\"";
        }
        out << "],\n";
        out << "  \"final_state\": \"" << order_lifecycle_state_name(final_state_) << "\",\n";
        out << "  \"orphan_incident\": " << (orphan_incident_ ? "true" : "false") << ",\n";
        out << "  \"finished\": " << (finished_ ? "true" : "false") << "\n";
        out << "}\n";
        return out.str();
    }

    bool persist(std::string& error) const {
        return atomic_write(path_, json(), error);
    }

    void persist_ignoring_error() const {
        std::string ignored;
        static_cast<void>(persist(ignored));
    }

    const OrderLifecycleConfig* config_{nullptr};
    std::filesystem::path run_directory_;
    std::filesystem::path path_;
    std::vector<std::filesystem::path> active_locks_;
    std::string payload_hash_;
    cgate::Plaza2PublisherMessageResult add_submission_;
    cgate::Plaza2PublisherMessageResult cancel_submission_;
    std::optional<OrderReplyObservation> add_reply_;
    std::optional<OrderReplyObservation> cancel_reply_;
    std::optional<OrderObservation> observation_;
    std::vector<OrderLifecycleState> states_;
    OrderLifecycleState final_state_{OrderLifecycleState::PossiblySent};
    bool finished_{false};
    bool orphan_incident_{false};
};

struct LifecycleEvidence {
    std::optional<OrderReplyObservation> add_reply;
    std::optional<OrderReplyObservation> cancel_reply;
    std::optional<OrderObservation> observation;
    bool poll_failed{false};
    std::string poll_error;
};

void consume_poll(const OrderLifecycleConfig& config, const OrderLifecyclePollResult& poll, LifecycleEvidence& evidence,
                  RunJournal& journal) {
    for (const auto& reply : poll.replies) {
        if (reply.command_kind == Plaza2TradeCommandKind::AddOrder && reply.user_id == config.add_user_id) {
            evidence.add_reply = reply;
            journal.record_add_reply(reply);
        } else if (reply.command_kind == Plaza2TradeCommandKind::DelOrder && reply.user_id == config.cancel_user_id) {
            evidence.cancel_reply = reply;
            journal.record_cancel_reply(reply);
        }
    }
    for (const auto& observation : poll.observations) {
        if (observation.ext_id == config.ext_id && observation.client_code == config.client_code) {
            evidence.observation = observation;
            journal.record_observation(observation);
        }
    }
    if (!poll.ok) {
        evidence.poll_failed = true;
        evidence.poll_error = poll.error;
    }
}

bool observation_terminal(const std::optional<OrderObservation>& observation) {
    return observation.has_value() &&
           (observation->state == OrderLifecycleState::Filled || observation->state == OrderLifecycleState::Cancelled);
}

bool observation_working(const std::optional<OrderObservation>& observation) {
    return observation.has_value() && (observation->state == OrderLifecycleState::Working ||
                                       observation->state == OrderLifecycleState::PartiallyFilled);
}

void poll_until(const OrderLifecycleConfig& config, OrderLifecycleTransport& transport, OrderLifecycleClock& clock,
                std::chrono::milliseconds timeout, bool stop_on_working, LifecycleEvidence& evidence,
                RunJournal& journal) {
    const auto deadline = clock.now() + timeout;
    for (std::uint32_t attempt = 0; attempt < config.max_poll_attempts && clock.now() < deadline; ++attempt) {
        const auto poll = transport.poll(deadline);
        consume_poll(config, poll, evidence, journal);
        if (!poll.ok || poll.deadline_reached || observation_terminal(evidence.observation) ||
            (stop_on_working && observation_working(evidence.observation)) ||
            (stop_on_working && evidence.add_reply.has_value() && !evidence.add_reply->accepted)) {
            break;
        }
    }
}

void reconcile_once(const OrderLifecycleConfig& config, OrderLifecycleTransport& transport, LifecycleEvidence& evidence,
                    RunJournal& journal) {
    const auto reconciliation = transport.reconcile();
    consume_poll(config, reconciliation, evidence, journal);
}

bool reply_id_matches_observation(std::int64_t reply_id, const OrderObservation& observation) {
    return reply_id != 0 && (reply_id == observation.public_order_id || reply_id == observation.private_order_id ||
                             contains_id(observation.public_order_id_aliases, reply_id) ||
                             contains_id(observation.private_order_id_aliases, reply_id));
}

OrderLifecycleResult finish_result(OrderLifecycleResult result, RunJournal& journal, OrderLifecycleState state,
                                   bool safe_terminal, bool orphan, std::string message,
                                   const LifecycleEvidence& evidence) {
    result.state = state;
    result.safe_terminal = safe_terminal;
    result.orphan_incident_written = orphan;
    result.message = std::move(message);
    result.observation = evidence.observation;
    result.add_reply = evidence.add_reply;
    result.cancel_reply = evidence.cancel_reply;
    result.ok = safe_terminal && (state == OrderLifecycleState::Filled || state == OrderLifecycleState::Cancelled);
    std::string journal_error;
    const bool journal_finished = safe_terminal && !orphan;
    if (!journal.finish(state, journal_finished, orphan, journal_error)) {
        result.ok = false;
        result.safe_terminal = false;
        result.message += "; journal persistence failed: " + journal_error;
    }
    result.transitions = journal.states();
    result.journal_path = journal.path();
    return result;
}

} // namespace

std::string_view order_lifecycle_state_name(OrderLifecycleState state) noexcept {
    switch (state) {
    case OrderLifecycleState::DefinitelyNotSent:
        return "definitely_not_sent";
    case OrderLifecycleState::PossiblySent:
        return "possibly_sent";
    case OrderLifecycleState::Posted:
        return "posted";
    case OrderLifecycleState::Rejected:
        return "rejected";
    case OrderLifecycleState::Working:
        return "working";
    case OrderLifecycleState::PartiallyFilled:
        return "partially_filled";
    case OrderLifecycleState::Filled:
        return "filled";
    case OrderLifecycleState::CancelPending:
        return "cancel_pending";
    case OrderLifecycleState::Cancelled:
        return "cancelled";
    case OrderLifecycleState::UnresolvedOrphanIncident:
        return "unresolved_orphan_incident";
    }
    return "unresolved_orphan_incident";
}

std::optional<OrderObservation> observe_order(std::int32_t ext_id, std::string_view client_code, Plaza2TradeSide side,
                                              std::int64_t expected_original_quantity,
                                              std::span<const private_state::OwnOrderSnapshot> orders,
                                              std::span<const private_state::OwnTradeSnapshot> trades) {
    std::vector<const private_state::OwnOrderSnapshot*> matches;
    for (const auto& order : orders) {
        if (order.ext_id == ext_id && order.client_code == client_code) {
            matches.push_back(&order);
        }
    }
    if (matches.empty()) {
        return std::nullopt;
    }

    const auto* latest = *std::max_element(matches.begin(), matches.end(), [](const auto* lhs, const auto* rhs) {
        if (lhs->moment != rhs->moment) {
            return lhs->moment < rhs->moment;
        }
        return lhs->moment_ns < rhs->moment_ns;
    });

    OrderObservation observation;
    observation.ext_id = ext_id;
    observation.client_code = std::string(client_code);
    observation.original_quantity = expected_original_quantity;
    observation.remaining_quantity = std::max(latest->public_amount_rest, latest->private_amount_rest);

    for (const auto* order : matches) {
        if (observation.public_order_id == 0) {
            observation.public_order_id = order->public_order_id;
        } else if (order->public_order_id != 0 && observation.public_order_id != order->public_order_id) {
            observation.identity_conflict = true;
        }
        if (observation.private_order_id == 0) {
            observation.private_order_id = order->private_order_id;
        } else if (order->private_order_id != 0 && observation.private_order_id != order->private_order_id) {
            observation.identity_conflict = true;
        }
        append_aliases(observation.public_order_id_aliases, order->public_order_id_aliases);
        append_aliases(observation.private_order_id_aliases, order->private_order_id_aliases);
        if (order->public_order_id != 0) {
            append_aliases(observation.public_order_id_aliases,
                           std::span<const std::int64_t>(&order->public_order_id, 1));
        }
        if (order->private_order_id != 0) {
            append_aliases(observation.private_order_id_aliases,
                           std::span<const std::int64_t>(&order->private_order_id, 1));
        }
        observation.identity_conflict = observation.identity_conflict || order->identity_conflict;
        observation.from_trade_replication = observation.from_trade_replication || order->from_trade_repl;
        observation.from_user_orderbook = observation.from_user_orderbook || order->from_user_book;
        observation.from_current_day_snapshot = observation.from_current_day_snapshot || order->from_current_day;
        observation.trade_repl_commit_sequence =
            std::max(observation.trade_repl_commit_sequence, order->trade_repl_commit_sequence);
        observation.user_orderbook_commit_sequence =
            std::max(observation.user_orderbook_commit_sequence, order->user_orderbook_commit_sequence);
        observation.original_quantity =
            std::max({observation.original_quantity, order->public_amount, order->private_amount});
    }

    std::set<std::int64_t> matched_deals;
    for (const auto& trade : trades) {
        const auto public_id = side == Plaza2TradeSide::Buy ? trade.public_order_id_buy : trade.public_order_id_sell;
        const auto private_id = side == Plaza2TradeSide::Buy ? trade.private_order_id_buy : trade.private_order_id_sell;
        const auto trade_ext_id = side == Plaza2TradeSide::Buy ? trade.ext_id_buy : trade.ext_id_sell;
        const bool public_match = public_id != 0 && (public_id == observation.public_order_id ||
                                                     contains_id(observation.public_order_id_aliases, public_id));
        const bool private_match = private_id != 0 && (private_id == observation.private_order_id ||
                                                       contains_id(observation.private_order_id_aliases, private_id));
        const bool ext_match = trade_ext_id != 0 && trade_ext_id == ext_id;
        if ((!public_match && !private_match && !ext_match) || !matched_deals.insert(trade.id_deal).second) {
            continue;
        }
        observation.matched_own_trades.push_back({
            .deal_id = trade.id_deal,
            .quantity = trade.amount,
            .price = trade.price,
            .matched_by_public_order_id = public_match,
            .matched_by_private_order_id = private_match,
            .matched_by_ext_id = ext_match,
        });
        observation.executed_quantity += trade.amount;
    }
    observation.from_own_trades = !observation.matched_own_trades.empty();
    observation.original_quantity =
        std::max(observation.original_quantity, observation.executed_quantity + observation.remaining_quantity);

    const bool cancel_action = latest->public_action == 0 && latest->private_action == 0;
    const bool fill_action = latest->public_action == 2 || latest->private_action == 2;
    if (observation.original_quantity > 0 && observation.executed_quantity >= observation.original_quantity) {
        observation.remaining_quantity = 0;
        observation.state = OrderLifecycleState::Filled;
    } else if (cancel_action) {
        observation.state = OrderLifecycleState::Cancelled;
    } else if (observation.remaining_quantity == 0 && (fill_action || observation.executed_quantity > 0)) {
        observation.state = OrderLifecycleState::Filled;
    } else if (observation.executed_quantity > 0) {
        observation.state = OrderLifecycleState::PartiallyFilled;
    } else {
        observation.state = OrderLifecycleState::Working;
    }
    return observation;
}

std::chrono::steady_clock::time_point SystemOrderLifecycleClock::now() const noexcept {
    return std::chrono::steady_clock::now();
}

std::string_view pre_send_failure_name(PreSendFailure failure) noexcept {
    switch (failure) {
    case PreSendFailure::None:
        return "none";
    case PreSendFailure::ConflictingMode:
        return "conflicting_mode";
    case PreSendFailure::DryRunArmed:
        return "dry_run_armed";
    case PreSendFailure::DisabledProfile:
        return "disabled_profile";
    case PreSendFailure::NonTestProfile:
        return "non_test_profile";
    case PreSendFailure::InvalidQuantity:
        return "invalid_quantity";
    case PreSendFailure::InvalidOrderType:
        return "invalid_order_type";
    case PreSendFailure::InstrumentMissing:
        return "instrument_missing";
    case PreSendFailure::SessionNotTradable:
        return "session_not_tradable";
    case PreSendFailure::InvalidPrice:
        return "invalid_price";
    case PreSendFailure::PriceNotTickAligned:
        return "price_not_tick_aligned";
    case PreSendFailure::Aggr20NotFreshTwoSided:
        return "aggr20_not_fresh_two_sided";
    case PreSendFailure::MarketablePrice:
        return "marketable_price";
    case PreSendFailure::NotionalCeilingExceeded:
        return "notional_ceiling_exceeded";
    case PreSendFailure::DistanceCeilingExceeded:
        return "distance_ceiling_exceeded";
    case PreSendFailure::LimitsSnapshotMissing:
        return "limits_snapshot_missing";
    case PreSendFailure::InvalidIdentifier:
        return "invalid_identifier";
    case PreSendFailure::DuplicateIdentifier:
        return "duplicate_identifier";
    case PreSendFailure::PlanHashMismatch:
        return "plan_hash_mismatch";
    case PreSendFailure::CommandValidationFailed:
        return "command_validation_failed";
    case PreSendFailure::JournalFailure:
        return "journal_failure";
    }
    return "journal_failure";
}

PreSendPlan build_pre_send_plan(const OrderLifecycleConfig& config) {
    if (config.dry_run == config.send_test_order) {
        return fail_plan(PreSendFailure::ConflictingMode, "choose exactly one of dry-run or --send-test-order");
    }
    if (config.dry_run && config.any_arm_flag) {
        return fail_plan(PreSendFailure::DryRunArmed, "dry-run rejects all network/order arm flags");
    }
    if (!config.profile_enabled) {
        return fail_plan(PreSendFailure::DisabledProfile, "native validation rejected disabled profile");
    }
    if (config.environment != cgate::Plaza2Environment::Test) {
        return fail_plan(PreSendFailure::NonTestProfile, "native validation accepts TEST profiles only");
    }
    if (config.quantity != 1) {
        return fail_plan(PreSendFailure::InvalidQuantity,
                         "first smoke policy requires quantity exactly one independent of max_quantity");
    }
    if (config.order_type != Plaza2TradeOrderType::Limit) {
        return fail_plan(PreSendFailure::InvalidOrderType, "first smoke policy accepts limit orders only");
    }
    if (!config.smoke.instrument_exists || config.isin_id <= 0) {
        return fail_plan(PreSendFailure::InstrumentMissing, "instrument is absent from committed refdata");
    }
    if (!config.smoke.tradable_session) {
        return fail_plan(PreSendFailure::SessionNotTradable, "session is not tradable");
    }

    const auto price = parse_nonnegative_decimal(config.price);
    const auto tick = parse_nonnegative_decimal(config.smoke.tick_size);
    const auto bid = parse_nonnegative_decimal(config.smoke.top_bid);
    const auto ask = parse_nonnegative_decimal(config.smoke.top_ask);
    const auto max_notional = parse_nonnegative_decimal(config.limits.max_notional);
    if (!price.has_value() || !tick.has_value() || tick->scaled <= 0 || !bid.has_value() || !ask.has_value() ||
        !max_notional.has_value() || max_notional->scaled <= 0 || bid->scaled >= ask->scaled) {
        return fail_plan(PreSendFailure::InvalidPrice, "price, tick, AGGR20 sides, or notional ceiling is invalid");
    }
    if (price->scaled % tick->scaled != 0) {
        return fail_plan(PreSendFailure::PriceNotTickAligned, "limit price is not aligned to the instrument tick");
    }
    if (!config.smoke.aggr20_two_sided || config.smoke.max_aggr20_age_ms == 0 ||
        config.smoke.aggr20_age_ms > config.smoke.max_aggr20_age_ms) {
        return fail_plan(PreSendFailure::Aggr20NotFreshTwoSided, "AGGR20 must be fresh and two-sided");
    }
    const bool marketable =
        config.side == Plaza2TradeSide::Buy ? price->scaled >= ask->scaled : price->scaled <= bid->scaled;
    if (marketable) {
        return fail_plan(PreSendFailure::MarketablePrice, "limit price must be passive and non-marketable");
    }
    if (price->scaled > max_notional->scaled) {
        return fail_plan(PreSendFailure::NotionalCeilingExceeded, "independent notional ceiling exceeded");
    }
    const auto distance = config.side == Plaza2TradeSide::Buy ? std::max<std::int64_t>(0, bid->scaled - price->scaled)
                                                              : std::max<std::int64_t>(0, price->scaled - ask->scaled);
    const auto distance_ticks = static_cast<std::uint64_t>(distance / tick->scaled);
    if (distance % tick->scaled != 0 || distance_ticks > config.limits.max_distance_ticks) {
        return fail_plan(PreSendFailure::DistanceCeilingExceeded, "independent distance ceiling exceeded");
    }
    if (!config.smoke.limits_snapshot_applicable) {
        return fail_plan(PreSendFailure::LimitsSnapshotMissing, "applicable committed limits snapshot is required");
    }
    if (!valid_run_token(config.run_id) || config.profile_id.empty() || !valid_sha256(config.profile_fingerprint) ||
        config.ext_id <= 0 || config.add_user_id == 0 || config.cancel_user_id == 0 ||
        config.add_user_id == config.cancel_user_id || config.journal_root.empty()) {
        return fail_plan(PreSendFailure::InvalidIdentifier,
                         "run, profile fingerprint, ext_id, user IDs, and journal root must be explicit and unique");
    }
    if (has_unfinished_identifier(config)) {
        return fail_plan(PreSendFailure::DuplicateIdentifier, "ext_id or user_id belongs to an unfinished run journal");
    }

    AddOrderRequest request;
    request.broker_code = config.broker_code;
    request.isin_id = config.isin_id;
    request.client_code = config.client_code;
    request.dir = config.side;
    request.type = config.order_type;
    request.amount = config.quantity;
    request.price = config.price;
    request.comment = config.comment;
    request.ext_id = config.ext_id;
    request.is_check_limit = 1;
    Plaza2TradeCodec codec;
    auto command = codec.encode(Plaza2TradeCommandRequest{request});
    if (!command.validation.ok()) {
        return fail_plan(PreSendFailure::CommandValidationFailed, command.validation.message);
    }
    const auto payload_hash = cgate::plaza2_sha256_hex(command.payload);

    std::ostringstream json;
    json << "{\n";
    json << "  \"schema\": \"moex.plaza2.pre_send_plan.v1\",\n";
    json << "  \"profile_id\": \"" << json_escape(config.profile_id) << "\",\n";
    json << "  \"profile_fingerprint\": \"" << config.profile_fingerprint << "\",\n";
    json << "  \"environment\": \"test\",\n";
    json << "  \"profile_enabled\": true,\n";
    json << "  \"command\": \"AddOrder\",\n";
    json << "  \"message_id\": " << command.msgid << ",\n";
    json << "  \"payload_sha256\": \"" << payload_hash << "\",\n";
    json << "  \"user_id\": " << config.add_user_id << ",\n";
    json << "  \"cancel_user_id\": " << config.cancel_user_id << ",\n";
    json << "  \"ext_id\": " << config.ext_id << ",\n";
    json << "  \"isin_id\": " << config.isin_id << ",\n";
    json << "  \"side\": \"" << (config.side == Plaza2TradeSide::Buy ? "buy" : "sell") << "\",\n";
    json << "  \"order_type\": \"limit\",\n";
    json << "  \"price\": \"" << json_escape(config.price) << "\",\n";
    json << "  \"quantity\": 1,\n";
    json << "  \"checks\": {\n";
    json << "    \"aggr20_fresh_two_sided\": true,\n";
    json << "    \"applicable_limits_snapshot\": true,\n";
    json << "    \"distance_ceiling\": true,\n";
    json << "    \"instrument_in_refdata\": true,\n";
    json << "    \"notional_ceiling\": true,\n";
    json << "    \"passive_non_marketable\": true,\n";
    json << "    \"quantity_one\": true,\n";
    json << "    \"tick_aligned\": true,\n";
    json << "    \"tradable_session\": true,\n";
    json << "    \"unique_identifiers\": true\n";
    json << "  }\n";
    json << "}\n";

    PreSendPlan plan{
        .ok = true,
        .failure = PreSendFailure::None,
        .message = "pre-send plan validated",
        .canonical_json = json.str(),
        .add_command = std::move(command),
    };
    plan.sha256 = cgate::plaza2_sha256_hex(plan.canonical_json);
    if (config.send_test_order &&
        (!valid_sha256(config.authorized_plan_sha256) || config.authorized_plan_sha256 != plan.sha256)) {
        return fail_plan(PreSendFailure::PlanHashMismatch,
                         "--send-test-order requires the exact SHA-256 of canonical pre_send_plan.json");
    }
    return plan;
}

bool write_pre_send_plan(const std::filesystem::path& output_directory, const PreSendPlan& plan, std::string& error) {
    if (!plan.ok || plan.canonical_json.empty()) {
        error = "cannot write an invalid pre-send plan";
        return false;
    }
    return atomic_write(output_directory / "pre_send_plan.json", plan.canonical_json, error);
}

OrderLifecycleController::OrderLifecycleController(OrderLifecycleConfig config, OrderLifecycleTransport& transport,
                                                   OrderLifecycleClock& clock)
    : config_(std::move(config)), transport_(transport), clock_(clock) {}

OrderLifecycleResult OrderLifecycleController::run() {
    OrderLifecycleResult result;
    const auto plan = build_pre_send_plan(config_);
    if (!plan.ok) {
        result.safe_terminal = true;
        result.state = OrderLifecycleState::DefinitelyNotSent;
        result.message = std::string(pre_send_failure_name(plan.failure)) + ": " + plan.message;
        result.transitions = {OrderLifecycleState::DefinitelyNotSent};
        return result;
    }
    if (config_.dry_run) {
        std::string write_error;
        const auto output_directory = config_.journal_root / "plans" / config_.run_id;
        if (!write_pre_send_plan(output_directory, plan, write_error)) {
            result.safe_terminal = true;
            result.state = OrderLifecycleState::DefinitelyNotSent;
            result.message = "failed to write canonical pre-send plan: " + write_error;
            result.transitions = {OrderLifecycleState::DefinitelyNotSent};
            return result;
        }
        result.ok = true;
        result.safe_terminal = true;
        result.state = OrderLifecycleState::DefinitelyNotSent;
        result.message = "dry-run wrote canonical pre_send_plan.json without opening CGate";
        result.transitions = {OrderLifecycleState::DefinitelyNotSent};
        result.journal_path = output_directory / "pre_send_plan.json";
        return result;
    }

    RunJournal journal;
    std::string journal_error;
    const auto payload_hash = cgate::plaza2_sha256_hex(plan.add_command.payload);
    if (!journal.begin(config_, payload_hash, journal_error)) {
        result.safe_terminal = true;
        result.state = OrderLifecycleState::DefinitelyNotSent;
        result.message = "journal_failure: " + journal_error;
        result.transitions = {OrderLifecycleState::DefinitelyNotSent};
        return result;
    }
    result.journal_path = journal.path();

    LifecycleEvidence evidence;
    result.add_submission = transport_.post(plan.add_command, config_.add_user_id);
    journal.record_add_submission(result.add_submission);
    journal.record_state(result.add_submission.certainty == cgate::Plaza2SubmissionCertainty::Posted
                             ? OrderLifecycleState::Posted
                         : result.add_submission.certainty == cgate::Plaza2SubmissionCertainty::PossiblySent
                             ? OrderLifecycleState::PossiblySent
                             : OrderLifecycleState::DefinitelyNotSent);
    if (result.add_submission.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent) {
        return finish_result(std::move(result), journal, OrderLifecycleState::DefinitelyNotSent, true, false,
                             "AddOrder was definitely not sent", evidence);
    }

    poll_until(config_, transport_, clock_, config_.add_observation_timeout, true, evidence, journal);
    if (evidence.poll_failed || (!evidence.observation.has_value() && !evidence.add_reply.has_value())) {
        reconcile_once(config_, transport_, evidence, journal);
    }

    if (evidence.observation.has_value() && evidence.observation->identity_conflict) {
        return finish_result(std::move(result), journal, OrderLifecycleState::UnresolvedOrphanIncident, false, true,
                             "replication exposed a public/private order identity conflict", evidence);
    }
    if (observation_terminal(evidence.observation)) {
        return finish_result(std::move(result), journal, evidence.observation->state, true, false,
                             evidence.observation->state == OrderLifecycleState::Filled
                                 ? "order filled before cancellation"
                                 : "order reached a factual cancelled state",
                             evidence);
    }
    if (evidence.add_reply.has_value() && !evidence.add_reply->accepted && !evidence.observation.has_value()) {
        return finish_result(std::move(result), journal, OrderLifecycleState::Rejected, true, false,
                             "AddOrder reply rejected the command", evidence);
    }
    if (!observation_working(evidence.observation)) {
        return finish_result(
            std::move(result), journal, OrderLifecycleState::UnresolvedOrphanIncident, false, true,
            evidence.poll_failed
                ? "polling failed after a possible AddOrder submission and reconciliation was inconclusive"
                : "possible AddOrder submission could not be resolved by reply or replication",
            evidence);
    }
    if (!evidence.add_reply.has_value() || !evidence.add_reply->accepted || !evidence.add_reply->order_id.has_value()) {
        return finish_result(
            std::move(result), journal, OrderLifecycleState::UnresolvedOrphanIncident, false, true,
            "working order lacks an accepted user_id-correlated AddOrder reply ID required by DelOrder", evidence);
    }
    if (!reply_id_matches_observation(*evidence.add_reply->order_id, *evidence.observation)) {
        return finish_result(std::move(result), journal, OrderLifecycleState::UnresolvedOrphanIncident, false, true,
                             "AddOrder reply ID does not match any replicated public/private identifier alias",
                             evidence);
    }

    DelOrderRequest cancel;
    cancel.broker_code = config_.broker_code;
    // Locked vendor semantics: DelOrder consumes the AddOrder reply order_id.
    cancel.order_id = *evidence.add_reply->order_id;
    cancel.client_code = config_.client_code;
    cancel.isin_id = config_.isin_id;
    Plaza2TradeCodec codec;
    const auto cancel_command = codec.encode(Plaza2TradeCommandRequest{cancel});
    if (!cancel_command.validation.ok()) {
        return finish_result(std::move(result), journal, OrderLifecycleState::UnresolvedOrphanIncident, false, true,
                             "DelOrder encoding failed after AddOrder became working", evidence);
    }

    result.cancel_submission = transport_.post(cancel_command, config_.cancel_user_id);
    journal.record_cancel_submission(result.cancel_submission);
    if (result.cancel_submission.certainty != cgate::Plaza2SubmissionCertainty::DefinitelyNotSent) {
        journal.record_state(OrderLifecycleState::CancelPending);
    }
    poll_until(config_, transport_, clock_, config_.cancel_observation_timeout, false, evidence, journal);
    if (evidence.poll_failed || !observation_terminal(evidence.observation)) {
        reconcile_once(config_, transport_, evidence, journal);
    }
    if (evidence.observation.has_value() && evidence.observation->identity_conflict) {
        return finish_result(std::move(result), journal, OrderLifecycleState::UnresolvedOrphanIncident, false, true,
                             "identity conflict appeared while cancellation was pending", evidence);
    }
    if (observation_terminal(evidence.observation)) {
        return finish_result(std::move(result), journal, evidence.observation->state, true, false,
                             evidence.observation->state == OrderLifecycleState::Filled
                                 ? "order filled while cancellation was pending"
                                 : "remaining order quantity was factually cancelled",
                             evidence);
    }
    const auto unresolved_message =
        result.cancel_submission.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent
            ? "DelOrder was definitely not sent and the working order remains unresolved"
            : "DelOrder outcome remains unresolved after polling and reconciliation";
    return finish_result(std::move(result), journal, OrderLifecycleState::UnresolvedOrphanIncident, false, true,
                         unresolved_message, evidence);
}

} // namespace moex::plaza2_trade
