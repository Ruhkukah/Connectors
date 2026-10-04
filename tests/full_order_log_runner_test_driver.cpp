#include "plaza2_cgate/fake_cgate_control.hpp"
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
            return full_order_log_runner_main(argc, argv);
        }
    return 2;
}
