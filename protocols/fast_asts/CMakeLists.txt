# MOEX ASTS (securities and FX) FAST market data: operator-aware generated decoder,
# feed sequencing/recovery, an order-by-order book and the TCP replay client. The
# stop-bit primitives and the UDP receiver are shared with the Spectra FAST module.
add_library(moex_fast_asts STATIC src/asts_replay_client.cpp)

target_include_directories(moex_fast_asts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_link_libraries(moex_fast_asts PUBLIC moex_fast_spectra)
target_compile_features(moex_fast_asts PUBLIC cxx_std_20)
