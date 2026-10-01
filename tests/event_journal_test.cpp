#include "moex/connector_host/event_journal.hpp"
#include "command_input.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <unistd.h>

using namespace moex::connector_host;
void require(bool condition, const char* text) {
    if (!condition)
        throw std::runtime_error(text);
}
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("moex-journal-" + std::to_string(getpid()));
    try {
        std::filesystem::create_directories(root);
        const auto path = root / "events.ndjson";
        {
            EventJournal journal(path);
            journal.append("reservation", "{\"next_ext_id\":17,\"next_user_id\":40}");
            journal.append("reply", "{\"message\":" + json_string("quote\" slash\\ newline\n") + "}");
            journal.flush();
            bool locked{};
            try {
                EventJournal second(path);
            } catch (const std::runtime_error&) {
                locked = true;
            }
            require(locked, "concurrent log ownership allowed");
        }
        {
            std::ofstream partial(path, std::ios::app);
            partial << "{\"event\":\"partial";
        }
        {
            EventJournal restarted(path);
            require(restarted.reservations().next_ext_id == 17 && restarted.reservations().next_user_id == 40,
                    "restart reservation missing");
            restarted.append("shutdown");
        }
        std::ifstream input(path);
        std::string line;
        int count{};
        while (std::getline(input, line)) {
            require(line.find("\"utc\":") != std::string::npos && line.find("\"msk\":") != std::string::npos &&
                        line.find("+03:00") != std::string::npos,
                    "timestamp missing");
            require(line.find("partial") == std::string::npos, "partial crash tail retained");
            ++count;
        }
        require(count == 3, "journal did not append all events");
        const auto stable = root / "instance.state";
        const auto first_log = root / "day-one.ndjson";
        const auto second_log = root / "day-two.ndjson";
        {
            EventJournal journal(first_log, stable);
            journal.append("reservation", "{\"next_ext_id\":501,\"next_user_id\":900}");
            for (int i = 0; i < 1000; ++i)
                journal.append("reply", "{\"message\":" + json_string(std::string(1024, 'x')) + "}");
            journal.flush();
            bool locked{};
            try {
                EventJournal other_file(second_log, stable);
            } catch (const std::runtime_error&) {
                locked = true;
            }
            require(locked, "two log files shared an instance identity concurrently");
        }
        {
            EventJournal restarted(first_log, stable);
            require(restarted.recovery_read_bytes() == 0, "restart reread the checkpointed megabyte");
            require(restarted.reservations().next_ext_id == 501, "checkpoint reservation lost");
        }
        // Simulate complete, uncheckpointed records followed by a torn crash tail.
        {
            std::ofstream tail(first_log, std::ios::app);
            tail << "{\"next_ext_id\":502,\"next_user_id\":901}\n{\"next_ext_id\":999";
        }
        {
            EventJournal restarted(first_log, stable);
            require(restarted.recovery_read_bytes() > 0 && restarted.recovery_read_bytes() < 100,
                    "restart scanned beyond the uncheckpointed tail");
            require(restarted.reservations().next_ext_id == 502 && restarted.reservations().next_user_id == 901,
                    "complete crash-tail reservation not recovered");
        }
        {
            EventJournal rotated(second_log, stable);
            require(rotated.reservations().next_ext_id == 502 && rotated.reservations().next_user_id == 901,
                    "log rotation reused identifiers");
            require(rotated.recovery_read_bytes() == 0, "empty rotated file unexpectedly scanned");
        }
        std::ifstream saved_state(stable);
        std::ostringstream saved;
        saved << saved_state.rdbuf();
        auto numeric_corruption = saved.str();
        const auto counters = numeric_corruption.find(" 502 901 ");
        require(counters != std::string::npos, "test checkpoint missing reserved counters");
        numeric_corruption.replace(counters, 9, " 1 1 ");
        std::ofstream(stable) << numeric_corruption;
        bool rollback_refused{};
        try {
            EventJournal corrupted_counters(second_log, stable);
        } catch (const std::runtime_error&) {
            rollback_refused = true;
        }
        require(rollback_refused, "valid-looking checkpoint corruption rolled identifiers back");
        {
            std::ofstream corrupt(stable);
            corrupt << "MOEXJ2 damaged";
        }
        bool corrupt_refused{};
        try {
            EventJournal corrupt(second_log, stable);
        } catch (const std::runtime_error&) {
            corrupt_refused = true;
        }
        require(corrupt_refused, "damaged checkpoint silently reset identifiers");
        CommandInput commands;
        std::vector<std::string> lines;
        int input_errors{};
        const auto on_line = [&](const std::string& value) { lines.push_back(value); };
        const auto on_error = [&](std::string_view) { ++input_errors; };
        commands.feed(std::string(65536, 'x'), on_line, on_error);
        commands.feed(std::string(4096, 'x'), on_line, on_error);
        commands.feed("more invalid input\nkill on\ncan", on_line, on_error);
        commands.feed("cel-all 1001\n", on_line, on_error);
        require(input_errors == 1 && lines == std::vector<std::string>{"kill on", "cancel-all 1001"},
                "overlong input terminated processing or corrupted later emergency commands");
        const auto ini = root / "cgate.ini";
        {
            std::ofstream out(ini);
            out << "[cgate]\nlog=p2:p2syslog\n[p2syslog]\n";
        }
        validate_cgate_logging("ini=" + ini.string());
        validate_cgate_logging("ini=config/cgate.ini", root);
        require(json_string(std::string(1, '\xff')) == "\"\\ufffd\"", "invalid UTF8 corrupted JSON");
        for (const auto option : {";log=", ";log=std", ";minloglevel=error"}) {
            bool refused{};
            try {
                validate_cgate_logging("ini=" + ini.string() + option);
            } catch (const std::invalid_argument&) {
                refused = true;
            }
            require(refused, "logging-disabled settings accepted");
        }
        {
            std::ofstream out(ini);
            out << "[cgate]\nlog=\n[p2syslog]\n";
        }
        bool refused{};
        try {
            validate_cgate_logging("ini=" + ini.string());
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        require(refused, "logging-disabled ini accepted");
        for (const auto contents :
             {"[cgate]\nlog=p2:p2syslog\n;[p2syslog]\n", "[cgate]\n# log=p2:p2syslog\n[p2syslog]\n"}) {
            std::ofstream(ini) << contents;
            bool invalid{};
            try {
                validate_cgate_logging("ini=" + ini.string());
            } catch (const std::invalid_argument&) {
                invalid = true;
            }
            require(invalid, "commented logging configuration passed validation");
        }
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
