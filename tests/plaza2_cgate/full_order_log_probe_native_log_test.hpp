#pragma once
#include "full_order_log_probe_native_log.hpp"
#include <chrono>
#include <vector>

namespace full_order_log_probe {
inline void run_native_log_tests() {
    const auto expect = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("fol-native-log-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(folder);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{folder};
    const auto entry = [](std::string text) { return "2001-02-03 04:05:06.007008;" + text + ";TID 123"; };
    const auto init =
        entry("cgate.listener.p2repl;;init: init listener: "
              "p2repl://FORTS_ORDLOG_REPL_MATCH1;scheme=fixture;name=internal. @cee{name: , obj: 0xAA, thread: 7}");
    const auto request = [&](std::uint64_t revision, std::uint64_t life = 7) {
        return entry("cgate.listener.p2repl;;Open rev.orders_log=" + std::to_string(revision) +
                     ";lifenum=" + std::to_string(life) + " @cee{name: internal, obj: 0xAA, thread: 7}");
    };
    const auto source = [&](std::uint64_t revision, std::uint64_t life = 7) {
        return std::vector<std::string>{
            entry("cgate.listener.p2repl;;switchMode: switch to snapshot. @cee{name: internal, obj: 0xAA, thread: 7}"),
            entry("p2repl-cli;;Stream created;strm 0xBB"),
            entry("p2repl-cli;;Stream opening started;name 'FORTS_ORDLOG_REPL_SNAPSHOT';srv_addr 'fixture';strm 0xBB"),
            entry("p2repl-cli;;Stream opened;chan 0x99;strm 0xBB"),
            entry("cgate.listener.p2repl;;ReplCallback: stream opened from address `fixture`. @cee{name: internal, "
                  "obj: 0xAA, thread: 7}"),
            entry("p2repl-cli;;Current life nums;name 'FORTS_ORDLOG_REPL_SNAPSHOT';cli lifenum " +
                  std::to_string(life) + ";srv lifenum " + std::to_string(life) + ";strm 0xBB"),
            entry("P2ReplStorage;;\ttbl 0;revs " + std::to_string(revision) + "-" + std::to_string(revision + 100) +
                  "(10);strm 0xBB"),
            entry("P2ReplStorage;;\ttbl 1;revs 0-999999(123);strm 0xBB"),
            entry("P2ReplStorage;;\ttbl 0;revs " + std::to_string(revision + 101) + "-" +
                  std::to_string(revision + 232) + "(20);strm 0xBB"),
            entry("p2repl-cli;;Stream is on-line;strm 0xBB")};
    };
    {
        std::ostringstream output;
        NativeCatchupLog log(folder / "absent", "FORTS_ORDLOG_REPL_MATCH1", &output);
        std::uint64_t calls{};
        log.on_request = [&](const auto& anchor) {
            ++calls;
            expect(anchor.revision == 9007199254741001ULL, "native request precision");
        };
        log.line(init);
        log.line(request(9007199254741001ULL));
        for (const auto& line : source(9007199254741001ULL))
            log.line(line);
        log.line(entry("P2ReplStorage;;\ttbl 0;revs 999999999-999999999999999999(20);strm 0xBB"));
        log.finish();
        expect(calls == 1 && log.anchor()->life == 7, "native request callback/source");
        expect(output.str().find("\"catchup_end_revision\":\"9007199254741233\"") != std::string::npos,
               "native end must include last snapshot packet and freeze at ONLINE");
        expect(output.str().find("999999999999999999") == std::string::npos,
               "later live revision contaminated native catch-up end");
        expect(output.str().find("0xAA") == std::string::npos && output.str().find("fixture") == std::string::npos,
               "native diagnostics must not copy full vendor lines/settings");
    }
    {
        std::ostringstream output;
        NativeCatchupLog log(folder / "unknown", "FORTS_ORDLOG_REPL_MATCH1", &output);
        log.line(entry("cgate.listener.p2repl;;init: init listener: p2repl://FORTS_ORDLOG_REPL_MATCH2;name=foreign. "
                       "@cee{name: , obj: 0xCC, thread: 7}"));
        log.line(entry(
            "cgate.listener.p2repl;;Open rev.orders_log=100;lifenum=7 @cee{name: foreign, obj: 0xCC, thread: 7}"));
        log.line(entry("P2ReplStorage;;\ttbl 0;cur_rev 100;strm 0xCC"));
        log.finish();
        expect(!log.anchor() && output.str().find("\"request_revision\":null") != std::string::npos,
               "unrelated matching listener or delivered row cannot invent request revision");
    }
    {
        std::ostringstream output;
        NativeCatchupLog log(folder / "ambiguous", "FORTS_ORDLOG_REPL_MATCH1", &output);
        log.line(init);
        log.line(request(100));
        auto lines = source(100);
        lines[2] = entry("p2repl-cli;;Stream opening started;name 'UNRELATED_SNAPSHOT';strm 0xBB");
        for (const auto& line : lines)
            log.line(line);
        log.finish();
        expect(output.str().find("\"catchup_end_revision\":null") != std::string::npos,
               "unrelated redirected stream cannot qualify catch-up end");
    }
    {
        std::ostringstream output;
        NativeCatchupLog log(folder / "reconnect", "FORTS_ORDLOG_REPL_MATCH1", &output);
        log.line(init);
        log.line(request(100));
        for (const auto& line : source(100))
            log.line(line);
        log.line(request(1000, 8));
        for (const auto& line : source(1000, 8))
            log.line(line);
        log.finish();
        expect(log.anchor()->connect == 2 && log.anchor()->life == 8 &&
                   output.str().find("\"catchup_end_revision\":\"1232\"") != std::string::npos,
               "each new native request has its own lifecycle/end");
    }
    {
        const auto path = folder / "append.log";
        {
            std::ofstream file(path);
            file << init << '\n' << request(1) << '\n';
        }
        std::ostringstream output;
        NativeCatchupLog log(path, "FORTS_ORDLOG_REPL_MATCH1", &output);
        log.poll();
        expect(!log.anchor(), "preexisting native log must be ignored");
        {
            std::ofstream file(path, std::ios::app);
            file << init << '\n' << request(100);
        }
        log.poll();
        expect(!log.anchor(), "partial native line must remain pending");
        {
            std::ofstream file(path, std::ios::app);
            file << '\n';
        }
        log.poll();
        expect(log.anchor() && log.anchor()->revision == 100, "complete appended native request lost");
        { std::ofstream file(path, std::ios::trunc); }
        bool refused{};
        try {
            log.poll();
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        expect(refused, "native log truncation must be refused");
    }
    {
        const auto ini = folder / "client.ini", log = folder / "native.log";
        {
            std::ofstream out(ini);
            out << "[CGate]\nlog=p2:p2syslog\n[P2SysLog]\n logfile = " << log.string() << " ; comment\n";
        }
        expect(native_log_path("ini=client.ini;log=p2:p2syslog", folder) == log,
               "validated native log config resolution");
    }
}
} // namespace full_order_log_probe
