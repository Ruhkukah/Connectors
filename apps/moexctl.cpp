#include "moex/connector_host/operator_config.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

int main(int argc, char** argv) {
    using namespace moex::connector_host;
    try {
        std::vector<std::string_view> arguments;
        for (int i = 1; i < argc; ++i)
            arguments.emplace_back(argv[i]);
        const auto request = parse_operator_arguments(arguments);
        if (request.help) {
            std::cout << operator_help();
            return 0;
        }
        std::string canonical;
        if (request.command == "order-test") {
            std::ifstream input(request.canonical_plan_path, std::ios::binary);
            if (!input)
                throw std::invalid_argument("cannot read canonical plan");
            char ch{};
            while (canonical.size() <= 65536 && input.get(ch))
                canonical += ch;
            if (canonical.size() > 65536)
                throw std::invalid_argument("canonical plan too large");
        }
        ConnectorHost host(request.config);
        if (const auto error = host.start()) {
            std::cout << render_snapshot(host.snapshot(), request.json);
            return 3;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request.wait_ms);
        do {
            if (host.poll())
                break;
            if (host.snapshot().observation_ready)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        int result = host.snapshot().observation_ready ? 0 : 4;
        if (result == 0 && request.command == "order-test") {
            if (host.authorize(canonical, request.authorized_sha256))
                result = 5;
            else if (!host.submit().ok)
                result = 6;
        }
        // Capture current qualification/terminal evidence before explicit close.
        std::cout << render_snapshot(host.snapshot(), request.json);
        if (host.stop())
            return 7;
        return result;
    } catch (const std::invalid_argument& error) {
        std::cerr << "moexctl: " << error.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "moexctl: operation failed\n";
        return 3;
    }
}
