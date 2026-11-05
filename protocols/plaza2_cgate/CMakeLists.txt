add_library(moex_plaza2_cgate_metadata STATIC
    src/plaza2_metadata.cpp)

if(BUILD_TESTING)
    # The fake CGate shared library links this static target.
    set_target_properties(moex_plaza2_cgate_metadata PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_sources(moex_plaza2_cgate_metadata PRIVATE src/plaza2_field_read_audit.cpp)
    target_compile_definitions(moex_plaza2_cgate_metadata PUBLIC MOEX_CGATE_FIELD_READ_AUDIT=1)
endif()

target_include_directories(moex_plaza2_cgate_metadata
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_compile_features(moex_plaza2_cgate_metadata PUBLIC cxx_std_20)

# Runtime source/file identity hashing.
add_library(moex_plaza2_cgate_hash STATIC src/plaza2_sha256.cpp)
set_target_properties(moex_plaza2_cgate_hash PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(moex_plaza2_cgate_hash PUBLIC moex_plaza2_cgate_metadata)
target_compile_features(moex_plaza2_cgate_hash PUBLIC cxx_std_20)

add_library(moex_plaza2_cgate_runtime STATIC
    src/plaza2_runtime.cpp)

target_link_libraries(moex_plaza2_cgate_runtime
    PUBLIC
        moex_plaza2_cgate_metadata
        moex_plaza2_cgate_hash
    PRIVATE
        ${CMAKE_DL_LIBS})

target_include_directories(moex_plaza2_cgate_runtime
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_compile_features(moex_plaza2_cgate_runtime PUBLIC cxx_std_20)

if(BUILD_TESTING)
add_library(moex_plaza2_cgate_fake_engine STATIC
    src/plaza2_fake_engine.cpp)

target_link_libraries(moex_plaza2_cgate_fake_engine
    PUBLIC
        moex_plaza2_cgate_metadata)

target_include_directories(moex_plaza2_cgate_fake_engine
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_compile_features(moex_plaza2_cgate_fake_engine PUBLIC cxx_std_20)

add_library(moex_plaza2_cgate_fake_scenarios STATIC
    generated/plaza2_fake_scenarios.cpp)

target_link_libraries(moex_plaza2_cgate_fake_scenarios
    PUBLIC
        moex_plaza2_cgate_fake_engine)

target_include_directories(moex_plaza2_cgate_fake_scenarios
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_compile_features(moex_plaza2_cgate_fake_scenarios PUBLIC cxx_std_20)

endif()
add_library(moex_plaza2_cgate_private_state STATIC
    src/plaza2_private_state.cpp)

set_target_properties(moex_plaza2_cgate_private_state PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

target_link_libraries(moex_plaza2_cgate_private_state
    PUBLIC
        moex_plaza2_cgate_metadata)

target_include_directories(moex_plaza2_cgate_private_state
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_compile_features(moex_plaza2_cgate_private_state PUBLIC cxx_std_20)

add_library(moex_plaza2_cgate_bridge STATIC
    src/plaza2_credential_provider.cpp
    src/plaza2_private_state_bridge.cpp)

target_link_libraries(moex_plaza2_cgate_bridge
    PUBLIC
        moex_plaza2_cgate_private_state
        moex_plaza2_cgate_runtime)

target_include_directories(moex_plaza2_cgate_bridge
    PUBLIC
        "${MOEX_SOURCE_ROOT}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_compile_features(moex_plaza2_cgate_bridge PUBLIC cxx_std_20)

add_library(moex_plaza2_cgate_aggr20_md STATIC
    src/plaza2_aggr20_md.cpp
    src/plaza2_public_deals.cpp)

target_link_libraries(moex_plaza2_cgate_aggr20_md
    PUBLIC
        moex_plaza2_cgate_bridge)

target_include_directories(moex_plaza2_cgate_aggr20_md
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_compile_features(moex_plaza2_cgate_aggr20_md PUBLIC cxx_std_20)

add_library(moex_plaza2_cgate_full_order_log STATIC src/plaza2_full_order_log.cpp)
target_link_libraries(moex_plaza2_cgate_full_order_log PUBLIC moex_plaza2_cgate_runtime)
target_compile_features(moex_plaza2_cgate_full_order_log PUBLIC cxx_std_20)

# Deployment copies this subdirectory beside the full vendor forts_scheme.ini.
install(FILES schema/full_order_log/online.ini schema/full_order_log/snapshot.ini
    schema/full_order_log/manifest.json schema/full_order_log/README.md
    DESTINATION share/moex/plaza2/cgate/schema/full_order_log)

# FOL-only optional recorder/exporter; no Zstd linkage is added to moexctl or AGGR.
find_path(MOEX_ZSTD_INCLUDE_DIR zstd.h REQUIRED)
find_library(MOEX_ZSTD_LIBRARY NAMES zstd REQUIRED)
find_package(Threads REQUIRED)
add_library(moex_full_order_log_capture STATIC
    src/plaza2_full_order_log_capture.cpp src/plaza2_full_order_log_archive.cpp src/plaza2_full_order_log_dbn.cpp
    src/plaza2_full_order_log_backfill.cpp
    src/plaza2_full_order_log_oi.cpp src/plaza2_full_order_log_deals.cpp src/plaza2_full_order_log_refdata.cpp)
target_include_directories(moex_full_order_log_capture PRIVATE "${MOEX_ZSTD_INCLUDE_DIR}")
target_link_libraries(moex_full_order_log_capture PUBLIC moex_plaza2_cgate_full_order_log PRIVATE moex_plaza2_cgate_aggr20_md "${MOEX_ZSTD_LIBRARY}" Threads::Threads)

target_compile_definitions(moex_full_order_log_capture PRIVATE MOEX_CAPTURE_SOURCE_SHA="${MOEX_SOURCE_GIT_SHA}")
