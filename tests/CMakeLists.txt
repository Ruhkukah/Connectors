find_package(Threads REQUIRED)

add_test(NAME reconstruct_orders_test COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/reconstruct_orders_test.py" "${MOEX_SOURCE_ROOT}/scripts/reconstruct_orders.py")

add_library(plaza2_runtime_test_support STATIC plaza2_cgate/plaza2_runtime_test_support.cpp)
target_link_libraries(plaza2_runtime_test_support PUBLIC moex_plaza2_cgate_runtime)
target_include_directories(plaza2_runtime_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/plaza2_cgate")
target_compile_definitions(plaza2_runtime_test_support PRIVATE MOEX_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")

add_library(plaza2_fake_cgate_runtime SHARED plaza2_cgate/fake_cgate_runtime.cpp)
target_link_libraries(plaza2_fake_cgate_runtime PRIVATE moex_plaza2_cgate_metadata)
target_include_directories(plaza2_fake_cgate_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support"
    "${MOEX_SOURCE_ROOT}/protocols/plaza2_cgate/include")
set_target_properties(plaza2_fake_cgate_runtime PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests"
    RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")

function(moex_test_target name source)
    add_executable(${name} "${source}")
    target_link_libraries(${name} PRIVATE ${ARGN})
    target_compile_definitions(${name} PRIVATE MOEX_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/plaza2_trade" "${MOEX_SOURCE_ROOT}/apps")
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
endfunction()

function(moex_test name source)
    moex_test_target(${name} "${source}" ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "unit;plaza2;sanitizer")
endfunction()

function(moex_runtime_test name source)
    moex_test_target(${name} "${source}" plaza2_runtime_test_support ${ARGN})
    add_dependencies(${name} plaza2_fake_cgate_runtime)
    add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:plaza2_fake_cgate_runtime>")
    set_tests_properties(${name} PROPERTIES LABELS "integration;plaza2;sanitizer")
endfunction()

foreach(name IN ITEMS plaza2_runtime_probe_test
        plaza2_runtime_adapter_test plaza2_runtime_listener_test plaza2_schema_contract_test)
    moex_runtime_test(${name} plaza2_cgate/${name}.cpp)
endforeach()
moex_runtime_test(connector_host_test connector_host_test.cpp moex_connector_host)

moex_test(plaza2_generated_metadata_test plaza2_cgate/plaza2_generated_metadata_test.cpp moex_plaza2_cgate_metadata)
moex_test(plaza2_fake_engine_scenario_test plaza2_cgate/plaza2_fake_engine_scenario_test.cpp moex_plaza2_cgate_fake_scenarios)
moex_test(plaza2_fake_engine_rules_test plaza2_cgate/plaza2_fake_engine_rules_test.cpp moex_plaza2_cgate_fake_engine)
foreach(name IN ITEMS plaza2_private_state_projection_test plaza2_private_state_visibility_test
        plaza2_private_state_invalidation_test plaza2_private_state_provenance_test)
    moex_test(${name} plaza2_cgate/${name}.cpp moex_plaza2_cgate_private_state moex_plaza2_cgate_fake_scenarios)
endforeach()
foreach(name IN ITEMS plaza2_aggr20_md_validation_test plaza2_aggr20_md_projection_test
        plaza2_aggr20_md_optimization_test)
    moex_runtime_test(${name} plaza2_cgate/${name}.cpp moex_plaza2_cgate_aggr20_md)
endforeach()
add_executable(plaza2_aggr20_md_benchmark plaza2_cgate/plaza2_aggr20_md_benchmark.cpp)
target_link_libraries(plaza2_aggr20_md_benchmark PRIVATE moex_plaza2_cgate_aggr20_md)

foreach(name IN ITEMS plaza2_trade_command_validation_test plaza2_trade_command_encoding_test
        plaza2_trade_reply_decoding_test)
    moex_test(${name} plaza2_trade/${name}.cpp moex_plaza2_trade_codec)
endforeach()
foreach(name IN ITEMS plaza2_trade_fake_session_test plaza2_trade_fake_session_replication_test)
    moex_test(${name} plaza2_trade/${name}.cpp moex_plaza2_trade_fake_session moex_plaza2_cgate_private_state)
endforeach()
moex_test(plaza2_public_deals_test plaza2_cgate/plaza2_public_deals_test.cpp moex_plaza2_cgate_aggr20_md)
moex_test(plaza2_publisher_rate_test plaza2_cgate/plaza2_publisher_rate_test.cpp moex_plaza2_cgate_metadata)
foreach(name IN ITEMS connector_host_dtc_protocol_test connector_host_dtc_server_test connector_host_dtc_public_deals_test)
    moex_test(${name} ${name}.cpp moex_connector_host)
    set_tests_properties(${name} PROPERTIES LABELS "dtc;read-only;sanitizer")
endforeach()
add_executable(plaza2_text_test plaza2_cgate/plaza2_text_test.cpp)
target_link_libraries(plaza2_text_test PRIVATE moex_plaza2_cgate_aggr20_md)
add_test(NAME plaza2_text_roundtrip_test COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/plaza2_cgate/plaza2_text_roundtrip_test.py" "$<TARGET_FILE:plaza2_text_test>")
moex_test_target(moex_connector_host_dtc_runner_receipt_test dtc_runner_receipt_test.cpp moex_connector_host)
target_compile_definitions(moex_connector_host_dtc_runner_receipt_test PRIVATE
    MOEX_SOURCE_GIT_SHA="${MOEX_SOURCE_GIT_SHA}" MOEX_BUILD_CONFIGURATION="$<CONFIG>"
    MOEX_CXX_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}" MOEX_CXX_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}")
add_test(NAME dtc_runner_receipt_json COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/dtc_runner_receipt_test.py" "$<TARGET_FILE:moex_connector_host_dtc_runner_receipt_test>")

foreach(name IN ITEMS order_manager_test event_journal_test)
    moex_test(${name} ${name}.cpp moex_connector_host)
endforeach()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/trading_host_test.cpp")
    moex_runtime_test(trading_host_test trading_host_test.cpp moex_connector_host)
endif()
moex_test_target(trading_day_test trading_day_test.cpp plaza2_runtime_test_support moex_connector_host)
add_dependencies(trading_day_test plaza2_fake_cgate_runtime)
add_test(NAME trading_day_test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/trading_day_test.py"
    "$<TARGET_FILE:trading_day_test>" "$<TARGET_FILE:plaza2_fake_cgate_runtime>"
    "${MOEX_SOURCE_ROOT}/scripts/reconstruct_orders.py")
set_tests_properties(trading_day_test PROPERTIES LABELS "integration;plaza2;sanitizer" TIMEOUT 60)

moex_runtime_test(plaza2_session_recovery_test plaza2_cgate/plaza2_session_recovery_test.cpp moex_plaza2_trade_session)
if(MOEX_BUILD_APPS)
    moex_test_target(moexctl_command_channel_test moexctl_command_channel_test.cpp plaza2_runtime_test_support)
    target_compile_definitions(moexctl_command_channel_test PRIVATE MOEX_SOURCE_GIT_SHA="${MOEX_SOURCE_GIT_SHA}")
    add_dependencies(moexctl_command_channel_test moexctl plaza2_fake_cgate_runtime)
    add_test(NAME moexctl_command_channel_test COMMAND moexctl_command_channel_test
        "$<TARGET_FILE:moexctl>" "$<TARGET_FILE:plaza2_fake_cgate_runtime>")
    set_tests_properties(moexctl_command_channel_test PROPERTIES LABELS "integration;plaza2;sanitizer" TIMEOUT 90)
endif()
moex_test(plaza2_private_state_transaction_perf_test plaza2_cgate/plaza2_private_state_transaction_perf_test.cpp
    moex_plaza2_cgate_bridge)
target_compile_definitions(plaza2_private_state_transaction_perf_test PRIVATE
    MOEX_RELEASE_PERFORMANCE_ACCEPTANCE=$<CONFIG:Release>)

add_executable(plaza2_full_order_log_test plaza2_cgate/plaza2_full_order_log_test.cpp)
target_link_libraries(plaza2_full_order_log_test PRIVATE moex_plaza2_cgate_full_order_log)
add_test(NAME plaza2_full_order_log_test COMMAND plaza2_full_order_log_test)

moex_runtime_test(plaza2_full_order_log_session_test plaza2_cgate/plaza2_full_order_log_session_test.cpp moex_plaza2_trade_session moex_plaza2_cgate_full_order_log)
