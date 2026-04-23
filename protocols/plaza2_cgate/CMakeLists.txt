if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/schema/plaza2_forts_reviewed.ini")
    message(FATAL_ERROR
        "Reviewed PLAZA II scheme fixture is missing at protocols/plaza2_cgate/schema/plaza2_forts_reviewed.ini. "
        "Materialize it from the locked docs with: "
        "python3 tools/plaza2_schema_materialize.py --project-root .")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/plaza2_generated_metadata.cpp")
    message(FATAL_ERROR
        "Generated PLAZA II metadata is missing at protocols/plaza2_cgate/generated/plaza2_generated_metadata.cpp. "
        "Re-generate it with: "
        "python3 tools/plaza2_codegen.py --schema protocols/plaza2_cgate/schema/plaza2_forts_reviewed.ini "
        "--out protocols/plaza2_cgate/generated")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/plaza2_fake_scenarios.cpp")
    message(FATAL_ERROR
        "Generated PLAZA II fake scenarios are missing at protocols/plaza2_cgate/generated/plaza2_fake_scenarios.cpp. "
        "Re-generate them with: "
        "python3 tools/plaza2_fake_scenario_materialize.py --project-root .")
endif()

add_library(moex_plaza2_cgate_metadata STATIC
    generated/plaza2_generated_metadata.cpp)

target_include_directories(moex_plaza2_cgate_metadata
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_compile_features(moex_plaza2_cgate_metadata PUBLIC cxx_std_20)

add_library(moex_plaza2_cgate_runtime STATIC
    src/plaza2_runtime.cpp)

target_link_libraries(moex_plaza2_cgate_runtime
    PUBLIC
        moex_plaza2_cgate_metadata
    PRIVATE
        ${CMAKE_DL_LIBS})

target_include_directories(moex_plaza2_cgate_runtime
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_compile_features(moex_plaza2_cgate_runtime PUBLIC cxx_std_20)

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

add_library(moex_plaza2_cgate_private_state STATIC
    src/plaza2_private_state.cpp)

target_link_libraries(moex_plaza2_cgate_private_state
    PUBLIC
        moex_plaza2_cgate_fake_engine)

target_include_directories(moex_plaza2_cgate_private_state
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_compile_features(moex_plaza2_cgate_private_state PUBLIC cxx_std_20)
