add_library(moex_connector_host STATIC
    src/connector_host.cpp
    src/dtc_market_data.cpp
    src/full_order_log_dtc.cpp
    src/dtc_read_only_server.cpp
    src/operator_config.cpp
    src/order_manager.cpp
    src/event_journal.cpp
    src/trading_host.cpp)
target_include_directories(moex_connector_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(moex_connector_host PUBLIC moex_plaza2_trade_session moex_plaza2_cgate_full_order_log)
target_compile_features(moex_connector_host PUBLIC cxx_std_20)
