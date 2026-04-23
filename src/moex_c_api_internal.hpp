#pragma once

#include "adapters/alorengine_capi/moex_c_api.h"
#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include "moex/plaza2_twime_reconciler/plaza2_twime_reconciler.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct MoexHandleTag {
    std::string connector_name;
    std::string instance_id;
    std::string profile_path;
    std::string replay_path;
    bool started = false;
    bool profile_loaded = false;
    bool prod_armed = false;
    bool shadow_mode_enabled = true;
    bool replay_loaded = false;
    bool started_callback_emitted = false;
    MoexLowRateCallback low_rate_callback = nullptr;
    void* low_rate_user_data = nullptr;
    MoexBackpressureCounters counters{};
    std::vector<MoexPolledEvent> replay_events;
    std::size_t next_replay_index = 0;
    std::unique_ptr<moex::plaza2::private_state::Plaza2PrivateStateProjector> plaza2_private_state;
    std::unique_ptr<moex::plaza2_twime_reconciler::Plaza2TwimeReconciler> plaza2_twime_reconciler;
};

namespace moex::capi_internal {

MoexResult install_private_state_projector(MoexConnectorHandle handle,
                                           plaza2::private_state::Plaza2PrivateStateProjector projector);
MoexResult install_reconciler_snapshot(MoexConnectorHandle handle,
                                       plaza2_twime_reconciler::Plaza2TwimeReconciler reconciler);

} // namespace moex::capi_internal
