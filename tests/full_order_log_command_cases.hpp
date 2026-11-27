#pragma once

// Included with the offline process/command-socket helpers in the writer test.
namespace {
void command_result(const fs::path& log, std::string_view line, std::string_view error = {}) {
    const auto response = host::send_command(log.string() + ".sock", std::string(line));
    test::require(response.find(error.empty() ? "\"ok\":true" : "\"ok\":false") != std::string::npos &&
                      response.find(error) != std::string::npos,
                  "unexpected response to " + std::string(line) + ": " + response);
}

std::string_view order_status(std::string_view status, std::string_view key) {
    const auto begin = status.find("{\"client_order_id\":\"" + std::string(key) + "\"");
    if (begin == std::string_view::npos)
        return {};
    return status.substr(begin, status.find('}', begin) - begin);
}

void wait_order(Process& owner, const fs::path& log, std::string_view key, std::string_view state, int id = 0) {
    wait_status(owner, log, [&](const auto& status) {
        const auto order = order_status(status, key);
        return order.find("\"state\":\"" + std::string(state) + "\"") != std::string_view::npos &&
               (!id || order.find("\"order_id\":" + std::to_string(id) + ',') != std::string_view::npos);
    });
}

void matrix_ready(Process& owner, const fs::path& log) {
    const auto status = wait_status(owner, log, [](const auto& value) {
        return value.find("\"order_entry_ready\":true") != std::string::npos &&
               value.find("\"reconstructing\":false") != std::string::npos;
    });
    private_online(status);
}

void matrix_stop(Process& owner, const fs::path& log) {
    command_result(log, "quit");
    test::require(owner.wait() == 0 && !fs::exists(log.string() + ".sock"),
                  "offline matrix did not stop flat and remove the socket: " + owner.errors());
}

void fol_command_matrix(Process& owner, const fs::path& log, const fs::path& posts) {
    for (const auto& [line, error] : {std::pair{"place zero 1001 buy 0 102500 day", "quantity exceeds"},
                                      {"place oversized 1001 buy 3 102500 day", "quantity exceeds"},
                                      {"place off_step 1001 buy 1 102501 day", "not tick aligned"},
                                      {"place unselected 9999 buy 1 102500 day", "FullOrderLog Add requires"}})
        command_result(log, line, error);
    command_result(log, "kill on");
    command_result(log, "place killed 1001 buy 1 102500 day", "kill switch enabled");
    test::require(!fs::exists(posts), "local Add refusals reached the fake native publisher");
    command_result(log, "kill off");

    command_result(log, "place matrix_buy 1001 buy 1 102500 day");
    wait_order(owner, log, "matrix_buy", "Working", 20003);
    command_result(log, "move matrix_buy 1 102250");
    wait_order(owner, log, "matrix_buy", "Working", 20004);
    command_result(log, "move matrix_buy 1 102250");
    wait_order(owner, log, "matrix_buy", "Working", 20005);
    command_result(log, "move matrix_buy 1 102000");
    wait_order(owner, log, "matrix_buy", "Working", 20006);
    command_result(log, "cancel matrix_buy");
    wait_order(owner, log, "matrix_buy", "Cancelled", 20006);
    const auto after_cancel = contents(posts);
    command_result(log, "cancel matrix_buy");
    command_result(log, "move matrix_buy 1 102000", "only a working order can be moved");
    command_result(log, "cancel unknown", "unknown client order id");
    test::require(contents(posts) == after_cancel, "terminal/unknown commands unexpectedly posted");

    command_result(log, "place matrix_sell 1001 sell 1 102750 day");
    wait_order(owner, log, "matrix_sell", "Working", 20007);
    command_result(log, "cancel matrix_sell");
    wait_order(owner, log, "matrix_sell", "Cancelled", 20007);
    command_result(log, "place matrix_ioc_buy 1001 buy 1 102250 ioc");
    wait_order(owner, log, "matrix_ioc_buy", "Cancelled", 20008);
    command_result(log, "place matrix_ioc_sell 1001 sell 1 102750 ioc");
    wait_order(owner, log, "matrix_ioc_sell", "Cancelled", 20009);
    command_result(log, "place matrix_filled 1001 buy 1 103000 day");
    wait_order(owner, log, "matrix_filled", "Filled", 20010);
    const auto after_fill = contents(posts);
    command_result(log, "move matrix_filled 1 102500", "only a working order can be moved");
    command_result(log, "cancel matrix_filled");
    test::require(contents(posts) == after_fill, "terminal filled-order Move/Del reached native publisher");
    matrix_stop(owner, log);
    for (const auto detail : {"dir=1 type=1 price=102500 id=20003", "dir=2 type=1 price=102750 id=20007",
                              "dir=1 type=2 price=102250 id=20008", "dir=2 type=2 price=102750 id=20009",
                              "target=20003 price=102250 id=20004", "target=20004 price=102250 id=20005",
                              "target=20005 price=102000 id=20006", "target=20006 post_ns="})
        test::require(after_fill.find(detail) != std::string::npos, "wrong matrix native wire: " + after_fill);
    test::require(occurrences(after_fill, "AddOrder wire_valid=1") == 5 &&
                      occurrences(after_fill, "MoveOrder wire_valid=1") == 3 &&
                      occurrences(after_fill, "DelOrder wire_valid=1") == 2 &&
                      occurrences(after_fill, "post_ns=") == 10,
                  "unexpected matrix native calls: " + after_fill);
}

void verify_bulk_reply(const fs::path& log, const fs::path& posts, int count) {
    const auto reply_count =
        "\"msgid\":186,\"code\":0,\"message\":\"OK\",\"num_orders\":" + std::to_string(count) + ',';
    const auto native = contents(posts);
    test::require(native.find("isin=1001 ext_id=0 cancelled=" + std::to_string(count) + ' ') != std::string::npos &&
                      contents(log).find(reply_count) != std::string::npos,
                  "scoped native bulk count did not match reply and private cancellation confirmations: " + native);
}

void fol_mass_cancel_matrix(Process& owner, const fs::path& log, const fs::path& posts) {
    command_result(log, "cancel-all 0", "cancel-all requires an instrument id");
    command_result(log, "cancel-all 1001");
    test::require(!fs::exists(posts) && contents(log).find("\"event\":\"cancel_all_empty\"") != std::string::npos,
                  "zero working orders did not give the snapshot-confirmed local no-op");
    unsigned next{};
    for (const auto size : {1, 2}) {
        std::vector<std::string> keys;
        for (int i = 0; i < size; ++i) {
            auto& key = keys.emplace_back("bulk_" + std::to_string(next++));
            command_result(log, "place " + key + " 1001 buy 1 102500 day");
            wait_order(owner, log, key, "Working");
        }
        command_result(log, "cancel-all 1001");
        for (const auto& key : keys)
            wait_order(owner, log, key, "Cancelled");
        verify_bulk_reply(log, posts, size);
    }
    matrix_stop(owner, log);
    const auto native = contents(posts);
    test::require(occurrences(native, "DelUserOrders wire_valid=1") == 2 &&
                      occurrences(native, "AddOrder wire_valid=1") == 3 && occurrences(native, "post_ns=") == 5,
                  "scoped DelUserOrders unexpectedly fell back or posted outside the tested instrument: " + native);
}

void fol_mass_cancel_reconstructed_five(const std::string& executable, const test::RuntimeFixturePaths& fixture,
                                        const fs::path& posts) {
    // Five resting same-ISIN orders exceed current entry limits. Feed them
    // through the private snapshot to verify cancellation authority at startup.
    fs::remove(posts);
    const auto ini = fixture.config_dir / "t1.ini";
    test::write_text_file(ini, contents(ini) + "profile=matrix-five\n");
    const fs::path log{"mass-five.ndjson"};
    auto args = arguments(executable, fixture, log, "fol_mass_five");
    args.push_back("--sole-instance");
    Process owner(args, "mass-five");
    matrix_ready(owner, log);
    std::vector<std::string> keys;
    for (int id = 20003; id <= 20007; ++id) {
        const auto& key = keys.emplace_back("recovered:321:" + std::to_string(id));
        wait_order(owner, log, key, "Working", id);
    }
    command_result(log, "cancel-all 1001");
    for (const auto& key : keys)
        wait_order(owner, log, key, "Cancelled");
    verify_bulk_reply(log, posts, 5);
    matrix_stop(owner, log);
    const auto native = contents(posts);
    test::require(occurrences(native, "DelUserOrders wire_valid=1") == 1 && occurrences(native, "post_ns=") == 1,
                  "reconstructed five-order cancel emitted other native posts: " + native);
}

void fol_rate_matrix(Process& owner, const fs::path& log, const fs::path& posts) {
    for (const auto line : {"place sent_a 1001 buy 1 102500 day", "place sent_b 1001 sell 1 102750 day",
                            "place queued_a 1001 buy 1 102500 day", "place queued_b 1001 sell 1 102750 day"})
        command_result(log, line);
    command_result(log, "kill on");
    for (const auto key : {"queued_a", "queued_b"})
        wait_order(owner, log, key, "Rejected");
    for (const auto key : {"sent_a", "sent_b"}) {
        wait_order(owner, log, key, "Working");
        command_result(log, "cancel " + std::string(key));
    }
    for (const auto key : {"sent_a", "sent_b"})
        wait_order(owner, log, key, "Cancelled");
    matrix_stop(owner, log);
    const auto native = contents(posts);
    const auto journal = contents(log);
    test::require(occurrences(native, "AddOrder wire_valid=1") == 2 &&
                      occurrences(native, "DelOrder wire_valid=1") == 2 &&
                      occurrences(journal, "kill switch enabled") >= 2 &&
                      journal.find("\"event\":\"throttle\"") != std::string::npos,
                  "rate/kill test did not throttle the burst and reject the queued Adds: " + native);
    std::vector<std::int64_t> times;
    for (std::size_t offset{}; (offset = native.find("post_ns=", offset)) != std::string::npos; ++offset) {
        offset += std::string_view("post_ns=").size();
        std::int64_t ns{};
        const auto result = std::from_chars(native.data() + offset, native.data() + native.size(), ns);
        test::require(result.ec == std::errc{}, "missing native monotonic post timestamp");
        times.push_back(ns);
    }
    test::require(times.size() == 4, "wrong total posts for 2/s fixture");
    for (std::size_t i = 2; i < times.size(); ++i)
        test::require(times[i] - times[i - 2] >= 1'000'000'000LL, "native posts exceeded two in a rolling second");
}

bool run_fol_command_case(std::string_view name, const std::string& executable,
                          const test::RuntimeFixturePaths& fixture) {
    const bool mass = name == "mass-cancel-matrix";
    const bool rate = name == "rate-matrix";
    if (name != "command-matrix" && !mass && !rate)
        return false;
    test::write_text_file(fixture.config_dir / "t1.ini", contents(fixture.config_dir / "t1.ini") + "profile=matrix\n");
    const fs::path log{"command-cases.ndjson"};
    auto args = arguments(executable, fixture, log, "fol_command_cases");
    if (rate) {
        erase_option(args, "--max-commands-per-second");
        args.insert(args.end(), {"--max-commands-per-second", "2"});
    }
    if (mass)
        args.push_back("--sole-instance");
    Process owner(args, name);
    matrix_ready(owner, log);
    const auto posts = fixture.config_dir / "fol-native-posts.log";
    if (mass) {
        fol_mass_cancel_matrix(owner, log, posts);
        fol_mass_cancel_reconstructed_five(executable, fixture, posts);
    } else if (rate)
        fol_rate_matrix(owner, log, posts);
    else
        fol_command_matrix(owner, log, posts);
    return true;
}
} // namespace
