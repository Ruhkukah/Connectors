#include "moex/twime_trade/twime_session_persistence.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace moex::twime_trade {

namespace {

std::string bool_string(bool value) {
    return value ? "1" : "0";
}

bool parse_bool(std::string_view value) {
    return value == "1" || value == "true";
}

std::uint64_t parse_u64(const std::unordered_map<std::string, std::string>& values, const char* key,
                        std::uint64_t fallback = 0) {
    const auto found = values.find(key);
    if (found == values.end()) {
        return fallback;
    }
    return static_cast<std::uint64_t>(std::stoull(found->second));
}

std::int64_t parse_i64(const std::unordered_map<std::string, std::string>& values, const char* key,
                      std::int64_t fallback = 0) {
    const auto found = values.find(key);
    if (found == values.end()) {
        return fallback;
    }
    return static_cast<std::int64_t>(std::stoll(found->second));
}

} // namespace

std::optional<TwimeSessionPersistenceSnapshot>
TwimeInMemorySessionPersistenceStore::load(std::string_view session_id) const {
    const auto found = snapshots_.find(std::string(session_id));
    if (found == snapshots_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void TwimeInMemorySessionPersistenceStore::save(std::string_view session_id,
                                                const TwimeSessionPersistenceSnapshot& snapshot) {
    snapshots_[std::string(session_id)] = snapshot;
}

TwimeFileSessionPersistenceStore::TwimeFileSessionPersistenceStore(std::filesystem::path root_dir)
    : root_dir_(std::move(root_dir)) {}

std::filesystem::path TwimeFileSessionPersistenceStore::snapshot_path(std::string_view session_id) const {
    return root_dir_ / (std::string(session_id) + ".state");
}

std::optional<TwimeSessionPersistenceSnapshot>
TwimeFileSessionPersistenceStore::load(std::string_view session_id) const {
    const auto path = snapshot_path(session_id);
    if (!std::filesystem::exists(path)) {
        return std::nullopt;
    }

    std::ifstream input(path);
    if (!input) {
        return std::nullopt;
    }

    std::unordered_map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        values.emplace(line.substr(0, separator), line.substr(separator + 1));
    }

    TwimeSessionPersistenceSnapshot snapshot;
    snapshot.recovery_state.next_outbound_seq = parse_u64(values, "next_outbound_seq", 1);
    snapshot.recovery_state.next_expected_inbound_seq = parse_u64(values, "next_expected_inbound_seq", 1);
    snapshot.recovery_state.last_establishment_id = parse_u64(values, "last_establishment_id", 0);
    snapshot.recovery_state.recovery_epoch = parse_u64(values, "recovery_epoch", 1);
    snapshot.recovery_state.last_clean_shutdown =
        parse_bool(values.contains("last_clean_shutdown") ? values.at("last_clean_shutdown") : "1");
    snapshot.reject_seen = parse_bool(values.contains("reject_seen") ? values.at("reject_seen") : "0");
    snapshot.last_reject_code = parse_i64(values, "last_reject_code", 0);
    snapshot.active_keepalive_interval_ms =
        static_cast<std::uint32_t>(parse_u64(values, "active_keepalive_interval_ms", 0));
    snapshot.reconnect_attempt_count = parse_u64(values, "reconnect_attempt_count", 0);
    snapshot.last_transition_time_ms = parse_u64(values, "last_transition_time_ms", 0);
    return snapshot;
}

void TwimeFileSessionPersistenceStore::save(std::string_view session_id,
                                            const TwimeSessionPersistenceSnapshot& snapshot) {
    std::filesystem::create_directories(root_dir_);
    std::ofstream output(snapshot_path(session_id), std::ios::trunc);
    output << "next_outbound_seq=" << snapshot.recovery_state.next_outbound_seq << '\n';
    output << "next_expected_inbound_seq=" << snapshot.recovery_state.next_expected_inbound_seq << '\n';
    output << "last_establishment_id=" << snapshot.recovery_state.last_establishment_id << '\n';
    output << "recovery_epoch=" << snapshot.recovery_state.recovery_epoch << '\n';
    output << "last_clean_shutdown=" << bool_string(snapshot.recovery_state.last_clean_shutdown) << '\n';
    output << "reject_seen=" << bool_string(snapshot.reject_seen) << '\n';
    output << "last_reject_code=" << snapshot.last_reject_code << '\n';
    output << "active_keepalive_interval_ms=" << snapshot.active_keepalive_interval_ms << '\n';
    output << "reconnect_attempt_count=" << snapshot.reconnect_attempt_count << '\n';
    output << "last_transition_time_ms=" << snapshot.last_transition_time_ms << '\n';
}

TwimePersistentRecoveryStateStore::TwimePersistentRecoveryStateStore(TwimeSessionPersistenceStore& store)
    : store_(store) {}

std::optional<TwimeRecoveryState> TwimePersistentRecoveryStateStore::load(std::string_view session_id) const {
    const auto snapshot = store_.load(session_id);
    if (!snapshot.has_value()) {
        return std::nullopt;
    }
    return snapshot->recovery_state;
}

void TwimePersistentRecoveryStateStore::save(std::string_view session_id, const TwimeRecoveryState& state) {
    auto snapshot = store_.load(session_id).value_or(TwimeSessionPersistenceSnapshot{});
    snapshot.recovery_state = state;
    store_.save(session_id, snapshot);
}

TwimeSessionPersistenceSnapshot make_twime_session_persistence_snapshot(const TwimeSessionHealthSnapshot& health,
                                                                       const TwimeRecoveryState& recovery_state) {
    TwimeSessionPersistenceSnapshot snapshot;
    snapshot.recovery_state = recovery_state;
    snapshot.reject_seen = health.reject_seen;
    snapshot.last_reject_code = health.last_reject_code;
    snapshot.active_keepalive_interval_ms = health.active_keepalive_interval_ms;
    snapshot.reconnect_attempt_count = health.reconnect_attempts;
    snapshot.last_transition_time_ms = health.last_transition_time_ms;
    return snapshot;
}

} // namespace moex::twime_trade
