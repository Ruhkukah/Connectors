#include "plaza2_runtime_test_support.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"

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
            "--isin-id",
            "1001",
            "--instance-id",
            std::string(instance),
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
    test::require(client.wait() == 0, "command client failed");
    return response;
}
void no_posts(const std::filesystem::path& log) {
    std::ifstream input(log);
    const std::string contents((std::istreambuf_iterator<char>(input)), {});
    test::require(contents.find("\"event\":\"command\"") == std::string::npos,
                  "read-only CLI test posted a trading command");
}
std::string raw_command(const std::filesystem::path& log, std::string_view frame) {
    const auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    test::require(fd >= 0, "raw command socket failed");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = log.string() + ".sock";
    test::require(path.size() < sizeof(address.sun_path), "raw socket path too long");
    std::copy(path.begin(), path.end(), address.sun_path);
    const auto connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    const auto written = connected == 0 ? ::write(fd, frame.data(), frame.size()) : -1;
    std::string response;
    if (written == static_cast<ssize_t>(frame.size())) {
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
    const auto root = std::filesystem::path("/tmp") / ("moexctl_channel_" + std::to_string(::getpid()));
    test::require(std::filesystem::create_directory(root), "test directory already exists");
    const auto fixture =
        test::materialize_runtime_fixture(root / "runtime", argv[2], moex::plaza2::cgate::Plaza2Environment::Test,
                                          test::build_vendor_like_runtime_scheme("SPECTRA 9.9.0", "9.9", "T1"));
    const auto settings = "ini=" + (fixture.config_dir / "t1.ini").string() + ";key=00000000";
    ::setenv("MOEX_CLI_TEST_SETTINGS", settings.c_str(), 1);
    ::setenv("MOEX_CLI_TEST_CREDENTIALS", "fake-test-only", 1);
    ::setenv("MOEX_CLI_TEST_KEY", "00000000", 1);
    ::setenv("MOEX_CLI_TEST_BROKER", "BRK1", 1);
    ::setenv("MOEX_CLI_TEST_CLIENT", "C01", 1);
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
    scenario("SIGHUP", [&] {
        const auto log = root / "hup.ndjson";
        Child owner(run_arguments(argv[1], fixture, log, "cli_hup"), root / "hup.err");
        ready(owner, [&] { owner.send("status\n"); });
        owner.signal(SIGHUP);
        std::this_thread::sleep_for(200ms);
        test::require(owner.alive(), "SIGHUP terminated the running host");
        test::require(remote_command(argv[1], log, "status").find("\"reconstructing\":false") != std::string::npos,
                      "SIGHUP lost command access");
        remote_command(argv[1], log, "quit");
        test::require(owner.wait() == 0, "SIGHUP host shutdown failed");
        no_posts(log);
    });
    scenario("SIGPIPE", [&] {
        const auto log = root / "pipe.ndjson";
        Child owner(run_arguments(argv[1], fixture, log, "cli_pipe"), root / "pipe.err");
        ready(owner, [&] { owner.send("status\n"); });
        owner.close_output();
        owner.send("status\n");
        std::this_thread::sleep_for(200ms);
        test::require(owner.alive(), "broken stdout terminated the running host");
        test::require(remote_command(argv[1], log, "status").find("\"reconstructing\":false") != std::string::npos,
                      "broken stdout lost socket responses");
        remote_command(argv[1], log, "quit");
        test::require(owner.wait() == 0, "SIGPIPE host shutdown failed");
        no_posts(log);
    });
    scenario("stdin EOF/reopen", [&] {
        const auto log = root / "eof.ndjson", fifo = root / "input.fifo";
        test::require(::mkfifo(fifo.c_str(), 0600) == 0, "FIFO creation failed");
        const auto reader = ::open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
        test::require(reader >= 0, "FIFO reader failed");
        Child owner(run_arguments(argv[1], fixture, log, "cli_eof"), root / "eof.err", reader);
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
    scenario("UNIX command channel", [&] {
        const auto log = root / "socket.ndjson";
        Child owner(run_arguments(argv[1], fixture, log, "cli_socket"), root / "socket.err");
        ready(owner, [&] { owner.send("status\n"); });
        owner.close_input();
        struct stat endpoint {};
        test::require(::stat((log.string() + ".sock").c_str(), &endpoint) == 0 && (endpoint.st_mode & 0777) == 0600 &&
                          S_ISSOCK(endpoint.st_mode),
                      "owner-only command socket missing");
        test::require(remote_command(argv[1], log, "kill on").find("\"ok\":true") != std::string::npos,
                      "kill switch could not be enabled through socket");
        test::require(remote_command(argv[1], log, "kill off").find("\"ok\":true") != std::string::npos,
                      "kill switch could not be disabled through socket");
        remote_command(argv[1], log, "quit");
        test::require(owner.wait() == 0, "socket quit did not stop host");
        test::require(!std::filesystem::exists(log.string() + ".sock"), "socket endpoint survived host stop");
        std::ifstream input(log);
        const std::string contents((std::istreambuf_iterator<char>(input)), {});
        test::require(contents.find("\"event\":\"kill_switch\"") != std::string::npos,
                      "socket command did not reach host");
        no_posts(log);
    });
    scenario("CLI journal provenance and refusals", [&] {
        const auto log = root / "journal.ndjson";
        Child owner(run_arguments(argv[1], fixture, log, "cli_journal"), root / "journal.err");
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
            test::require(remote_command(argv[1], log, line).find("\"ok\":false") != std::string::npos,
                          "CLI refusal fixture was admitted");
        test::require(raw_command(log, "status\nkill on\n").find("\"ok\":false") != std::string::npos,
                      "malformed socket frame was admitted");
        test::require(raw_command(log, std::string(65537, 'x') + "\n").find("\"ok\":false") != std::string::npos,
                      "oversized socket frame was admitted");
        // An oversized stdin line is discarded; its refusal still needs an audit record.
        owner.send(std::string(65537, 'x') + "\n");
        while (owner.line().find("\"ok\":false") == std::string::npos) {
        }
        remote_command(argv[1], log, "quit");
        test::require(owner.wait() == 0, "journal host stop failed");
        std::ifstream input(log);
        const std::string contents((std::istreambuf_iterator<char>(input)), {});
        const auto start = contents.find("\"event\":\"startup\"");
        test::require(start != std::string::npos, "CLI startup record missing");
        const auto startup = contents.substr(start, contents.find('\n', start) - start);
        std::ifstream executable(argv[1], std::ios::binary);
        const std::string binary((std::istreambuf_iterator<char>(executable)), {});
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
        no_posts(log);
    });
    scenario("quit refuses Working orders", [&] {
        const auto log = root / "quit-working.ndjson";
        auto arguments = run_arguments(argv[1], fixture, log, "cli_working");
        arguments.push_back("--allow-orders");
        Child owner(arguments, root / "quit-working.err");
        make_working(owner);
        owner.send("quit\n");
        test::require(reply(owner).find("\"ok\":false") != std::string::npos && owner.alive(),
                      "quit stopped a host with Working orders without --force");
        owner.send("quit --force\n");
        test::require(owner.wait() == 0, "forced quit did not stop host");
    });
    scenario("cancel-all plus quit drains", [&] {
        const auto log = root / "quit-drain.ndjson";
        auto arguments = run_arguments(argv[1], fixture, log, "cli_quit_drain");
        arguments.push_back("--allow-orders");
        Child owner(arguments, root / "quit-drain.err");
        make_working(owner);
        owner.send("cancel-all 1001\nquit --force\nplace during_shutdown 1001 buy 1 102500 day\nkill off\n");
        test::require(owner.wait() == 0, "cancel/quit drain did not stop host");
        cancelled_before_stop(log);
        std::ifstream input(log);
        const std::string contents((std::istreambuf_iterator<char>(input)), {});
        journal_input(contents, "place during_shutdown 1001 buy 1 102500 day", "stdin");
        journal_input(contents, "kill off", "stdin");
    });
    scenario("SIGTERM drains pending cancellation input", [&] {
        const auto log = root / "signal-drain.ndjson";
        auto arguments = run_arguments(argv[1], fixture, log, "cli_signal_drain");
        arguments.push_back("--allow-orders");
        Child owner(arguments, root / "signal-drain.err");
        make_working(owner);
        owner.signal(SIGSTOP);
        owner.send("cancel-all 1001\n");
        owner.signal(SIGTERM);
        owner.signal(SIGCONT);
        test::require(owner.wait() == 0, "signal drain did not stop host");
        cancelled_before_stop(log);
    });
    scenario("storage failure keeps command owner alive", [&] {
        const auto log = root / "storage.ndjson", state = root / "storage.state";
        auto arguments = run_arguments(argv[1], fixture, log, "cli_storage");
        arguments.insert(arguments.end(), {"--allow-orders", "--state", state.string()});
        Child owner(arguments, root / "storage.err");
        make_working(owner);
        test::require(std::filesystem::remove(state), "remove CLI identity checkpoint");
        std::filesystem::create_directory(state);
        std::this_thread::sleep_for(350ms);
        test::require(owner.alive(), "storage failure exited the CLI with a working order");
        const auto status = remote_command(argv[1], log, "status");
        test::require(status.find("\"cancel_only\":true") != std::string::npos &&
                          status.find("\"order_entry_ready\":false") != std::string::npos,
                      "storage-failed CLI did not report cancel-only entry protection");
        test::require(remote_command(argv[1], log, "place blocked 1001 buy 1 102500 day").find("\"ok\":false") !=
                          std::string::npos,
                      "storage-failed CLI admitted another Add");
        test::require(remote_command(argv[1], log, "cancel rel7_working").find("\"ok\":true") != std::string::npos,
                      "storage-failed CLI refused durable-ID cancellation");
        owner.signal(SIGTERM);
        test::require(owner.wait() == 7, "storage-failed CLI shutdown concealed its storage error");
    });
    if (failures == 0)
        test::remove_tree(root);
    return failures == 0 ? 0 : 1;
}
