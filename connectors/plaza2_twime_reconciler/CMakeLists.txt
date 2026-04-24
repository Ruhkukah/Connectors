add_library(moex_plaza2_twime_reconciler STATIC
    src/plaza2_twime_reconciler.cpp)

set_target_properties(moex_plaza2_twime_reconciler PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

target_link_libraries(moex_plaza2_twime_reconciler
    PUBLIC
        moex_plaza2_cgate_private_state
        moex_twime_trade)

target_include_directories(moex_plaza2_twime_reconciler
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_features(moex_plaza2_twime_reconciler PUBLIC cxx_std_20)

add_library(moex_plaza2_twime_integrated_test STATIC
    src/plaza2_twime_integrated_test_runner.cpp)

set_target_properties(moex_plaza2_twime_integrated_test PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

target_link_libraries(moex_plaza2_twime_integrated_test
    PUBLIC
        moex_plaza2_cgate_live_session
        moex_plaza2_twime_reconciler
        moex_phase0_core)

target_include_directories(moex_plaza2_twime_integrated_test
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

target_compile_features(moex_plaza2_twime_integrated_test PUBLIC cxx_std_20)
