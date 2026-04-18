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

set(MOEX_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(MOEX_BUILD_ROOT "${CMAKE_CURRENT_BINARY_DIR}")
set(MOEX_TOOLS_BUILD_DIR "${MOEX_BUILD_ROOT}/tools")
set(MOEX_APPS_BUILD_DIR "${MOEX_BUILD_ROOT}/apps")
set(MOEX_TEST_OUTPUT_DIR "${MOEX_BUILD_ROOT}/test-output")

file(MAKE_DIRECTORY "${MOEX_TOOLS_BUILD_DIR}")
file(MAKE_DIRECTORY "${MOEX_APPS_BUILD_DIR}")
file(MAKE_DIRECTORY "${MOEX_TEST_OUTPUT_DIR}")

add_library(moex_phase0_core STATIC
    src/phase0_core.cpp)

target_include_directories(moex_phase0_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_features(moex_phase0_core PUBLIC cxx_std_20)

target_compile_definitions(moex_phase0_core
    PRIVATE
        MOEX_PHASE0_PROJECT_VERSION="${PROJECT_VERSION}"
        MOEX_PHASE0_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")

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
moex_generate_python_launcher("${MOEX_APPS_BUILD_DIR}" "moex_cert_runner" "apps/moex_cert_runner.py")
moex_generate_python_launcher("${MOEX_APPS_BUILD_DIR}" "moex_replay_tool" "apps/moex_replay_tool.py")

add_test(
    NAME phase0_selfcheck
    COMMAND "${MOEX_BUILD_ROOT}/tests/moex_phase0_selfcheck")

add_test(
    NAME matrix_validate_repo
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/matrix_validate"
        --matrix-dir "${CMAKE_CURRENT_SOURCE_DIR}/matrix")

add_test(
    NAME profile_check_replay
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/profile_check"
        --profile "${CMAKE_CURRENT_SOURCE_DIR}/profiles/replay.yaml")

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
    NAME spec_indexer_local_fixture
    COMMAND "${MOEX_TOOLS_BUILD_DIR}/spec_indexer"
        --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/spec_indexer_config.json"
        --workspace "${MOEX_TEST_OUTPUT_DIR}/spec-indexer")

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
