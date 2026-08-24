add_library(moex_plaza2_trade_offline_codec STATIC
    src/plaza2_trade_codec.cpp
    src/plaza2_trade_fake_session.cpp)

target_include_directories(moex_plaza2_trade_offline_codec
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_link_libraries(moex_plaza2_trade_offline_codec
    PUBLIC
        moex_plaza2_cgate_fake_engine)

function(moex_assert_plaza2_trade_offline_boundary)
    get_target_property(offline_links moex_plaza2_trade_offline_codec LINK_LIBRARIES)
    if(moex_plaza2_cgate_runtime IN_LIST offline_links)
        message(FATAL_ERROR "The offline PLAZA trade codec must not link the CGate runtime")
    endif()
endfunction()
cmake_language(DEFER CALL moex_assert_plaza2_trade_offline_boundary)

target_compile_features(moex_plaza2_trade_offline_codec PUBLIC cxx_std_20)

add_library(moex_plaza2_trade_order_lifecycle STATIC
    src/plaza2_order_lifecycle.cpp)

target_include_directories(moex_plaza2_trade_order_lifecycle
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(moex_plaza2_trade_order_lifecycle
    PUBLIC
        moex_plaza2_trade_offline_codec
        moex_plaza2_cgate_private_state
        moex_plaza2_cgate_runtime)

target_compile_features(moex_plaza2_trade_order_lifecycle PUBLIC cxx_std_20)
