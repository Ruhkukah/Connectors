add_library(moex_plaza2_trade_codec STATIC
    src/plaza2_trade_codec.cpp)

target_include_directories(moex_plaza2_trade_codec
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_link_libraries(moex_plaza2_trade_codec
    PUBLIC
        moex_plaza2_cgate_metadata)

target_compile_features(moex_plaza2_trade_codec PUBLIC cxx_std_20)

add_library(moex_plaza2_trade_session STATIC
    src/cgate_session.cpp)

target_include_directories(moex_plaza2_trade_session
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(moex_plaza2_trade_session
    PUBLIC
        moex_plaza2_trade_codec
        moex_plaza2_cgate_private_state
        moex_plaza2_cgate_aggr20_md
        moex_plaza2_cgate_runtime)

target_compile_features(moex_plaza2_trade_session PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_library(moex_plaza2_trade_fake_session STATIC src/plaza2_trade_fake_session.cpp)
    target_link_libraries(moex_plaza2_trade_fake_session PUBLIC
        moex_plaza2_trade_codec moex_plaza2_cgate_fake_engine)
endif()
