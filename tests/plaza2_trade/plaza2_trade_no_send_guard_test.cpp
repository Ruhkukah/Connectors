#include "plaza2_trade_test_support.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using moex::plaza2_trade::is_sendable;
using moex::plaza2_trade::Plaza2TradeCodec;
using moex::plaza2_trade::Plaza2TradeCommandRequest;
using moex::plaza2_trade::test_support::make_add_order;
using moex::plaza2_trade::test_support::require;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("missing file: " + path.string());
    }
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void test_encoded_command_is_not_sendable() {
    const Plaza2TradeCodec codec;
    const auto encoded = codec.encode(Plaza2TradeCommandRequest{make_add_order()});
    require(encoded.validation.ok(), "sample command should encode");
    require(encoded.offline_only, "encoded command should carry offline-only marker");
    require(!is_sendable(encoded), "encoded command must not be sendable");
}

void test_offline_codec_does_not_reference_cgate_publisher_api() {
    const auto source_root = std::filesystem::path(MOEX_SOURCE_ROOT);
    const auto source = read_file(source_root / "connectors/plaza2_trade/src/plaza2_trade_codec.cpp");
    const auto fake_source = read_file(source_root / "connectors/plaza2_trade/src/plaza2_trade_fake_session.cpp");
    const auto header =
        read_file(source_root / "connectors/plaza2_trade/include/moex/plaza2_trade/plaza2_trade_codec.hpp");
    const auto fake_header =
        read_file(source_root / "connectors/plaza2_trade/include/moex/plaza2_trade/plaza2_trade_fake_session.hpp");
    const auto cmake = read_file(source_root / "connectors/plaza2_trade/CMakeLists.txt");
    const auto combined = source + fake_source + header + fake_header + cmake;
    require(combined.find("cg_pub_") == std::string::npos, "offline codec must not reference CGate publisher API");
    require(combined.find("publish(") == std::string::npos, "offline codec must not expose publish operation");
    require(combined.find("submit_live(") == std::string::npos, "offline codec must not expose live submit operation");
    require(combined.find("moex_plaza2_cgate_runtime") == std::string::npos,
            "offline trade module must not link CGate runtime");
}

} // namespace

int main() {
    try {
        test_encoded_command_is_not_sendable();
        test_offline_codec_does_not_reference_cgate_publisher_api();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
