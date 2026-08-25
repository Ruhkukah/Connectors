function(moex_generate_python_launcher launcher_dir launcher_name script_relative_path)
    set(LAUNCHER_SOURCE "${MOEX_SOURCE_ROOT}/${script_relative_path}")
    set(LAUNCHER_DEST "${launcher_dir}/${launcher_name}")
    set(LAUNCHER_SCRIPT_PATH "${LAUNCHER_SOURCE}")
    configure_file("${MOEX_SOURCE_ROOT}/cmake/python_launcher.sh.in" "${LAUNCHER_DEST}" @ONLY)
    file(CHMOD "${LAUNCHER_DEST}"
        PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
endfunction()

set(moex_python_launchers
    "tools|spec_indexer|tools/spec_indexer.py"
    "tools|plaza2_scheme_lock|tools/plaza2_scheme_lock.py"
    "tools|plaza2_phase3a_materialize|tools/plaza2_phase3a_materialize.py"
    "tools|plaza2_schema_materialize|tools/plaza2_schema_materialize.py"
    "tools|plaza2_codegen|tools/plaza2_codegen.py"
    "tools|plaza2_fake_scenario_materialize|tools/plaza2_fake_scenario_materialize.py"
    "tools|plaza2_phase5a_trade_materialize|tools/plaza2_phase5a_trade_materialize.py"
    "tools|plaza2_phase5b_trade_codec_materialize|tools/plaza2_phase5b_trade_codec_materialize.py"
    "tools|osengine_inventory|tools/osengine_inventory.py"
    "tools|matrix_validate|tools/matrix_validate.py"
    "tools|profile_check|tools/profile_check.py"
    "tools|twime_schema_materialize|tools/twime_schema_materialize.py"
    "tools|twime_schema_indexer|tools/twime_schema_indexer.py"
    "tools|twime_codegen|tools/twime_codegen.py"
    "tools|source_style_check|tools/source_style_check.py"
    "tools|repo_style_check|tools/repo_style_check.py"
    "tools|unicode_guard|tools/unicode_guard.py"
    "apps|moex_cert_runner|apps/moex_cert_runner.py"
    "apps|moex_plaza2_cert_runner|apps/moex_plaza2_cert_runner.py"
    "apps|moex_plaza2_twime_integrated_runner|apps/moex_plaza2_twime_integrated_runner.py"
    "apps|moex_plaza2_aggr20_md_runner|apps/moex_plaza2_aggr20_md_runner.py"
    "apps|moex_replay_tool|apps/moex_replay_tool.py")
foreach(launcher_spec IN LISTS moex_python_launchers)
    string(REPLACE "|" ";" launcher_fields "${launcher_spec}")
    list(GET launcher_fields 0 launcher_kind)
    list(GET launcher_fields 1 launcher_name)
    list(GET launcher_fields 2 launcher_script)
    if(launcher_kind STREQUAL "tools")
        set(launcher_dir "${MOEX_TOOLS_BUILD_DIR}")
    else()
        set(launcher_dir "${MOEX_APPS_BUILD_DIR}")
    endif()
    moex_generate_python_launcher("${launcher_dir}" "${launcher_name}" "${launcher_script}")
endforeach()

function(moex_add_app target_name source_file link_library)
    add_executable(${target_name} "${MOEX_SOURCE_ROOT}/apps/${source_file}")
    target_link_libraries(${target_name} PRIVATE ${link_library})
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    if(MOEX_SOURCE_GIT_SHA)
        target_compile_definitions(${target_name} PRIVATE MOEX_SOURCE_GIT_SHA="${MOEX_SOURCE_GIT_SHA}")
    endif()
    set_target_properties(${target_name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MOEX_APPS_BUILD_DIR}")
endfunction()

moex_add_app(moex_twime_cert_runner twime_cert_runner.cpp moex_twime_trade)
moex_add_app(moex_plaza2_test_runner plaza2_test_runner.cpp moex_plaza2_cgate_live_session)
moex_add_app(moex_plaza2_test_connectivity_qualifier plaza2_test_connectivity_qualifier.cpp
    moex_plaza2_cgate_connectivity_qualifier)
moex_add_app(moex_plaza2_aggr20_md_test_runner plaza2_aggr20_md_test_runner.cpp moex_plaza2_cgate_aggr20_md)
moex_add_app(moex_plaza2_order_lifecycle_runner plaza2_order_lifecycle_runner.cpp moex_plaza2_trade_order_lifecycle)
moex_add_app(moex_plaza2_twime_integrated_test_runner plaza2_twime_integrated_test_runner.cpp
    moex_plaza2_twime_integrated_test)
