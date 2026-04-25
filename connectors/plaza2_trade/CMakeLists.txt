add_library(moex_plaza2_trade_offline_codec STATIC
    src/plaza2_trade_codec.cpp)

target_include_directories(moex_plaza2_trade_offline_codec
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_compile_features(moex_plaza2_trade_offline_codec PUBLIC cxx_std_20)
