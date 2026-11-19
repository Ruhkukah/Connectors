# MOEX Spectra FAST market data: generated decoder, feed sequencing/recovery, UDP receiver.
# The decoder and the stream state machine are header-only; only the socket layer is compiled.
add_library(moex_fast_spectra STATIC src/fast_udp.cpp)

target_include_directories(moex_fast_spectra
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")

target_compile_features(moex_fast_spectra PUBLIC cxx_std_20)
