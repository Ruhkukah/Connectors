add_library(moex_twime_trade STATIC
    src/twime_cert_scenario_runner.cpp
    src/twime_fake_transport.cpp
    src/twime_rate_limit_model.cpp
    src/twime_recovery_state.cpp
    src/twime_sequence_state.cpp
    src/twime_session.cpp
    src/transport/twime_loopback_transport.cpp
    src/transport/twime_scripted_transport.cpp)

target_include_directories(moex_twime_trade
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(moex_twime_trade
    PUBLIC
        moex_twime_sbe)

target_compile_features(moex_twime_trade PUBLIC cxx_std_20)
