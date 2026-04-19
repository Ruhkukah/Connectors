cmake_minimum_required(VERSION 3.24)

project(
    MoexConnector
    VERSION 0.1.0
    DESCRIPTION "Standalone C++20/Linux-first MOEX connector suite skeleton"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
enable_testing()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(DOTNET_EXECUTABLE NAMES dotnet)
set(MOEX_ALORENGINE_PROJECT "" CACHE FILEPATH "Optional path to AlorEngine.csproj for the shadow replay integration harness")
if(NOT MOEX_ALORENGINE_PROJECT AND DEFINED ENV{ALORENGINE_PROJECT})
    set(MOEX_ALORENGINE_PROJECT "$ENV{ALORENGINE_PROJECT}")
endif()

set(MOEX_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(MOEX_BUILD_ROOT "${CMAKE_CURRENT_BINARY_DIR}")
set(MOEX_TOOLS_BUILD_DIR "${MOEX_BUILD_ROOT}/tools")
set(MOEX_APPS_BUILD_DIR "${MOEX_BUILD_ROOT}/apps")
set(MOEX_TEST_OUTPUT_DIR "${MOEX_BUILD_ROOT}/test-output")
set(MOEX_PYTHON_DEPS_DIR "${MOEX_BUILD_ROOT}/python-deps")

set(MOEX_ALL_SANITIZER_FLAGS
    "${CMAKE_CXX_FLAGS} ${CMAKE_C_FLAGS} ${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS}")
string(FIND "${MOEX_ALL_SANITIZER_FLAGS}" "-fsanitize=" MOEX_SANITIZER_FLAGS_INDEX)
if(MOEX_SANITIZER_FLAGS_INDEX GREATER -1)
    set(MOEX_SANITIZERS_ENABLED TRUE)
else()
    set(MOEX_SANITIZERS_ENABLED FALSE)
endif()

file(MAKE_DIRECTORY "${MOEX_TOOLS_BUILD_DIR}")
file(MAKE_DIRECTORY "${MOEX_APPS_BUILD_DIR}")
file(MAKE_DIRECTORY "${MOEX_TEST_OUTPUT_DIR}")
file(MAKE_DIRECTORY "${MOEX_PYTHON_DEPS_DIR}")

execute_process(
    COMMAND
        "${Python3_EXECUTABLE}" -c
        "import sys; sys.path.insert(0, r'${MOEX_PYTHON_DEPS_DIR}'); import yaml"
    RESULT_VARIABLE MOEX_PYYAML_STATUS
    OUTPUT_QUIET
    ERROR_QUIET)

if(NOT MOEX_PYYAML_STATUS EQUAL 0)
    execute_process(
        COMMAND
            "${Python3_EXECUTABLE}" -m pip install
            --disable-pip-version-check
            --quiet
            --target "${MOEX_PYTHON_DEPS_DIR}"
            -r "${CMAKE_CURRENT_SOURCE_DIR}/requirements.txt"
        RESULT_VARIABLE MOEX_PIP_INSTALL_STATUS)
    if(NOT MOEX_PIP_INSTALL_STATUS EQUAL 0)
        message(FATAL_ERROR "Failed to install Python requirements into ${MOEX_PYTHON_DEPS_DIR}")
    endif()
endif()

add_library(moex_phase0_core STATIC
    src/phase0_core.cpp)

add_library(moex_phase0_abi SHARED
    src/phase0_core.cpp)

target_include_directories(moex_phase0_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_include_directories(moex_phase0_abi
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_features(moex_phase0_core PUBLIC cxx_std_20)
target_compile_features(moex_phase0_abi PUBLIC cxx_std_20)

target_compile_definitions(moex_phase0_core
    PRIVATE
        MOEX_PHASE0_PROJECT_VERSION="${PROJECT_VERSION}"
        MOEX_PHASE0_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")

target_compile_definitions(moex_phase0_abi
    PRIVATE
        MOEX_PHASE0_PROJECT_VERSION="${PROJECT_VERSION}"
        MOEX_PHASE0_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")

set_target_properties(moex_phase0_abi PROPERTIES
    OUTPUT_NAME "moex_phase0_abi"
    LIBRARY_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/lib")

add_subdirectory(protocols/twime_sbe)
add_subdirectory(connectors/twime_trade)

add_executable(moex_phase0_selfcheck
    tests/phase0_selfcheck.cpp)

target_link_libraries(moex_phase0_selfcheck
    PRIVATE
        moex_phase0_core)

set_property(TARGET moex_phase0_selfcheck PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")

function(moex_generate_python_launcher launcher_dir launcher_name script_relative_path)
    set(LAUNCHER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${script_relative_path}")
    set(LAUNCHER_DEST "${launcher_dir}/${launcher_name}")
    set(LAUNCHER_SCRIPT_PATH "${LAUNCHER_SOURCE}")
    set(LAUNCHER_PYTHON_DEPS_DIR "${MOEX_PYTHON_DEPS_DIR}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/python_launcher.sh.in"
        "${LAUNCHER_DEST}"
        @ONLY)
    file(CHMOD "${LAUNCHER_DEST}"
        PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
endfunction()

moex_generate_python_launcher("${MOEX_TOOLS_BUILD_DIR}" "spec_indexer" "tools/spec_indexer.py")
moex_generate_python_launcher("${MOEX_TOOLS_BUILD_DIR}" "plaza2_scheme_lock" "tools/plaza2_scheme_lock.py")
moex_generate_python_launcher("${MOEX_TOOLS_BUILD_DIR}" "osengine_inventory" "tools/osengine_inventory.py")
moex_generate_python_launcher("${MOEX_TOOLS_BUILD_DIR}" "matrix_validate" "tools/matrix_validate.py")
moex_generate_python_launcher("${MOEX_TOOLS_BUILD_DIR}" "profile_check" "tools/profile_check.py")
moex_generate_python_launcher("${MOEX_TOOLS_BUILD_DIR}" "twime_schema_materialize" "tools/twime_schema_materialize.py")
moex_generate_python_launcher("${MOEX_TOOLS_BUILD_DIR}" "twime_schema_indexer" "tools/twime_schema_indexer.py")
moex_generate_python_launcher("${MOEX_TOOLS_BUILD_DIR}" "twime_codegen" "tools/twime_codegen.py")
moex_generate_python_launcher("${MOEX_TOOLS_BUILD_DIR}" "source_style_check" "tools/source_style_check.py")
moex_generate_python_launcher("${MOEX_TOOLS_BUILD_DIR}" "unicode_guard" "tools/unicode_guard.py")
moex_generate_python_launcher("${MOEX_APPS_BUILD_DIR}" "moex_cert_runner" "apps/moex_cert_runner.py")
moex_generate_python_launcher("${MOEX_APPS_BUILD_DIR}" "moex_replay_tool" "apps/moex_replay_tool.py")

add_executable(moex_twime_cert_runner
    apps/twime_cert_runner.cpp)
target_link_libraries(moex_twime_cert_runner
    PRIVATE
        moex_twime_trade)
target_compile_features(moex_twime_cert_runner PRIVATE cxx_std_20)
set_target_properties(moex_twime_cert_runner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${MOEX_APPS_BUILD_DIR}"
    OUTPUT_NAME "moex_twime_cert_runner")

add_library(twime_test_support STATIC
    tests/twime_sbe/twime_test_support.cpp)
target_link_libraries(twime_test_support
    PUBLIC
        moex_twime_sbe)
target_include_directories(twime_test_support
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/twime_sbe")
target_compile_features(twime_test_support PUBLIC cxx_std_20)
target_compile_definitions(twime_test_support
    PRIVATE
        MOEX_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")

function(moex_add_twime_test target_name source_file)
    add_executable(${target_name} "${source_file}")
    target_link_libraries(${target_name}
        PRIVATE
            twime_test_support)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_compile_definitions(${target_name}
        PRIVATE
            MOEX_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")
    set_property(TARGET ${target_name} PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
    add_test(
        NAME ${target_name}
        COMMAND "${MOEX_BUILD_ROOT}/tests/${target_name}")
endfunction()

function(moex_add_twime_trade_test target_name source_file)
    add_executable(${target_name} "${source_file}")
    target_link_libraries(${target_name}
        PRIVATE
            moex_twime_trade
            twime_test_support)
    target_include_directories(${target_name}
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/twime_trade"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/twime_sbe")
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_compile_definitions(${target_name}
        PRIVATE
            MOEX_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")
    set_property(TARGET ${target_name} PROPERTY RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/tests")
    add_test(
        NAME ${target_name}
        COMMAND "${MOEX_BUILD_ROOT}/tests/${target_name}")
endfunction()

moex_add_twime_test(twime_schema_inventory_test tests/twime_sbe/twime_schema_inventory_test.cpp)
moex_add_twime_test(twime_header_codec_test tests/twime_sbe/twime_header_codec_test.cpp)
moex_add_twime_test(twime_primitive_types_test tests/twime_sbe/twime_primitive_types_test.cpp)
moex_add_twime_test(twime_message_roundtrip_test tests/twime_sbe/twime_message_roundtrip_test.cpp)
moex_add_twime_test(twime_frame_assembler_test tests/twime_sbe/twime_frame_assembler_test.cpp)
moex_add_twime_test(twime_decode_error_test tests/twime_sbe/twime_decode_error_test.cpp)
moex_add_twime_test(twime_cert_log_formatter_test tests/twime_sbe/twime_cert_log_formatter_test.cpp)
moex_add_twime_test(twime_enum_set_validation_test tests/twime_sbe/twime_enum_set_validation_test.cpp)
moex_add_twime_test(twime_optional_string_policy_test tests/twime_sbe/twime_optional_string_policy_test.cpp)

moex_add_twime_trade_test(twime_session_establish_test tests/twime_trade/twime_session_establish_test.cpp)
moex_add_twime_trade_test(twime_session_reject_test tests/twime_trade/twime_session_reject_test.cpp)
moex_add_twime_trade_test(twime_keepalive_ack_test tests/twime_trade/twime_keepalive_ack_test.cpp)
moex_add_twime_trade_test(twime_heartbeat_fake_clock_test tests/twime_trade/twime_heartbeat_fake_clock_test.cpp)
moex_add_twime_trade_test(twime_heartbeat_rate_limit_test tests/twime_trade/twime_heartbeat_rate_limit_test.cpp)
moex_add_twime_trade_test(twime_terminate_test tests/twime_trade/twime_terminate_test.cpp)
moex_add_twime_trade_test(twime_sequence_gap_test tests/twime_trade/twime_sequence_gap_test.cpp)
moex_add_twime_trade_test(twime_message_layer_rules_test tests/twime_trade/twime_message_layer_rules_test.cpp)
moex_add_twime_trade_test(twime_retransmit_request_test tests/twime_trade/twime_retransmit_request_test.cpp)
moex_add_twime_trade_test(twime_retransmit_limit_test tests/twime_trade/twime_retransmit_limit_test.cpp)
moex_add_twime_trade_test(twime_retransmission_test tests/twime_trade/twime_retransmission_test.cpp)
moex_add_twime_trade_test(twime_flood_reject_test tests/twime_trade/twime_flood_reject_test.cpp)
moex_add_twime_trade_test(twime_business_reject_test tests/twime_trade/twime_business_reject_test.cpp)
moex_add_twime_trade_test(twime_cert_scenario_runner_test tests/twime_trade/twime_cert_scenario_runner_test.cpp)

add_custom_target(twime_codegen_check
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/twime_codegen"
        --schema "${CMAKE_CURRENT_SOURCE_DIR}/protocols/twime_sbe/schema/twime_spectra-7.7.xml"
        --out "${CMAKE_CURRENT_SOURCE_DIR}/protocols/twime_sbe/generated"
        --check
    COMMENT "Verify deterministic TWIME generated metadata")

add_executable(twime_fixture_check_tool
    tools/twime_fixture_check.cpp)
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
        --matrix-dir "${CMAKE_CURRENT_SOURCE_DIR}/matrix")

add_test(
    NAME unicode_guard_check
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/unicode_guard"
        .github
        CMakeLists.txt
        include
        src
        protocols
        connectors
        tools
        tests
        matrix
        profiles
        cert
        docs)

add_test(
    NAME profile_check_replay
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/profile_check"
        --profile "${CMAKE_CURRENT_SOURCE_DIR}/profiles/replay.yaml")

add_test(
    NAME profile_check_prod_with_arm
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/profile_check"
        --profile "${CMAKE_CURRENT_SOURCE_DIR}/profiles/prod_fast_twime.template.yaml"
        --armed)

add_test(
    NAME profile_check_prod_requires_arm
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/profile_check"
        --profile "${CMAKE_CURRENT_SOURCE_DIR}/profiles/prod_fast_twime.template.yaml")
set_tests_properties(profile_check_prod_requires_arm PROPERTIES WILL_FAIL TRUE)

add_test(
    NAME cert_runner_stub
    COMMAND "${MOEX_APPS_BUILD_DIR}/moex_cert_runner"
        --scenario "${CMAKE_CURRENT_SOURCE_DIR}/cert/stub/phase0_stub.yaml"
        --output-dir "${MOEX_TEST_OUTPUT_DIR}/cert-runner")

add_test(
    NAME twime_cert_runner_session_establish
    COMMAND "${MOEX_APPS_BUILD_DIR}/moex_cert_runner"
        --scenario "${CMAKE_CURRENT_SOURCE_DIR}/cert/scenarios/twime/session_establish.yaml"
        --output-dir "${MOEX_TEST_OUTPUT_DIR}/twime-cert-runner")

add_test(
    NAME twime_cert_runner_client_sequence_heartbeat
    COMMAND "${MOEX_APPS_BUILD_DIR}/moex_cert_runner"
        --scenario "${CMAKE_CURRENT_SOURCE_DIR}/cert/scenarios/twime/client_sequence_heartbeat_null_nextseqno.yaml"
        --output-dir "${MOEX_TEST_OUTPUT_DIR}/twime-cert-runner")

add_test(
    NAME twime_cert_runner_terminate_requires_inbound
    COMMAND "${MOEX_APPS_BUILD_DIR}/moex_cert_runner"
        --scenario "${CMAKE_CURRENT_SOURCE_DIR}/cert/scenarios/twime/terminate_requires_inbound_terminate.yaml"
        --output-dir "${MOEX_TEST_OUTPUT_DIR}/twime-cert-runner")

add_test(
    NAME spec_indexer_local_fixture
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/spec_indexer"
        --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/spec_indexer_config.json"
        --workspace "${MOEX_TEST_OUTPUT_DIR}/spec-indexer")

add_test(
    NAME spec_indexer_policy_fixture
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/spec_indexer"
        --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/spec_indexer_policy_config.yaml"
        --workspace "${MOEX_TEST_OUTPUT_DIR}/spec-indexer-policy")

add_test(
    NAME spec_indexer_policy_assert
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_spec_indexer_policy.py"
        "${MOEX_TEST_OUTPUT_DIR}/spec-indexer-policy/manifests/policy.json")

add_test(
    NAME plaza2_scheme_lock_fixture
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/plaza2_scheme_lock"
        --scheme-dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/plaza2/scheme"
        --output "${MOEX_TEST_OUTPUT_DIR}/plaza2-scheme-lock/manifest.json")

add_test(
    NAME osengine_inventory_fixture
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/osengine_inventory"
        --root "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/osengine/MoexFixFastTwimeFutures"
        --root "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/osengine/Plaza"
        --output "${MOEX_TEST_OUTPUT_DIR}/osengine-inventory/inventory.json")

add_test(
    NAME twime_codegen_check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_twime_codegen_deterministic.py"
        "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(twime_codegen_check PROPERTIES
    ENVIRONMENT "PYTHONPATH=${MOEX_PYTHON_DEPS_DIR}")

add_test(
    NAME twime_schema_manifest_check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_twime_schema_manifest.py"
        "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(
    NAME twime_metadata_invariants_check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_twime_metadata_invariants.py"
        "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(twime_metadata_invariants_check PROPERTIES
    ENVIRONMENT "PYTHONPATH=${MOEX_PYTHON_DEPS_DIR}")

add_test(
    NAME twime_reviewed_fixtures_check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_twime_reviewed_fixtures.py"
        "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(
    NAME twime_fixture_golden_check
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/twime_fixture_check"
        --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/twime_sbe"
        --check)

add_test(
    NAME source_style_check
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/source_style_check"
        --require-clang-format
        "${CMAKE_CURRENT_SOURCE_DIR}/connectors/twime_trade"
        "${CMAKE_CURRENT_SOURCE_DIR}/protocols/twime_sbe"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/twime_trade"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/twime_sbe"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/twime_codegen.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/twime_schema_common.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/twime_schema_indexer.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/twime_schema_materialize.py")

if(DOTNET_EXECUTABLE)
    if(MOEX_SANITIZERS_ENABLED)
        message(STATUS "Sanitizer flags detected; skipping dotnet ABI tests because dotnet cannot reliably load sanitized native libraries across hosts.")
    else()
        add_test(
            NAME dotnet_abi_smoke
            COMMAND "${DOTNET_EXECUTABLE}" run
                --project "${CMAKE_CURRENT_SOURCE_DIR}/tests/dotnet/AbiSmoke/AbiSmoke.csproj"
                --framework net10.0
                -- "$<TARGET_FILE:moex_phase0_abi>"
                   "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/shadow_replay/synthetic_replay.txt")
        set_tests_properties(dotnet_abi_smoke PROPERTIES
            ENVIRONMENT
                "DOTNET_CLI_HOME=${MOEX_TEST_OUTPUT_DIR}/dotnet-home;DOTNET_CLI_TELEMETRY_OPTOUT=1;DOTNET_NOLOGO=1;NUGET_PACKAGES=$ENV{HOME}/.nuget/packages")

        add_test(
            NAME dotnet_abi_policy
            COMMAND "${DOTNET_EXECUTABLE}" run
                --project "${CMAKE_CURRENT_SOURCE_DIR}/tests/dotnet/AbiPolicy/AbiPolicy.csproj"
                --framework net10.0
                -- "$<TARGET_FILE:moex_phase0_abi>"
                   "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/shadow_replay/synthetic_replay.txt")
        set_tests_properties(dotnet_abi_policy PROPERTIES
            ENVIRONMENT
                "DOTNET_CLI_HOME=${MOEX_TEST_OUTPUT_DIR}/dotnet-home;DOTNET_CLI_TELEMETRY_OPTOUT=1;DOTNET_NOLOGO=1;NUGET_PACKAGES=$ENV{HOME}/.nuget/packages")

        if(EXISTS "${MOEX_ALORENGINE_PROJECT}")
            add_test(
                NAME dotnet_shadow_replay
                COMMAND "${DOTNET_EXECUTABLE}" run
                    --project "${CMAKE_CURRENT_SOURCE_DIR}/tests/dotnet/ShadowReplay/ShadowReplay.csproj"
                    --framework net10.0
                    "-p:AlorEngineProject=${MOEX_ALORENGINE_PROJECT}"
                    "-p:RestorePackagesPath=$ENV{HOME}/.nuget/packages"
                    -- "$<TARGET_FILE:moex_phase0_abi>"
                       "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/shadow_replay/synthetic_replay.txt"
                       "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/shadow_replay/expected_report.json"
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
