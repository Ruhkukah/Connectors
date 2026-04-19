#include "moex/twime_trade/twime_session_persistence.hpp"

#include "twime_live_session_test_support.hpp"

#include <iostream>

int main() {
    try {
        using namespace moex::twime_trade;

        const auto root = moex::twime_trade::test::temp_state_dir("twime_session_persistence");
        TwimeFileSessionPersistenceStore store(root);

        TwimeSessionPersistenceSnapshot snapshot;
        snapshot.recovery_state.next_outbound_seq = 12;
        snapshot.recovery_state.next_expected_inbound_seq = 21;
        snapshot.recovery_state.last_establishment_id = 999;
        snapshot.recovery_state.recovery_epoch = 5;
        snapshot.recovery_state.last_clean_shutdown = true;
        snapshot.reject_seen = true;
        snapshot.last_reject_code = 7;
        snapshot.active_keepalive_interval_ms = 1500;
        snapshot.reconnect_attempt_count = 2;
        snapshot.last_transition_time_ms = 4000;
        store.save("phase2f_persist", snapshot);

        const auto loaded = store.load("phase2f_persist");
        moex::twime_sbe::test::require(loaded.has_value(), "persistence store must reload saved snapshot");
        moex::twime_sbe::test::require(loaded->recovery_state.last_clean_shutdown,
                                       "clean shutdown flag must persist");
        moex::twime_sbe::test::require(loaded->last_reject_code == 7, "reject code must persist");
        moex::twime_sbe::test::require(loaded->recovery_state.next_expected_inbound_seq == 21,
                                       "sequence state must persist");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
