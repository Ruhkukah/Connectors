#pragma once

#include "moex/plaza2/cgate/plaza2_field_read_audit.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace moex::plaza2::test {
class FieldReadContract {
    inline static thread_local FieldReadContract* active{};
    std::array<generated::FieldCode, 512> codes{};
    std::size_t count{};
    bool overflow{};
    static void record(generated::FieldCode code) noexcept {
        if (std::find(active->codes.begin(), active->codes.begin() + active->count, code) !=
            active->codes.begin() + active->count)
            return;
        if (active->count == active->codes.size())
            active->overflow = true;
        else
            active->codes[active->count++] = code;
    }

  public:
    FieldReadContract() {
        if (active)
            throw std::runtime_error("field-read audit already active");
        active = this;
        generated::SetFieldReadObserver(record);
    }
    ~FieldReadContract() {
        generated::SetFieldReadObserver(nullptr);
        active = nullptr;
    }
    bool all_required(std::optional<generated::FieldCode> omitted = {}) const {
        return !overflow && std::all_of(codes.begin(), codes.begin() + count, [&](auto code) {
            return code != omitted && generated::FindFieldByCode(code) != nullptr;
        });
    }
    void verify(std::string_view suite, std::size_t minimum) const {
        if (count < minimum)
            throw std::runtime_error("actual field accessor reads were not recorded for " + std::string(suite));
        if (!all_required())
            throw std::runtime_error("actual field accessor read is missing from required schema list");
        // Remove an actually observed requirement to prove that this guard
        // checks accessor reads, rather than simply enumerating descriptors.
        if (all_required(codes[0]))
            throw std::runtime_error("missing actual field requirement escaped the audit");
        std::cout << suite << " actual accessor field codes: " << count << ", all required\n";
    }
};
} // namespace moex::plaza2::test
