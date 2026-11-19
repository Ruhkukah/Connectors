# MOEX ASTS (securities and FX) FAST market data: operator-aware generated decoder,
# feed sequencing/recovery and an order-by-order book. Header-only; the stop-bit
# primitives and the UDP receiver are shared with the Spectra FAST module.
add_library(moex_fast_asts INTERFACE)

target_include_directories(moex_fast_asts
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_link_libraries(moex_fast_asts INTERFACE moex_fast_spectra)
target_compile_features(moex_fast_asts INTERFACE cxx_std_20)
