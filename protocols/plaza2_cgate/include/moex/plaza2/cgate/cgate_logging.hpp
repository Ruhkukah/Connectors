#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace moex::plaza2::cgate {
inline void validate_cgate_logging(std::string_view settings, const std::filesystem::path& config_dir = {}) {
    const auto trim = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        return first == std::string::npos ? std::string{}
                                          : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    };
    const auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
        return value;
    };
    const auto check_cgate = [](const std::string& key, const std::string& value) {
        // The vendor CGate manual defines debug as the default minimum.
        // Trace includes that diagnostic output; higher thresholds suppress it.
        if ((key == "log" && value != "p2:p2syslog") ||
            (key == "minloglevel" && value != "debug" && value != "trace") ||
            ((key == "logging" || key == "enabled") && (value == "0" || value == "false" || value == "off")))
            throw std::invalid_argument("CGate default logging must remain enabled; remove logging/severity overrides");
    };
    const auto check_sink = [](const std::string& key, const std::string& value) {
        // Section 2.4.1 documents logfile=nul as disabling the P2 log file.
        if ((key == "logfile" && (value == "nul" || value == "null" || value == "/dev/null")) ||
            ((key == "logging" || key == "enabled") && (value == "0" || value == "false" || value == "off")))
            throw std::invalid_argument("CGate P2 logging sink must remain enabled");
    };
    std::string text(settings), ini;
    std::replace(text.begin(), text.end(), ';', '\n');
    std::istringstream overrides(text);
    std::string line;
    bool log{};
    while (std::getline(overrides, line)) {
        const auto equal = line.find('=');
        if (equal == std::string::npos)
            continue;
        const auto key = lower(trim(line.substr(0, equal)));
        const auto value = trim(line.substr(equal + 1));
        check_cgate(key, lower(value));
        log |= key == "log" && lower(value) == "p2:p2syslog";
        if (key == "ini")
            ini = value;
    }
    if (ini.empty())
        throw std::invalid_argument("CGate env settings require an ini with default logging");
    std::filesystem::path path(ini);
    if (!path.is_absolute() && !config_dir.empty()) {
        const auto nested = config_dir / path;
        path = std::filesystem::exists(nested) ? nested : config_dir / path.filename();
    }
    std::ifstream input(path);
    if (!input)
        throw std::invalid_argument("cannot read CGate logging ini");
    std::string section;
    std::string logfile;
    bool sink{};
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';')
            continue;
        for (std::size_t i = 1; i < line.size(); ++i)
            if ((line[i] == '#' || line[i] == ';') && std::isspace(static_cast<unsigned char>(line[i - 1]))) {
                line = trim(line.substr(0, i));
                break;
            }
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            sink |= section == "p2syslog";
            continue;
        }
        const auto equal = line.find('=');
        if (equal == std::string::npos)
            continue;
        const auto key = lower(trim(line.substr(0, equal))), raw_value = trim(line.substr(equal + 1));
        const auto value = lower(raw_value);
        if (section == "cgate")
            check_cgate(key, value);
        else if (section == "p2syslog") {
            check_sink(key, value);
            if (key == "logfile")
                logfile = raw_value;
        }
        log |= section == "cgate" && key == "log" && value == "p2:p2syslog";
    }
    if (!log || !sink)
        throw std::invalid_argument(
            "CGate requires log=p2:p2syslog in env settings or [cgate], and a [p2syslog] section");
    const auto log_path = std::filesystem::path(logfile).lexically_normal();
    if (!log_path.is_absolute() || log_path.filename().empty() || log_path == "/dev/null")
        throw std::invalid_argument("CGate [p2syslog] requires an absolute enabled logfile path");
}
} // namespace moex::plaza2::cgate
