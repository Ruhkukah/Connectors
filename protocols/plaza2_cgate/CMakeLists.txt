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

add_library(moex_plaza2_cgate_metadata STATIC
    generated/plaza2_generated_metadata.cpp)

target_include_directories(moex_plaza2_cgate_metadata
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_compile_features(moex_plaza2_cgate_metadata PUBLIC cxx_std_20)
