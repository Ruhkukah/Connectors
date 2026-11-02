#include "moex/plaza2/cgate/plaza2_publisher_rate.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
using moex::plaza2::cgate::Plaza2PublisherRateGate;
void require(bool value) {
    if (!value)
        throw std::runtime_error("publisher rate boundary failed");
}

void submillisecond_rounding_regression() {
    Plaza2PublisherRateGate gate(5);
    constexpr std::uint64_t first_ns = 925742;
    constexpr std::uint64_t sixth_ns = 1000000000;
    static_assert(sixth_ns - first_ns == 999074258);
    for (std::uint64_t i = 0; i < 5; ++i)
        require(gate.admit((first_ns + i * 1000) / 1000000));
    // The old 1000-ms expiry admitted this sixth post after only 999.074258 ms.
    require(!gate.admit(sixth_ns / 1000000));
    require(!gate.admit(1000999999 / 1000000));
    require(gate.admit(1001000000 / 1000000));
}

void jittered_rolling_seconds(std::uint32_t capacity) {
    Plaza2PublisherRateGate gate(capacity);
    std::vector<std::uint64_t> admitted_ns;
    std::size_t first_in_window{};
    std::uint32_t random = 0x3265A17u;
    for (std::uint64_t cycle = 0; cycle < 30; ++cycle) {
        random = random * 1664525u + 1013904223u;
        const auto start_ns = cycle * 1000000000 + random % 1000000;
        for (std::uint32_t attempt = 0; attempt < capacity + 3; ++attempt) {
            random = random * 1664525u + 1013904223u;
            const auto now_ns = start_ns + attempt * 1000 + random % 1000;
            if (!gate.admit(now_ns / 1000000))
                continue;
            admitted_ns.push_back(now_ns);
            // Independent high-resolution oracle: do not copy the gate's
            // millisecond expiry predicate into the expected result.
            while (now_ns - admitted_ns[first_in_window] >= 1000000000)
                ++first_in_window;
            require(admitted_ns.size() - first_in_window <= capacity);
        }
    }
    require(admitted_ns.size() > capacity * 3 && gate.metrics().throttled > 0);
}
int main() {
    try {
        submillisecond_rounding_regression();
        for (const auto capacity : {1u, 5u, 3000u})
            jittered_rolling_seconds(capacity);
        Plaza2PublisherRateGate gate(2);
        require(gate.admit(0));
        require(gate.admit(500));
        require(!gate.admit(999));
        require(!gate.admit(1000));
        require(gate.admit(1001));
        require(!gate.admit(1499));
        require(!gate.admit(1500));
        require(gate.admit(1501));
        require(!gate.admit(1490));
        require(gate.metrics().clock_regressions == 1);
        gate.penalize(1501, 2000);
        require(!gate.admit(3500));
        require(gate.admit(3501));
        require(gate.metrics().admitted == 5 && gate.metrics().throttled == 6);
        Plaza2PublisherRateGate invalid(0), too_large(3001);
        require(!invalid.valid() && !invalid.admit(1000) && !too_large.valid());
        Plaza2PublisherRateGate maximum(3000);
        for (int i = 0; i < 3000; ++i)
            require(maximum.admit(0));
        require(!maximum.admit(999) && !maximum.admit(1000) && maximum.admit(1001));
        std::cout << "sub-ms rolling-rate oracle, penalty, clock regression and capacity boundaries PASS\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
