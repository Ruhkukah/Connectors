#pragma once
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace full_order_log_probe {
// Called only after validate_cgate_logging has checked the same settings/INI.
inline std::filesystem::path native_log_path(std::string_view settings, const std::filesystem::path& config_dir) {
    const auto trim = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n"), last = value.find_last_not_of(" \t\r\n");
        return first == std::string::npos ? std::string{} : value.substr(first, last - first + 1);
    };
    const auto lower = [](std::string value) {
        for (auto& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    };
    std::string text(settings), ini, section, logfile, line;
    for (auto& c : text)
        if (c == ';')
            c = '\n';
    std::istringstream values(text);
    while (std::getline(values, line)) {
        const auto equal = line.find('=');
        if (equal != std::string::npos && lower(trim(line.substr(0, equal))) == "ini")
            ini = trim(line.substr(equal + 1));
    }
    std::filesystem::path path(ini);
    if (!path.is_absolute())
        path = std::filesystem::exists(config_dir / path) ? config_dir / path : config_dir / path.filename();
    std::ifstream input(path);
    if (!input)
        throw std::invalid_argument("probe cannot read validated native logging INI");
    while (std::getline(input, line)) {
        line = trim(line);
        for (std::size_t i = 1; i < line.size(); ++i)
            if ((line[i] == ';' || line[i] == '#') && std::isspace(static_cast<unsigned char>(line[i - 1]))) {
                line = trim(line.substr(0, i));
                break;
            }
        if (line.empty())
            continue;
        if (line.front() == '[' && line.back() == ']')
            section = lower(trim(line.substr(1, line.size() - 2)));
        const auto equal = line.find('=');
        if (section == "p2syslog" && equal != std::string::npos && lower(trim(line.substr(0, equal))) == "logfile")
            logfile = trim(line.substr(equal + 1));
    }
    if (!std::filesystem::path(logfile).is_absolute())
        throw std::invalid_argument("probe native logfile is not absolute");
    return std::filesystem::path(logfile).lexically_normal();
}
struct NativeCatchupAnchor {
    std::uint64_t revision{}, life{}, connect{};
};
class NativeCatchupLog {
    std::filesystem::path path_;
    std::string stream_, partial_, listener_, candidate_, opened_, thread_, address_;
    std::set<std::string> names_;
    std::optional<NativeCatchupAnchor> anchor_;
    std::uint64_t offset_{}, end_{}, count_{};
    bool redirect_{}, snapshot_{}, correlated_{}, life_verified_{}, complete_{}, reported_{}, stopped_{};
    std::ostream* output_;
    static std::string token(std::string_view line, std::string_view key, std::string_view delimiters = "; ,}") {
        auto at = line.find(key);
        if (at == std::string_view::npos)
            return {};
        at += key.size();
        return std::string(line.substr(at, line.find_first_of(delimiters, at) - at));
    }
    static std::uint64_t number(std::string_view line, std::string_view key) {
        const auto value = token(line, key, "; ,}.-(");
        std::uint64_t result{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
        return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() ? result : 0;
    }
    void report(bool done) {
        if (reported_)
            return;
        if (output_)
            *output_ << "{\"kind\":\"native_ordlog_catchup\",\"connect\":" << count_
                     << ",\"request_revision\":" << (anchor_ ? '"' + std::to_string(anchor_->revision) + '"' : "null")
                     << ",\"request_lifenum\":" << (anchor_ ? '"' + std::to_string(anchor_->life) + '"' : "null")
                     << ",\"catchup_end_revision\":" << (done && end_ ? '"' + std::to_string(end_) + '"' : "null")
                     << ",\"revision_source\":\"vendor_native_log\",\"complete\":" << (done && end_ ? "true" : "false")
                     << ",\"unavailable_reason\":"
                     << (done && end_ ? "null" : "\"request_or_correlated_snapshot_online_not_observed\"") << "}\n"
                     << std::flush;
        reported_ = true;
    }

  public:
    std::function<void(const NativeCatchupAnchor&)> on_request;
    NativeCatchupLog(std::filesystem::path path, std::string stream, std::ostream* output = nullptr)
        : path_(std::move(path)), stream_(std::move(stream)), output_(output) {
        if (std::filesystem::exists(path_)) {
            if (std::filesystem::is_symlink(path_) || !std::filesystem::is_regular_file(path_))
                throw std::invalid_argument("unsafe probe native logfile");
            offset_ = std::filesystem::file_size(path_);
        }
    }
    ~NativeCatchupLog() {
        try {
            finish();
        } catch (...) {
        }
    }
    const std::optional<NativeCatchupAnchor>& anchor() const {
        return anchor_;
    }
    // A narrow parser, not a CGate protocol authority. Unsupported/missing diagnostics stay unavailable.
    void line(std::string_view value) {
        if (stopped_)
            return;
        const auto name = token(value, "@cee{name: ");
        if (value.find(";cgate.listener.p2repl;;init: init listener: p2repl://" + stream_ + ";") !=
            std::string_view::npos) {
            if (names_.size() >= 4)
                throw std::invalid_argument("too many internal ORDLOG listeners");
            const auto internal = token(value, ";name=", ". ;");
            if (!internal.empty())
                names_.insert(internal);
        }
        if (value.find(";cgate.listener.p2repl;;Open rev.orders_log=") != std::string_view::npos &&
            names_.contains(name)) {
            if (anchor_)
                report(complete_);
            const auto revision = number(value, "rev.orders_log="), life = number(value, "lifenum=");
            if (!revision || !life)
                throw std::invalid_argument("invalid native ORDLOG request revision");
            anchor_ = NativeCatchupAnchor{revision, life, ++count_};
            listener_ = name;
            thread_ = token(value, ";TID ");
            redirect_ = snapshot_ = correlated_ = life_verified_ = complete_ = reported_ = false;
            end_ = 0;
            candidate_.clear();
            opened_.clear();
            address_.clear();
            if (output_)
                *output_ << "{\"kind\":\"native_ordlog_request\",\"connect\":" << count_
                         << ",\"revision\":" << std::quoted(std::to_string(revision))
                         << ",\"lifenum\":" << std::quoted(std::to_string(life)) << "}\n"
                         << std::flush;
            if (on_request)
                on_request(*anchor_);
        }
        if (!anchor_ || complete_ || thread_.empty() || token(value, ";TID ") != thread_)
            return;
        if (value.find("switchMode: switch to snapshot.") != std::string_view::npos && name == listener_)
            redirect_ = true;
        const auto handle = token(value, ";strm ");
        if (redirect_ && candidate_.empty() && value.find(";p2repl-cli;;Stream created;") != std::string_view::npos)
            candidate_ = handle;
        if (candidate_.empty() || (handle != candidate_ && name != listener_))
            return;
        if (value.find(";p2repl-cli;;Stream opening started;") != std::string_view::npos) {
            snapshot_ = value.find(";name 'FORTS_ORDLOG_REPL_SNAPSHOT';") != std::string_view::npos;
            address_ = token(value, ";srv_addr '", "'");
        }
        if (snapshot_ && value.find(";p2repl-cli;;Stream opened;") != std::string_view::npos)
            opened_ = handle;
        if (snapshot_ && opened_ == candidate_ && name == listener_ && !address_.empty() &&
            token(value, "ReplCallback: stream opened from address `", "`") == address_)
            correlated_ = true;
        if (!correlated_ || handle != candidate_)
            return;
        if (value.find(";p2repl-cli;;Current life nums;name 'FORTS_ORDLOG_REPL_SNAPSHOT';") != std::string_view::npos)
            life_verified_ =
                number(value, "cli lifenum ") == anchor_->life && number(value, "srv lifenum ") == anchor_->life;
        if (!life_verified_)
            return;
        if (value.find(";P2ReplStorage;;\ttbl 0;revs ") != std::string_view::npos) {
            const auto start = value.find(";revs ") + 6, dash = value.find('-', start);
            const auto revision = dash == std::string_view::npos ? 0 : number(value.substr(dash), "-");
            if (revision >= anchor_->revision)
                end_ = std::max(end_, revision);
        }
        if (value.find(";p2repl-cli;;Stream is on-line;") != std::string_view::npos) {
            complete_ = true;
            report(true);
        }
    }
    void poll() {
        if (!std::filesystem::exists(path_))
            return;
        if (std::filesystem::is_symlink(path_) || !std::filesystem::is_regular_file(path_) ||
            std::filesystem::file_size(path_) < offset_)
            throw std::invalid_argument("probe native log replaced or truncated");
        std::ifstream input(path_, std::ios::binary);
        input.seekg(offset_);
        std::string bytes(1024 * 1024, '\0');
        input.read(bytes.data(), bytes.size());
        bytes.resize(input.gcount());
        offset_ += bytes.size();
        partial_ += bytes;
        std::size_t consumed{};
        for (auto newline = partial_.find('\n', consumed); newline != std::string::npos;
             newline = partial_.find('\n', consumed)) {
            if (newline - consumed > 65536)
                throw std::invalid_argument("native diagnostic line exceeds bound");
            line(std::string_view(partial_).substr(consumed, newline - consumed));
            consumed = newline + 1;
        }
        partial_.erase(0, consumed);
        if (partial_.size() > 65536)
            throw std::invalid_argument("native diagnostic line exceeds bound");
    }
    void finish() {
        if (!stopped_) {
            poll();
            report(complete_);
            stopped_ = true;
        }
    }
};
} // namespace full_order_log_probe
