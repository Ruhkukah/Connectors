#include "moex/plaza2_trade/plaza2_recovered_cancel.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace moex::plaza2_trade {
namespace ps = plaza2::private_state;
namespace cg = plaza2::cgate;
std::string_view recovered_order_outcome_name(RecoveredOrderOutcome value) noexcept {
    switch (value) {
    case RecoveredOrderOutcome::ExactlyOneWorkingMatch:
        return "EXACTLY_ONE_WORKING_MATCH";
    case RecoveredOrderOutcome::NoMatch:
        return "NO_MATCH";
    case RecoveredOrderOutcome::MultipleMatches:
        return "MULTIPLE_MATCHES";
    case RecoveredOrderOutcome::IdentityConflict:
        return "IDENTITY_CONFLICT";
    case RecoveredOrderOutcome::TerminalAlready:
        return "TERMINAL_ALREADY";
    case RecoveredOrderOutcome::Ambiguous:
        return "AMBIGUOUS";
    case RecoveredOrderOutcome::PrivateStreamNotCurrent:
        return "PRIVATE_STREAM_NOT_CURRENT";
    case RecoveredOrderOutcome::GenerationNotFresh:
        return "GENERATION_NOT_FRESH";
    }
    return "AMBIGUOUS";
}

RecoveredOrderReconciliation reconcile_recovered_order(const RecoveredOrderKey& key, std::uint64_t generation,
                                                       bool fresh, std::span<const ps::OwnOrderSnapshot> orders,
                                                       std::span<const ps::OwnTradeSnapshot> trades) {
    RecoveredOrderReconciliation result;
    result.key = key;
    result.generation = generation;
    if (!fresh) {
        result.outcome = RecoveredOrderOutcome::PrivateStreamNotCurrent;
        return result;
    }
    const auto price = ps::parse_session_decimal(key.price);
    if (key.epoch.empty() || key.account.size() != 7 || key.isin_id <= 0 || key.sess_id <= 0 || key.ext_id <= 0 ||
        key.quantity <= 0 || !price)
        return result;
    std::vector<ps::OwnOrderSnapshot> matching;
    for (const auto& row : orders) {
        if (!row.from_trade_repl || row.from_user_book || row.from_current_day)
            continue;
        const bool known_id = key.known_exchange_id > 0 && (row.public_order_id == key.known_exchange_id ||
                                                            row.private_order_id == key.known_exchange_id);
        if (!known_id && (row.ext_id != key.ext_id || row.client_code != key.account))
            continue;
        // This capability is restricted to ordinary orders, not iceberg aliases.
        if (row.identity_conflict || row.multileg || row.ext_id != key.ext_id || row.client_code != key.account ||
            row.isin_id != key.isin_id || row.sess_id != key.sess_id || row.dir != static_cast<int>(key.side) ||
            ps::parse_session_decimal(row.price) != price || row.private_order_id <= 0 ||
            row.public_order_id != row.private_order_id || (key.known_exchange_id > 0 && !known_id) ||
            row.public_amount != row.private_amount || row.private_amount < 0 || row.private_amount > key.quantity ||
            row.public_amount_rest != row.private_amount_rest || row.public_action != row.private_action ||
            row.private_amount_rest < 0 || row.private_amount_rest > key.quantity ||
            row.trade_repl_commit_sequence == 0) {
            result.outcome = RecoveredOrderOutcome::IdentityConflict;
            return result;
        }
        matching.push_back(row);
    }
    if (matching.empty()) {
        result.outcome = RecoveredOrderOutcome::NoMatch;
        return result;
    }
    if (matching.size() != 1) {
        result.outcome = RecoveredOrderOutcome::MultipleMatches;
        return result;
    }
    const auto& row = matching.front();
    std::vector<ps::OwnTradeSnapshot> exact_trades;
    std::set<std::int64_t> deals;
    std::int64_t executed = 0;
    for (const auto& trade : trades) {
        const bool buy = key.side == Plaza2TradeSide::Buy;
        const auto public_id = buy ? trade.public_order_id_buy : trade.public_order_id_sell;
        const auto private_id = buy ? trade.private_order_id_buy : trade.private_order_id_sell;
        const auto ext = buy ? trade.ext_id_buy : trade.ext_id_sell;
        const auto& account = buy ? trade.code_buy : trade.code_sell;
        if (public_id != row.public_order_id && private_id != row.private_order_id &&
            (ext != key.ext_id || account != key.account))
            continue;
        if (trade.multileg || trade.isin_id != key.isin_id || trade.sess_id != key.sess_id || account != key.account ||
            public_id != row.public_order_id || private_id != row.private_order_id || ext != key.ext_id ||
            trade.amount <= 0 || trade.id_deal <= 0 || !deals.insert(trade.id_deal).second ||
            trade.amount > key.quantity - executed) {
            result.outcome = RecoveredOrderOutcome::IdentityConflict;
            return result;
        }
        executed += trade.amount;
        exact_trades.push_back(trade);
    }
    const auto remaining = row.private_amount_rest;
    const bool cancelled = row.private_action == 0 && remaining == 0;
    const bool filled = row.private_action == 2 && remaining == 0 && executed == key.quantity;
    const bool working =
        (row.private_action == 1 || row.private_action == 2) && remaining > 0 && executed == key.quantity - remaining;
    if (!cancelled && !filled && !working)
        return result;
    auto observation = observe_order(key.ext_id, key.account, key.side, key.quantity, matching, exact_trades);
    if (!observation || observation->identity_conflict) {
        result.outcome = RecoveredOrderOutcome::IdentityConflict;
        return result;
    }
    observation->original_quantity = key.quantity;
    observation->remaining_quantity = remaining;
    observation->executed_quantity = executed;
    observation->state = filled         ? OrderLifecycleState::Filled
                         : cancelled    ? OrderLifecycleState::Cancelled
                         : executed > 0 ? OrderLifecycleState::PartiallyFilled
                                        : OrderLifecycleState::Working;
    result.observation = observation;
    result.exchange_order_id = row.private_order_id;
    result.outcome = working ? RecoveredOrderOutcome::ExactlyOneWorkingMatch : RecoveredOrderOutcome::TerminalAlready;
    std::ostringstream evidence;
    evidence << "{\"generation\":" << generation << ",\"epoch\":" << std::quoted(key.epoch)
             << ",\"account\":" << std::quoted(key.account) << ",\"isin_id\":" << key.isin_id
             << ",\"sess_id\":" << key.sess_id << ",\"ext_id\":" << key.ext_id
             << ",\"side\":" << static_cast<int>(key.side) << ",\"price_units\":" << price->units
             << ",\"quantity\":" << key.quantity << ",\"order_id\":" << row.private_order_id
             << ",\"remaining\":" << remaining << ",\"executed\":" << executed << ",\"action\":" << row.private_action
             << ",\"trade_commit\":" << row.trade_repl_commit_sequence << ",\"moment\":" << row.moment
             << ",\"moment_ns\":" << row.moment_ns << ",\"deals\":[";
    bool first = true;
    for (const auto& trade : exact_trades) {
        if (!first)
            evidence << ',';
        first = false;
        evidence << '[' << trade.id_deal << ',' << trade.amount << ',' << std::quoted(trade.price) << ']';
    }
    evidence << "]}";
    result.evidence_json = evidence.str();
    result.evidence_sha256 = cg::plaza2_sha256_hex(result.evidence_json);
    return result;
}

bool write_recovered_artifact(const std::filesystem::path& path, std::string_view bytes, std::string& error) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        error = "exclusive artifact create failed";
        return false;
    }
    bool ok = ::fchmod(fd, 0600) == 0;
    std::size_t done = 0;
    while (ok && done < bytes.size()) {
        const auto count = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            ok = false;
            break;
        }
        done += static_cast<std::size_t>(count);
    }
    ok = (::fsync(fd) == 0) && ok;
    ok = (::close(fd) == 0) && ok;
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (directory < 0)
        ok = false;
    else {
        ok = (::fsync(directory) == 0) && ok;
        ::close(directory);
    }
    if (!ok)
        error = "artifact write or durability barrier failed; retained for inspection";
    return ok;
}

bool consume_recovered_artifact(const std::filesystem::path& path, std::string_view bytes, std::string& error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        error = "protected artifact unavailable";
        return false;
    }
    struct stat st{};
    bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 07777) == 0600 &&
              st.st_uid == ::geteuid() && st.st_nlink == 1 && st.st_size == static_cast<off_t>(bytes.size());
    std::string actual(bytes.size(), '\0');
    std::size_t done = 0;
    while (ok && done < actual.size()) {
        const auto n = ::read(fd, actual.data() + done, actual.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (!ok || actual != bytes) {
        error = "artifact contents, ownership or mode changed";
        return false;
    }
    return write_recovered_artifact(path.string() + ".consumed", cg::plaza2_sha256_hex(bytes) + "\n", error);
}
} // namespace moex::plaza2_trade
