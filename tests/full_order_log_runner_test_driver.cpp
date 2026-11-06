#include "plaza2_cgate/fake_cgate_control.hpp"
#include <iostream>
#define main full_order_log_runner_main
#include "../apps/moex_full_order_log_dtc_runner.cpp"
#undef main
int main(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string_view(argv[i]) == "--library-path") {
            moex::plaza2::test::fake::Control control(argv[i + 1]);
            moex::plaza2::test::fake::Scenario scenario;
            scenario.options[static_cast<std::size_t>(moex::plaza2::test::fake::Option::FullOrderLogRefdata)] = "1";
            control.configure(scenario);
            const auto result = full_order_log_runner_main(argc, argv);
            const auto deals = moex::plaza2::generated::StreamCode::kFortsDealsRepl;
            if (result == 0 &&
                std::ranges::any_of(std::span(argv, argc),
                                    [](const char* arg) { return std::string_view(arg) == "--open-interest"; }) &&
                (control.opens(deals) != 1 ||
                 control.listener_settings(deals) != "p2repl://FORTS_DEALS_REPL_MATCH3;tables=deal" ||
                 !control.commands().empty())) {
                std::cerr << "FOL must share one restricted DEALS listener and submit zero orders\n";
                return 1;
            }
            return result;
        }
    return 2;
}
