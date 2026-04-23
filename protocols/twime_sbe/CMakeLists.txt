if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/schema/twime_spectra-7.7.xml")
    message(FATAL_ERROR
        "Pinned TWIME schema is missing at protocols/twime_sbe/schema/twime_spectra-7.7.xml. "
        "Materialize it from the local spec-lock cache with: "
        "python3 tools/twime_schema_materialize.py --manifest spec-lock/prod/twime/manifest.json")
endif()

add_library(moex_twime_sbe STATIC
    src/twime_codec.cpp
    src/twime_frame_assembler.cpp
    src/twime_cert_log_formatter.cpp
    generated/twime_generated_metadata.cpp)

set_target_properties(moex_twime_sbe PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

target_include_directories(moex_twime_sbe
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_compile_features(moex_twime_sbe PUBLIC cxx_std_20)
