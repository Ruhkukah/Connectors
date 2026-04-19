#include "moex/twime_trade/transport/twime_endpoint_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

#include <netdb.h>

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

bool twime_is_placeholder_host(std::string_view host) noexcept {
    const auto lowered = lowercase_copy(host);
    return lowered.find("placeholder") != std::string::npos;
}

bool twime_host_looks_production_like(std::string_view host) noexcept {
    const auto lowered = lowercase_copy(host);
    return lowered.find("moex") != std::string::npos || lowered.find("spectra") != std::string::npos ||
           lowered.find("alor") != std::string::npos || lowered.find("broker") != std::string::npos;
}

bool twime_host_is_private_nonlocal_ipv4(std::string_view host) noexcept {
    const auto lowered = lowercase_copy(host);
    if (lowered.rfind("10.", 0) == 0 || lowered.rfind("192.168.", 0) == 0) {
        return true;
    }
    if (lowered.rfind("172.", 0) != 0) {
        return false;
    }

    const auto second_dot = lowered.find('.', 4);
    if (second_dot == std::string::npos) {
        return false;
    }

    try {
        const auto octet = std::stoi(lowered.substr(4, second_dot - 4));
        return octet >= 16 && octet <= 31;
    } catch (...) {
        return false;
    }
}

TwimeEndpointResolveResult resolve_twime_endpoint(const TwimeTcpEndpoint& endpoint) {
    if (endpoint.port == 0 || endpoint.host.empty() || twime_is_placeholder_host(endpoint.host)) {
        return {.error_code = TwimeTransportErrorCode::InvalidConfiguration};
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* raw_results = nullptr;
    const auto service = std::to_string(endpoint.port);
    const int status = ::getaddrinfo(endpoint.host.c_str(), service.c_str(), &hints, &raw_results);
    if (status != 0 || raw_results == nullptr) {
        return {.error_code = TwimeTransportErrorCode::InvalidConfiguration};
    }

    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> results(raw_results, &::freeaddrinfo);
    for (auto* item = results.get(); item != nullptr; item = item->ai_next) {
        if (item->ai_family != AF_INET && item->ai_family != AF_INET6) {
            continue;
        }

        TwimeResolvedEndpoint resolved;
        resolved.family = item->ai_family;
        resolved.address_size = static_cast<socklen_t>(item->ai_addrlen);
        resolved.resolved_host = endpoint.host;
        std::memcpy(&resolved.address, item->ai_addr, item->ai_addrlen);
        return {
            .endpoint = resolved,
            .error_code = TwimeTransportErrorCode::None,
        };
    }

    return {.error_code = TwimeTransportErrorCode::InvalidConfiguration};
}

} // namespace moex::twime_trade::transport
