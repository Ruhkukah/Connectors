#include "local_tcp_server.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main() {
    try {
        const auto started = std::chrono::steady_clock::now();
        {
            moex::test::LocalTcpServer server;
            server.stop();
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        require(elapsed < std::chrono::seconds{1}, "LocalTcpServer stop without client must return promptly");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
