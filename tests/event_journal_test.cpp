#include "moex/connector_host/event_journal.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
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
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
