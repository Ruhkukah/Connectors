add_executable(moex_phase0_selfcheck
    ${MOEX_SOURCE_ROOT}/tests/phase0_selfcheck.cpp)

target_link_libraries(moex_phase0_selfcheck
    PRIVATE
        moex_phase0_core)

set_property(TARGET moex_phase0_selfcheck PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")

add_executable(moex_phase4b_capi_read_side_test
    ${MOEX_SOURCE_ROOT}/tests/phase4b_capi_read_side_test.cpp)
target_link_libraries(moex_phase4b_capi_read_side_test
    PRIVATE
        moex_phase0_core
        moex_plaza2_cgate_fake_scenarios
        twime_test_support)
target_include_directories(moex_phase4b_capi_read_side_test
    PRIVATE
        "${MOEX_SOURCE_ROOT}/src"
        "${MOEX_SOURCE_ROOT}/tests/support"
        "${MOEX_SOURCE_ROOT}/tests/twime_sbe"
        "${MOEX_SOURCE_ROOT}/tests/twime_trade"
        "${MOEX_SOURCE_ROOT}/tests/plaza2_twime_reconciler")
target_compile_features(moex_phase4b_capi_read_side_test PRIVATE cxx_std_20)
target_compile_definitions(moex_phase4b_capi_read_side_test
    PRIVATE
        MOEX_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")
set_property(TARGET moex_phase4b_capi_read_side_test PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
add_test(
    NAME moex_phase4b_capi_read_side_test
    COMMAND "${MOEX_BUILD_ROOT}/tests/moex_phase4b_capi_read_side_test")

add_library(twime_test_support STATIC
    ${MOEX_SOURCE_ROOT}/tests/twime_sbe/twime_test_support.cpp)
target_link_libraries(twime_test_support
    PUBLIC
        moex_twime_sbe)
target_include_directories(twime_test_support
    PUBLIC
        "${MOEX_SOURCE_ROOT}/tests/twime_sbe")
target_compile_features(twime_test_support PUBLIC cxx_std_20)
target_compile_definitions(twime_test_support
    PRIVATE
        MOEX_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")

add_library(local_tcp_server_support STATIC
    ${MOEX_SOURCE_ROOT}/tests/support/local_tcp_server.cpp)
target_compile_features(local_tcp_server_support PUBLIC cxx_std_20)
target_include_directories(local_tcp_server_support
    PUBLIC
        "${MOEX_SOURCE_ROOT}/tests/support")

add_executable(local_tcp_server_stop_test
    ${MOEX_SOURCE_ROOT}/tests/support/local_tcp_server_stop_test.cpp)
target_link_libraries(local_tcp_server_stop_test
    PRIVATE
        local_tcp_server_support)
target_compile_features(local_tcp_server_stop_test PRIVATE cxx_std_20)
set_property(TARGET local_tcp_server_stop_test PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
add_test(
    NAME local_tcp_server_stop_test
    COMMAND "${MOEX_BUILD_ROOT}/tests/local_tcp_server_stop_test")

include("${MOEX_SOURCE_ROOT}/cmake/MoexTestCases.cmake")

moex_add_noarg_case_runner(twime_sbe_tests
    SOURCE_DIR tests/twime_sbe
    CASES
        twime_schema_inventory_test twime_header_codec_test twime_primitive_types_test
        twime_message_roundtrip_test twime_frame_assembler_test twime_decode_error_test
        twime_cert_log_formatter_test twime_enum_set_validation_test twime_optional_string_policy_test
    INCLUDE_DIRECTORIES
        "${MOEX_SOURCE_ROOT}/tests/support"
        "${MOEX_SOURCE_ROOT}/tests/twime_trade"
        "${MOEX_SOURCE_ROOT}/tests/twime_sbe"
    LIBRARIES
        twime_test_support
    LABELS unit twime sanitizer)

function(moex_add_twime_case_runner target_name source_dir)
    moex_add_noarg_case_runner(${target_name}
        SOURCE_DIR ${source_dir}
        CASES ${ARGN}
        INCLUDE_DIRECTORIES
        "${MOEX_SOURCE_ROOT}/tests/support"
        "${MOEX_SOURCE_ROOT}/tests/twime_trade"
        "${MOEX_SOURCE_ROOT}/tests/twime_sbe"
        LIBRARIES moex_twime_trade local_tcp_server_support twime_test_support
        LABELS unit twime sanitizer)
endfunction()

moex_add_twime_case_runner(twime_session_tests tests/twime_trade
    twime_session_establish_test twime_session_reject_test twime_keepalive_ack_test
    twime_heartbeat_fake_clock_test twime_heartbeat_rate_limit_test twime_reconnect_timing_test
    twime_terminate_test twime_sequence_gap_test twime_message_layer_rules_test
    twime_retransmit_request_test twime_retransmit_limit_test twime_retransmission_test
    twime_flood_reject_test twime_business_reject_test twime_cert_scenario_runner_test)

moex_add_twime_case_runner(twime_byte_transport_tests tests/twime_trade
    twime_loopback_transport_test=tests/twime_trade/transport/twime_loopback_transport_test.cpp
    twime_transport_partial_read_test=tests/twime_trade/transport/twime_transport_partial_read_test.cpp
    twime_transport_partial_write_test=tests/twime_trade/transport/twime_transport_partial_write_test.cpp
    twime_transport_remote_close_test=tests/twime_trade/transport/twime_transport_remote_close_test.cpp
    twime_transport_fault_injection_test=tests/twime_trade/transport/twime_transport_fault_injection_test.cpp
    twime_session_byte_stream_establish_test twime_session_byte_stream_fragmentation_test
    twime_session_byte_stream_batched_frames_test)

moex_add_twime_case_runner(twime_tcp_transport_tests tests/twime_trade/transport
    twime_tcp_transport_connect_test twime_tcp_transport_partial_read_test
    twime_tcp_transport_partial_write_test twime_tcp_transport_remote_close_test
    twime_tcp_transport_fault_test twime_tcp_transport_reconnect_policy_test
    twime_tcp_transport_loopback_server_test)

moex_add_twime_case_runner(twime_tcp_session_tests tests/twime_trade
    twime_session_tcp_establish_test twime_session_tcp_fragmentation_test
    twime_session_tcp_batched_frames_test twime_session_tcp_remote_close_test
    twime_session_tcp_terminate_flow_test)

moex_add_twime_case_runner(twime_endpoint_gate_tests tests/twime_trade
    twime_endpoint_resolver_test=tests/twime_trade/transport/twime_endpoint_resolver_test.cpp
    twime_test_network_gate_test=tests/twime_trade/transport/twime_test_network_gate_test.cpp
    twime_credential_provider_test=tests/twime_trade/transport/twime_credential_provider_test.cpp
    twime_external_profile_validation_test=tests/twime_trade/transport/twime_external_profile_validation_test.cpp
    twime_external_endpoint_blocking_test=tests/twime_trade/transport/twime_external_endpoint_blocking_test.cpp
    twime_external_endpoint_allow_test=tests/twime_trade/transport/twime_external_endpoint_allow_test.cpp
    twime_session_tcp_external_establish_test twime_manual_operator_gate_test
    twime_external_session_validation_test)

moex_add_twime_case_runner(twime_live_runner_tests tests/twime_trade
    twime_session_health_test twime_session_persistence_test twime_live_session_runner_test
    twime_live_session_reject_test twime_live_session_establish_timeout_test
    twime_live_session_reconnect_disabled_test twime_live_session_reconnect_enabled_test)

add_custom_target(twime_codegen_check
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/twime_codegen"
        --schema "${MOEX_SOURCE_ROOT}/protocols/twime_sbe/schema/twime_spectra-7.7.xml"
        --out "${MOEX_SOURCE_ROOT}/protocols/twime_sbe/generated"
        --check
    COMMENT "Verify deterministic TWIME generated metadata")

add_custom_target(plaza2_codegen_check
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/plaza2_codegen"
        --schema "${MOEX_SOURCE_ROOT}/protocols/plaza2_cgate/schema/plaza2_forts_reviewed.ini"
        --out "${MOEX_SOURCE_ROOT}/protocols/plaza2_cgate/generated"
        --check
    COMMENT "Verify deterministic PLAZA II generated metadata")

add_executable(twime_fixture_check_tool
    ${MOEX_SOURCE_ROOT}/tools/twime_fixture_check.cpp)
target_link_libraries(twime_fixture_check_tool
    PRIVATE
        twime_test_support)
target_compile_features(twime_fixture_check_tool PRIVATE cxx_std_20)
set_property(TARGET twime_fixture_check_tool PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_TOOLS_BUILD_DIR}")
set_target_properties(twime_fixture_check_tool PROPERTIES OUTPUT_NAME "twime_fixture_check")

add_test(
    NAME phase0_selfcheck
    COMMAND "${MOEX_BUILD_ROOT}/tests/moex_phase0_selfcheck")

add_test(
    NAME matrix_validate_repo
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/matrix_validate"
        --matrix-dir "${MOEX_SOURCE_ROOT}/matrix")

add_test(
    NAME unicode_guard_check
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/unicode_guard"
        .github
        CMakeLists.txt
        cmake
        include
        src
        protocols
        connectors
        tools
        tests
        matrix
        profiles
        cert
        docs
        scripts
        apps)
set_tests_properties(unicode_guard_check PROPERTIES
    WORKING_DIRECTORY "${MOEX_SOURCE_ROOT}")

add_test(
    NAME repo_style_check
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/repo_style_check")
set_tests_properties(repo_style_check PROPERTIES
    WORKING_DIRECTORY "${MOEX_SOURCE_ROOT}")

add_test(
    NAME repo_style_check_fixture_minified
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/repo_style_check"
        --paths ${MOEX_SOURCE_ROOT}/tests/fixtures/repo_style/minified_source.py
        --include-excluded)
set_tests_properties(repo_style_check_fixture_minified PROPERTIES
    WORKING_DIRECTORY "${MOEX_SOURCE_ROOT}"
    WILL_FAIL TRUE)

function(add_profile_check test_name profile_path)
    add_test(
        NAME ${test_name}
        COMMAND "${MOEX_TOOLS_BUILD_DIR}/profile_check"
            --profile "${MOEX_SOURCE_ROOT}/${profile_path}")
endfunction()

set(MOEX_VALID_PROFILE_CHECKS
    profile_check_replay profiles/replay.yaml
    profile_check_twime_tcp_loopback profiles/test_twime_tcp_loopback.yaml
    profile_check_twime_tcp_fragmentation profiles/test_twime_tcp_fragmentation.yaml
    profile_check_twime_tcp_remote_close profiles/test_twime_tcp_remote_close.yaml
    profile_check_twime_tcp_external_template profiles/test_twime_tcp_external.template.yaml
    profile_check_twime_tcp_external_local_example profiles/test_twime_tcp_external_local.example.yaml
    profile_check_twime_live_session_template profiles/test_twime_live_session.template.yaml
    profile_check_twime_live_session_local_example profiles/test_twime_live_session_local.example.yaml
    profile_check_plaza2_repl_live_session_template profiles/test_plaza2_repl_live_session.template.yaml
    profile_check_plaza2_repl_live_session_local_example profiles/test_plaza2_repl_live_session_local.example.yaml
    profile_check_plaza2_aggr20_md_template profiles/test_plaza2_aggr20_md.template.yaml
    profile_check_plaza2_aggr20_md_local_example profiles/test_plaza2_aggr20_md_local.example.yaml
    profile_check_plaza2_twime_integrated_template profiles/test_plaza2_twime_integrated_session.template.yaml
    profile_check_plaza2_twime_integrated_local_example profiles/test_plaza2_twime_integrated_session_local.example.yaml)
list(LENGTH MOEX_VALID_PROFILE_CHECKS MOEX_VALID_PROFILE_CHECKS_LENGTH)
math(EXPR MOEX_VALID_PROFILE_CHECKS_LAST "${MOEX_VALID_PROFILE_CHECKS_LENGTH} - 1")
foreach(index RANGE 0 ${MOEX_VALID_PROFILE_CHECKS_LAST} 2)
    math(EXPR path_index "${index} + 1")
    list(GET MOEX_VALID_PROFILE_CHECKS ${index} test_name)
    list(GET MOEX_VALID_PROFILE_CHECKS ${path_index} profile_path)
    add_profile_check(${test_name} ${profile_path})
endforeach()

set(MOEX_REJECTED_PROFILE_CHECKS
    profile_check_twime_tcp_external_fixture_real_host_fails tests/fixtures/profiles/twime_external_nonloopback_fixture.yaml
    profile_check_twime_live_session_fixture_real_host_fails tests/fixtures/profiles/twime_live_session_nonloopback_fixture.yaml
    profile_check_plaza2_repl_fixture_real_host_fails tests/fixtures/profiles/plaza2_repl_nonloopback_fixture.yaml
    profile_check_plaza2_twime_integrated_fixture_real_host_fails tests/fixtures/profiles/plaza2_twime_integrated_nonloopback_fixture.yaml)
list(LENGTH MOEX_REJECTED_PROFILE_CHECKS MOEX_REJECTED_PROFILE_CHECKS_LENGTH)
math(EXPR MOEX_REJECTED_PROFILE_CHECKS_LAST "${MOEX_REJECTED_PROFILE_CHECKS_LENGTH} - 1")
foreach(index RANGE 0 ${MOEX_REJECTED_PROFILE_CHECKS_LAST} 2)
    math(EXPR path_index "${index} + 1")
    list(GET MOEX_REJECTED_PROFILE_CHECKS ${index} test_name)
    list(GET MOEX_REJECTED_PROFILE_CHECKS ${path_index} profile_path)
    add_profile_check(${test_name} ${profile_path})
    set_tests_properties(${test_name} PROPERTIES WILL_FAIL TRUE)
endforeach()

add_test(
    NAME profile_check_prod_with_arm
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/profile_check"
        --profile "${MOEX_SOURCE_ROOT}/profiles/prod_fast_twime.template.yaml"
        --armed)

add_test(
    NAME profile_check_prod_requires_arm
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/profile_check"
        --profile "${MOEX_SOURCE_ROOT}/profiles/prod_fast_twime.template.yaml")
set_tests_properties(profile_check_prod_requires_arm PROPERTIES WILL_FAIL TRUE)

function(add_cert_scenario_test test_name scenario_path output_name)
    add_test(
        NAME ${test_name}
        COMMAND "${MOEX_APPS_BUILD_DIR}/moex_cert_runner"
            --scenario "${MOEX_SOURCE_ROOT}/${scenario_path}"
            --output-dir "${MOEX_TEST_OUTPUT_DIR}/${output_name}")
endfunction()

add_cert_scenario_test(cert_runner_stub cert/stub/phase0_stub.yaml cert-runner)
add_cert_scenario_test(twime_cert_runner_session_establish cert/scenarios/twime/session_establish.yaml twime-cert-runner)
add_cert_scenario_test(twime_cert_runner_client_sequence_heartbeat cert/scenarios/twime/client_sequence_heartbeat_null_nextseqno.yaml twime-cert-runner)
add_cert_scenario_test(twime_cert_runner_terminate_requires_inbound cert/scenarios/twime/terminate_requires_inbound_terminate.yaml twime-cert-runner)
add_cert_scenario_test(twime_cert_runner_message_counter_reset cert/scenarios/twime/message_counter_reset.yaml twime-cert-runner)

add_test(
    NAME twime_cert_runner_requires_test_network_arm_for_external_endpoint
    COMMAND "${MOEX_APPS_BUILD_DIR}/moex_cert_runner"
        --profile "${MOEX_SOURCE_ROOT}/tests/fixtures/profiles/twime_external_nonloopback_fixture.yaml"
        --output-dir "${MOEX_TEST_OUTPUT_DIR}/twime-external-runner"
        --validate-only)
set_tests_properties(twime_cert_runner_requires_test_network_arm_for_external_endpoint PROPERTIES WILL_FAIL TRUE)

add_test(
    NAME twime_cert_runner_requires_credentials_for_external_endpoint
    COMMAND "${MOEX_APPS_BUILD_DIR}/moex_cert_runner"
        --profile "${MOEX_SOURCE_ROOT}/tests/fixtures/profiles/twime_external_nonloopback_fixture.yaml"
        --output-dir "${MOEX_TEST_OUTPUT_DIR}/twime-external-runner"
        --armed-test-network
        --validate-only)
set_tests_properties(twime_cert_runner_requires_credentials_for_external_endpoint PROPERTIES WILL_FAIL TRUE)

add_test(
    NAME twime_cert_runner_redacts_credentials_in_output
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/assert_twime_cert_runner_redaction.py"
        "${MOEX_APPS_BUILD_DIR}/moex_cert_runner"
        "${MOEX_SOURCE_ROOT}/tests/fixtures/profiles/twime_external_nonloopback_fixture.yaml"
        "${MOEX_TEST_OUTPUT_DIR}/twime-external-runner-redaction"
        "MOEX_TWIME_TEST_CREDENTIALS")
add_test(
    NAME twime_cert_runner_requires_test_session_arm_for_external_endpoint
    COMMAND "${MOEX_APPS_BUILD_DIR}/moex_cert_runner"
        --profile "${MOEX_SOURCE_ROOT}/tests/fixtures/profiles/twime_live_session_nonloopback_fixture.yaml"
        --scenario "${MOEX_SOURCE_ROOT}/cert/scenarios/twime/live_test_session_establish.yaml"
        --output-dir "${MOEX_TEST_OUTPUT_DIR}/twime-live-session-runner"
        --armed-test-network)
set_tests_properties(twime_cert_runner_requires_test_session_arm_for_external_endpoint PROPERTIES WILL_FAIL TRUE)
add_test(
    NAME twime_cert_runner_requires_credentials_for_live_external_endpoint
    COMMAND "${MOEX_APPS_BUILD_DIR}/moex_cert_runner"
        --profile "${MOEX_SOURCE_ROOT}/tests/fixtures/profiles/twime_live_session_nonloopback_fixture.yaml"
        --scenario "${MOEX_SOURCE_ROOT}/cert/scenarios/twime/live_test_session_establish.yaml"
        --output-dir "${MOEX_TEST_OUTPUT_DIR}/twime-live-session-runner"
        --armed-test-network
        --armed-test-session)
set_tests_properties(twime_cert_runner_requires_credentials_for_live_external_endpoint PROPERTIES WILL_FAIL TRUE)

add_test(
    NAME spec_indexer_local_fixture
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/spec_indexer"
        --config "${MOEX_SOURCE_ROOT}/tests/fixtures/spec_indexer_config.json"
        --workspace "${MOEX_TEST_OUTPUT_DIR}/spec-indexer")

add_test(
    NAME spec_indexer_policy_fixture
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/spec_indexer"
        --config "${MOEX_SOURCE_ROOT}/tests/fixtures/spec_indexer_policy_config.yaml"
        --workspace "${MOEX_TEST_OUTPUT_DIR}/spec-indexer-policy")

add_test(
    NAME spec_indexer_policy_assert
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/assert_spec_indexer_policy.py"
        "${MOEX_TEST_OUTPUT_DIR}/spec-indexer-policy/manifests/policy.json")
set_tests_properties(spec_indexer_policy_assert PROPERTIES
    DEPENDS spec_indexer_policy_fixture)

add_test(
    NAME plaza2_scheme_lock_fixture
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/plaza2_scheme_lock"
        --scheme-dir "${MOEX_SOURCE_ROOT}/tests/fixtures/plaza2/scheme"
        --output "${MOEX_TEST_OUTPUT_DIR}/plaza2-scheme-lock/manifest.json")

add_test(
    NAME plaza2_phase3a_check
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/plaza2_phase3a_materialize"
        --project-root "${MOEX_SOURCE_ROOT}"
        --check)

add_test(
    NAME plaza2_schema_materialize_check
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/plaza2_schema_materialize"
        --project-root "${MOEX_SOURCE_ROOT}"
        --check)

add_test(
    NAME plaza2_schema_materialize_missing_cache_check
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/assert_plaza2_schema_materialize_missing_cache.py"
        "${MOEX_SOURCE_ROOT}"
        "${MOEX_TOOLS_BUILD_DIR}/plaza2_schema_materialize")

add_test(
    NAME plaza2_codegen_check
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/plaza2_codegen"
        --schema "${MOEX_SOURCE_ROOT}/protocols/plaza2_cgate/schema/plaza2_forts_reviewed.ini"
        --out "${MOEX_SOURCE_ROOT}/protocols/plaza2_cgate/generated"
        --check)

add_test(
    NAME plaza2_fake_scenario_materialize_check
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/plaza2_fake_scenario_materialize"
        --project-root "${MOEX_SOURCE_ROOT}"
        --check)

add_test(
    NAME plaza2_scheme_parser_check
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/assert_plaza2_scheme_parser.py"
        "${MOEX_SOURCE_ROOT}")

add_test(
    NAME plaza2_fake_scenario_parser_check
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/assert_plaza2_fake_scenario_parser.py"
        "${MOEX_SOURCE_ROOT}")

add_test(
    NAME plaza2_metadata_invariants_check
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/assert_plaza2_metadata_invariants.py"
        "${MOEX_SOURCE_ROOT}")

add_test(
    NAME plaza2_runtime_scheme_lock_check
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/assert_plaza2_runtime_scheme_lock.py"
        "${MOEX_SOURCE_ROOT}")

add_test(
    NAME plaza2_trade_spec_lock_check
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/assert_plaza2_trade_spec_lock.py"
        "${MOEX_SOURCE_ROOT}"
        "${MOEX_TOOLS_BUILD_DIR}/plaza2_phase5a_trade_materialize")

add_test(
    NAME plaza2_trade_codec_materialize_check
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/assert_plaza2_trade_codec_materialize.py"
        "${MOEX_SOURCE_ROOT}"
        "${MOEX_TOOLS_BUILD_DIR}/plaza2_phase5b_trade_codec_materialize")

foreach(test_name IN ITEMS
        plaza2_trade_command_validation_test
        plaza2_trade_command_encoding_test
        plaza2_trade_reply_decoding_test
        plaza2_trade_no_send_guard_test
        plaza2_trade_fake_session_test
        plaza2_trade_fake_session_replication_test)
    add_executable(${test_name}
        ${MOEX_SOURCE_ROOT}/tests/plaza2_trade/${test_name}.cpp)
    target_link_libraries(${test_name}
        PRIVATE
            moex_plaza2_trade_offline_codec
            moex_plaza2_cgate_private_state)
    target_include_directories(${test_name}
        PRIVATE
            "${MOEX_SOURCE_ROOT}/tests/plaza2_trade")
    target_compile_features(${test_name} PRIVATE cxx_std_20)
    target_compile_definitions(${test_name}
        PRIVATE
            MOEX_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")
    set_property(TARGET ${test_name} PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
    add_test(
        NAME ${test_name}
        COMMAND "${MOEX_BUILD_ROOT}/tests/${test_name}")
endforeach()

add_executable(plaza2_order_lifecycle_scenarios_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_trade/plaza2_order_lifecycle_scenarios_test.cpp)
target_link_libraries(plaza2_order_lifecycle_scenarios_test
    PRIVATE
        moex_plaza2_trade_order_lifecycle)
target_compile_features(plaza2_order_lifecycle_scenarios_test PRIVATE cxx_std_20)
set_property(TARGET plaza2_order_lifecycle_scenarios_test
    PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
add_test(
    NAME plaza2_order_lifecycle_scenarios_test
    COMMAND "${MOEX_BUILD_ROOT}/tests/plaza2_order_lifecycle_scenarios_test")

add_executable(plaza2_trade_transport_scenarios_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_trade/plaza2_trade_transport_scenarios_test.cpp)
target_link_libraries(plaza2_trade_transport_scenarios_test
    PRIVATE
        plaza2_runtime_test_support
        moex_plaza2_trade_order_lifecycle)
target_include_directories(plaza2_trade_transport_scenarios_test
    PRIVATE
        "${MOEX_SOURCE_ROOT}/tests/plaza2_trade")
target_compile_features(plaza2_trade_transport_scenarios_test PRIVATE cxx_std_20)
set_property(TARGET plaza2_trade_transport_scenarios_test
    PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
add_test(
    NAME plaza2_trade_transport_scenarios_test
    COMMAND "${MOEX_BUILD_ROOT}/tests/plaza2_trade_transport_scenarios_test"
            "$<TARGET_FILE:plaza2_fake_cgate_runtime>")

add_executable(connector_host_test ${MOEX_SOURCE_ROOT}/tests/connector_host_test.cpp)
target_link_libraries(connector_host_test PRIVATE plaza2_runtime_test_support moex_connector_host)
add_dependencies(connector_host_test plaza2_fake_cgate_runtime)
add_test(NAME connector_host_test COMMAND connector_host_test "$<TARGET_FILE:plaza2_fake_cgate_runtime>")
set_tests_properties(connector_host_test PROPERTIES LABELS "plaza2;sanitizer")
add_test(NAME moexctl_test COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/assert_moexctl.py"
    "$<TARGET_FILE:moexctl>" "$<TARGET_FILE:connector_host_test>" "$<TARGET_FILE:plaza2_fake_cgate_runtime>")
set_tests_properties(moexctl_test PROPERTIES LABELS "plaza2;sanitizer")

add_executable(moex_c_api_v2_test ${MOEX_SOURCE_ROOT}/tests/moex_c_api_v2_test.cpp)
target_link_libraries(moex_c_api_v2_test
    PRIVATE
        moex_phase0_core
        plaza2_runtime_test_support)
target_include_directories(moex_c_api_v2_test
    PRIVATE
        "${MOEX_SOURCE_ROOT}/src"
        "${MOEX_SOURCE_ROOT}/tests/plaza2_cgate")
target_compile_features(moex_c_api_v2_test PRIVATE cxx_std_20)
set_property(TARGET moex_c_api_v2_test PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
add_test(
    NAME moex_c_api_v2_test
    COMMAND "${MOEX_BUILD_ROOT}/tests/moex_c_api_v2_test" "$<TARGET_FILE:plaza2_fake_cgate_runtime>")
set_tests_properties(moex_c_api_v2_test PROPERTIES LABELS "cabi;plaza2;sanitizer")

add_executable(moex_c_api_v2_fixture ${MOEX_SOURCE_ROOT}/tests/moex_c_api_v2_fixture.cpp)
target_link_libraries(moex_c_api_v2_fixture PRIVATE plaza2_runtime_test_support)
target_include_directories(moex_c_api_v2_fixture PRIVATE "${MOEX_SOURCE_ROOT}/tests/plaza2_cgate")
target_compile_features(moex_c_api_v2_fixture PRIVATE cxx_std_20)
set_property(TARGET moex_c_api_v2_fixture PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")

add_executable(plaza2_generated_metadata_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_generated_metadata_test.cpp)
target_link_libraries(plaza2_generated_metadata_test
    PRIVATE
        moex_plaza2_cgate_metadata)
target_compile_features(plaza2_generated_metadata_test PRIVATE cxx_std_20)
set_property(TARGET plaza2_generated_metadata_test PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
add_test(
    NAME plaza2_generated_metadata_test
    COMMAND "${MOEX_BUILD_ROOT}/tests/plaza2_generated_metadata_test")

add_executable(plaza2_fake_engine_scenario_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_fake_engine_scenario_test.cpp)
target_link_libraries(plaza2_fake_engine_scenario_test
    PRIVATE
        moex_plaza2_cgate_fake_scenarios)
target_compile_features(plaza2_fake_engine_scenario_test PRIVATE cxx_std_20)
set_property(TARGET plaza2_fake_engine_scenario_test PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
add_test(
    NAME plaza2_fake_engine_scenario_test
    COMMAND "${MOEX_BUILD_ROOT}/tests/plaza2_fake_engine_scenario_test")

add_executable(plaza2_fake_engine_rules_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_fake_engine_rules_test.cpp)
target_link_libraries(plaza2_fake_engine_rules_test
    PRIVATE
        moex_plaza2_cgate_fake_engine)
target_compile_features(plaza2_fake_engine_rules_test PRIVATE cxx_std_20)
set_property(TARGET plaza2_fake_engine_rules_test PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
add_test(
    NAME plaza2_fake_engine_rules_test
    COMMAND "${MOEX_BUILD_ROOT}/tests/plaza2_fake_engine_rules_test")

function(moex_add_plaza2_private_state_test target_name source_file)
    add_executable(${target_name} "${source_file}")
    target_link_libraries(${target_name}
        PRIVATE
            moex_plaza2_cgate_private_state
            moex_plaza2_cgate_fake_scenarios)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    set_property(TARGET ${target_name} PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
    add_test(
        NAME ${target_name}
        COMMAND "${MOEX_BUILD_ROOT}/tests/${target_name}")
endfunction()

moex_add_plaza2_private_state_test(plaza2_private_state_projection_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_private_state_projection_test.cpp)
moex_add_plaza2_private_state_test(plaza2_private_state_visibility_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_private_state_visibility_test.cpp)
moex_add_plaza2_private_state_test(plaza2_private_state_invalidation_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_private_state_invalidation_test.cpp)
moex_add_plaza2_private_state_test(plaza2_private_state_provenance_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_private_state_provenance_test.cpp)

add_library(plaza2_runtime_test_support STATIC
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_runtime_test_support.cpp)
target_link_libraries(plaza2_runtime_test_support
    PUBLIC
        moex_plaza2_cgate_runtime)
target_include_directories(plaza2_runtime_test_support
    PUBLIC
        "${MOEX_SOURCE_ROOT}/tests/plaza2_cgate")
target_compile_features(plaza2_runtime_test_support PUBLIC cxx_std_20)
target_compile_definitions(plaza2_runtime_test_support
    PRIVATE
        MOEX_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")

add_library(plaza2_fake_cgate_runtime SHARED
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/fake_cgate_runtime.cpp)
target_link_libraries(plaza2_fake_cgate_runtime
    PRIVATE
        moex_plaza2_cgate_metadata)
target_include_directories(plaza2_fake_cgate_runtime
    PRIVATE
        "${MOEX_SOURCE_ROOT}/protocols/plaza2_cgate/generated")
target_compile_features(plaza2_fake_cgate_runtime PUBLIC cxx_std_20)
set_target_properties(plaza2_fake_cgate_runtime PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests"
    RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests"
    OUTPUT_NAME "plaza2_fake_cgate_runtime")
add_dependencies(plaza2_trade_transport_scenarios_test plaza2_fake_cgate_runtime)

function(moex_add_plaza2_cgate_test target_name source_file)
    add_executable(${target_name} "${source_file}")
    add_dependencies(${target_name} plaza2_fake_cgate_runtime)
    target_link_libraries(${target_name}
        PRIVATE
            plaza2_runtime_test_support)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    set_property(TARGET ${target_name} PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
    add_test(
        NAME ${target_name}
        COMMAND "${MOEX_BUILD_ROOT}/tests/${target_name}" "$<TARGET_FILE:plaza2_fake_cgate_runtime>")
endfunction()

moex_add_plaza2_cgate_test(plaza2_runtime_probe_test ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_runtime_probe_test.cpp)
moex_add_plaza2_cgate_test(plaza2_scheme_drift_test ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_scheme_drift_test.cpp)
moex_add_plaza2_cgate_test(plaza2_runtime_adapter_test ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_runtime_adapter_test.cpp)
target_compile_definitions(plaza2_runtime_adapter_test PRIVATE MOEX_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")

add_executable(plaza2_manual_operator_gate_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_manual_operator_gate_test.cpp)
target_link_libraries(plaza2_manual_operator_gate_test
    PRIVATE
        plaza2_runtime_test_support
        moex_plaza2_cgate_live_session)
target_compile_features(plaza2_manual_operator_gate_test PRIVATE cxx_std_20)
set_property(TARGET plaza2_manual_operator_gate_test PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
add_test(
    NAME plaza2_manual_operator_gate_test
    COMMAND "${MOEX_BUILD_ROOT}/tests/plaza2_manual_operator_gate_test")

function(moex_add_plaza2_live_runner_test target_name source_file)
    add_executable(${target_name} "${source_file}")
    add_dependencies(${target_name} plaza2_fake_cgate_runtime)
    target_link_libraries(${target_name}
        PRIVATE
            plaza2_runtime_test_support
            moex_plaza2_cgate_live_session)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    set_property(TARGET ${target_name} PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
    add_test(
        NAME ${target_name}
        COMMAND "${MOEX_BUILD_ROOT}/tests/${target_name}" "$<TARGET_FILE:plaza2_fake_cgate_runtime>")
endfunction()

moex_add_plaza2_live_runner_test(plaza2_live_session_validation_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_live_session_validation_test.cpp)
moex_add_plaza2_live_runner_test(plaza2_live_session_runner_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_live_session_runner_test.cpp)

add_executable(plaza2_trade_connectivity_qualifier_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_trade_connectivity_qualifier_test.cpp)
add_dependencies(plaza2_trade_connectivity_qualifier_test plaza2_fake_cgate_runtime)
target_link_libraries(plaza2_trade_connectivity_qualifier_test
    PRIVATE
        plaza2_runtime_test_support
        moex_plaza2_cgate_connectivity_qualifier)
target_compile_features(plaza2_trade_connectivity_qualifier_test PRIVATE cxx_std_20)
set_property(TARGET plaza2_trade_connectivity_qualifier_test PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
add_test(
    NAME plaza2_trade_connectivity_qualifier_test
    COMMAND "${MOEX_BUILD_ROOT}/tests/plaza2_trade_connectivity_qualifier_test"
        "$<TARGET_FILE:plaza2_fake_cgate_runtime>")

function(moex_add_plaza2_aggr20_md_test target_name source_file)
    add_executable(${target_name} "${source_file}")
    add_dependencies(${target_name} plaza2_fake_cgate_runtime)
    target_link_libraries(${target_name}
        PRIVATE
            plaza2_runtime_test_support
            moex_plaza2_cgate_aggr20_md)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    set_property(TARGET ${target_name} PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
    add_test(
        NAME ${target_name}
        COMMAND "${MOEX_BUILD_ROOT}/tests/${target_name}" "$<TARGET_FILE:plaza2_fake_cgate_runtime>")
endfunction()

moex_add_plaza2_aggr20_md_test(plaza2_aggr20_md_validation_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_aggr20_md_validation_test.cpp)
moex_add_plaza2_aggr20_md_test(plaza2_aggr20_md_projection_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_aggr20_md_projection_test.cpp)
moex_add_plaza2_aggr20_md_test(plaza2_aggr20_md_runner_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_cgate/plaza2_aggr20_md_runner_test.cpp)

function(moex_add_plaza2_twime_reconciler_test target_name source_file)
    add_executable(${target_name} "${source_file}")
    target_link_libraries(${target_name}
        PRIVATE
            moex_plaza2_twime_reconciler
            moex_plaza2_cgate_fake_scenarios
            twime_test_support)
    target_include_directories(${target_name}
        PRIVATE
            "${MOEX_SOURCE_ROOT}/tests/plaza2_twime_reconciler"
            "${MOEX_SOURCE_ROOT}/tests/twime_trade"
            "${MOEX_SOURCE_ROOT}/tests/twime_sbe")
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_compile_definitions(${target_name}
        PRIVATE
            MOEX_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")
    set_property(TARGET ${target_name} PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
    add_test(
        NAME ${target_name}
        COMMAND "${MOEX_BUILD_ROOT}/tests/${target_name}")
endfunction()

moex_add_plaza2_twime_reconciler_test(plaza2_twime_reconciler_normalization_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_twime_reconciler/plaza2_twime_reconciler_normalization_test.cpp)
moex_add_plaza2_twime_reconciler_test(plaza2_twime_reconciler_orders_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_twime_reconciler/plaza2_twime_reconciler_orders_test.cpp)
moex_add_plaza2_twime_reconciler_test(plaza2_twime_reconciler_trades_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_twime_reconciler/plaza2_twime_reconciler_trades_test.cpp)
moex_add_plaza2_twime_reconciler_test(plaza2_twime_reconciler_invalidation_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_twime_reconciler/plaza2_twime_reconciler_invalidation_test.cpp)

add_test(
    NAME plaza2_cert_runner_requires_test_plaza2_arm
    COMMAND "${MOEX_APPS_BUILD_DIR}/moex_plaza2_cert_runner"
        --profile "${MOEX_SOURCE_ROOT}/tests/fixtures/profiles/plaza2_repl_nonloopback_fixture.yaml"
        --output-dir "${MOEX_TEST_OUTPUT_DIR}/plaza2-cert-runner"
        --armed-test-network
        --armed-test-session)
set_tests_properties(plaza2_cert_runner_requires_test_plaza2_arm PROPERTIES WILL_FAIL TRUE)

add_test(
    NAME plaza2_connectivity_qualifier_no_send_guard
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/assert_plaza2_qualifier_no_send.py"
        "${MOEX_SOURCE_ROOT}")

add_test(
    NAME plaza2_aggr20_md_runner_requires_market_data_arm
    COMMAND "${MOEX_APPS_BUILD_DIR}/moex_plaza2_aggr20_md_runner"
        --profile "${MOEX_SOURCE_ROOT}/profiles/test_plaza2_aggr20_md.template.yaml"
        --output-dir "${MOEX_TEST_OUTPUT_DIR}/plaza2-aggr20-md-runner"
        --armed-test-network
        --armed-test-session
        --armed-test-plaza2)
set_tests_properties(plaza2_aggr20_md_runner_requires_market_data_arm PROPERTIES WILL_FAIL TRUE)

function(moex_add_plaza2_twime_integrated_test target_name source_file)
    add_executable(${target_name} "${source_file}")
    add_dependencies(${target_name} plaza2_fake_cgate_runtime)
    target_link_libraries(${target_name}
        PRIVATE
            moex_plaza2_twime_integrated_test
            local_tcp_server_support
            plaza2_runtime_test_support
            twime_test_support)
    target_include_directories(${target_name}
        PRIVATE
            "${MOEX_SOURCE_ROOT}/tests/support"
            "${MOEX_SOURCE_ROOT}/tests/twime_trade"
            "${MOEX_SOURCE_ROOT}/tests/twime_sbe"
            "${MOEX_SOURCE_ROOT}/tests/plaza2_cgate"
            "${MOEX_SOURCE_ROOT}/tests/plaza2_twime_reconciler"
            "${MOEX_SOURCE_ROOT}/tests/plaza2_twime_integrated")
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_compile_definitions(${target_name}
        PRIVATE
            MOEX_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")
    set_property(TARGET ${target_name} PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
    add_test(
        NAME ${target_name}
        COMMAND "${MOEX_BUILD_ROOT}/tests/${target_name}" "$<TARGET_FILE:plaza2_fake_cgate_runtime>")
endfunction()

moex_add_plaza2_twime_integrated_test(plaza2_twime_integrated_validation_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_twime_integrated/plaza2_twime_integrated_validation_test.cpp)
moex_add_plaza2_twime_integrated_test(plaza2_twime_integrated_runner_test
    ${MOEX_SOURCE_ROOT}/tests/plaza2_twime_integrated/plaza2_twime_integrated_runner_test.cpp)

add_test(
    NAME plaza2_twime_integrated_runner_requires_test_reconcile_arm
    COMMAND "${MOEX_APPS_BUILD_DIR}/moex_plaza2_twime_integrated_runner"
        --profile "${MOEX_SOURCE_ROOT}/tests/fixtures/profiles/plaza2_twime_integrated_nonloopback_fixture.yaml"
        --output-dir "${MOEX_TEST_OUTPUT_DIR}/plaza2-twime-integrated-runner"
        --armed-test-network
        --armed-test-session
        --armed-test-plaza2)
set_tests_properties(plaza2_twime_integrated_runner_requires_test_reconcile_arm PROPERTIES WILL_FAIL TRUE)

add_test(
    NAME phase4d_vps_scripts_check
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/scripts/assert_phase4d_vps_scripts.py"
        "${MOEX_SOURCE_ROOT}")
add_test(
    NAME phase5d_aggr20_md_scripts_check
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/scripts/assert_phase5d_aggr20_md.py"
        "${MOEX_SOURCE_ROOT}")

add_test(
    NAME osengine_inventory_fixture
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/osengine_inventory"
        --root "${MOEX_SOURCE_ROOT}/tests/fixtures/osengine/MoexFixFastTwimeFutures"
        --root "${MOEX_SOURCE_ROOT}/tests/fixtures/osengine/Plaza"
        --output "${MOEX_TEST_OUTPUT_DIR}/osengine-inventory/inventory.json")

add_test(
    NAME twime_codegen_check
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/assert_twime_codegen_deterministic.py"
        "${MOEX_SOURCE_ROOT}")

add_test(
    NAME twime_schema_manifest_check
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/assert_twime_schema_manifest.py"
        "${MOEX_SOURCE_ROOT}")

add_test(
    NAME twime_metadata_invariants_check
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/assert_twime_metadata_invariants.py"
        "${MOEX_SOURCE_ROOT}")

add_test(
    NAME twime_reviewed_fixtures_check
    COMMAND "${Python3_EXECUTABLE}" "${MOEX_SOURCE_ROOT}/tests/assert_twime_reviewed_fixtures.py"
        "${MOEX_SOURCE_ROOT}")

add_test(
    NAME twime_fixture_golden_check
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/twime_fixture_check"
        --fixtures "${MOEX_SOURCE_ROOT}/tests/fixtures/twime_sbe"
        --check)

add_test(
    NAME source_style_check
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/source_style_check"
        --require-clang-format
        "${MOEX_SOURCE_ROOT}/.github"
        "${MOEX_SOURCE_ROOT}/CMakeLists.txt"
        "${MOEX_SOURCE_ROOT}/cmake"
        "${MOEX_SOURCE_ROOT}/include"
        "${MOEX_SOURCE_ROOT}/src"
        "${MOEX_SOURCE_ROOT}/protocols"
        "${MOEX_SOURCE_ROOT}/connectors"
        "${MOEX_SOURCE_ROOT}/tools"
        "${MOEX_SOURCE_ROOT}/tests"
        "${MOEX_SOURCE_ROOT}/matrix"
        "${MOEX_SOURCE_ROOT}/profiles"
        "${MOEX_SOURCE_ROOT}/cert"
        "${MOEX_SOURCE_ROOT}/docs"
        "${MOEX_SOURCE_ROOT}/scripts"
        "${MOEX_SOURCE_ROOT}/apps")
set_tests_properties(source_style_check PROPERTIES
    WORKING_DIRECTORY "${MOEX_SOURCE_ROOT}")

if(DOTNET_EXECUTABLE)
    if(MOEX_SANITIZERS_ENABLED)
        message(STATUS "Sanitizer flags detected; skipping dotnet ABI tests because dotnet cannot reliably load sanitized native libraries across hosts.")
    else()
        set(MOEX_CABI_V2_FIXTURE_DIR "${MOEX_TEST_OUTPUT_DIR}/cabi-v2-dotnet-fixture")
        add_test(
            NAME moex_c_api_v2_fixture
            COMMAND "${MOEX_BUILD_ROOT}/tests/moex_c_api_v2_fixture"
                "$<TARGET_FILE:plaza2_fake_cgate_runtime>"
                "${MOEX_CABI_V2_FIXTURE_DIR}")
        set_tests_properties(moex_c_api_v2_fixture PROPERTIES
            RUN_SERIAL TRUE
            LABELS "cabi;plaza2;integration")

        add_test(
            NAME dotnet_abi_smoke
            COMMAND "${DOTNET_EXECUTABLE}" run
                --project "${MOEX_SOURCE_ROOT}/tests/dotnet/AbiSmoke/AbiSmoke.csproj"
                --framework net10.0
                -- "$<TARGET_FILE:moex_phase0_abi>"
                   "${MOEX_SOURCE_ROOT}/tests/fixtures/shadow_replay/synthetic_replay.txt")
        set_tests_properties(dotnet_abi_smoke PROPERTIES
            RUN_SERIAL TRUE
            ENVIRONMENT
                "DOTNET_CLI_HOME=${MOEX_TEST_OUTPUT_DIR}/dotnet-home;DOTNET_CLI_TELEMETRY_OPTOUT=1;DOTNET_NOLOGO=1;NUGET_PACKAGES=$ENV{HOME}/.nuget/packages")

        add_test(
            NAME dotnet_abi_v2_smoke
            COMMAND "${DOTNET_EXECUTABLE}" run
                --project "${MOEX_SOURCE_ROOT}/tests/dotnet/AbiV2Smoke/AbiV2Smoke.csproj"
                --framework net10.0
                -- "$<TARGET_FILE:moex_phase0_abi>"
                   "${MOEX_CABI_V2_FIXTURE_DIR}")
        set(MOEX_CABI_V2_DOTNET_ENV
            "DOTNET_CLI_HOME=${MOEX_TEST_OUTPUT_DIR}/dotnet-home"
            "DOTNET_CLI_TELEMETRY_OPTOUT=1"
            "DOTNET_NOLOGO=1"
            "NUGET_PACKAGES=$ENV{HOME}/.nuget/packages"
            "MOEX_CABI_V2_ENV=ini=config/t1.ini"
            "MOEX_CABI_V2_BROKER=BRK1"
            "MOEX_CABI_V2_CLIENT=C01"
            "MOEX_CABI_V2_CREDENTIALS=fixture-placeholder"
            "MOEX_CABI_V2_KEY=00000000"
            "MOEX_FAKE_ZERO_POSITION=1"
            "MOEX_FAKE_MISSING_ORDER=1"
            "MOEX_FAKE_CLIENT_CODE=BRK1C01"
            "MOEX_FAKE_PUB_REPLY_ORDER_ID=20003"
            "MOEX_FAKE_CANCEL_AFTER_DEL=1")
        set_tests_properties(dotnet_abi_v2_smoke PROPERTIES
            DEPENDS moex_c_api_v2_fixture
            RUN_SERIAL TRUE
            ENVIRONMENT "${MOEX_CABI_V2_DOTNET_ENV}")

        add_test(
            NAME dotnet_abi_policy
            COMMAND "${DOTNET_EXECUTABLE}" run
                --project "${MOEX_SOURCE_ROOT}/tests/dotnet/AbiPolicy/AbiPolicy.csproj"
                --framework net10.0
                -- "$<TARGET_FILE:moex_phase0_abi>"
                   "${MOEX_SOURCE_ROOT}/tests/fixtures/shadow_replay/synthetic_replay.txt")
        set_tests_properties(dotnet_abi_policy PROPERTIES
            RUN_SERIAL TRUE
            ENVIRONMENT
                "DOTNET_CLI_HOME=${MOEX_TEST_OUTPUT_DIR}/dotnet-home;DOTNET_CLI_TELEMETRY_OPTOUT=1;DOTNET_NOLOGO=1;NUGET_PACKAGES=$ENV{HOME}/.nuget/packages")

        if(EXISTS "${MOEX_ALORENGINE_PROJECT}")
            add_test(
                NAME dotnet_shadow_replay
                COMMAND "${DOTNET_EXECUTABLE}" run
                    --project "${MOEX_SOURCE_ROOT}/tests/dotnet/ShadowReplay/ShadowReplay.csproj"
                    --framework net10.0
                    "-p:AlorEngineProject=${MOEX_ALORENGINE_PROJECT}"
                    "-p:RestorePackagesPath=$ENV{HOME}/.nuget/packages"
                    -- "$<TARGET_FILE:moex_phase0_abi>"
                       "${MOEX_SOURCE_ROOT}/tests/fixtures/shadow_replay/synthetic_replay.txt"
                       "${MOEX_SOURCE_ROOT}/tests/fixtures/shadow_replay/expected_report.json"
                       "${MOEX_TEST_OUTPUT_DIR}/shadow-replay/diff.json")
            set_tests_properties(dotnet_shadow_replay PROPERTIES
                ENVIRONMENT
                    "DOTNET_CLI_HOME=$ENV{HOME};DOTNET_CLI_TELEMETRY_OPTOUT=1;DOTNET_NOLOGO=1;NUGET_PACKAGES=$ENV{HOME}/.nuget/packages")
        else()
            message(STATUS "MOEX_ALORENGINE_PROJECT not set or not found; skipping dotnet_shadow_replay test")
        endif()
    endif()
else()
    message(STATUS "dotnet executable not found; skipping dotnet_abi_smoke test")
endif()


get_property(moex_registered_tests DIRECTORY PROPERTY TESTS)
foreach(test_name IN LISTS moex_registered_tests)
    if(test_name MATCHES "^plaza2_" OR test_name STREQUAL "moex_phase4b_capi_read_side_test")
        set_property(TEST "${test_name}" APPEND PROPERTY LABELS plaza2)
    endif()
    if(test_name MATCHES "^twime_")
        set_property(TEST "${test_name}" APPEND PROPERTY LABELS twime)
    endif()
    if(test_name MATCHES "^(unicode_guard_check|repo_style_check|source_style_check)$")
        set_property(TEST "${test_name}" APPEND PROPERTY LABELS preflight tooling)
    elseif(test_name MATCHES "(check|fixture|inventory|profile|cert_runner|scripts)")
        set_property(TEST "${test_name}" APPEND PROPERTY LABELS tooling)
    endif()
    if(test_name MATCHES "(runtime|live|integrated|fake_session|tcp|transport)" OR
       test_name MATCHES "^dotnet_")
        set_property(TEST "${test_name}" APPEND PROPERTY LABELS integration)
    else()
        set_property(TEST "${test_name}" APPEND PROPERTY LABELS unit)
    endif()
    if(test_name MATCHES "^twime_.*_test$" OR
       test_name MATCHES "^(phase0_selfcheck|plaza2_order_lifecycle_scenarios_test|plaza2_trade_transport_scenarios_test)$" OR
       test_name MATCHES "^(plaza2_runtime_adapter_test|plaza2_runtime_probe_test|plaza2_trade_no_send_guard_test)$")
        set_property(TEST "${test_name}" APPEND PROPERTY LABELS sanitizer)
    endif()
    if(test_name MATCHES "^dotnet_")
        set_property(TEST "${test_name}" APPEND PROPERTY LABELS abi)
    endif()
endforeach()
