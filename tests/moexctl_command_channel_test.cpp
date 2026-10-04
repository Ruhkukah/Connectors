#include "plaza2_runtime_test_support.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"
#include "scope_exit.hpp"
#include "command_socket.hpp"
#include "command_input.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef MOEX_SOURCE_GIT_SHA
#define MOEX_SOURCE_GIT_SHA "unknown"
#endif

namespace {
using namespace std::chrono_literals;
namespace test = moex::plaza2::test;

class Child {
  public:
    Child(const std::vector<std::string>& arguments, const std::filesystem::path& diagnostics, int input_fd = -1) {
        int input[2]{-1, -1}, output[2];
        test::require(::pipe(output) == 0, "stdout pipe failed");
        if (input_fd < 0)
            test::require(::pipe(input) == 0, "stdin pipe failed");
        const auto error = ::open(diagnostics.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        test::require(error >= 0, "diagnostics file failed");
        pid_ = ::fork();
        test::require(pid_ >= 0, "fork failed");
        if (pid_ == 0) {
            std::signal(SIGPIPE, SIG_DFL);
            std::signal(SIGHUP, SIG_DFL);
            ::dup2(input_fd < 0 ? input[0] : input_fd, STDIN_FILENO);
            ::dup2(output[1], STDOUT_FILENO);
            ::dup2(error, STDERR_FILENO);
            for (const auto fd : {input[0], input[1], output[0], output[1], error, input_fd})
                if (fd > STDERR_FILENO)
                    ::close(fd);
            std::vector<char*> argv;
            for (const auto& argument : arguments)
                argv.push_back(const_cast<char*>(argument.c_str()));
            argv.push_back(nullptr);
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }
        if (input[0] >= 0)
            ::close(input[0]);
        ::close(output[1]);
        ::close(error);
        input_ = input[1];
        output_ = output[0];
        ::fcntl(output_, F_SETFL, O_NONBLOCK);
        // A subsequent command-client exec must not keep the owner's input/output alive.
        for (const auto fd : {input_, output_})
            if (fd >= 0)
                ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    ~Child() {
        close_input();
        close_output();
        if (alive()) {
            ::kill(pid_, SIGTERM);
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (alive() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(10ms);
            if (alive()) {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, &status_, 0);
                reaped_ = true;
            }
        }
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    bool alive() {
        if (!reaped_ && ::waitpid(pid_, &status_, WNOHANG) == pid_)
            reaped_ = true;
        return !reaped_;
    }
    void signal(int value) {
        test::require(::kill(pid_, value) == 0, "signal failed");
    }
    void send(std::string_view line) {
        test::require(input_ >= 0 && ::write(input_, line.data(), line.size()) == static_cast<ssize_t>(line.size()),
                      "stdin command failed");
    }
    void close_input() {
        if (input_ >= 0)
            ::close(input_);
        input_ = -1;
    }
    void close_output() {
        if (output_ >= 0)
            ::close(output_);
        output_ = -1;
    }
    std::string line(std::chrono::milliseconds timeout = 3s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto end = pending_.find('\n'); end != std::string::npos) {
                auto result = pending_.substr(0, end);
                pending_.erase(0, end + 1);
                return result;
            }
            pollfd fd{.fd = output_, .events = POLLIN};
            if (::poll(&fd, 1, 20) > 0) {
                char data[4096];
                const auto size = ::read(output_, data, sizeof(data));
                if (size > 0)
                    pending_.append(data, static_cast<std::size_t>(size));
            }
            if (!alive() && pending_.empty())
                break;
        }
        throw std::runtime_error("CLI response missing (wait status " + std::to_string(status_) + ", reaped " +
                                 std::to_string(reaped_) + ")");
    }
    int wait() {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (alive() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        test::require(!alive(), "CLI did not stop");
        return WIFEXITED(status_) ? WEXITSTATUS(status_) : 128 + WTERMSIG(status_);
    }

  private:
    pid_t pid_{};
    int input_{-1}, output_{-1}, status_{};
    bool reaped_{};
    std::string pending_;
};

std::vector<std::string> run_arguments(const std::string& executable, const test::RuntimeFixturePaths& fixture,
                                       const std::filesystem::path& log, std::string_view instance) {
    return {executable,
            "plaza2",
            "run",
            "--runtime-root",
            fixture.root.string(),
            "--scheme-dir",
            fixture.scheme_dir.string(),
            "--config-dir",
            fixture.config_dir.string(),
            "--library-path",
            fixture.library_path.string(),
            "--env-settings-var",
            "MOEX_CLI_TEST_SETTINGS",
            "--credentials-env",
            "MOEX_CLI_TEST_CREDENTIALS",
            "--software-key-env",
            "MOEX_CLI_TEST_KEY",
            "--broker-code-env",
            "MOEX_CLI_TEST_BROKER",
            "--client-code-env",
            "MOEX_CLI_TEST_CLIENT",
            "--login-env",
            "MOEX_CLI_TEST_LOGIN",
            "--ext-id-range",
            "100:199",
            "--max-commands-per-second",
            "30",
            "--isin-id",
            "1001",
            "--instance-id",
            std::string(instance),
            "--max-quantity",
            "100",
            "--max-open-orders",
            "100",
            "--max-notional",
            "1001=100000000",
            "--max-position",
            "1001=100",
            "--log",
            log.string()};
}
void ready(Child& owner, const std::function<void()>& send) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    do {
        send();
        if (owner.line(10s).find("\"reconstructing\":false") != std::string::npos)
            return;
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("CLI did not reconstruct its Live/fake snapshots");
}
std::string remote_command(const std::string& executable, const std::filesystem::path& log, std::string command) {
    Child client({executable, "plaza2", "cmd", "--log", log.string(), std::move(command)},
                 log.string() + ".client.err");
    client.close_input();
    const auto response = client.line();
    test::require(client.wait() == (response.find("\"ok\":false") == std::string::npos ? 0 : 2),
                  "command client exit status did not reflect its refusal");
    return response;
}
void no_posts(const std::filesystem::path& log) {
    std::ifstream input(log);
    const std::string contents((std::istreambuf_iterator<char>(input)), {});
    test::require(contents.find("\"event\":\"command\"") == std::string::npos,
                  "read-only CLI test posted a trading command");
}
std::string raw_command(const std::filesystem::path& log, std::string_view frame, bool disconnect = false) {
    const auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    test::require(fd >= 0, "raw command socket failed");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = log.string() + ".sock";
    test::require(path.size() < sizeof(address.sun_path), "raw socket path too long");
    std::copy(path.begin(), path.end(), address.sun_path);
    const auto connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    const auto framed = std::string(32, 'a') + '\t' +
                        std::to_string(moex::connector_host::command_socket_detail::monotonic_ms() + 5000) + '\t' +
                        std::string(frame);
    const auto written = connected == 0 ? ::write(fd, framed.data(), framed.size()) : -1;
    std::string response;
    if (disconnect) {
        ::close(fd);
        test::require(written == static_cast<ssize_t>(framed.size()), "partial socket write failed");
        return {};
    }
    if (written == static_cast<ssize_t>(framed.size())) {
        pollfd descriptor{.fd = fd, .events = POLLIN};
        if (::poll(&descriptor, 1, 3000) > 0) {
            char data[4096];
            const auto count = ::read(fd, data, sizeof(data));
            if (count > 0)
                response.assign(data, static_cast<std::size_t>(count));
        }
    }
    ::close(fd);
    test::require(!response.empty(), "raw socket response missing");
    return response;
}
void expired_frame(const std::filesystem::path& path) {
    moex::connector_host::CommandSocket owner(path);
    const auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    test::require(fd >= 0, "expired-frame socket failed");
    moex::connector_host::ScopeExit close([&] { ::close(fd); });
    const auto endpoint = moex::connector_host::command_socket_detail::address(path);
    test::require(::connect(fd, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0,
                  "expired-frame connection failed");
    const auto frame = std::string(32, 'b') + '\t' +
                       std::to_string(moex::connector_host::command_socket_detail::monotonic_ms() - 1) + "\tkill on\n";
    test::require(::write(fd, frame.data(), frame.size()) == static_cast<ssize_t>(frame.size()),
                  "expired-frame write failed");
    unsigned dispatched{};
    owner.poll(
        [&](std::string) {
            ++dispatched;
            return "{\"ok\":true}";
        },
        [](std::string_view, std::string_view) { return "{\"ok\":false}"; });
    test::require(dispatched == 0, "expired request reached the owner dispatcher");
    char response[256];
    const auto size = ::read(fd, response, sizeof(response));
    test::require(size > 0 && std::string_view(response, size).find(std::string(32, 'b')) != std::string_view::npos,
                  "expired-frame refusal omitted its nonce");
}
std::string field(std::string_view record, std::string_view name) {
    const auto key = "\"" + std::string(name) + "\":\"";
    const auto offset = record.find(key);
    if (offset == std::string_view::npos)
        return {};
    const auto value = record.substr(offset + key.size());
    return std::string(value.substr(0, value.find('"')));
}
bool hex(std::string_view value, std::size_t size) {
    return value.size() == size && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}
void journal_input(const std::string& contents, std::string_view line, std::string_view channel) {
    const auto expected_line = "\"line\":" + moex::plaza2::cgate::text::json_quote_utf8(line);
    for (const auto event : {"operator_input", "local_refusal"}) {
        std::size_t count{};
        std::istringstream records(contents);
        std::string record;
        while (std::getline(records, record))
            count += record.find("\"event\":\"" + std::string(event) + "\"") != std::string::npos &&
                     record.find(expected_line) != std::string::npos && field(record, "channel") == channel;
        test::require(count == 1, "CLI omitted/doubled operator input or local refusal");
    }
}
std::string reply(Child& owner) {
    for (;;) {
        const auto response = owner.line();
        if (response.find("\"ok\":") != std::string::npos)
            return response;
    }
}
void make_working(Child& owner) {
    ready(owner, [&] { owner.send("status\n"); });
    owner.send("place rel7_working 1001 buy 1 102500 day\n");
    test::require(reply(owner).find("\"ok\":true") != std::string::npos, "fake Working Add was refused");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        owner.send("status\n");
        const auto status = owner.line();
        if (status.find("\"client_order_id\":\"rel7_working\"") != std::string::npos &&
            status.find("\"state\":\"Working\"") != std::string::npos)
            return;
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("fake Add did not become Working");
}
void cancelled_before_stop(const std::filesystem::path& log) {
    std::ifstream input(log);
    std::string record;
    bool posted{}, acknowledged{};
    while (std::getline(input, record)) {
        posted |= record.find("\"event\":\"command\"") != std::string::npos &&
                  record.find("DelUserOrders") != std::string::npos;
        acknowledged |=
            record.find("\"event\":\"reply\"") != std::string::npos && record.find("186") != std::string::npos;
    }
    test::require(posted && acknowledged, "shutdown dropped queued cancellation or its acknowledgement");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    std::signal(SIGPIPE, SIG_IGN);
    const auto executable = std::filesystem::absolute(argv[1]).string();
    const auto library = std::filesystem::absolute(argv[2]);
    const auto temporary_directory = std::filesystem::canonical(std::filesystem::temp_directory_path());
    const auto fixture_root = temporary_directory / ("moexctl_channel_" + std::to_string(::getpid()));
    test::require(fixture_root.parent_path() == temporary_directory,
                  "CLI fixture escaped the configured temporary directory");
    test::require(std::filesystem::create_directory(fixture_root), "test directory already exists");
    const auto fixture = test::materialize_runtime_fixture(
        fixture_root / "runtime", library, moex::plaza2::cgate::Plaza2Environment::Test,
        test::build_vendor_like_runtime_scheme("SPECTRA 9.9.0", "9.9", "T1"));
    const auto original_directory = std::filesystem::current_path();
    std::filesystem::current_path(fixture_root);
    moex::connector_host::ScopeExit restore_directory([&] { std::filesystem::current_path(original_directory); });
    // Keep UNIX socket names short even when the authorized TMPDIR is long.
    // Children inherit this directory; executable and runtime paths remain absolute.
    const std::filesystem::path root{"."};
    const auto settings = "ini=" + (fixture.config_dir / "t1.ini").string() + ";key=00000000";
    ::setenv("MOEX_CLI_TEST_SETTINGS", settings.c_str(), 1);
    ::setenv("MOEX_CLI_TEST_CREDENTIALS", "fake-test-only", 1);
    ::setenv("MOEX_CLI_TEST_KEY", "00000000", 1);
    ::setenv("MOEX_CLI_TEST_BROKER", "BRK1", 1);
    ::setenv("MOEX_CLI_TEST_CLIENT", "C01", 1);
    ::setenv("MOEX_CLI_TEST_LOGIN", "private-cli-owner", 1);
    unsigned failures{};
    auto scenario = [&](std::string_view name, const auto& body) {
        try {
            body();
            std::cout << name << ": PASS\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << name << ": " << error.what() << '\n';
        }
    };
    scenario("socket permissions and peer ownership", [&] {
        using namespace moex::connector_host;
        int peers[2];
        test::require(::socketpair(AF_UNIX, SOCK_STREAM, 0, peers) == 0, "peer credential fixture failed");
        ScopeExit close_peers([&] {
            ::close(peers[0]);
            ::close(peers[1]);
        });
        test::require(command_socket_detail::owned_peer(peers[0]) &&
                          !command_socket_detail::owned_peer(peers[0], ::geteuid() ^ 1) &&
                          !command_socket_detail::owned_peer(-1),
                      "command peer ownership did not fail closed");
        const auto mask = ::umask(0077);
        ScopeExit restore_mask([&] { ::umask(mask); });
        const auto path = root / "permission.sock";
        {
            CommandSocket socket(path);
            const auto current_mask = ::umask(0077);
            struct stat endpoint {};
            test::require(current_mask == 0077 && ::lstat(path.c_str(), &endpoint) == 0 && S_ISSOCK(endpoint.st_mode) &&
                              (endpoint.st_mode & 0777) == 0600,
                          "command socket was not private or changed the process umask");
            test::require(::unlink(path.c_str()) == 0, "socket replacement fixture failed");
            std::ofstream replacement(path);
            replacement << "preserve replacement";
        }
        test::require(std::filesystem::is_regular_file(path), "socket destructor unlinked a replacement path");
        bool failed{};
        try {
            CommandSocket socket(root / "missing-directory" / "bind.sock");
        } catch (const std::runtime_error&) {
            failed = true;
        }
        test::require(failed && ::umask(0077) == 0077, "failed bind leaked the temporary umask");
        const auto link = root / "symlink.sock";
        test::require(::symlink(path.c_str(), link.c_str()) == 0, "symlink fixture failed");
        failed = false;
        try {
            CommandSocket socket(link);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        test::require(failed && std::filesystem::is_symlink(link) && std::filesystem::is_regular_file(path),
                      "command bind followed or removed a symlink");
    });
    scenario("EOF audit is bounded and clears partial input", [&] {
        moex::connector_host::CommandInput input;
        unsigned refused{};
        std::string last;
        const auto line = [&](const auto& value) { last = value; };
        input.feed("kill on", line, [](auto) {});
        input.finish([&](auto value, auto error) {
            ++refused;
            test::require(value == "kill on" && error == "incomplete command at input EOF", "partial EOF audit lost");
        });
        input.feed("status\n", line, [](auto) {});
        input.finish([&](auto, auto) { ++refused; });
        test::require(last == "status" && refused == 1, "EOF retained partial bytes or doubled a refusal");
        input.feed(std::string(65537, 'x'), line, [&](auto) { ++refused; });
        input.finish([&](auto, auto) { ++refused; });
        test::require(refused == 2, "oversized input was audited twice on EOF");
    });
    scenario("expired socket frame never dispatches", [&] { expired_frame(root / "expired.sock"); });
    scenario("command client rejects a wrong nonce", [&] {
        const auto path = root / "wrong-nonce.sock";
        const auto listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
        test::require(listener >= 0, "wrong-nonce listener failed");
        moex::connector_host::ScopeExit close_listener([&] {
            ::close(listener);
            ::unlink(path.c_str());
        });
        const auto address = moex::connector_host::command_socket_detail::address(path);
        test::require(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
                          ::listen(listener, 1) == 0,
                      "wrong-nonce fixture bind failed");
        Child client({executable, "plaza2", "cmd", "--command-socket", path.string(), "status"},
                     root / "wrong-nonce.err");
        client.close_input();
        pollfd waiting{.fd = listener, .events = POLLIN};
        test::require(::poll(&waiting, 1, 3000) > 0, "wrong-nonce client did not connect");
        const auto fd = ::accept(listener, nullptr, nullptr);
        test::require(fd >= 0, "wrong-nonce accept failed");
        moex::connector_host::ScopeExit close_client([&] { ::close(fd); });
        pollfd request{.fd = fd, .events = POLLIN};
        test::require(::poll(&request, 1, 3000) > 0, "wrong-nonce client did not send a request");
        char data[256];
        test::require(::read(fd, data, sizeof(data)) > 0, "wrong-nonce request missing");
        const auto response = "{\"nonce\":\"" + std::string(32, '0') + "\",\"ok\":true}\n";
        test::require(::write(fd, response.data(), response.size()) == static_cast<ssize_t>(response.size()),
                      "wrong-nonce response failed");
        std::string accepted;
        try {
            accepted = client.line();
        } catch (const std::runtime_error&) {
        }
        test::require(client.wait() == 2 && accepted.empty(), "client accepted an unrelated command response");
    });
    scenario("mandatory per-instrument risk flags", [&] {
        for (const auto option : {"--max-quantity", "--max-open-orders", "--max-notional", "--max-position"}) {
            const auto label = std::string(option).substr(2);
            const auto log = root / ("missing-" + label + ".ndjson");
            auto arguments = run_arguments(executable, fixture, log, "cli_missing_" + label);
            const auto found = std::find(arguments.begin(), arguments.end(), option);
            test::require(found != arguments.end(), "risk test option missing");
            arguments.erase(found, found + 2);
            arguments.push_back("--allow-orders");
            Child owner(arguments, root / ("missing-" + label + ".err"));
            owner.close_input();
            test::require(owner.wait() == 2, "allow-orders accepted a missing mandatory risk flag");
            no_posts(log);
        }
        const auto log = root / "uncovered-risk.ndjson";
        auto arguments = run_arguments(executable, fixture, log, "cli_uncovered_risk");
        const auto found = std::find(arguments.begin(), arguments.end(), "--max-position");
        *(found + 1) = "1002=100";
        arguments.push_back("--allow-orders");
        Child owner(arguments, root / "uncovered-risk.err");
        owner.close_input();
        test::require(owner.wait() == 2, "allow-orders accepted an uncovered target position cap");
        no_posts(log);
    });
    for (const auto option : {"--max-quantity", "--max-open-orders", "--reply-timeout-ms"}) {
        const auto label = "prevalidate-" + std::string(option).substr(2);
        scenario(label, [&] {
            const auto log = root / (label + ".ndjson");
            auto arguments = run_arguments(executable, fixture, log, "cli_" + label);
            if (option == std::string_view("--reply-timeout-ms"))
                arguments.insert(arguments.end(), {option, "0"});
            else {
                const auto found = std::find(arguments.begin(), arguments.end(), option);
                *(found + 1) = "0";
            }
            arguments.push_back("--allow-orders");
            Child owner(arguments, root / (label + ".err"));
            owner.close_input();
            test::require(owner.wait() == 2, "CLI accepted invalid trading bounds");
            const auto state = root / ("cli_" + label + ".state");
            test::require(!std::filesystem::exists(log) && !std::filesystem::exists(state) &&
                              !std::filesystem::exists(state.string() + ".lock") &&
                              !std::filesystem::exists(log.string() + ".sock"),
                          "invalid arguments created owner resources before validation");
        });
    }
    for (const bool global : {false, true}) {
        const auto label = global ? "global-notional" : "missing-rate";
        scenario(std::string("explicit rate/per-instrument notional: ") + label, [&] {
            const auto log = root / (std::string(label) + ".ndjson");
            auto arguments = run_arguments(executable, fixture, log, "cli_" + std::string(label));
            if (global)
                arguments.insert(arguments.end(), {"--max-notional", "100000000"});
            else {
                const auto found = std::find(arguments.begin(), arguments.end(), "--max-commands-per-second");
                arguments.erase(found, found + 2);
            }
            arguments.push_back("--allow-orders");
            Child owner(arguments, root / (std::string(label) + ".err"));
            owner.send("quit --force\n");
            owner.close_input();
            test::require(owner.wait() == 2, global ? "CLI accepted cross-instrument numeric notional"
                                                    : "allow-orders accepted its default command rate");
            no_posts(log);
        });
    }
    scenario("mandatory login and instance ext_id scope", [&] {
        for (const auto option : {"--login-env", "--ext-id-range"}) {
            const auto label = std::string(option).substr(2);
            const auto log = root / ("missing-" + label + ".ndjson");
            auto arguments = run_arguments(executable, fixture, log, "cli_missing_" + label);
            const auto found = std::find(arguments.begin(), arguments.end(), option);
            arguments.erase(found, found + 2);
            arguments.push_back("--allow-orders");
            Child owner(arguments, root / ("missing-" + label + ".err"));
            owner.close_input();
            test::require(owner.wait() == 2, "allow-orders accepted a missing login or assigned range");
            no_posts(log);
        }
        unsigned index{};
        for (const auto range : {"0:199", "100:99", "100:2147483647", "100", "100:199:299"}) {
            const auto label = "invalid-range-" + std::to_string(index++);
            const auto log = root / (label + ".ndjson");
            auto arguments = run_arguments(executable, fixture, log, "cli_" + label);
            const auto found = std::find(arguments.begin(), arguments.end(), "--ext-id-range");
            *(found + 1) = range;
            arguments.push_back("--allow-orders");
            Child owner(arguments, root / (label + ".err"));
            owner.close_input();
            test::require(owner.wait() == 2, "allow-orders accepted an invalid assigned range");
            no_posts(log);
        }
        ::unsetenv("MOEX_CLI_TEST_MISSING_LOGIN");
        ::setenv("MOEX_CLI_TEST_EMPTY_LOGIN", "", 1);
        for (const auto variable : {"MOEX_CLI_TEST_MISSING_LOGIN", "MOEX_CLI_TEST_EMPTY_LOGIN"}) {
            const auto label = std::string(variable);
            const auto log = root / (label + ".ndjson");
            auto arguments = run_arguments(executable, fixture, log, "cli_" + label);
            const auto found = std::find(arguments.begin(), arguments.end(), "--login-env");
            *(found + 1) = variable;
            arguments.push_back("--allow-orders");
            Child owner(arguments, root / (label + ".err"));
            owner.close_input();
            test::require(owner.wait() == 2, "allow-orders accepted an unavailable login value");
            no_posts(log);
        }
    });
    scenario("explicit login and instance ext_id scope", [&] {
        const auto log = root / "identity-scope.ndjson";
        auto arguments = run_arguments(executable, fixture, log, "cli_identity_scope");
        const auto rate = std::find(arguments.begin(), arguments.end(), "--max-commands-per-second");
        *(rate + 1) = "7";
        arguments.push_back("--allow-orders");
        Child owner(arguments, root / "identity-scope.err");
        make_working(owner);
        const auto status = remote_command(executable, log, "status");
        test::require(status.find("\"ext_id\":100,") != std::string::npos,
                      "CLI Add did not start at its assigned ext_id range");
        remote_command(executable, log, "quit --force");
        test::require(owner.wait() == 8, "forced shutdown with outstanding scoped order exited successfully");
        std::ifstream journal(log), errors(root / "identity-scope.err");
        const std::string contents((std::istreambuf_iterator<char>(journal)), {});
        const std::string diagnostics((std::istreambuf_iterator<char>(errors)), {});
        test::require(contents.find("\"rate\":7,") != std::string::npos,
                      "CLI did not apply its explicitly configured command rate");
        test::require(status.find("private-cli-owner") == std::string::npos &&
                          contents.find("private-cli-owner") == std::string::npos &&
                          diagnostics.find("private-cli-owner") == std::string::npos,
                      "CLI exposed its configured login value");
    });
    scenario("channel bind failure precedes CGate login", [&] {
        const auto occupied = root / "occupied.sock";
        {
            std::ofstream file(occupied);
            file << "preserve";
        }
        unsigned index{};
        for (const auto& path : {occupied, root / std::string(120, 'x')}) {
            const auto label = "channel-bind-" + std::to_string(index++);
            const auto log = root / (label + ".ndjson");
            auto arguments = run_arguments(executable, fixture, log, "cli_" + label);
            arguments.insert(arguments.end(), {"--command-socket", path.string()});
            Child owner(arguments, root / (label + ".err"));
            owner.close_input();
            test::require(owner.wait() == 2, "invalid command endpoint did not fail startup");
            std::ifstream input(log);
            const std::string contents((std::istreambuf_iterator<char>(input)), {});
            test::require(contents.find("\"event\":\"runtime_identity\"") == std::string::npos,
                          "command endpoint failed only after CGate login");
            no_posts(log);
        }
        std::ifstream file(occupied);
        std::string content;
        file >> content;
        test::require(content == "preserve", "command endpoint cleanup removed a non-socket file");
    });
    scenario("verified stale command socket restarts", [&] {
        const auto path = root / "stale.sock", log = root / "stale.ndjson";
        const auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        test::require(fd >= 0, "stale socket fixture failed");
        const auto address = moex::connector_host::command_socket_detail::address(path);
        test::require(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
                      "stale socket fixture bind failed");
        ::close(fd); // The same owner crashed, leaving no listener on this socket inode.
        auto arguments = run_arguments(executable, fixture, log, "cli_stale");
        arguments.insert(arguments.end(), {"--command-socket", path.string()});
        Child owner(arguments, root / "stale.err");
        ready(owner, [&] { owner.send("status\n"); });
        owner.send("quit\n");
        test::require(owner.wait() == 0, "verified stale command socket blocked restart");
        test::require(!std::filesystem::exists(path), "restarted owner's socket was not removed at shutdown");
        no_posts(log);
    });
    scenario("active command socket cannot be replaced", [&] {
        const auto path = root / "active.sock";
        moex::connector_host::CommandSocket original(path);
        struct stat before {
        }, after{};
        test::require(::lstat(path.c_str(), &before) == 0, "active socket fixture missing");
        bool refused{};
        try {
            moex::connector_host::CommandSocket replacement(path);
        } catch (const std::runtime_error&) {
            refused = true;
        }
        test::require(refused && ::lstat(path.c_str(), &after) == 0 && before.st_dev == after.st_dev &&
                          before.st_ino == after.st_ino,
                      "command startup replaced an active owner's socket");
    });
    scenario("SIGHUP", [&] {
        const auto log = root / "hup.ndjson";
        Child owner(run_arguments(executable, fixture, log, "cli_hup"), root / "hup.err");
        ready(owner, [&] { owner.send("status\n"); });
        owner.signal(SIGHUP);
        std::this_thread::sleep_for(200ms);
        test::require(owner.alive(), "SIGHUP terminated the running host");
        test::require(remote_command(executable, log, "status").find("\"reconstructing\":false") != std::string::npos,
                      "SIGHUP lost command access");
        remote_command(executable, log, "quit");
        test::require(owner.wait() == 0, "SIGHUP host shutdown failed");
        no_posts(log);
    });
    scenario("SIGPIPE", [&] {
        const auto log = root / "pipe.ndjson";
        Child owner(run_arguments(executable, fixture, log, "cli_pipe"), root / "pipe.err");
        ready(owner, [&] { owner.send("status\n"); });
        owner.close_output();
        owner.send("status\n");
        std::this_thread::sleep_for(200ms);
        test::require(owner.alive(), "broken stdout terminated the running host");
        test::require(remote_command(executable, log, "status").find("\"reconstructing\":false") != std::string::npos,
                      "broken stdout lost socket responses");
        remote_command(executable, log, "quit");
        test::require(owner.wait() == 0, "SIGPIPE host shutdown failed");
        no_posts(log);
    });
    scenario("stdin EOF/reopen", [&] {
        const auto log = root / "eof.ndjson", fifo = root / "input.fifo";
        test::require(::mkfifo(fifo.c_str(), 0600) == 0, "FIFO creation failed");
        const auto reader = ::open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
        test::require(reader >= 0, "FIFO reader failed");
        Child owner(run_arguments(executable, fixture, log, "cli_eof"), root / "eof.err", reader);
        ::close(reader);
        auto writer = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
        test::require(writer >= 0, "FIFO writer failed");
        auto send = [&] { test::require(::write(writer, "status\n", 7) == 7, "FIFO status failed"); };
        ready(owner, send);
        ::close(writer);
        std::this_thread::sleep_for(1s);
        test::require(owner.alive(), "stdin EOF terminated the host");
        writer = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
        test::require(writer >= 0, "FIFO writer reopen failed");
        test::require(::write(writer, "kill on\n", 8) == 8, "FIFO resumed command failed");
        ::close(writer);
        // Earlier status replies cannot satisfy this new command after the writer reopened.
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        bool acknowledged{};
        while (std::chrono::steady_clock::now() < deadline && !acknowledged)
            acknowledged = owner.line().find("\"ok\":true") != std::string::npos;
        test::require(acknowledged, "stdin input did not recover after EOF");
        owner.signal(SIGTERM);
        test::require(owner.wait() == 0, "EOF host shutdown failed");
        no_posts(log);
    });
    scenario("partial stdin EOF is audited", [&] {
        const auto log = root / "partial-eof.ndjson";
        Child owner(run_arguments(executable, fixture, log, "cli_partial_eof"), root / "partial-eof.err");
        ready(owner, [&] { owner.send("status\n"); });
        const std::string partial = "kill on";
        owner.send(partial);
        owner.close_input();
        std::this_thread::sleep_for(200ms);
        const auto status = remote_command(executable, log, "status");
        test::require(status.find("\"reconstructing\":false") != std::string::npos,
                      "partial stdin EOF stopped the owner loop");
        remote_command(executable, log, "quit");
        test::require(owner.wait() == 0, "partial EOF owner did not stop");
        std::ifstream journal(log);
        const std::string contents((std::istreambuf_iterator<char>(journal)), {});
        journal_input(contents, partial, "stdin");
        test::require(contents.find("incomplete command at input EOF") != std::string::npos,
                      "partial stdin EOF was silently discarded");
        test::require(contents.substr(0, contents.find("\"line\":\"quit\"")).find("\"event\":\"kill_switch\"") ==
                          std::string::npos,
                      "an unterminated stdin command executed before quit");
        no_posts(log);
    });
    scenario("UNIX command channel", [&] {
        const auto log = root / "socket.ndjson";
        Child owner(run_arguments(executable, fixture, log, "cli_socket"), root / "socket.err");
        ready(owner, [&] { owner.send("status\n"); });
        owner.close_input();
        struct stat endpoint {};
        test::require(::stat((log.string() + ".sock").c_str(), &endpoint) == 0 && (endpoint.st_mode & 0777) == 0600 &&
                          S_ISSOCK(endpoint.st_mode),
                      "owner-only command socket missing");
        test::require(remote_command(executable, log, "kill on").find("\"ok\":true") != std::string::npos,
                      "kill switch could not be enabled through socket");
        test::require(remote_command(executable, log, "kill off").find("\"ok\":true") != std::string::npos,
                      "kill switch could not be disabled through socket");
        remote_command(executable, log, "quit");
        test::require(owner.wait() == 0, "socket quit did not stop host");
        test::require(!std::filesystem::exists(log.string() + ".sock"), "socket endpoint survived host stop");
        std::ifstream input(log);
        const std::string contents((std::istreambuf_iterator<char>(input)), {});
        test::require(contents.find("\"event\":\"kill_switch\"") != std::string::npos,
                      "socket command did not reach host");
        no_posts(log);
    });
    scenario("timed-out socket command never dispatches later", [&] {
        const auto log = root / "deadline.ndjson";
        Child owner(run_arguments(executable, fixture, log, "cli_deadline"), root / "deadline.err");
        ready(owner, [&] { owner.send("status\n"); });
        std::array<int, 3> idle{-1, -1, -1};
        moex::connector_host::ScopeExit close_idle([&] {
            for (const auto fd : idle)
                if (fd >= 0)
                    ::close(fd);
        });
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto endpoint = log.string() + ".sock";
        test::require(endpoint.size() < sizeof(address.sun_path), "deadline fixture socket path too long");
        std::copy(endpoint.begin(), endpoint.end(), address.sun_path);
        for (auto& fd : idle) {
            fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            test::require(fd >= 0 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
                              ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
                          "idle command client failed");
        }
        Child client({executable, "plaza2", "cmd", "--log", log.string(), "kill on"}, root / "deadline.client.err");
        client.close_input();
        std::string response;
        try {
            response = client.line(6s);
        } catch (const std::runtime_error&) {
        }
        const auto result = client.wait();
        // The old server accepts each queued idle client for another five
        // seconds, executing the failed client's command after its timeout.
        std::this_thread::sleep_for(11s);
        const auto status = remote_command(executable, log, "status");
        owner.send("quit\n");
        test::require(owner.wait() == 0, "deadline fixture owner did not stop");
        std::ifstream input(log);
        const std::string contents((std::istreambuf_iterator<char>(input)), {});
        test::require(result == 0 || contents.find("\"line\":\"kill on\"") == std::string::npos,
                      "socket command executed after its client reported failure");
        test::require(hex(field(status, "nonce"), 32), "command response omitted its nonce");
        no_posts(log);
    });
    scenario("CLI journal provenance and refusals", [&] {
        const auto log = root / "journal.ndjson";
        Child owner(run_arguments(executable, fixture, log, "cli_journal"), root / "journal.err");
        ready(owner, [&] { owner.send("status\n"); });
        const std::string stdin_parse = "place stdin_parse_error invalid_isin buy 1 1 day";
        const std::string stdin_admission = "cancel stdin_unknown_order";
        for (const auto& line : {stdin_parse, stdin_admission}) {
            owner.send(line + "\n");
            while (owner.line().find("\"ok\":false") == std::string::npos) {
            }
        }
        const std::string socket_parse = "move socket_parse_error";
        const std::string socket_admission = "cancel socket_unknown_order";
        for (const auto& line : {socket_parse, socket_admission})
            test::require(remote_command(executable, log, line).find("\"ok\":false") != std::string::npos,
                          "CLI refusal fixture was admitted");
        test::require(raw_command(log, "status\nkill on\n").find("\"ok\":false") != std::string::npos,
                      "malformed socket frame was admitted");
        const std::string socket_partial = "kill on";
        raw_command(log, socket_partial, true);
        std::this_thread::sleep_for(100ms);
        test::require(remote_command(executable, log, "status").find("\"reconstructing\":false") != std::string::npos,
                      "partial socket disconnect stopped the owner loop");
        test::require(raw_command(log, std::string(65537, 'x') + "\n").find("\"ok\":false") != std::string::npos,
                      "oversized socket frame was admitted");
        // An oversized stdin line is discarded; its refusal still needs an audit record.
        owner.send(std::string(65537, 'x') + "\n");
        while (owner.line().find("\"ok\":false") == std::string::npos) {
        }
        remote_command(executable, log, "quit");
        test::require(owner.wait() == 0, "journal host stop failed");
        std::ifstream input(log);
        const std::string contents((std::istreambuf_iterator<char>(input)), {});
        const auto start = contents.find("\"event\":\"startup\"");
        test::require(start != std::string::npos, "CLI startup record missing");
        const auto startup = contents.substr(start, contents.find('\n', start) - start);
        test::require(startup.find("\"max_notional_scaled\":9223372036854775807") != std::string::npos &&
                          startup.find("\"max_quote_notional_scaled\":10000000000000") != std::string::npos &&
                          startup.find("\"max_position\":100") != std::string::npos,
                      "mapped CLI risk configuration inherited the legacy overall default or lost target caps");
        std::ifstream binary_input(executable, std::ios::binary);
        const std::string binary((std::istreambuf_iterator<char>(binary_input)), {});
        const auto expected_hash = moex::plaza2::cgate::plaza2_sha256_hex(binary);
        test::require(
            field(startup, "source_git_sha") == MOEX_SOURCE_GIT_SHA &&
                (std::string_view(MOEX_SOURCE_GIT_SHA) == "unknown" || hex(field(startup, "source_git_sha"), 40)) &&
                hex(field(startup, "binary_sha256"), 64) && field(startup, "binary_sha256") == expected_hash,
            "CLI startup omitted actual build/executable identities");
        journal_input(contents, stdin_parse, "stdin");
        journal_input(contents, stdin_admission, "stdin");
        journal_input(contents, socket_parse, "command_socket");
        journal_input(contents, socket_admission, "command_socket");
        journal_input(contents, "status\nkill on\n", "command_socket");
        journal_input(contents, socket_partial, "command_socket");
        test::require(contents.find("incomplete command disconnected") != std::string::npos,
                      "partial socket disconnect was not audited");

        std::size_t socket_overflow{}, stdin_overflow{};
        std::istringstream records(contents);
        std::string record;
        while (std::getline(records, record)) {
            if (record.find("\"event\":\"local_refusal\"") != std::string::npos &&
                field(record, "error") == "command line exceeds 65536 bytes") {
                socket_overflow += field(record, "channel") == "command_socket";
                stdin_overflow += field(record, "channel") == "stdin";
            }
        }
        test::require(socket_overflow == 1 && stdin_overflow == 1, "CLI overflow refusals omitted/doubled");
        test::require(contents.substr(0, contents.find("\"line\":\"quit\"")).find("\"event\":\"kill_switch\"") ==
                          std::string::npos,
                      "a second or unterminated socket command executed before quit");
        no_posts(log);
    });
    scenario("quit refuses Working orders", [&] {
        const auto log = root / "quit-working.ndjson";
        auto arguments = run_arguments(executable, fixture, log, "cli_working");
        arguments.push_back("--allow-orders");
        Child owner(arguments, root / "quit-working.err");
        make_working(owner);
        owner.send("quit\n");
        test::require(reply(owner).find("\"ok\":false") != std::string::npos && owner.alive(),
                      "quit stopped a host with Working orders without --force");
        owner.send("quit --force\n");
        test::require(owner.wait() == 8, "forced quit reported success with outstanding work");
        std::ifstream journal(log);
        std::string record;
        unsigned incomplete{};
        while (std::getline(journal, record))
            incomplete += record.find("\"event\":\"cancel_drain\"") != std::string::npos &&
                          field(record, "reason") == "quit" && field(record, "outcome") == "incomplete" &&
                          record.find("\"working_orders\":true") != std::string::npos;
        test::require(incomplete == 2, "refused/forced quit drain outcomes were omitted or reported completed");
    });
    scenario("cancel-all plus quit drains", [&] {
        const auto log = root / "quit-drain.ndjson";
        auto arguments = run_arguments(executable, fixture, log, "cli_quit_drain");
        arguments.insert(arguments.end(), {"--allow-orders", "--sole-instance"});
        Child owner(arguments, root / "quit-drain.err");
        make_working(owner);
        owner.send("cancel-all 1001\nquit --force\nplace during_shutdown 1001 buy 1 102500 day\nkill off\n");
        test::require(owner.wait() == 8, "cancel/quit reported success without native terminal proof");
        cancelled_before_stop(log);
        std::ifstream input(log);
        const std::string contents((std::istreambuf_iterator<char>(input)), {});
        journal_input(contents, "place during_shutdown 1001 buy 1 102500 day", "stdin");
        journal_input(contents, "kill off", "stdin");
    });
    scenario("SIGTERM drains pending cancellation input", [&] {
        const auto log = root / "signal-drain.ndjson";
        auto arguments = run_arguments(executable, fixture, log, "cli_signal_drain");
        arguments.insert(arguments.end(), {"--allow-orders", "--sole-instance"});
        Child owner(arguments, root / "signal-drain.err");
        make_working(owner);
        owner.signal(SIGSTOP);
        owner.send("cancel-all 1001\n");
        owner.signal(SIGTERM);
        owner.signal(SIGCONT);
        test::require(owner.wait() == 8, "signal drain reported success without native terminal proof");
        cancelled_before_stop(log);
    });
    scenario("sole-instance signals cancel every configured instrument before draining", [&] {
        for (const auto signal : {SIGINT, SIGTERM}) {
            const std::string reason = signal == SIGINT ? "SIGINT" : "SIGTERM";
            const auto label = "signal-cancel-" + reason;
            const auto log = root / (label + ".ndjson");
            auto arguments = run_arguments(executable, fixture, log, "cli_" + label);
            arguments.insert(arguments.end(), {"--allow-orders", "--sole-instance", "--isin-id", "1002",
                                               "--max-notional", "1002=100000000", "--max-position", "1002=100"});
            Child owner(arguments, root / (label + ".err"));
            make_working(owner);
            const auto start = std::chrono::steady_clock::now();
            owner.signal(signal);
            const auto result = owner.wait();
            const auto elapsed = std::chrono::steady_clock::now() - start;
            test::require(elapsed < 12s, "signal cancellation exceeded its bounded drain");
            std::ifstream journal(log);
            std::string record, drain;
            unsigned first{}, second{}, acknowledgements{};
            bool sole_instance{};
            while (std::getline(journal, record)) {
                sole_instance |= record.find("\"event\":\"startup\"") != std::string::npos &&
                                 record.find("\"sole_instance\":true") != std::string::npos;
                if (record.find("\"event\":\"command\"") != std::string::npos &&
                    record.find("DelUserOrders") != std::string::npos) {
                    first += record.find("\"isin_id\":1001") != std::string::npos;
                    second += record.find("\"isin_id\":1002") != std::string::npos;
                }
                acknowledgements += record.find("\"event\":\"reply\"") != std::string::npos &&
                                    record.find("\"msgid\":186") != std::string::npos;
                if (record.find("\"event\":\"cancel_drain\"") != std::string::npos)
                    drain = record;
            }
            test::require(sole_instance && first == 1 && second == 1 && acknowledgements == 2,
                          "signal did not cancel each configured instrument once and process its acknowledgement");
            test::require(field(drain, "reason") == reason, "signal drain outcome was not journaled");
            const bool timed_out = field(drain, "outcome") == "timed_out";
            test::require(result == (field(drain, "outcome") == "completed" ? 0 : 8),
                          "signal exit status hid an incomplete or timed-out drain");
            test::require(timed_out ? elapsed >= 9500ms
                                    : field(drain, "outcome") == "completed" &&
                                          drain.find("\"pending_cancellations\":false") != std::string::npos &&
                                          drain.find("\"working_orders\":false") != std::string::npos,
                          "signal exited after an acknowledgement without terminal proof or a mature deadline");
        }
    });
    scenario("signal gets a fresh drain after an operator quit", [&] {
        const auto log = root / "fresh-signal-drain.ndjson";
        auto arguments = run_arguments(executable, fixture, log, "cli_fresh_signal");
        arguments.push_back("--allow-orders");
        Child owner(arguments, root / "fresh-signal-drain.err");
        make_working(owner);
        owner.send("cancel rel7_working\nquit --force\n");
        while (reply(owner).find("\"draining\":true") == std::string::npos) {
        }
        std::this_thread::sleep_for(2s);
        test::require(owner.alive(), "operator cancellation did not remain in its drain");
        const auto start = std::chrono::steady_clock::now();
        owner.signal(SIGTERM);
        const auto result = owner.wait();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        test::require(elapsed >= 9500ms && elapsed < 12s,
                      "signal cancellation inherited the earlier operator drain deadline");
        test::require(result == 8, "timed-out fresh signal drain exited successfully");
    });
    scenario("second signal interrupts the drain and reports outstanding work", [&] {
        const auto log = root / "second-signal.ndjson";
        auto arguments = run_arguments(executable, fixture, log, "cli_second_signal");
        arguments.push_back("--allow-orders");
        Child owner(arguments, root / "second-signal.err");
        make_working(owner);
        owner.signal(SIGTERM);
        const auto status = remote_command(executable, log, "status"); // First signal reached the owner loop.
        test::require(status.find("\"sole_instance\":false") != std::string::npos,
                      "CLI enabled instrument-wide cancellation without explicit authorization");
        const auto start = std::chrono::steady_clock::now();
        owner.signal(SIGINT);
        const auto result = owner.wait();
        test::require(std::chrono::steady_clock::now() - start < 1s, "second signal continued draining");
        test::require(result == 8, "second signal concealed outstanding orders");
        std::ifstream diagnostics(root / "second-signal.err"), journal(log);
        const std::string errors((std::istreambuf_iterator<char>(diagnostics)), {});
        const std::string records((std::istreambuf_iterator<char>(journal)), {});
        test::require(errors.find("rel7_working") != std::string::npos &&
                          records.find("\"outcome\":\"incomplete\"") != std::string::npos,
                      "second signal omitted the outstanding report or incomplete drain audit");
        unsigned known_id_cancels{}, broad_cancels{};
        std::istringstream command_records(records);
        std::string record;
        while (std::getline(command_records, record)) {
            if (record.find("\"event\":\"command\"") == std::string::npos)
                continue;
            known_id_cancels += record.find("DelOrder") != std::string::npos;
            broad_cancels += record.find("DelUserOrders") != std::string::npos;
        }
        test::require(known_id_cancels == 1 && broad_cancels == 0,
                      "default signal cancellation did not use the instance's known order ID");
    });
    scenario("storage failure keeps command owner alive", [&] {
        const auto log = root / "storage.ndjson", state = root / "storage.state";
        auto arguments = run_arguments(executable, fixture, log, "cli_storage");
        arguments.insert(arguments.end(), {"--allow-orders", "--state", state.string()});
        Child owner(arguments, root / "storage.err");
        make_working(owner);
        test::require(std::filesystem::remove(state), "remove CLI identity checkpoint");
        std::filesystem::create_directory(state);
        std::this_thread::sleep_for(350ms);
        test::require(owner.alive(), "storage failure exited the CLI with a working order");
        const auto status = remote_command(executable, log, "status");
        test::require(status.find("\"cancel_only\":true") != std::string::npos &&
                          status.find("\"order_entry_ready\":false") != std::string::npos,
                      "storage-failed CLI did not report cancel-only entry protection");
        test::require(remote_command(executable, log, "place blocked 1001 buy 1 102500 day").find("\"ok\":false") !=
                          std::string::npos,
                      "storage-failed CLI admitted another Add");
        test::require(remote_command(executable, log, "cancel rel7_working").find("\"ok\":true") != std::string::npos,
                      "storage-failed CLI refused durable-ID cancellation");
        test::require(std::filesystem::remove(state), "repair CLI identity checkpoint directory");
        test::require(remote_command(executable, log, "storage ok").find("\"ok\":true") != std::string::npos,
                      "CLI did not admit an explicit storage recovery check");
        const auto recovered = remote_command(executable, log, "status");
        test::require(recovered.find("\"cancel_only\":false") != std::string::npos,
                      "storage recovery failed to clear cancel-only mode");
        test::require(
            remote_command(executable, log, "place still_killed 1001 buy 1 102500 day").find("kill switch enabled") !=
                std::string::npos,
            "storage recovery silently disabled the kill switch");
        owner.signal(SIGTERM);
        test::require(owner.wait() == 8, "recovered CLI reported successful shutdown with working orders");
        std::ifstream journal(log);
        const std::string contents((std::istreambuf_iterator<char>(journal)), {});
        test::require(contents.find("\"event\":\"storage_recovered\"") != std::string::npos,
                      "explicit storage recovery was not journaled");
    });
    std::filesystem::current_path(original_directory);
    restore_directory.release();
    if (failures == 0)
        test::remove_tree(fixture_root);
    return failures == 0 ? 0 : 1;
}
