#include "adapters/alorengine_capi/moex_c_api.h"
#include "moex_core/phase0_core.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    const auto info = moex::phase0::build_info();
    if (info.project_version.empty()) {
        std::cerr << "project version is empty\n";
        return EXIT_FAILURE;
    }

    if (moex_phase0_abi_version() != MOEX_C_ABI_VERSION) {
        std::cerr << "ABI version mismatch\n";
        return EXIT_FAILURE;
    }

    if (moex_phase0_prod_requires_arm("prod", false)) {
        std::cerr << "prod arming gate failed\n";
        return EXIT_FAILURE;
    }

    if (!moex_phase0_prod_requires_arm("test", false)) {
        std::cerr << "test profile incorrectly rejected\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
