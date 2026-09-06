#include "moex/plaza2_trade/plaza2_order_lifecycle.hpp"

#include <algorithm>
#include <array>
#include <charconv>
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
        error = "failed to publish journal file: " + filesystem_error.message();
        return false;
    }
    return true;
}

std::optional<std::string> journal_string_field(std::string_view text, std::string_view key) {
    const auto marker = std::string("\"") + std::string(key) + "\": \"";
    const auto begin = text.find(marker);
    if (begin == std::string_view::npos) {
        return std::nullopt;
    }
    const auto value_begin = begin + marker.size();
    const auto value_end = text.find('"', value_begin);
    if (value_end == std::string_view::npos) {
        return std::nullopt;
    }
    return std::string(text.substr(value_begin, value_end - value_begin));
}

std::optional<std::int64_t> journal_integer_field(std::string_view text, std::string_view key) {
    const auto marker = std::string("\"") + std::string(key) + "\": ";
    const auto begin = text.find(marker);
    if (begin == std::string_view::npos) {
        return std::nullopt;
    }
    const auto value_begin = begin + marker.size();
    const auto value_end = text.find_first_of(",\n}", value_begin);
    const auto value = text.substr(value_begin, value_end == std::string_view::npos ? text.size() - value_begin
                                                                                    : value_end - value_begin);
    std::int64_t parsed = 0;
    const auto [ptr, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<bool> journal_boolean_field(std::string_view text, std::string_view key) {
    const auto marker = std::string("\"") + std::string(key) + "\": ";
    const auto begin = text.find(marker);
    if (begin == std::string_view::npos) {
        return std::nullopt;
    }
    const auto value_begin = begin + marker.size();
    const auto has_boundary = [&](std::size_t length) {
        const auto value_end = value_begin + length;
        return value_end >= text.size() || text[value_end] == ',' || text[value_end] == '\n' || text[value_end] == '}';
    };
    if (text.substr(value_begin, 4) == "true" && has_boundary(4)) {
        return true;
    }
    if (text.substr(value_begin, 5) == "false" && has_boundary(5)) {
        return false;
    }
    return std::nullopt;
}

std::optional<OrderLifecycleState> journal_state_field(std::string_view value) {
    constexpr std::array states = {
        OrderLifecycleState::DefinitelyNotSent,
        OrderLifecycleState::PossiblySent,
        OrderLifecycleState::Posted,
        OrderLifecycleState::Rejected,
        OrderLifecycleState::Working,
        OrderLifecycleState::PartiallyFilled,
        OrderLifecycleState::Filled,
        OrderLifecycleState::CancelPending,
        OrderLifecycleState::Cancelled,
        OrderLifecycleState::UnresolvedOrphanIncident,
        OrderLifecycleState::Idle,
        OrderLifecycleState::Authorized,
        OrderLifecycleState::AddPending,
    };
    for (const auto state : states) {
        if (order_lifecycle_state_name(state) == value) {
            return state;
        }
    }
    return std::nullopt;
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
           std::filesystem::exists(user_lock_path(config, config.cancel_user_id)) ||
           std::filesystem::exists(user_lock_path(config, config.recovery_user_id));
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
    bool begin(const OrderLifecycleConfig& config, std::string payload_hash, std::string recovery_payload_hash,
               std::string& error) {
        config_ = &config;
        payload_hash_ = std::move(payload_hash);
        recovery_payload_hash_ = std::move(recovery_payload_hash);
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
            user_lock_path(config, config.recovery_user_id),
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
        if (!persist(error)) {
            release_locks();
            return false;
        }
        return true;
    }

    bool record_add_submission(const cgate::Plaza2PublisherMessageResult& submission) {
        add_submission_ = submission;
        return persist_record();
    }

    bool record_cancel_submission(const cgate::Plaza2PublisherMessageResult& submission) {
        cancel_submission_ = submission;
        return persist_record();
    }

    bool record_recovery_submission(const cgate::Plaza2PublisherMessageResult& submission) {
        recovery_submission_ = submission;
        return persist_record();
    }

    bool record_add_reply(const OrderReplyObservation& reply) {
        add_reply_timeout_observed_ = add_reply_timeout_observed_ || reply.timed_out;
        if (!reply.timed_out || !add_reply_.has_value()) {
            add_reply_ = reply;
        }
        return persist_record();
    }

    bool record_cancel_reply(const OrderReplyObservation& reply) {
        cancel_reply_timeout_observed_ = cancel_reply_timeout_observed_ || reply.timed_out;
        if (!reply.timed_out || !cancel_reply_.has_value()) {
            cancel_reply_ = reply;
        }
        return persist_record();
    }

    bool record_recovery_reply(const OrderReplyObservation& reply) {
        recovery_reply_timeout_observed_ = recovery_reply_timeout_observed_ || reply.timed_out;
        if (!reply.timed_out || !recovery_reply_.has_value()) {
            recovery_reply_ = reply;
        }
        return persist_record();
    }

    bool record_observation(const OrderObservation& observation) {
        observation_ = observation;
        append_state(observation.state);
        return persist_record();
    }

    bool record_state(OrderLifecycleState state) {
        append_state(state);
        return persist_record();
    }

    bool finish(OrderLifecycleState state, bool market_safe_terminal, bool orphan, bool evidence_consistent,
                std::string& error) {
        append_state(state);
        final_state_ = state;
        market_safe_terminal_ = market_safe_terminal && evidence_consistent;
        evidence_consistent_ = evidence_consistent;
        orphan_incident_ = orphan;
        if (!persist(error)) {
            mark_degraded(error);
            return false;
        }
        if (market_safe_terminal_ && !degraded_) {
            release_locks();
        }
        return true;
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

    const std::vector<OrderLifecycleState>& states() const noexcept {
        return states_;
    }

    bool degraded() const noexcept {
        return degraded_;
    }

    const std::string& last_error() const noexcept {
        return last_error_;
    }

  private:
    std::string json() const {
        std::ostringstream out;
        out << "{\n";
        out << "  \"schema\": \"moex.plaza2.order_run_journal.v2\",\n";
        out << "  \"run_id\": \"" << json_escape(config_->run_id) << "\",\n";
        out << "  \"profile_id\": \"" << json_escape(config_->profile_id) << "\",\n";
        out << "  \"profile_fingerprint\": \"" << json_escape(config_->profile_fingerprint) << "\",\n";
        out << "  \"add_command\": \"AddOrder\",\n";
        out << "  \"cancel_command\": \"DelOrder\",\n";
        out << "  \"recovery_command\": \"DelUserOrders\",\n";
        out << "  \"ext_id\": " << config_->ext_id << ",\n";
        out << "  \"add_user_id\": " << config_->add_user_id << ",\n";
        out << "  \"cancel_user_id\": " << config_->cancel_user_id << ",\n";
        out << "  \"recovery_user_id\": " << config_->recovery_user_id << ",\n";
        out << "  \"payload_sha256\": \"" << payload_hash_ << "\",\n";
        out << "  \"recovery_payload_sha256\": \"" << recovery_payload_hash_ << "\",\n";
        out << "  \"add_submission_certainty\": \"" << submission_certainty_name(add_submission_.certainty) << "\",\n";
        out << "  \"add_post_invoked\": " << (add_submission_.post_invoked ? "true" : "false") << ",\n";
        out << "  \"add_reply_observed\": " << (add_reply_.has_value() ? "true" : "false") << ",\n";
        out << "  \"add_reply_timeout_observed\": " << (add_reply_timeout_observed_ ? "true" : "false") << ",\n";
        out << "  \"add_reply_accepted\": " << (add_reply_.has_value() && add_reply_->accepted ? "true" : "false")
            << ",\n";
        out << "  \"add_reply_order_id\": "
            << (add_reply_.has_value() && add_reply_->order_id.has_value() ? *add_reply_->order_id : 0) << ",\n";
        out << "  \"cancel_submission_certainty\": \"" << submission_certainty_name(cancel_submission_.certainty)
            << "\",\n";
        out << "  \"cancel_post_invoked\": " << (cancel_submission_.post_invoked ? "true" : "false") << ",\n";
        out << "  \"cancel_reply_observed\": " << (cancel_reply_.has_value() ? "true" : "false") << ",\n";
        out << "  \"cancel_reply_timeout_observed\": " << (cancel_reply_timeout_observed_ ? "true" : "false") << ",\n";
        out << "  \"recovery_submission_certainty\": \"" << submission_certainty_name(recovery_submission_.certainty)
            << "\",\n";
        out << "  \"recovery_post_invoked\": " << (recovery_submission_.post_invoked ? "true" : "false") << ",\n";
        out << "  \"recovery_reply_observed\": " << (recovery_reply_.has_value() ? "true" : "false") << ",\n";
        out << "  \"recovery_reply_timeout_observed\": " << (recovery_reply_timeout_observed_ ? "true" : "false")
            << ",\n";
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
        out << "  \"market_safe_terminal\": " << (market_safe_terminal_ ? "true" : "false") << ",\n";
        out << "  \"evidence_consistent\": " << (evidence_consistent_ ? "true" : "false") << ",\n";
        out << "  \"journal_degraded\": " << (degraded_ ? "true" : "false") << ",\n";
        out << "  \"finished\": " << (market_safe_terminal_ && evidence_consistent_ && !degraded_ ? "true" : "false")
            << "\n";
        out << "}\n";
        return out.str();
    }

    bool persist(std::string& error) const {
        return atomic_write(path_, json(), error);
    }

    bool persist_record() {
        std::string error;
        if (!persist(error)) {
            mark_degraded(error);
            return false;
        }
        return true;
    }

    void mark_degraded(std::string error) {
        degraded_ = true;
        if (last_error_.empty()) {
            last_error_ = std::move(error);
        }
    }

    void append_state(OrderLifecycleState state) {
        if (states_.empty() || states_.back() != state) {
            states_.push_back(state);
        }
    }

    void release_locks() {
        std::error_code filesystem_error;
        for (const auto& lock : active_locks_) {
            std::filesystem::remove(lock, filesystem_error);
        }
        active_locks_.clear();
    }

    const OrderLifecycleConfig* config_{nullptr};
    std::filesystem::path run_directory_;
    std::filesystem::path path_;
    std::vector<std::filesystem::path> active_locks_;
    std::string payload_hash_;
    std::string recovery_payload_hash_;
    cgate::Plaza2PublisherMessageResult add_submission_;
    cgate::Plaza2PublisherMessageResult cancel_submission_;
    cgate::Plaza2PublisherMessageResult recovery_submission_;
    std::optional<OrderReplyObservation> add_reply_;
    std::optional<OrderReplyObservation> cancel_reply_;
    std::optional<OrderReplyObservation> recovery_reply_;
    std::optional<OrderObservation> observation_;
    std::vector<OrderLifecycleState> states_;
    OrderLifecycleState final_state_{OrderLifecycleState::PossiblySent};
    bool market_safe_terminal_{false};
    bool evidence_consistent_{true};
    bool orphan_incident_{false};
    bool degraded_{false};
    bool add_reply_timeout_observed_{false};
    bool cancel_reply_timeout_observed_{false};
    bool recovery_reply_timeout_observed_{false};
    std::string last_error_;
};

struct LifecycleEvidence {
    std::optional<OrderReplyObservation> add_reply;
    std::optional<OrderReplyObservation> cancel_reply;
    std::optional<OrderReplyObservation> recovery_reply;
    std::optional<OrderObservation> observation;
    bool add_reply_timeout_observed{false};
    bool cancel_reply_timeout_observed{false};
    bool recovery_reply_timeout_observed{false};
    bool consistent{true};
    bool poll_failed{false};
    std::string poll_error;
};

void merge_reply(std::optional<OrderReplyObservation>& target, bool& timeout_observed,
                 const OrderReplyObservation& reply, bool& evidence_consistent) {
    if (reply.timed_out) {
        timeout_observed = true;
        if (!target.has_value()) {
            target = reply;
        }
        return;
    }
    if (target.has_value() && !target->timed_out &&
        (target->accepted != reply.accepted || target->code != reply.code || target->order_id != reply.order_id)) {
        evidence_consistent = false;
    }
    target = reply;
}

bool reply_id_matches_observation(std::int64_t reply_id, const OrderObservation& observation);

// In the MOEX TEST contour USERORDERBOOK is a pre-send census only.  It is
// not lifecycle evidence and must never be allowed to replace or conflict
// with the TRADE order surface.  Source-neutral observations remain accepted
// for the transport-neutral scenario tests, but an observation carrying both
// surfaces is rejected rather than treated as a reconciliation.
bool lifecycle_observation_allowed(const OrderObservation& observation) {
    return !observation.from_user_orderbook && !observation.from_current_day_snapshot;
}

void consume_poll(const OrderLifecycleConfig& config, const OrderLifecyclePollResult& poll, LifecycleEvidence& evidence,
                  RunJournal& journal) {
    for (const auto& reply : poll.replies) {
        if (reply.command_kind == Plaza2TradeCommandKind::AddOrder && reply.user_id == config.add_user_id) {
            merge_reply(evidence.add_reply, evidence.add_reply_timeout_observed, reply, evidence.consistent);
            journal.record_add_reply(reply);
        } else if (reply.command_kind == Plaza2TradeCommandKind::DelOrder && reply.user_id == config.cancel_user_id) {
            merge_reply(evidence.cancel_reply, evidence.cancel_reply_timeout_observed, reply, evidence.consistent);
            journal.record_cancel_reply(reply);
        } else if (reply.command_kind == Plaza2TradeCommandKind::DelUserOrders &&
                   reply.user_id == config.recovery_user_id) {
            merge_reply(evidence.recovery_reply, evidence.recovery_reply_timeout_observed, reply, evidence.consistent);
            journal.record_recovery_reply(reply);
        }
    }
    for (const auto& observation : poll.observations) {
        if (!lifecycle_observation_allowed(observation)) {
            continue;
        }
        if (observation.ext_id == config.ext_id && observation.client_code == config.client_code) {
            if (evidence.observation.has_value()) {
                const auto public_conflict = evidence.observation->public_order_id != 0 &&
                                             observation.public_order_id != 0 &&
                                             evidence.observation->public_order_id != observation.public_order_id;
                const auto private_conflict = evidence.observation->private_order_id != 0 &&
                                              observation.private_order_id != 0 &&
                                              evidence.observation->private_order_id != observation.private_order_id;
                evidence.consistent = evidence.consistent && !public_conflict && !private_conflict;
            }
            evidence.observation = observation;
            evidence.consistent = evidence.consistent && !observation.identity_conflict;
            journal.record_observation(observation);
        }
    }
    if (evidence.add_reply.has_value() && !evidence.add_reply->timed_out && evidence.observation.has_value()) {
        if (!evidence.add_reply->accepted ||
            (evidence.add_reply->order_id.has_value() &&
             !reply_id_matches_observation(*evidence.add_reply->order_id, *evidence.observation))) {
            evidence.consistent = false;
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

bool accepted_add_reply_has_id(const LifecycleEvidence& evidence) {
    return evidence.add_reply.has_value() && !evidence.add_reply->timed_out && evidence.add_reply->accepted &&
           evidence.add_reply->order_id.has_value();
}

bool definitive_add_rejection(const LifecycleEvidence& evidence) {
    return evidence.add_reply.has_value() && !evidence.add_reply->timed_out && !evidence.add_reply->accepted &&
           !evidence.observation.has_value();
}

bool definitive_cancel_rejection(const LifecycleEvidence& evidence) {
    return evidence.cancel_reply.has_value() && !evidence.cancel_reply->timed_out && !evidence.cancel_reply->accepted;
}

bool definitive_recovery_rejection(const LifecycleEvidence& evidence) {
    return evidence.recovery_reply.has_value() && !evidence.recovery_reply->timed_out &&
           !evidence.recovery_reply->accepted;
}

bool reply_id_matches_observation(std::int64_t reply_id, const OrderObservation& observation) {
    return reply_id != 0 && (reply_id == observation.public_order_id || reply_id == observation.private_order_id ||
                             contains_id(observation.public_order_id_aliases, reply_id) ||
                             contains_id(observation.private_order_id_aliases, reply_id));
}

bool usable_cancel_precondition(const LifecycleEvidence& evidence) {
    return observation_working(evidence.observation) && accepted_add_reply_has_id(evidence) &&
           reply_id_matches_observation(*evidence.add_reply->order_id, *evidence.observation) && evidence.consistent;
}

bool add_phase_resolved(const LifecycleEvidence& evidence) {
    return observation_terminal(evidence.observation) || definitive_add_rejection(evidence) ||
           usable_cancel_precondition(evidence);
}

void poll_add_until_resolution(const OrderLifecycleConfig& config, OrderLifecycleTransport& transport,
                               OrderLifecycleClock& clock, LifecycleEvidence& evidence, RunJournal& journal) {
    const auto deadline = clock.now() + config.add_observation_timeout;
    for (std::uint32_t attempt = 0; attempt < config.max_poll_attempts && clock.now() < deadline; ++attempt) {
        const auto poll = transport.poll(deadline);
        consume_poll(config, poll, evidence, journal);
        if (!poll.ok || poll.deadline_reached || add_phase_resolved(evidence)) {
            break;
        }
    }
}

void poll_until_terminal(const OrderLifecycleConfig& config, OrderLifecycleTransport& transport,
                         OrderLifecycleClock& clock, std::chrono::milliseconds timeout, LifecycleEvidence& evidence,
                         RunJournal& journal) {
    const auto deadline = clock.now() + timeout;
    for (std::uint32_t attempt = 0; attempt < config.max_poll_attempts && clock.now() < deadline; ++attempt) {
        const auto poll = transport.poll(deadline);
        consume_poll(config, poll, evidence, journal);
        if (!poll.ok || poll.deadline_reached || observation_terminal(evidence.observation)) {
            break;
        }
    }
}

void reconcile_once(const OrderLifecycleConfig& config, OrderLifecycleTransport& transport, LifecycleEvidence& evidence,
                    RunJournal& journal) {
    const auto reconciliation = transport.reconcile();
    consume_poll(config, reconciliation, evidence, journal);
}

OrderLifecycleResult finish_result(OrderLifecycleResult result, RunJournal& journal, OrderLifecycleState state,
                                   bool market_safe_terminal, bool orphan, std::string message,
                                   const LifecycleEvidence& evidence) {
    const bool inconsistent_replication_terminal =
        !evidence.consistent && (state == OrderLifecycleState::Filled || state == OrderLifecycleState::Cancelled);
    if (inconsistent_replication_terminal) {
        state = OrderLifecycleState::UnresolvedOrphanIncident;
        market_safe_terminal = false;
        orphan = true;
        message += "; terminal replication is not releasable while lifecycle evidence is inconsistent";
    }
    result.state = state;
    result.market_safe_terminal = market_safe_terminal;
    result.evidence_consistent = evidence.consistent;
    result.message = std::move(message);
    result.observation = evidence.observation;
    result.add_reply = evidence.add_reply;
    result.cancel_reply = evidence.cancel_reply;
    result.recovery_reply = evidence.recovery_reply;
    std::string journal_error;
    const bool final_persisted =
        journal.finish(state, market_safe_terminal, orphan, evidence.consistent, journal_error);
    result.journal_degraded = journal.degraded();
    result.journal_ok = final_persisted && !result.journal_degraded;
    result.orphan_incident_written = orphan && final_persisted;
    result.ok = market_safe_terminal && result.journal_ok && result.evidence_consistent;
    if (!final_persisted) {
        result.message += "; journal persistence failed: " + journal_error;
    } else if (result.journal_degraded) {
        result.message += "; journal degraded earlier: " + journal.last_error();
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
    case OrderLifecycleState::Idle:
        return "idle";
    case OrderLifecycleState::Authorized:
        return "authorized";
    case OrderLifecycleState::AddPending:
        return "add_pending";
    }
    return "unresolved_orphan_incident";
}

std::optional<OrderObservation> observe_order(std::int32_t ext_id, std::string_view client_code, Plaza2TradeSide side,
                                              std::int64_t expected_original_quantity,
                                              std::span<const private_state::OwnOrderSnapshot> orders,
                                              std::span<const private_state::OwnTradeSnapshot> trades) {
    std::vector<const private_state::OwnOrderSnapshot*> matches;
    for (const auto& order : orders) {
        // Lifecycle correlation is sourced from TRADE orders_log (and its
        // own-deal tables). USERORDERBOOK is intentionally excluded; the
        // TEST venue does not reconcile the two views.
        if (order.ext_id == ext_id && order.client_code == client_code && order.from_trade_repl &&
            !order.from_user_book && !order.from_current_day) {
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
        return fail_plan(PreSendFailure::InvalidQuantity, "first smoke policy requires quantity exactly one");
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
    if (!price.has_value() || !tick.has_value() || tick->scaled <= 0 || !bid.has_value() || !ask.has_value() ||
        bid->scaled >= ask->scaled) {
        return fail_plan(PreSendFailure::InvalidPrice, "price, tick, or AGGR20 sides are invalid");
    }
    if (price->scaled % tick->scaled != 0) {
        return fail_plan(PreSendFailure::PriceNotTickAligned, "limit price is not aligned to the instrument tick");
    }
    if (!config.smoke.aggr20_two_sided || config.policy.max_aggr20_age_ms == 0 ||
        config.smoke.aggr20_age_ms > config.policy.max_aggr20_age_ms) {
        return fail_plan(PreSendFailure::Aggr20NotFreshTwoSided, "AGGR20 must be fresh and two-sided");
    }
    const bool marketable =
        config.side == Plaza2TradeSide::Buy ? price->scaled >= ask->scaled : price->scaled <= bid->scaled;
    if (marketable) {
        return fail_plan(PreSendFailure::MarketablePrice, "limit price must be passive and non-marketable");
    }
    const auto distance = config.side == Plaza2TradeSide::Buy ? std::max<std::int64_t>(0, bid->scaled - price->scaled)
                                                              : std::max<std::int64_t>(0, price->scaled - ask->scaled);
    const auto distance_ticks = static_cast<std::uint64_t>(distance / tick->scaled);
    if (distance % tick->scaled != 0 || distance_ticks > config.policy.max_distance_ticks) {
        return fail_plan(PreSendFailure::DistanceCeilingExceeded, "independent distance ceiling exceeded");
    }
    if (!config.smoke.limits_snapshot_applicable) {
        return fail_plan(PreSendFailure::LimitsSnapshotMissing, "applicable committed limits snapshot is required");
    }
    if (config.smoke.market_data_source.empty() || config.smoke.aggr20_source_sequence == 0 ||
        config.smoke.aggr20_observed_at_utc.empty() || config.smoke.trading_day.empty() ||
        config.smoke.session_id.empty() || config.smoke.session_state.empty() || config.smoke.refdata_source.empty() ||
        config.smoke.refdata_source_sequence == 0 || config.smoke.limits_source.empty() ||
        config.smoke.limits_commit_sequence == 0 || config.policy.version.empty() ||
        !valid_sha256(config.policy.sha256)) {
        return fail_plan(PreSendFailure::LimitsSnapshotMissing,
                         "reviewed market, session, refdata, limits, and policy provenance must be explicit");
    }
    if (!valid_run_token(config.run_id) || config.profile_id.empty() || !valid_sha256(config.profile_fingerprint) ||
        config.ext_id <= 0 || config.add_user_id == 0 || config.cancel_user_id == 0 || config.recovery_user_id == 0 ||
        config.add_user_id == config.cancel_user_id || config.add_user_id == config.recovery_user_id ||
        config.cancel_user_id == config.recovery_user_id || config.base_contract_code.empty() ||
        (config.instrument_mask != 1 && config.instrument_mask != 2 && config.instrument_mask != 4) ||
        config.journal_root.empty()) {
        return fail_plan(PreSendFailure::InvalidIdentifier,
                         "run, profile fingerprint, exact instrument context, ext_id, user IDs, and journal root must "
                         "be explicit and unique");
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

    DelUserOrdersRequest recovery;
    recovery.broker_code = config.broker_code;
    recovery.buy_sell = config.side == Plaza2TradeSide::Buy ? 1 : 2;
    recovery.non_system = 0;
    recovery.code = config.client_code;
    recovery.base_contract_code = config.base_contract_code;
    recovery.ext_id = config.ext_id;
    recovery.isin_id = config.isin_id;
    recovery.instrument_mask = config.instrument_mask;
    auto recovery_command = codec.encode(Plaza2TradeCommandRequest{recovery});
    if (!recovery_command.validation.ok()) {
        return fail_plan(PreSendFailure::CommandValidationFailed,
                         "exact-ext_id recovery: " + recovery_command.validation.message);
    }
    const auto recovery_payload_hash = cgate::plaza2_sha256_hex(recovery_command.payload);

    // Only static, human-authorized intent is hashed. Dynamic market/session
    // observations are written separately and are re-derived by the concrete
    // transport immediately before AddOrder.
    std::ostringstream json;
    json << "{\n";
    json << "  \"schema\": \"moex.plaza2.authorized_order_intent.v1\",\n";
    json << "  \"profile_id\": \"" << json_escape(config.profile_id) << "\",\n";
    json << "  \"profile_fingerprint\": \"" << config.profile_fingerprint << "\",\n";
    json << "  \"environment\": \"test\",\n";
    json << "  \"command\": \"AddOrder\",\n";
    json << "  \"message_id\": " << command.msgid << ",\n";
    json << "  \"payload_sha256\": \"" << payload_hash << "\",\n";
    json << "  \"recovery_message_id\": " << recovery_command.msgid << ",\n";
    json << "  \"recovery_payload_sha256\": \"" << recovery_payload_hash << "\",\n";
    json << "  \"add_user_id\": " << config.add_user_id << ",\n";
    json << "  \"cancel_user_id\": " << config.cancel_user_id << ",\n";
    json << "  \"recovery_user_id\": " << config.recovery_user_id << ",\n";
    json << "  \"ext_id\": " << config.ext_id << ",\n";
    json << "  \"isin_id\": " << config.isin_id << ",\n";
    json << "  \"base_contract_code\": \"" << json_escape(config.base_contract_code) << "\",\n";
    json << "  \"instrument_mask\": " << static_cast<int>(config.instrument_mask) << ",\n";
    json << "  \"side\": \"" << (config.side == Plaza2TradeSide::Buy ? "buy" : "sell") << "\",\n";
    json << "  \"order_type\": \"limit\",\n";
    json << "  \"price\": \"" << json_escape(config.price) << "\",\n";
    json << "  \"comment\": \"" << json_escape(config.comment) << "\",\n";
    json << "  \"quantity\": 1,\n";
    json << "  \"client_code_sha256\": \"" << cgate::plaza2_sha256_hex(config.client_code) << "\",\n";
    json << "  \"broker_code_sha256\": \"" << cgate::plaza2_sha256_hex(config.broker_code) << "\",\n";
    json << "  \"smoke_policy\": {\n";
    json << "    \"version\": \"" << json_escape(config.policy.version) << "\",\n";
    json << "    \"sha256\": \"" << config.policy.sha256 << "\",\n";
    json << "    \"max_distance_ticks\": " << config.policy.max_distance_ticks << ",\n";
    json << "    \"max_aggr20_age_ms\": " << config.policy.max_aggr20_age_ms << ",\n";
    json << "    \"require_zero_starting_position\": "
         << (config.policy.require_zero_starting_position ? "true" : "false") << "\n";
    json << "  }\n";
    json << "}\n";

    std::ostringstream evidence_json;
    evidence_json << "{\n";
    evidence_json << "  \"schema\": \"moex.plaza2.reviewed_execution_evidence.v1\",\n";
    evidence_json << "  \"authorized_intent_sha256\": \"PENDING\",\n";
    evidence_json << "  \"reviewed_evidence\": {\n";
    evidence_json << "    \"tick_size\": \"" << json_escape(config.smoke.tick_size) << "\",\n";
    evidence_json << "    \"top_bid\": \"" << json_escape(config.smoke.top_bid) << "\",\n";
    evidence_json << "    \"top_ask\": \"" << json_escape(config.smoke.top_ask) << "\",\n";
    evidence_json << "    \"market_data_source\": \"" << json_escape(config.smoke.market_data_source) << "\",\n";
    evidence_json << "    \"aggr20_source_sequence\": " << config.smoke.aggr20_source_sequence << ",\n";
    evidence_json << "    \"aggr20_source_revision\": " << config.smoke.aggr20_source_revision << ",\n";
    evidence_json << "    \"aggr20_observed_at_utc\": \"" << json_escape(config.smoke.aggr20_observed_at_utc)
                  << "\",\n";
    evidence_json << "    \"aggr20_age_ms\": " << config.smoke.aggr20_age_ms << ",\n";
    evidence_json << "    \"max_aggr20_age_ms\": " << config.policy.max_aggr20_age_ms << ",\n";
    evidence_json << "    \"trading_day\": \"" << json_escape(config.smoke.trading_day) << "\",\n";
    evidence_json << "    \"session_id\": \"" << json_escape(config.smoke.session_id) << "\",\n";
    evidence_json << "    \"session_state\": \"" << json_escape(config.smoke.session_state) << "\",\n";
    evidence_json << "    \"tradable_session\": " << (config.smoke.tradable_session ? "true" : "false") << ",\n";
    evidence_json << "    \"refdata_source\": \"" << json_escape(config.smoke.refdata_source) << "\",\n";
    evidence_json << "    \"refdata_source_sequence\": " << config.smoke.refdata_source_sequence << ",\n";
    evidence_json << "    \"refdata_source_revision\": " << config.smoke.refdata_source_revision << ",\n";
    evidence_json << "    \"instrument_exists\": " << (config.smoke.instrument_exists ? "true" : "false") << ",\n";
    evidence_json << "    \"limits_source\": \"" << json_escape(config.smoke.limits_source) << "\",\n";
    evidence_json << "    \"limits_commit_sequence\": " << config.smoke.limits_commit_sequence << ",\n";
    evidence_json << "    \"limits_snapshot_applicable\": "
                  << (config.smoke.limits_snapshot_applicable ? "true" : "false") << "\n";
    evidence_json << "  }\n";
    evidence_json << "}\n";

    PreSendPlan plan{
        .ok = true,
        .failure = PreSendFailure::None,
        .message = "pre-send plan validated",
        .canonical_json = json.str(),
        .reviewed_evidence_json = evidence_json.str(),
        .add_command = std::move(command),
        .exact_ext_id_recovery_command = std::move(recovery_command),
    };
    plan.sha256 = cgate::plaza2_sha256_hex(plan.canonical_json);
    const auto intent_placeholder = std::string("\"authorized_intent_sha256\": \"PENDING\"");
    const auto intent_value = std::string("\"authorized_intent_sha256\": \"") + plan.sha256 + "\"";
    while (true) {
        const auto position = plan.reviewed_evidence_json.find(intent_placeholder);
        if (position == std::string::npos) {
            break;
        }
        plan.reviewed_evidence_json.replace(position, intent_placeholder.size(), intent_value);
    }
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
    if (!atomic_write(output_directory / "authorized_intent.json", plan.canonical_json, error)) {
        return false;
    }
    if (!atomic_write(output_directory / "pre_send_plan.json", plan.canonical_json, error)) {
        return false;
    }
    if (!plan.reviewed_evidence_json.empty() &&
        !atomic_write(output_directory / "reviewed_execution_evidence.json", plan.reviewed_evidence_json, error)) {
        return false;
    }
    return true;
}

RestartReconciliationResult reconcile_unfinished_run(const OrderLifecycleConfig& config,
                                                     std::span<const private_state::OwnOrderSnapshot> orders,
                                                     std::span<const private_state::OwnTradeSnapshot> trades) {
    RestartReconciliationResult result;
    result.journal_path = config.journal_root / config.run_id / "journal.json";
    const auto active = has_unfinished_identifier(config);
    const auto journal_exists = std::filesystem::exists(result.journal_path);
    if (!active && !journal_exists) {
        result.run_found = false;
        result.locks_retained = false;
        result.message = "no unfinished identifier locks found";
        return result;
    }
    result.run_found = true;
    result.locks_retained = active;
    if (!journal_exists) {
        result.ok = false;
        result.message = "unfinished identifier locks have no journal; retaining them";
        return result;
    }

    std::ifstream input(result.journal_path, std::ios::binary);
    const std::string journal_text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input && journal_text.empty()) {
        result.ok = false;
        result.message = "unfinished journal could not be read; retaining identifier locks";
        return result;
    }

    // The original journal is immutable evidence.  Validate its fixed run
    // identity and the canonical AddOrder payload before considering any
    // terminal observation for lock removal.
    const auto schema = journal_string_field(journal_text, "schema");
    const auto journal_run_id = journal_string_field(journal_text, "run_id");
    const auto journal_profile_id = journal_string_field(journal_text, "profile_id");
    const auto journal_profile = journal_string_field(journal_text, "profile_fingerprint");
    const auto journal_payload = journal_string_field(journal_text, "payload_sha256");
    const auto journal_recovery_payload = journal_string_field(journal_text, "recovery_payload_sha256");
    const auto journal_ext = journal_integer_field(journal_text, "ext_id");
    const auto journal_add_user = journal_integer_field(journal_text, "add_user_id");
    const auto journal_cancel_user = journal_integer_field(journal_text, "cancel_user_id");
    const auto journal_recovery_user = journal_integer_field(journal_text, "recovery_user_id");
    const auto journal_add_reply_id = journal_integer_field(journal_text, "add_reply_order_id");
    auto plan_config = config;
    plan_config.dry_run = true;
    plan_config.send_test_order = false;
    plan_config.authorized_plan_sha256.clear();
    plan_config.journal_root = result.journal_path.parent_path() / "reconciliation_plan";
    const auto plan = build_pre_send_plan(plan_config);
    if (!schema.has_value() || *schema != "moex.plaza2.order_run_journal.v2" || !journal_run_id.has_value() ||
        *journal_run_id != config.run_id || !journal_profile_id.has_value() ||
        *journal_profile_id != config.profile_id || !journal_profile.has_value() ||
        *journal_profile != config.profile_fingerprint || !journal_payload.has_value() ||
        !journal_recovery_payload.has_value() || !plan.ok ||
        *journal_payload != cgate::plaza2_sha256_hex(plan.add_command.payload) ||
        *journal_recovery_payload != cgate::plaza2_sha256_hex(plan.exact_ext_id_recovery_command.payload) ||
        !journal_ext.has_value() || *journal_ext != config.ext_id || !journal_add_user.has_value() ||
        *journal_add_user != config.add_user_id || !journal_cancel_user.has_value() ||
        *journal_cancel_user != config.cancel_user_id || !journal_recovery_user.has_value() ||
        *journal_recovery_user != config.recovery_user_id) {
        result.ok = false;
        result.message =
            "unfinished journal identity or payload does not match the reconciliation request; retaining locks";
        return result;
    }

    if (!active) {
        const auto final_state_name = journal_string_field(journal_text, "final_state");
        const auto final_state = final_state_name.has_value() ? journal_state_field(*final_state_name) : std::nullopt;
        const auto finished = journal_boolean_field(journal_text, "finished");
        const auto market_safe_terminal = journal_boolean_field(journal_text, "market_safe_terminal");
        const auto evidence_consistent = journal_boolean_field(journal_text, "evidence_consistent");
        const auto journal_degraded = journal_boolean_field(journal_text, "journal_degraded");
        const auto orphan_incident = journal_boolean_field(journal_text, "orphan_incident");
        const bool allowed_terminal =
            final_state.has_value() &&
            (*final_state == OrderLifecycleState::DefinitelyNotSent || *final_state == OrderLifecycleState::Rejected ||
             *final_state == OrderLifecycleState::Filled || *final_state == OrderLifecycleState::Cancelled);
        if (!final_state.has_value() || !allowed_terminal || !finished.value_or(false) ||
            !market_safe_terminal.value_or(false) || !evidence_consistent.value_or(false) ||
            journal_degraded.value_or(true) || orphan_incident.value_or(true)) {
            result.ok = false;
            result.resolved = false;
            result.locks_retained = false;
            result.message =
                "historical journal without identifier locks does not prove a safe completed terminal epoch";
            return result;
        }
        result.ok = true;
        result.resolved = true;
        result.locks_retained = false;
        result.state = *final_state;
        result.message = "journal already records a safe terminal run with released identifier locks";
        return result;
    }

    auto observation = observe_order(config.ext_id, config.client_code, config.side, config.quantity, orders, trades);
    if (!observation.has_value() && !config.broker_code.empty() && !config.client_code.empty()) {
        const auto participant = config.broker_code + config.client_code;
        if (participant != config.client_code)
            observation = observe_order(config.ext_id, participant, config.side, config.quantity, orders, trades);
    }
    if (!observation.has_value() || !observation_terminal(observation) || observation->identity_conflict) {
        result.state = observation.has_value() ? observation->state : OrderLifecycleState::UnresolvedOrphanIncident;
        result.message = observation.has_value() && observation->identity_conflict
                             ? "restart observation has an identity conflict; retaining identifier locks"
                             : "restart observation is absent or still working; retaining identifier locks";
        return result;
    }

    const auto journal_sha256 = cgate::plaza2_sha256_hex(journal_text);
    const auto known_public_id = journal_integer_field(journal_text, "public_order_id").value_or(0);
    const auto known_private_id = journal_integer_field(journal_text, "private_order_id").value_or(0);
    const auto known_add_reply_id = journal_add_reply_id.value_or(0);
    if ((known_add_reply_id != 0 && !reply_id_matches_observation(known_add_reply_id, *observation)) ||
        (known_public_id != 0 && !reply_id_matches_observation(known_public_id, *observation)) ||
        (known_private_id != 0 && !reply_id_matches_observation(known_private_id, *observation))) {
        result.ok = false;
        result.message = "restart observation does not preserve the historical order identity; retaining locks";
        return result;
    }

    const auto run_directory = result.journal_path.parent_path();
    const auto write_resolution = [&](bool prepared, bool locks_released, std::string& error) {
        std::ostringstream resolution;
        resolution << "{\n"
                   << "  \"schema\": \"moex.plaza2.restart_reconciliation.v2\",\n"
                   << "  \"run_id\": \"" << json_escape(config.run_id) << "\",\n"
                   << "  \"journal_sha256\": \"" << journal_sha256 << "\",\n"
                   << "  \"ext_id\": " << config.ext_id << ",\n"
                   << "  \"state\": \"" << order_lifecycle_state_name(observation->state) << "\",\n"
                   << "  \"public_order_id\": " << observation->public_order_id << ",\n"
                   << "  \"private_order_id\": " << observation->private_order_id << ",\n"
                   << "  \"evidence_consistent\": true,\n"
                   << "  \"resolution_prepared\": " << (prepared ? "true" : "false") << ",\n"
                   << "  \"locks_released\": " << (locks_released ? "true" : "false") << "\n"
                   << "}\n";
        return atomic_write(run_directory / "restart_reconciliation.json", resolution.str(), error);
    };
    std::string error;
    if (!write_resolution(true, false, error)) {
        result.ok = false;
        result.message = "restart resolution preparation failed: " + error;
        return result;
    }

    const std::array locks = {
        ext_lock_path(config),
        user_lock_path(config, config.add_user_id),
        user_lock_path(config, config.cancel_user_id),
        user_lock_path(config, config.recovery_user_id),
    };
    std::error_code filesystem_error;
    for (const auto& lock : locks) {
        std::filesystem::remove(lock, filesystem_error);
    }
    result.state = observation->state;
    result.locks_retained =
        std::any_of(locks.begin(), locks.end(), [](const auto& lock) { return std::filesystem::exists(lock); });
    if (result.locks_retained) {
        result.ok = false;
        result.message = "restart resolution prepared but some identifier locks remain";
        return result;
    }
    result.resolved = true;
    if (!write_resolution(false, true, error)) {
        result.ok = false;
        result.message = "restart final resolution publication failed: " + error;
        return result;
    }
    result.message = "restart reconciliation resolved a consistent terminal observation";
    return result;
}

OrderLifecycleController::OrderLifecycleController(OrderLifecycleConfig config, OrderLifecycleTransport& transport,
                                                   OrderLifecycleClock& clock)
    : config_(std::move(config)), transport_(transport), clock_(clock) {}

OrderLifecycleResult OrderLifecycleController::run() {
    OrderLifecycleResult result;
    const auto plan = build_pre_send_plan(config_);
    if (!plan.ok) {
        result.market_safe_terminal = true;
        result.state = OrderLifecycleState::DefinitelyNotSent;
        result.message = std::string(pre_send_failure_name(plan.failure)) + ": " + plan.message;
        result.transitions = {OrderLifecycleState::DefinitelyNotSent};
        return result;
    }
    if (config_.dry_run) {
        std::string write_error;
        const auto output_directory = config_.journal_root / "plans" / config_.run_id;
        if (!write_pre_send_plan(output_directory, plan, write_error)) {
            result.market_safe_terminal = true;
            result.journal_ok = false;
            result.state = OrderLifecycleState::DefinitelyNotSent;
            result.message = "failed to write canonical pre-send plan: " + write_error;
            result.transitions = {OrderLifecycleState::DefinitelyNotSent};
            return result;
        }
        result.ok = true;
        result.market_safe_terminal = true;
        result.state = OrderLifecycleState::DefinitelyNotSent;
        result.message = "dry-run wrote canonical pre_send_plan.json without opening CGate";
        result.transitions = {OrderLifecycleState::DefinitelyNotSent};
        result.journal_path = output_directory / "pre_send_plan.json";
        return result;
    }

    const auto binding_error = transport_.bind_authorized_plan(plan);
    if (binding_error) {
        result.market_safe_terminal = true;
        result.state = OrderLifecycleState::DefinitelyNotSent;
        result.message = "authorization binding failed: " + binding_error.message;
        result.transitions = {OrderLifecycleState::DefinitelyNotSent};
        return result;
    }

    RunJournal journal;
    std::string journal_error;
    const auto payload_hash = cgate::plaza2_sha256_hex(plan.add_command.payload);
    const auto recovery_payload_hash = cgate::plaza2_sha256_hex(plan.exact_ext_id_recovery_command.payload);
    if (!journal.begin(config_, payload_hash, recovery_payload_hash, journal_error)) {
        result.market_safe_terminal = true;
        result.journal_ok = false;
        result.journal_degraded = true;
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

    poll_add_until_resolution(config_, transport_, clock_, evidence, journal);
    if (!add_phase_resolved(evidence)) {
        reconcile_once(config_, transport_, evidence, journal);
    }

    if (observation_terminal(evidence.observation)) {
        return finish_result(std::move(result), journal, evidence.observation->state, true, false,
                             evidence.observation->state == OrderLifecycleState::Filled
                                 ? "order filled before cancellation"
                                 : "order reached a factual cancelled state",
                             evidence);
    }
    if (definitive_add_rejection(evidence)) {
        return finish_result(std::move(result), journal, OrderLifecycleState::Rejected, true, false,
                             "AddOrder reply rejected the command", evidence);
    }

    if (usable_cancel_precondition(evidence)) {
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
        poll_until_terminal(config_, transport_, clock_, config_.cancel_observation_timeout, evidence, journal);
        if (!observation_terminal(evidence.observation)) {
            reconcile_once(config_, transport_, evidence, journal);
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

    if (accepted_add_reply_has_id(evidence)) {
        const auto message = !evidence.consistent
                                 ? "AddOrder evidence is inconsistent, so no order identity was guessed"
                             : !evidence.observation.has_value()
                                 ? "accepted AddOrder reply ID lacks replication required by DelOrder policy"
                                 : "AddOrder reply ID does not match replicated public/private identifier aliases";
        return finish_result(std::move(result), journal, OrderLifecycleState::UnresolvedOrphanIncident, false, true,
                             message, evidence);
    }

    result.recovery_submission =
        transport_.post_exact_ext_id_recovery(plan.exact_ext_id_recovery_command, config_.recovery_user_id);
    journal.record_recovery_submission(result.recovery_submission);
    if (result.recovery_submission.certainty != cgate::Plaza2SubmissionCertainty::DefinitelyNotSent) {
        journal.record_state(OrderLifecycleState::CancelPending);
    }
    poll_until_terminal(config_, transport_, clock_, config_.cancel_observation_timeout, evidence, journal);
    if (!observation_terminal(evidence.observation)) {
        reconcile_once(config_, transport_, evidence, journal);
    }
    if (observation_terminal(evidence.observation)) {
        return finish_result(std::move(result), journal, evidence.observation->state, true, false,
                             evidence.observation->state == OrderLifecycleState::Filled
                                 ? "order filled while exact-ext_id recovery was pending"
                                 : "exact-ext_id recovery reached a factual cancelled state",
                             evidence);
    }
    const auto unresolved_message =
        result.recovery_submission.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent
            ? "exact-ext_id recovery was definitely not sent and AddOrder remains unresolved"
            : "exact-ext_id recovery outcome remains unresolved; a reply or timeout is not "
              "factual cancellation";
    return finish_result(std::move(result), journal, OrderLifecycleState::UnresolvedOrphanIncident, false, true,
                         unresolved_message, evidence);
}

struct PersistentOrderController::Impl {
    explicit Impl(OrderLifecycleConfig value, OrderLifecycleTransport& transport_value,
                  OrderLifecycleClock& clock_value)
        : config(std::move(value)), transport(transport_value), clock(clock_value) {}

    [[nodiscard]] cgate::Plaza2Error error(std::string message) const {
        return {.code = cgate::Plaza2ErrorCode::InvalidConfiguration, .message = std::move(message)};
    }

    [[nodiscard]] OrderLifecycleResult refusal(std::string message) const {
        auto out = result;
        out.ok = false;
        out.message = std::move(message);
        return out;
    }

    void sync_result() {
        result.state = lifecycle_state;
        result.evidence_consistent = evidence.consistent;
        result.observation = evidence.observation;
        result.add_reply = evidence.add_reply;
        result.cancel_reply = evidence.cancel_reply;
        result.recovery_reply = evidence.recovery_reply;
        result.transitions = journal ? journal->states() : std::vector<OrderLifecycleState>{lifecycle_state};
        if (journal) {
            result.journal_path = journal->path();
            result.journal_degraded = journal->degraded();
            result.journal_ok = !result.journal_degraded;
        }
    }

    [[nodiscard]] OrderLifecycleResult finish(OrderLifecycleState state, bool market_safe, bool orphan,
                                              std::string message) {
        lifecycle_state = state;
        if (!journal) {
            result.state = state;
            result.market_safe_terminal = market_safe;
            result.evidence_consistent = evidence.consistent;
            result.message = std::move(message);
            result.ok = market_safe && evidence.consistent;
            terminal_finished = result.ok;
            sync_result();
            return result;
        }
        result.message = std::move(message);
        const auto final_message = result.message;
        result = finish_result(std::move(result), *journal, state, market_safe, orphan, final_message, evidence);
        terminal_finished = result.ok;
        sync_result();
        return result;
    }

    [[nodiscard]] OrderLifecycleResult nonterminal(std::string message = {}) {
        result.ok = false;
        result.market_safe_terminal = false;
        result.message = std::move(message);
        sync_result();
        return result;
    }

    [[nodiscard]] OrderLifecycleResult consume_one_poll() {
        if (!journal) {
            return refusal("order journal is not open");
        }
        const auto deadline = clock.now() + std::chrono::milliseconds(1);
        const auto polled = transport.poll(deadline);
        consume_poll(config, polled, evidence, *journal);
        if (!polled.ok) {
            return nonterminal(polled.error.empty() ? "order poll failed" : polled.error);
        }
        if (observation_terminal(evidence.observation)) {
            const auto terminal = evidence.observation->state;
            return finish(terminal, true, false,
                          terminal == OrderLifecycleState::Filled ? "order reached Filled" : "order reached Cancelled");
        }
        if (definitive_add_rejection(evidence)) {
            return finish(OrderLifecycleState::Rejected, true, false, "AddOrder reply rejected the command");
        }
        if (!evidence.consistent) {
            return finish(OrderLifecycleState::UnresolvedOrphanIncident, false, true,
                          "order evidence became inconsistent; epoch is fail-closed");
        }
        if (cancel_requested || recovery_requested) {
            if ((cancel_requested && definitive_cancel_rejection(evidence)) ||
                (recovery_requested && definitive_recovery_rejection(evidence))) {
                return finish(OrderLifecycleState::UnresolvedOrphanIncident, false, true,
                              cancel_requested
                                  ? "DelOrder was definitively rejected while the order remains nonterminal"
                                  : "exact-ext recovery was definitively rejected while the order remains nonterminal");
            }
            // A cancel/recovery command is an application state, not merely a
            // reflection of the latest replicated row.  Keep it visible while
            // the command is unresolved, while still retaining the newest
            // quantities/evidence in the result and journal.
            lifecycle_state = OrderLifecycleState::CancelPending;
            journal->record_state(lifecycle_state);
        } else if (observation_working(evidence.observation)) {
            lifecycle_state = evidence.observation->state;
            journal->record_state(lifecycle_state);
        } else if (accepted_add_reply_has_id(evidence)) {
            lifecycle_state = OrderLifecycleState::Posted;
            journal->record_state(lifecycle_state);
        } else {
            lifecycle_state = result.add_submission.certainty == cgate::Plaza2SubmissionCertainty::PossiblySent
                                  ? OrderLifecycleState::PossiblySent
                                  : OrderLifecycleState::Posted;
            journal->record_state(lifecycle_state);
        }
        return nonterminal("order epoch remains active");
    }

    OrderLifecycleConfig config;
    OrderLifecycleTransport& transport;
    OrderLifecycleClock& clock;
    PreSendPlan plan;
    std::unique_ptr<RunJournal> journal;
    LifecycleEvidence evidence;
    OrderLifecycleResult result;
    OrderLifecycleState lifecycle_state{OrderLifecycleState::Idle};
    bool active_epoch{false};
    bool authorized_epoch{false};
    bool submission_attempted_flag{false};
    bool terminal_finished{false};
    bool cancel_requested{false};
    bool recovery_requested{false};
};

PersistentOrderController::PersistentOrderController(OrderLifecycleConfig config, OrderLifecycleTransport& transport,
                                                     OrderLifecycleClock& clock)
    : impl_(std::make_unique<Impl>(std::move(config), transport, clock)) {}

PersistentOrderController::~PersistentOrderController() = default;
PersistentOrderController::PersistentOrderController(PersistentOrderController&&) noexcept = default;
PersistentOrderController& PersistentOrderController::operator=(PersistentOrderController&&) noexcept = default;

cgate::Plaza2Error PersistentOrderController::begin(const PreSendPlan& plan) {
    auto& p = *impl_;
    if (p.active_epoch) {
        return p.error("an order epoch is already active");
    }
    if (!plan.ok || plan.canonical_json.empty() || plan.sha256.empty()) {
        return p.error("persistent order requires a valid exact authorized plan");
    }
    p.plan = plan;
    p.result = {};
    p.lifecycle_state = OrderLifecycleState::Authorized;
    p.active_epoch = true;
    p.authorized_epoch = true;
    p.submission_attempted_flag = false;
    p.terminal_finished = false;
    p.cancel_requested = false;
    p.recovery_requested = false;
    p.evidence = {};
    p.journal.reset();
    p.sync_result();
    return {};
}

OrderLifecycleResult PersistentOrderController::submit_order() {
    auto& p = *impl_;
    if (!p.active_epoch || !p.authorized_epoch) {
        return p.refusal("begin_order and exact authorization are required");
    }
    if (p.submission_attempted_flag) {
        return p.refusal("one AddOrder attempt maximum per order epoch");
    }
    if (p.lifecycle_state != OrderLifecycleState::Authorized) {
        return p.refusal("order epoch is not in Authorized state");
    }

    p.journal = std::make_unique<RunJournal>();
    std::string journal_error;
    const auto payload_hash = cgate::plaza2_sha256_hex(p.plan.add_command.payload);
    const auto recovery_payload_hash = cgate::plaza2_sha256_hex(p.plan.exact_ext_id_recovery_command.payload);
    if (!p.journal->begin(p.config, payload_hash, recovery_payload_hash, journal_error)) {
        p.lifecycle_state = OrderLifecycleState::DefinitelyNotSent;
        p.result = {};
        p.result.state = p.lifecycle_state;
        p.result.journal_ok = false;
        p.result.journal_degraded = true;
        p.result.market_safe_terminal = true;
        p.result.message = "journal_failure: " + journal_error;
        p.result.transitions = {p.lifecycle_state};
        p.terminal_finished = true;
        return p.result;
    }
    p.result.journal_path = p.journal->path();
    p.journal->record_state(OrderLifecycleState::Authorized);
    p.lifecycle_state = OrderLifecycleState::AddPending;
    p.journal->record_state(p.lifecycle_state);
    p.sync_result();
    p.result.add_submission = p.transport.post(p.plan.add_command, p.config.add_user_id);
    p.journal->record_add_submission(p.result.add_submission);
    p.submission_attempted_flag = true;
    p.lifecycle_state = p.result.add_submission.certainty == cgate::Plaza2SubmissionCertainty::Posted
                            ? OrderLifecycleState::Posted
                        : p.result.add_submission.certainty == cgate::Plaza2SubmissionCertainty::PossiblySent
                            ? OrderLifecycleState::PossiblySent
                            : OrderLifecycleState::DefinitelyNotSent;
    p.journal->record_state(p.lifecycle_state);
    if (p.lifecycle_state == OrderLifecycleState::DefinitelyNotSent) {
        return p.finish(OrderLifecycleState::DefinitelyNotSent, true, false, "AddOrder was definitely not sent");
    }
    return p.nonterminal("AddOrder attempt posted; poll_order is required for factual lifecycle evidence");
}

OrderLifecycleResult PersistentOrderController::poll_order() {
    auto& p = *impl_;
    if (!p.active_epoch || !p.submission_attempted_flag) {
        return p.refusal("submit_order is required before poll_order");
    }
    if (p.terminal_finished || p.lifecycle_state == OrderLifecycleState::UnresolvedOrphanIncident) {
        return p.result;
    }
    return p.consume_one_poll();
}

OrderLifecycleResult PersistentOrderController::cancel_order() {
    auto& p = *impl_;
    if (!p.active_epoch || !p.submission_attempted_flag) {
        return p.refusal("submit_order is required before cancel_order");
    }
    if (p.terminal_finished || p.lifecycle_state == OrderLifecycleState::UnresolvedOrphanIncident) {
        return p.result;
    }
    if (p.cancel_requested || p.recovery_requested) {
        return p.refusal("one cancel or exact-ext recovery attempt maximum per order epoch");
    }

    // Give the persistent controller one read-side turn so the explicit
    // cancel uses the same accepted Add reply identity checks as the one-shot
    // lifecycle. It never cancels automatically from poll_order().
    const auto observed = p.consume_one_poll();
    if (p.terminal_finished || p.lifecycle_state == OrderLifecycleState::UnresolvedOrphanIncident) {
        return observed;
    }
    if (usable_cancel_precondition(p.evidence)) {
        DelOrderRequest cancel;
        cancel.broker_code = p.config.broker_code;
        cancel.order_id = *p.evidence.add_reply->order_id;
        cancel.client_code = p.config.client_code;
        cancel.isin_id = p.config.isin_id;
        const auto command = Plaza2TradeCodec{}.encode(Plaza2TradeCommandRequest{cancel});
        if (!command.validation.ok()) {
            return p.finish(OrderLifecycleState::UnresolvedOrphanIncident, false, true,
                            "DelOrder encoding failed after AddOrder became working");
        }
        p.cancel_requested = true;
        p.result.cancel_submission = p.transport.post(command, p.config.cancel_user_id);
        p.journal->record_cancel_submission(p.result.cancel_submission);
        if (p.result.cancel_submission.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent) {
            return p.finish(OrderLifecycleState::UnresolvedOrphanIncident, false, true,
                            "DelOrder was definitely not sent and the working order remains unresolved");
        }
        p.lifecycle_state = OrderLifecycleState::CancelPending;
        p.journal->record_state(p.lifecycle_state);
        return p.nonterminal("DelOrder posted; poll_order is required for terminal evidence");
    }

    if (accepted_add_reply_has_id(p.evidence) && p.evidence.consistent && !p.evidence.observation.has_value()) {
        // An accepted Add with no replicated order cannot be safely addressed
        // by DelOrder. The existing exact-ext recovery is the only bounded
        // cleanup path and is still initiated only by this explicit cancel.
    } else if (p.lifecycle_state == OrderLifecycleState::Working ||
               p.lifecycle_state == OrderLifecycleState::PartiallyFilled) {
        return p.finish(OrderLifecycleState::UnresolvedOrphanIncident, false, true,
                        "working order lacks an unambiguous correlated Add reply identity");
    }

    p.recovery_requested = true;
    p.result.recovery_submission =
        p.transport.post_exact_ext_id_recovery(p.plan.exact_ext_id_recovery_command, p.config.recovery_user_id);
    p.journal->record_recovery_submission(p.result.recovery_submission);
    if (p.result.recovery_submission.certainty == cgate::Plaza2SubmissionCertainty::DefinitelyNotSent) {
        return p.finish(OrderLifecycleState::UnresolvedOrphanIncident, false, true,
                        "exact-ext recovery was definitely not sent and AddOrder remains unresolved");
    }
    p.lifecycle_state = OrderLifecycleState::CancelPending;
    p.journal->record_state(p.lifecycle_state);
    return p.nonterminal("exact-ext recovery posted; poll_order is required for terminal evidence");
}

cgate::Plaza2Error PersistentOrderController::finish_order_epoch() {
    auto& p = *impl_;
    if (!p.active_epoch) {
        return p.error("no active order epoch");
    }
    if (!p.terminal_finished || !p.result.ok || p.lifecycle_state == OrderLifecycleState::UnresolvedOrphanIncident) {
        return p.error("order epoch is not a safe terminal epoch");
    }
    p.active_epoch = false;
    p.authorized_epoch = false;
    return {};
}

bool PersistentOrderController::active() const noexcept {
    return impl_->active_epoch;
}

bool PersistentOrderController::authorized() const noexcept {
    return impl_->active_epoch && impl_->authorized_epoch;
}

bool PersistentOrderController::submission_attempted() const noexcept {
    return impl_->submission_attempted_flag;
}

bool PersistentOrderController::new_order_allowed() const noexcept {
    return !impl_->active_epoch;
}

OrderLifecycleState PersistentOrderController::state() const noexcept {
    return impl_->lifecycle_state;
}

const OrderLifecycleResult& PersistentOrderController::last_result() const noexcept {
    return impl_->result;
}

} // namespace moex::plaza2_trade
