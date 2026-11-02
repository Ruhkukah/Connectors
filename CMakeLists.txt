cmake_minimum_required(VERSION 3.24)

project(
    MoexConnector
    VERSION 1.0.0
    DESCRIPTION "Standalone C++20/Linux-first MOEX connector suite"
    LANGUAGES CXX)

# Archive builds may supply the revision from their verified source manifest.
if(NOT DEFINED MOEX_SOURCE_GIT_SHA)
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE MOEX_SOURCE_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT MOEX_SOURCE_GIT_SHA)
    set(MOEX_SOURCE_GIT_SHA "unknown")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
option(MOEX_BUILD_APPS "Build the CGate operator applications" ON)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

set(MOEX_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(MOEX_BUILD_ROOT "${CMAKE_CURRENT_BINARY_DIR}")
set(MOEX_APPS_BUILD_DIR "${MOEX_BUILD_ROOT}/apps")
set(MOEX_TEST_OUTPUT_DIR "${MOEX_BUILD_ROOT}/test-output")
file(MAKE_DIRECTORY "${MOEX_APPS_BUILD_DIR}" "${MOEX_TEST_OUTPUT_DIR}")

add_subdirectory(protocols/plaza2_cgate)
add_subdirectory(connectors/plaza2_trade)
add_subdirectory(connectors/connector_host)
if(MOEX_BUILD_APPS)
    add_subdirectory(apps)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
