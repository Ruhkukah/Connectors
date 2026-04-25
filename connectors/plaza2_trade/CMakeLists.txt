add_library(moex_plaza2_trade_offline_codec STATIC
    src/plaza2_trade_codec.cpp
    src/plaza2_trade_fake_session.cpp)

target_include_directories(moex_plaza2_trade_offline_codec
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_link_libraries(moex_plaza2_trade_offline_codec
    PUBLIC
        moex_plaza2_cgate_fake_engine)

target_compile_features(moex_plaza2_trade_offline_codec PUBLIC cxx_std_20)

add_library(moex_plaza2_trade_test_order_entry STATIC
    src/plaza2_trade_test_order_runner.cpp)

target_include_directories(moex_plaza2_trade_test_order_entry
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_link_libraries(moex_plaza2_trade_test_order_entry
    PUBLIC
        moex_plaza2_trade_offline_codec
        moex_plaza2_cgate_aggr20_md)

target_compile_features(moex_plaza2_trade_test_order_entry PUBLIC cxx_std_20)
