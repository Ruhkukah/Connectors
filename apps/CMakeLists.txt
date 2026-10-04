function(moex_add_app target_name source_file link_library)
    add_executable(${target_name} "${MOEX_SOURCE_ROOT}/apps/${source_file}")
    target_link_libraries(${target_name} PRIVATE ${link_library})
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    if(MOEX_SOURCE_GIT_SHA)
        target_compile_definitions(${target_name} PRIVATE
            MOEX_SOURCE_GIT_SHA="${MOEX_SOURCE_GIT_SHA}"
            MOEX_BUILD_CONFIGURATION="$<CONFIG>"
            MOEX_CXX_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
            MOEX_CXX_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}")
    endif()
    set_target_properties(${target_name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MOEX_APPS_BUILD_DIR}")
endfunction()

moex_add_app(moexctl moexctl.cpp moex_connector_host)
moex_add_app(moex_connector_host_dtc_runner moex_connector_host_dtc_runner.cpp moex_connector_host)

moex_add_app(moex_full_order_log_dtc_runner moex_full_order_log_dtc_runner.cpp moex_connector_host)
