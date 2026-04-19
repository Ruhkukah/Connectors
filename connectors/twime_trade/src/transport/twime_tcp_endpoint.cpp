#include "moex/twime_trade/transport/twime_tcp_endpoint.hpp"

#include <algorithm>
#include <cctype>

namespace moex::twime_trade::transport {

namespace {

std::string lowercase_copy(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

} // namespace

bool twime_is_explicit_loopback_host(std::string_view host) noexcept {
    const auto lowered = lowercase_copy(host);
    return lowered == "127.0.0.1" || lowered == "::1" || lowered == "localhost";
}

bool twime_host_looks_production_like(std::string_view host) noexcept {
    const auto lowered = lowercase_copy(host);
    return lowered.find("moex") != std::string::npos || lowered.find("spectra") != std::string::npos ||
           lowered.find("alor") != std::string::npos || lowered.find("broker") != std::string::npos;
}

} // namespace moex::twime_trade::transport
