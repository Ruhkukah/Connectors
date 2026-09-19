#pragma once

#include "plaza2_day_observer_journal.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace moex::plaza2::observer {

struct DayObserverListenerProfile {
    std::array<generated::StreamCode, 4> streams;
    std::array<std::string, 4> urls;
};

[[nodiscard]] inline bool uses_explicit_client_scheme(std::string_view url) noexcept {
    return url.find(";scheme=") != std::string_view::npos;
}

[[nodiscard]] inline DayObserverListenerProfile
make_day_observer_listener_profile(const std::filesystem::path& scheme_file) {
    using generated::StreamCode;
    const auto scheme = std::filesystem::absolute(scheme_file).string();
    return {
        .streams = {StreamCode::kFortsAggrRepl, StreamCode::kFortsRefdataRepl, StreamCode::kFortsSessionstateRepl,
                    StreamCode::kFortsInstrumentstateRepl},
        .urls = {"p2repl://FORTS_AGGR20_REPL;scheme=|FILE|" + scheme + "|Aggr",
                 "p2repl://FORTS_REFDATA_REPL;scheme=|FILE|" + scheme + "|REFDATA", "p2repl://FORTS_SESSIONSTATE_REPL",
                 "p2repl://FORTS_INSTRUMENTSTATE_REPL"},
    };
}

} // namespace moex::plaza2::observer
