#include "moex/plaza2/cgate/plaza2_publisher_rate.hpp"
#include <iostream>
#include <stdexcept>
using moex::plaza2::cgate::Plaza2PublisherRateGate;
void require(bool value) {
    if (!value)
        throw std::runtime_error("publisher rate boundary failed");
}
int main() {
    try {
        Plaza2PublisherRateGate gate(2);
        require(gate.admit(0));
        require(gate.admit(500));
        require(!gate.admit(999));
        require(gate.admit(1000));
        require(!gate.admit(1499));
        require(gate.admit(1500));
        require(!gate.admit(1490));
        require(gate.metrics().clock_regressions == 1);
        gate.penalize(1500, 2000);
        require(!gate.admit(3499));
        require(gate.admit(3500));
        require(gate.metrics().admitted == 5 && gate.metrics().throttled == 4);
        Plaza2PublisherRateGate invalid(0), too_large(3001);
        require(!invalid.valid() && !invalid.admit(1000) && !too_large.valid());
        Plaza2PublisherRateGate maximum(3000);
        for (int i = 0; i < 3000; ++i)
            require(maximum.admit(0));
        require(!maximum.admit(999) && maximum.admit(1000));
        std::cout << "rolling rate, penalty, clock regression and capacity boundaries PASS\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
