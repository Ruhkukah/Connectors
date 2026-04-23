add_library(moex_twime_trade STATIC
    src/twime_cert_scenario_runner.cpp
    src/twime_fake_transport.cpp
    src/twime_live_session_runner.cpp
    src/twime_manual_operator_gate.cpp
    src/twime_rate_limit_model.cpp
    src/twime_recovery_state.cpp
    src/twime_sequence_state.cpp
    src/twime_session_health.cpp
    src/twime_session_metrics.cpp
    src/twime_session_persistence.cpp
    src/twime_session.cpp
    src/transport/twime_loopback_transport.cpp
    src/transport/twime_credential_provider.cpp
    src/transport/twime_credential_redaction.cpp
    src/transport/twime_endpoint_resolver.cpp
    src/transport/twime_reconnect_policy.cpp
    src/transport/twime_scripted_transport.cpp
    src/transport/twime_socket_handle.cpp
    src/transport/twime_tcp_endpoint.cpp
    src/transport/twime_test_network_gate.cpp
    src/transport/twime_tcp_transport.cpp)

set_target_properties(moex_twime_trade PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

target_include_directories(moex_twime_trade
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/include")

target_link_libraries(moex_twime_trade
    PUBLIC
        moex_twime_sbe
        moex_phase0_core)

target_compile_features(moex_twime_trade PUBLIC cxx_std_20)
