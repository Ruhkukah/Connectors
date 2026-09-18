#pragma once

#include "moex/connector_host/connector_host.hpp"

#include <algorithm>
#include <array>

namespace moex::connector_host {

// Borrowed committed evidence, sampled together on the ConnectorHost owner
// thread. No order readiness, account state or persisted witness is inferred.
struct LateJoinDisplayEvidence {
    const plaza2::cgate::Plaza2Aggr20AuthoritySnapshot& authority;
    const plaza2_trade::Plaza2TransportHealth& transport;
    std::span<const plaza2::private_state::StreamHealthSnapshot> streams;
    std::span<const plaza2::private_state::TradingSessionSnapshot> sessions;
    std::span<const plaza2::private_state::InstrumentSnapshot> instruments;
    std::array<std::optional<plaza2::private_state::SourceRowProvenance>, 3> provenance;
    std::optional<std::uint64_t> refdata_lifenum;
    std::int32_t session_id{};
    std::int64_t isin_id{};
    std::int64_t now_seconds{};
    bool healthy{false};
};

[[nodiscard]] inline bool market_data_identity_current(const LateJoinDisplayEvidence& e) {
    using plaza2::generated::StreamCode;
    using plaza2::generated::TableCode;
    if (!e.healthy || !e.transport.valid || e.transport.private_count > e.transport.private_streams.size() ||
        e.transport.private_count > e.transport.private_states.size() || e.transport.connection != 3 ||
        e.transport.aggr != 3 || e.authority.state == plaza2::cgate::Plaza2Aggr20AuthorityState::Recovering ||
        e.authority.current_recovery_error.has_value() || !e.authority.transport_active ||
        !e.authority.snapshot_complete || !e.authority.aggr_online || e.session_id <= 0 || e.isin_id <= 0 ||
        !e.refdata_lifenum)
        return false;

    for (const auto stream :
         {StreamCode::kFortsRefdataRepl, StreamCode::kFortsSessionstateRepl, StreamCode::kFortsInstrumentstateRepl}) {
        const auto live = std::find(e.transport.private_streams.begin(),
                                    e.transport.private_streams.begin() + e.transport.private_count, stream);
        if (live == e.transport.private_streams.begin() + e.transport.private_count ||
            e.transport.private_states[static_cast<std::size_t>(live - e.transport.private_streams.begin())] != 3)
            return false;
        const auto state = std::find_if(e.streams.begin(), e.streams.end(),
                                        [stream](const auto& s) { return s.stream_code == stream; });
        if (state == e.streams.end() || !state->online || !state->snapshot_complete)
            return false;
    }
    constexpr std::array tables{TableCode::kFortsRefdataReplFutInstruments, TableCode::kFortsRefdataReplFutSessContents,
                                TableCode::kFortsRefdataReplSession};
    for (std::size_t i = 0; i < tables.size(); ++i) {
        const auto& p = e.provenance[i];
        if (!p || !p->present || p->stream_code != StreamCode::kFortsRefdataRepl || p->table_code != tables[i] ||
            p->lifenum != *e.refdata_lifenum)
            return false;
    }

    // Require one unambiguous current session, including published evening
    // and morning windows. Old historical rows cannot corroborate a restart.
    const plaza2::private_state::TradingSessionSnapshot* current = nullptr;
    const auto in_window = [&](std::int64_t begin, std::int64_t end) {
        return begin > 0 && end > begin && begin <= e.now_seconds && e.now_seconds < end;
    };
    for (const auto& session : e.sessions) {
        if (!in_window(session.begin, session.end) &&
            !(session.eve_on && in_window(session.eve_begin, session.eve_end)) &&
            !(session.mon_on && in_window(session.mon_begin, session.mon_end)))
            continue;
        if (current)
            return false;
        current = &session;
    }
    // PLAZA II 9.9 tables 137 and 139 enumerate these public states.
    // Known scheduled/suspended/completed/auction/close-only states still
    // corroborate identity; they do not grant trading permission.
    constexpr std::array session_states{0, 1, 2, 4};
    constexpr std::array instrument_states{0, 1, 2, 4, 5, 6, 7, 8, 9};
    if (!current || current->sess_id != e.session_id || !current->has_current_status ||
        std::find(session_states.begin(), session_states.end(), current->current_status) == session_states.end())
        return false;
    // INSTRUMENTSTATE has no sess_id: bind its committed status through the
    // current REFDATA membership. Status-stream reset clears has_current_status
    // in the projector, and both status streams must complete fresh snapshots.
    const auto instrument =
        std::find_if(e.instruments.begin(), e.instruments.end(), [&](const auto& i) { return i.isin_id == e.isin_id; });
    return instrument != e.instruments.end() && instrument->kind == plaza2::private_state::InstrumentKind::kFuture &&
           instrument->sess_id == e.session_id && instrument->current_session_member &&
           instrument->has_current_status && instrument->current_status_refdata_bound &&
           std::find(instrument_states.begin(), instrument_states.end(), instrument->current_status) !=
               instrument_states.end() &&
           instrument->trade_mode_id != 0;
}

[[nodiscard]] inline bool
ready_witness_matches(const std::optional<plaza2::cgate::Plaza2Aggr20SysEventSnapshot>& witness,
                      std::int32_t session_id, bool snapshot) {
    return witness && session_id > 0 && witness->sess_id == session_id && witness->event_type == 1 &&
           witness->message == "session_data_ready" && witness->source_repl_act == 0 &&
           witness->seen_during_snapshot == snapshot;
}

[[nodiscard]] inline bool late_join_display_corroborated(const LateJoinDisplayEvidence& e) {
    return market_data_identity_current(e) &&
           ready_witness_matches(e.authority.snapshot_ready_witness, e.session_id, true);
}

inline void apply_market_data_display_authority(ConnectorHostMarketDataSnapshot& out,
                                                const plaza2::cgate::Plaza2Aggr20AuthoritySnapshot& authority,
                                                bool healthy, bool identity_current, std::int32_t session_id,
                                                bool target_snapshot_present) {
    using plaza2::cgate::SessionReadyWitnessKind;
    healthy = healthy && authority.state != plaza2::cgate::Plaza2Aggr20AuthorityState::Recovering &&
              !authority.current_recovery_error;
    const bool current = healthy && identity_current && out.transport_active && out.snapshot_complete &&
                         out.aggr_online && target_snapshot_present;
    const bool online = ready_witness_matches(authority.online_ready_witness, session_id, false);
    const bool corroborated = current && ready_witness_matches(authority.snapshot_ready_witness, session_id, true);
    out.source_consistent = current && online && out.session_data_ready && authority.target_authoritative;
    out.target_authoritative = out.source_consistent && out.refdata_metadata_current;
    out.session_ready_witness.reset();
    out.session_ready_witness_kind = SessionReadyWitnessKind::None;
    if (out.source_consistent && authority.online_ready_witness) {
        out.session_ready_witness = authority.online_ready_witness;
        out.session_ready_witness_kind = SessionReadyWitnessKind::OnlineSynchronousEvent;
    } else if (healthy && corroborated && target_snapshot_present) {
        out.session_ready_witness = authority.snapshot_ready_witness;
        out.session_ready_witness_kind = SessionReadyWitnessKind::LateJoinCorroboratedSnapshot;
    }
    // PersistedOnlineWitness is unavailable until a real durable witness
    // write/read/revalidation path exists. A snapshot never fabricates it.
    out.book_snapshot_current = out.source_consistent || (healthy && corroborated && target_snapshot_present);
    out.market_data_display_allowed = out.book_snapshot_current && out.refdata_metadata_current;
    out.market_data_live = out.book_snapshot_current;
    out.valid = out.market_data_display_allowed;
    out.order_entry_allowed = false;
}

} // namespace moex::connector_host
