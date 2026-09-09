#include <cgate.h>
#include "moex/plaza2/cgate/plaza2_public_decode.hpp"
#include <cstdio>
using moex::plaza2::public_wire::Bcd16_5;
int main() {
    if (cg_env_open(""))
        return 1;
    unsigned long long state = 1;
    unsigned count = 0;
    for (unsigned i = 0; i < 100004; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        long long n = i == 0   ? 0
                      : i == 1 ? 1
                      : i == 2 ? 9999999999999999LL
                               : static_cast<long long>(state % 10000000000000000ULL);
        if (i & 1)
            n = -n;
        Bcd16_5 bytes{5, 16};
        auto value = static_cast<unsigned long long>(n < 0 ? -n : n) * 10;
        for (int j = 10; j >= 2; --j) {
            bytes[j] = value % 100;
            value /= 100;
        }
        if (n < 0)
            bytes[2] |= 0x80;
        int64_t native = 0;
        int8_t scale = 0;
        if (cg_bcd_get(bytes.data(), &native, &scale) || native != n || scale != 5 ||
            moex::plaza2::public_wire::decimal_scaled(bytes) != n)
            return 2;
        ++count;
    }
    cg_env_close();
    printf("{\"official_cg_bcd_get_cases\":%u,\"failed\":0,\"network\":\"none\"}\n", count);
}
