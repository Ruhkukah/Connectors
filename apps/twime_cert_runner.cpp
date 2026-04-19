#include "moex/twime_trade/twime_cert_scenario_runner.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string escape_json(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const auto ch : value) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        default:
            out.push_back(ch);
            break;
        }
    }
    return out;
}

std::string state_name(moex::twime_trade::TwimeSessionState state) {
    using moex::twime_trade::TwimeSessionState;
    switch (state) {
    case TwimeSessionState::Created:
        return "Created";
    case TwimeSessionState::ConnectingFake:
        return "ConnectingFake";
    case TwimeSessionState::Establishing:
        return "Establishing";
    case TwimeSessionState::Active:
        return "Active";
    case TwimeSessionState::Terminating:
        return "Terminating";
    case TwimeSessionState::Terminated:
        return "Terminated";
    case TwimeSessionState::Rejected:
        return "Rejected";
    case TwimeSessionState::Faulted:
        return "Faulted";
    case TwimeSessionState::Recovering:
        return "Recovering";
    }
    return "Unknown";
}

} // namespace

int main(int argc, char** argv) {
    std::string scenario_id;
    fs::path output_dir;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--scenario-id" && index + 1 < argc) {
            scenario_id = argv[++index];
            continue;
        }
        if (argument == "--output-dir" && index + 1 < argc) {
            output_dir = argv[++index];
            continue;
        }
        std::cerr << "unknown argument: " << argument << '\n';
        return 2;
    }

    if (scenario_id.empty() || output_dir.empty()) {
        std::cerr << "usage: moex_twime_cert_runner --scenario-id <id> --output-dir <dir>\n";
        return 2;
    }

    const auto scenario = moex::twime_trade::TwimeCertScenarioRunner::builtin(scenario_id);
    if (!scenario.has_value()) {
        std::cerr << "unknown TWIME synthetic scenario: " << scenario_id << '\n';
        return 2;
    }

    const auto result = moex::twime_trade::TwimeCertScenarioRunner{}.run(*scenario);
    if (!result.error_message.empty()) {
        std::cerr << "scenario failed: " << result.error_message << '\n';
        return 1;
    }
    fs::create_directories(output_dir);

    const auto log_path = output_dir / (scenario_id + ".cert.log");
    std::ofstream log(log_path);
    for (const auto& line : result.cert_log_lines) {
        log << line << '\n';
    }

    const auto summary_path = output_dir / (scenario_id + ".summary.json");
    std::ofstream summary(summary_path);
    summary << "{\n";
    summary << "  \"scenario_id\": \"" << escape_json(result.scenario_id) << "\",\n";
    summary << "  \"title\": \"" << escape_json(result.title) << "\",\n";
    summary << "  \"final_state\": \"" << escape_json(state_name(result.final_state)) << "\",\n";
    summary << "  \"event_count\": " << result.events.size() << ",\n";
    summary << "  \"log_path\": \"" << escape_json(log_path.string()) << "\"\n";
    summary << "}\n";

    std::cout << "synthetic TWIME cert log emitted: " << log_path << '\n';
    return 0;
}
