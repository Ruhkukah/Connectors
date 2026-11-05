#include "moex/plaza2/cgate/cgate_logging.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_backfill.hpp"

#include <charconv>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cg = moex::plaza2::cgate;
namespace fs = std::filesystem;

namespace {
constexpr std::string_view usage = "moex_fol_backfill_collector --test-live --primary CAPTURE --output NEW_CAPTURE"
                                   " --matching-id ID [--ordlog-lifenum NUM] [--reopen-sequence NUM]"
                                   " --seconds SECONDS --not-after UNIX_UTC_SECONDS --writer-state FILE --runtime-root "
                                   "DIR --scheme-dir DIR --config-dir DIR"
                                   " --library-path FILE --expected-release RELEASE --env-settings-var NAME"
                                   " --software-key-env NAME [--local-pass-env NAME]\n";
volatile std::sig_atomic_t stopped{};
void stop(int) {
    stopped = 1;
}

std::string env(std::string_view name) {
    const auto* value = std::getenv(std::string(name).c_str());
    if (!value || !*value)
        throw std::invalid_argument("required environment value missing");
    return value;
}

std::string secret(std::string_view name) {
    auto value = env(name);
    if (value.find_first_of(";\r\n") != std::string::npos)
        throw std::invalid_argument("secret contains a CGate settings delimiter");
    return value;
}

template <class T> T number(std::string_view value) {
    T out{};
    const auto [last, error] = std::from_chars(value.data(), value.data() + value.size(), out);
    if (error != std::errc{} || last != value.end() || out <= 0)
        throw std::invalid_argument("invalid positive number");
    return out;
}

fs::path absolute(std::string_view value) {
    fs::path path(value);
    if (!path.is_absolute())
        throw std::invalid_argument("absolute paths are required");
    return path;
}

void replace(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty())
        return;
    for (std::size_t pos{}; (pos = text.find(from, pos)) != std::string::npos; pos += to.size())
        text.replace(pos, from.size(), to);
}

void require_idle_router() {
#ifdef __linux__
    for (const auto* path : {"/proc/net/tcp", "/proc/net/tcp6"}) {
        std::ifstream sockets(path);
        std::string line;
        if (!std::getline(sockets, line) ||
            (line.find("rem_address") == line.npos && line.find("remote_address") == line.npos))
            throw std::runtime_error("cannot verify existing ORDLOG router sockets");
        while (std::getline(sockets, line)) {
            std::istringstream fields(line);
            std::string index, local, remote, state;
            if (!(fields >> index >> local >> remote >> state))
                throw std::runtime_error("invalid kernel TCP socket listing");
            const auto router = [](std::string_view address) {
                // Linux prints IPv4 and each IPv6 word in native byte order.
                return address == "0100007F:1006" || address == "0000000000000000FFFF00000100007F:1006";
            };
            if (state == "01" && (router(local) || router(remote)))
                throw std::runtime_error("ORDLOG router already has an established connection");
        }
        if (sockets.bad())
            throw std::runtime_error("cannot verify existing ORDLOG router sockets");
    }
#else
    throw std::runtime_error("native backfill requires Linux TCP socket preflight");
#endif
}

class WriterLock {
  public:
    explicit WriterLock(const fs::path& state) {
        const auto path = state.string() + ".lock";
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        struct stat info {};
        if (fd_ < 0 || ::fstat(fd_, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
            ::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            if (fd_ >= 0)
                ::close(fd_);
            throw std::runtime_error("existing owned writer state lock is unavailable");
        }
    }
    ~WriterLock() {
        ::close(fd_);
    }
    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;

  private:
    int fd_{};
};
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << usage;
        return 0;
    }
    std::string software_key, local_pass;
    try {
        if (argc < 2 || std::string_view(argv[1]) != "--test-live" || argc % 2 != 0)
            throw std::invalid_argument("explicit TEST-only live collection is required; see --help");
        std::map<std::string_view, std::string_view> args;
        for (int i = 2; i < argc; i += 2)
            if (!args.emplace(argv[i], argv[i + 1]).second)
                throw std::invalid_argument("duplicate setting");
        const auto take = [&](std::string_view name, bool optional = false) -> std::string_view {
            auto node = args.extract(name);
            if (!node.empty() && !node.mapped().empty())
                return node.mapped();
            if (!optional)
                throw std::invalid_argument("missing required setting " + std::string(name));
            return {};
        };
        const auto primary = absolute(take("--primary"));
        const auto output = absolute(take("--output"));
        const auto writer = absolute(take("--writer-state"));
        const auto matching = number<std::int32_t>(take("--matching-id"));
        const auto life = take("--ordlog-lifenum", true);
        const auto reopen = take("--reopen-sequence", true);
        const auto timeout = number<int>(take("--seconds"));
        if (timeout > 900)
            throw std::invalid_argument("recovery time limit exceeds 900 seconds");
        const auto not_after =
            std::chrono::sys_seconds(std::chrono::seconds(number<std::int64_t>(take("--not-after"))));
        if (not_after <= std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()))
            throw std::invalid_argument("recovery deadline has passed");
        cg::Plaza2Settings runtime{
            .environment = cg::Plaza2Environment::Test,
            .runtime_root = absolute(take("--runtime-root")),
            .library_path = absolute(take("--library-path")),
            .scheme_dir = absolute(take("--scheme-dir")),
            .config_dir = absolute(take("--config-dir")),
            .env_open_settings = env(take("--env-settings-var")),
            .expected_spectra_release = std::string(take("--expected-release")),
        };
        const auto key_name = std::string(take("--software-key-env"));
        const auto pass_name = take("--local-pass-env", true);
        if (!args.empty())
            throw std::invalid_argument("unknown setting; see --help");
        software_key = secret(key_name);
        const auto placeholder_prefix = "$"
                                        "{";
        const auto placeholder = placeholder_prefix + key_name + "}";
        if (runtime.env_open_settings.find("key=" + placeholder) == std::string::npos)
            throw std::invalid_argument("CGate env settings require the software-key placeholder");
        replace(runtime.env_open_settings, placeholder, software_key);
        if (runtime.env_open_settings.find(placeholder_prefix) != std::string::npos)
            throw std::invalid_argument("unresolved CGate environment placeholder");
        cg::validate_cgate_logging(runtime.env_open_settings, runtime.config_dir);
        std::string connection = "p2tcp://127.0.0.1:4102;app_name=moex_fol_backfill;timeout=2000";
        if (!pass_name.empty()) {
            local_pass = secret(pass_name);
            connection += ";local_pass=" + local_pass;
        }
        const auto relative = fs::weakly_canonical(output).lexically_relative(fs::canonical(primary));
        if (fs::exists(output) || relative.empty() || *relative.begin() != "..")
            throw std::invalid_argument("output must be a new directory outside the original capture");
        if (std::signal(SIGTERM, stop) == SIG_ERR || std::signal(SIGINT, stop) == SIG_ERR)
            throw std::runtime_error("cannot register shutdown signal handlers");
        const auto plan =
            cg::plan_full_order_log_backfill(primary, matching, life.empty() ? 0 : number<std::uint64_t>(life),
                                             reopen.empty() ? 0 : number<std::uint64_t>(reopen));
        const WriterLock lock(writer);
        require_idle_router();
        const auto proof =
            cg::collect_full_order_log_backfill(plan, {.directory = output}, runtime, connection,
                                                std::chrono::seconds(timeout), not_after, [] { return stopped; });
        std::cout
            << "{\"status\":\"committed_overlap_verified\",\"archive_repaired\":false,\"original_capture_identity\":\""
            << proof.plan.capture_identity << "\",\"recovery_capture_identity\":\"" << proof.recovery_capture_identity
            << "\",\"recovered_orders_log_rows\":" << proof.committed_rows_between << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::string message(error.what());
        replace(message, software_key, "<redacted>");
        replace(message, local_pass, "<redacted>");
        std::cerr << "backfill collector: " << message << '\n';
        return 2;
    }
}
