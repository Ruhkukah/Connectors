add_library(moex_plaza2_cgate_metadata STATIC
    src/plaza2_metadata.cpp)

if(BUILD_TESTING)
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
