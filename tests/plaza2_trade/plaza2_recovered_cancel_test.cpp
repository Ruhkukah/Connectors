#include "moex/plaza2_trade/plaza2_recovered_cancel.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

using namespace moex::plaza2_trade;
namespace ps = moex::plaza2::private_state;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        const RecoveredOrderKey key{.epoch = "epoch-1",
                                    .account = "BRK1C01",
                                    .isin_id = 1001,
                                    .sess_id = 321,
                                    .ext_id = 79,
                                    .side = Plaza2TradeSide::Sell,
                                    .price = "103000",
                                    .quantity = 2};
        ps::OwnOrderSnapshot row;
        row.public_order_id = row.private_order_id = 20003;
        row.ext_id = 79;
        row.client_code = key.account;
        row.isin_id = 1001;
        row.sess_id = 321;
        row.dir = 2;
        row.price = "103000";
        row.public_amount = row.private_amount = 2;
        row.public_amount_rest = row.private_amount_rest = 2;
        row.public_action = row.private_action = 1;
        row.from_trade_repl = true;
        row.trade_repl_commit_sequence = 3;
        const auto inspect = [&](auto rows, auto trades, bool fresh = true) {
            return reconcile_recovered_order(key, 2, fresh, rows, trades);
        };
        const std::vector<ps::OwnTradeSnapshot> no_trades;
        require(inspect(std::array{row}, no_trades).outcome == RecoveredOrderOutcome::ExactlyOneWorkingMatch,
                "Add reply missing: exact ext/account/instrument/session/ordinary exchange id recovers Working");
        auto known = key;
        known.known_exchange_id = 20003;
        require(reconcile_recovered_order(known, 2, true, std::array{row}, no_trades).outcome ==
                    RecoveredOrderOutcome::ExactlyOneWorkingMatch,
                "previous Working reply id matches");
        known.known_exchange_id = 20004;
        require(reconcile_recovered_order(known, 2, true, std::array{row}, no_trades).outcome ==
                    RecoveredOrderOutcome::IdentityConflict,
                "strong known exchange identity cannot be replaced by ext id");
        require(inspect(std::array{row, row}, no_trades).outcome == RecoveredOrderOutcome::MultipleMatches,
                "duplicates ambiguous");
        require(inspect(std::vector<ps::OwnOrderSnapshot>{}, no_trades).outcome == RecoveredOrderOutcome::NoMatch,
                "fresh absence alone is not terminal proof and sends nothing");
        require(inspect(std::array{row}, no_trades, false).outcome == RecoveredOrderOutcome::PrivateStreamNotCurrent,
                "fresh streams mandatory");
        auto other = row;
        other.ext_id = 80;
        require(inspect(std::array{other}, no_trades).outcome == RecoveredOrderOutcome::NoMatch,
                "lookalike price side quantity does not match");
        other = row;
        other.isin_id = 999;
        require(inspect(std::array{other}, no_trades).outcome == RecoveredOrderOutcome::IdentityConflict,
                "wrong numeric target");
        other = row;
        other.public_order_id = 123;
        require(inspect(std::array{other}, no_trades).outcome == RecoveredOrderOutcome::IdentityConflict,
                "ordinary order ID split rejected");
        other = row;
        other.identity_conflict = true;
        require(inspect(std::array{other}, no_trades).outcome == RecoveredOrderOutcome::IdentityConflict,
                "projector conflict retained");
        other = row;
        other.public_action = other.private_action = 0;
        other.public_amount_rest = other.private_amount_rest = 0;
        require(inspect(std::array{other}, no_trades).outcome == RecoveredOrderOutcome::TerminalAlready,
                "positive cancellation proves terminal after lost reply");
        ps::OwnTradeSnapshot trade;
        trade.id_deal = 9001;
        trade.isin_id = 1001;
        trade.sess_id = 321;
        trade.amount = 1;
        trade.price = "103000";
        trade.public_order_id_sell = trade.private_order_id_sell = 20003;
        trade.ext_id_sell = 79;
        trade.code_sell = key.account;
        other = row;
        other.public_amount_rest = other.private_amount_rest = 1;
        other.public_action = other.private_action = 2;
        const auto partial = inspect(std::array{other}, std::array{trade});
        require(partial.outcome == RecoveredOrderOutcome::ExactlyOneWorkingMatch &&
                    partial.observation->remaining_quantity == 1 && partial.observation->executed_quantity == 1,
                "partial fill binds remaining quantity");
        require(partial.evidence_sha256 != inspect(std::array{row}, no_trades).evidence_sha256,
                "changed remaining invalidates authorization");
        other.public_amount_rest = other.private_amount_rest = 0;
        trade.amount = 2;
        require(inspect(std::array{other}, std::array{trade}).outcome == RecoveredOrderOutcome::TerminalAlready,
                "full exact deal proof during outage");
        require(inspect(std::array{other}, no_trades).outcome == RecoveredOrderOutcome::Ambiguous,
                "zero quantity without fill proof is ambiguous");
        trade.public_order_id_sell = 20099;
        require(inspect(std::array{other}, std::array{trade}).outcome == RecoveredOrderOutcome::IdentityConflict,
                "deal with colliding ext cannot override order ids");
        require(reconcile_recovered_order(key, 3, true, std::array{row}, no_trades).evidence_sha256 !=
                    inspect(std::array{row}, no_trades).evidence_sha256,
                "generation changes evidence hash");

        const auto root = std::filesystem::temp_directory_path() / ("recovered-cancel-" + std::to_string(::getpid()));
        require(std::filesystem::create_directory(root), "exclusive test directory");
        const auto file = root / "approval.json";
        std::string error;
        require(write_recovered_artifact(file, "exact-plan", error), "exclusive plan write");
        struct stat st;
        require(::stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "private file mode");
        require(!write_recovered_artifact(file, "replacement", error), "cannot overwrite authority");
        require(!consume_recovered_artifact(file, "different-plan", error), "exact contents required");
        require(consume_recovered_artifact(file, "exact-plan", error), "consume exact one-shot authority");
        require(!consume_recovered_artifact(file, "exact-plan", error), "cannot replay consumption");
        const auto symlink = root / "link";
        std::filesystem::create_symlink(file, symlink);
        require(!consume_recovered_artifact(symlink, "exact-plan", error), "symlink refused");
        const auto insecure = root / "insecure";
        require(write_recovered_artifact(insecure, "plan", error), "second test artifact");
        ::chmod(insecure.c_str(), 0644);
        require(!consume_recovered_artifact(insecure, "plan", error), "insecure permissions refused");
        std::filesystem::remove_all(root);
        std::cout << "recovered cancel model and artifact regressions passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
