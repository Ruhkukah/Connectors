#include "adapters/alorengine_capi/moex_c_api.h"
#include "moex_core/phase0_core.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

#define CHECK_SIZE_AND_ALIGN(name, type)                                                                               \
    do {                                                                                                               \
        if (moex_sizeof_##name() != sizeof(type)) {                                                                    \
            std::cerr << #type " size mismatch\n";                                                                     \
            return EXIT_FAILURE;                                                                                       \
        }                                                                                                              \
        if (moex_alignof_##name() != alignof(type)) {                                                                  \
            std::cerr << #type " alignment mismatch\n";                                                                \
            return EXIT_FAILURE;                                                                                       \
        }                                                                                                              \
    } while (false)

int main() {
    const auto info = moex::phase0::build_info();
    if (info.project_version.empty()) {
        std::cerr << "project version is empty\n";
        return EXIT_FAILURE;
    }

    if (moex_phase0_abi_version() != MOEX_C_ABI_VERSION) {
        std::cerr << "ABI version mismatch\n";
        return EXIT_FAILURE;
    }

    if (moex_prod_requires_explicit_arm() == 0U) {
        std::cerr << "prod explicit arm policy mismatch\n";
        return EXIT_FAILURE;
    }

    if (moex_environment_start_allowed("prod", 0U) != 0U) {
        std::cerr << "prod arming gate failed\n";
        return EXIT_FAILURE;
    }

    if (moex_environment_start_allowed("test", 0U) == 0U) {
        std::cerr << "test profile incorrectly rejected\n";
        return EXIT_FAILURE;
    }

    CHECK_SIZE_AND_ALIGN(event_header, MoexEventHeader);
    CHECK_SIZE_AND_ALIGN(backpressure_counters, MoexBackpressureCounters);
    CHECK_SIZE_AND_ALIGN(health_snapshot, MoexHealthSnapshot);
    CHECK_SIZE_AND_ALIGN(connector_create_params, MoexConnectorCreateParams);
    CHECK_SIZE_AND_ALIGN(profile_load_params, MoexProfileLoadParams);
    CHECK_SIZE_AND_ALIGN(order_submit_request, MoexOrderSubmitRequest);
    CHECK_SIZE_AND_ALIGN(order_cancel_request, MoexOrderCancelRequest);
    CHECK_SIZE_AND_ALIGN(order_replace_request, MoexOrderReplaceRequest);
    CHECK_SIZE_AND_ALIGN(mass_cancel_request, MoexMassCancelRequest);
    CHECK_SIZE_AND_ALIGN(subscription_request, MoexSubscriptionRequest);
    CHECK_SIZE_AND_ALIGN(polled_event, MoexPolledEvent);
    CHECK_SIZE_AND_ALIGN(plaza2_private_connector_health, MoexPlaza2PrivateConnectorHealth);
    CHECK_SIZE_AND_ALIGN(plaza2_resume_markers, MoexPlaza2ResumeMarkers);
    CHECK_SIZE_AND_ALIGN(plaza2_stream_health_item, MoexPlaza2StreamHealthItem);
    CHECK_SIZE_AND_ALIGN(plaza2_trading_session_item, MoexPlaza2TradingSessionItem);
    CHECK_SIZE_AND_ALIGN(plaza2_instrument_item, MoexPlaza2InstrumentItem);
    CHECK_SIZE_AND_ALIGN(plaza2_matching_map_item, MoexPlaza2MatchingMapItem);
    CHECK_SIZE_AND_ALIGN(plaza2_limit_item, MoexPlaza2LimitItem);
    CHECK_SIZE_AND_ALIGN(plaza2_position_item, MoexPlaza2PositionItem);
    CHECK_SIZE_AND_ALIGN(plaza2_own_order_item, MoexPlaza2OwnOrderItem);
    CHECK_SIZE_AND_ALIGN(plaza2_own_trade_item, MoexPlaza2OwnTradeItem);
    CHECK_SIZE_AND_ALIGN(plaza2_twime_reconciler_health, MoexPlaza2TwimeReconcilerHealth);
    CHECK_SIZE_AND_ALIGN(plaza2_reconciled_order_item, MoexPlaza2ReconciledOrderItem);
    CHECK_SIZE_AND_ALIGN(plaza2_reconciled_trade_item, MoexPlaza2ReconciledTradeItem);
    CHECK_SIZE_AND_ALIGN(plaza2_private_connector_health, MoexPlaza2PrivateConnectorHealth);
    CHECK_SIZE_AND_ALIGN(plaza2_resume_markers, MoexPlaza2ResumeMarkers);
    CHECK_SIZE_AND_ALIGN(plaza2_stream_health_item, MoexPlaza2StreamHealthItem);
    CHECK_SIZE_AND_ALIGN(plaza2_trading_session_item, MoexPlaza2TradingSessionItem);
    CHECK_SIZE_AND_ALIGN(plaza2_instrument_item, MoexPlaza2InstrumentItem);
    CHECK_SIZE_AND_ALIGN(plaza2_matching_map_item, MoexPlaza2MatchingMapItem);
    CHECK_SIZE_AND_ALIGN(plaza2_limit_item, MoexPlaza2LimitItem);
    CHECK_SIZE_AND_ALIGN(plaza2_position_item, MoexPlaza2PositionItem);
    CHECK_SIZE_AND_ALIGN(plaza2_own_order_item, MoexPlaza2OwnOrderItem);
    CHECK_SIZE_AND_ALIGN(plaza2_own_trade_item, MoexPlaza2OwnTradeItem);
    CHECK_SIZE_AND_ALIGN(plaza2_twime_reconciler_health, MoexPlaza2TwimeReconcilerHealth);
    CHECK_SIZE_AND_ALIGN(plaza2_reconciled_order_item, MoexPlaza2ReconciledOrderItem);
    CHECK_SIZE_AND_ALIGN(plaza2_reconciled_trade_item, MoexPlaza2ReconciledTradeItem);

    MoexConnectorCreateParams create_params{};
    create_params.struct_size = sizeof(MoexConnectorCreateParams);
    create_params.abi_version = MOEX_C_ABI_VERSION;
    create_params.connector_name = "selfcheck";
    create_params.instance_id = "phase0";

    MoexConnectorHandle handle = nullptr;
    if (moex_create_connector(&create_params, &handle) != MOEX_RESULT_OK || handle == nullptr) {
        std::cerr << "create connector failed\n";
        return EXIT_FAILURE;
    }

    MoexProfileLoadParams profile_params{};
    profile_params.struct_size = sizeof(MoexProfileLoadParams);
    profile_params.abi_version = MOEX_C_ABI_VERSION;
    profile_params.profile_path = "profiles/replay.yaml";
    profile_params.armed = 0U;
    if (moex_load_profile(handle, &profile_params) != MOEX_RESULT_OK) {
        std::cerr << "load profile failed\n";
        return EXIT_FAILURE;
    }

    const auto replay_path = info.source_root + "/tests/fixtures/shadow_replay/synthetic_replay.txt";
    if (moex_load_synthetic_replay(handle, replay_path.c_str()) != MOEX_RESULT_OK) {
        std::cerr << "load replay failed\n";
        return EXIT_FAILURE;
    }

    if (moex_start_connector(handle) != MOEX_RESULT_OK) {
        std::cerr << "start connector failed\n";
        return EXIT_FAILURE;
    }

    MoexPolledEvent too_small_buffer{};
    std::uint32_t written = 0;
    if (moex_poll_events_v2(handle, &too_small_buffer, sizeof(MoexPolledEvent) - 1U, 1U, &written) !=
        MOEX_RESULT_INVALID_ARGUMENT) {
        std::cerr << "poll v2 small stride check failed\n";
        return EXIT_FAILURE;
    }

    if (moex_stop_connector(handle) != MOEX_RESULT_OK) {
        std::cerr << "stop connector failed\n";
        return EXIT_FAILURE;
    }

    if (moex_destroy_connector(handle) != MOEX_RESULT_OK) {
        std::cerr << "destroy connector failed\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#undef CHECK_SIZE_AND_ALIGN
