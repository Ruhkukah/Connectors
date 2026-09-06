cmake_minimum_required(VERSION 3.24)

project(
    MoexConnector
    VERSION 0.1.0
    DESCRIPTION "Standalone C++20/Linux-first MOEX connector suite"
    LANGUAGES CXX)

execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE MOEX_SOURCE_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
option(MOEX_BUILD_DOTNET_TESTS "Build and register the synthetic .NET ABI tests" ON)
option(MOEX_BUILD_OPERATOR_TOOLS "Build offline operator and evidence tools" ON)

if(BUILD_TESTING OR MOEX_BUILD_OPERATOR_TOOLS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
if(BUILD_TESTING AND MOEX_BUILD_DOTNET_TESTS)
    find_program(DOTNET_EXECUTABLE NAMES dotnet)
endif()

set(MOEX_ALORENGINE_PROJECT "" CACHE FILEPATH "Optional path to AlorEngine.csproj for the shadow replay integration harness")
if(NOT MOEX_ALORENGINE_PROJECT AND DEFINED ENV{ALORENGINE_PROJECT})
    set(MOEX_ALORENGINE_PROJECT "$ENV{ALORENGINE_PROJECT}")
endif()

set(MOEX_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(MOEX_BUILD_ROOT "${CMAKE_CURRENT_BINARY_DIR}")
set(MOEX_TOOLS_BUILD_DIR "${MOEX_BUILD_ROOT}/tools")
set(MOEX_APPS_BUILD_DIR "${MOEX_BUILD_ROOT}/apps")
set(MOEX_TEST_OUTPUT_DIR "${MOEX_BUILD_ROOT}/test-output")

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

add_library(moex_phase0_core STATIC src/phase0_core.cpp)
add_library(moex_phase0_abi SHARED src/phase0_core.cpp)
foreach(target_name IN ITEMS moex_phase0_core moex_phase0_abi)
    target_include_directories(${target_name} PUBLIC "${MOEX_SOURCE_ROOT}/include")
    target_compile_features(${target_name} PUBLIC cxx_std_20)
    target_compile_definitions(${target_name}
        PRIVATE
            MOEX_PHASE0_PROJECT_VERSION="${PROJECT_VERSION}"
            MOEX_PHASE0_SOURCE_ROOT="${MOEX_SOURCE_ROOT}")
endforeach()
set_target_properties(moex_phase0_abi PROPERTIES
    OUTPUT_NAME "moex_phase0_abi"
    LIBRARY_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${MOEX_BUILD_ROOT}/lib")

add_subdirectory(protocols/twime_sbe)
add_subdirectory(protocols/plaza2_cgate)
add_subdirectory(connectors/twime_trade)
add_subdirectory(connectors/plaza2_twime_reconciler)
add_subdirectory(connectors/plaza2_trade)
add_subdirectory(connectors/connector_host)

target_link_libraries(moex_phase0_core PUBLIC moex_plaza2_cgate_private_state moex_plaza2_twime_reconciler)
target_link_libraries(moex_phase0_abi PUBLIC moex_plaza2_cgate_private_state moex_plaza2_twime_reconciler)

if(BUILD_TESTING OR MOEX_BUILD_OPERATOR_TOOLS)
    add_subdirectory(apps)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
