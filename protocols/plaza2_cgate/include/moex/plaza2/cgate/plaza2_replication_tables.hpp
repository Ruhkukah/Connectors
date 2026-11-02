#pragma once

#include "moex/plaza2/cgate/plaza2_metadata.hpp"
#include <stdexcept>
#include <string>
#include <string_view>

namespace moex::plaza2::cgate {

// cg_lsn_new tables= restricts the server schema (CGate manual 2.6.1).
// Keep subscription selection and OPEN's required-table checks identical.
// PART balances are not projected; its sys_events are consumed.
[[nodiscard]] constexpr std::string_view required_replication_tables(generated::StreamCode stream) {
    using enum generated::StreamCode;
    switch (stream) {
    case kFortsTradeRepl:
        return "orders_log,user_deal,heartbeat,sys_events";
    case kFortsUserorderbookRepl:
        return "orders,info";
    case kFortsPosRepl:
        return "position,info";
    case kFortsPartRepl:
        return "sys_events";
    case kFortsRefdataRepl:
        return "session,fut_instruments,fut_sess_contents,fut_vcb,sys_messages";
    case kFortsSessionstateRepl:
        return "session_state";
    case kFortsInstrumentstateRepl:
        return "instrument_state,sys_events";
    case kFortsAggrRepl:
        return "orders_aggr,sys_events";
    case kFortsDealsRepl:
        return "deal";
    default:
        return {};
    }
}

[[nodiscard]] inline std::string required_replication_listener_settings(generated::StreamCode stream) {
    const auto tables = required_replication_tables(stream);
    const auto* descriptor = generated::FindStreamByCode(stream);
    if (!descriptor || tables.empty())
        throw std::invalid_argument("no reviewed replication table selection for stream");
    // The generated inventory names the AGGR family, not the selected depth.
    const auto name = stream == generated::StreamCode::kFortsAggrRepl ? std::string_view{"FORTS_AGGR20_REPL"}
                                                                      : descriptor->stream_name;
    return "p2repl://" + std::string(name) + ";tables=" + std::string(tables);
}

} // namespace moex::plaza2::cgate
