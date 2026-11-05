#pragma once

// The shared executable fixture helpers are defined before this header is included.
namespace {
void set_fake_control(const test::RuntimeFixturePaths& fixture, std::string_view event) {
    const auto path = fixture.config_dir / "fol-control";
    const auto pending = path.string() + ".next";
    test::write_text_file(pending, std::string(event) + '\n');
    fs::rename(pending, path);
}

bool writer_ready(std::string_view status) {
    return status.find("\"order_entry_ready\":true") != std::string_view::npos;
}

void require_accepted(const std::string& executable, const fs::path& log, const std::string& line,
                      const fs::path& label) {
    const auto result = command(executable, log, line, label);
    test::require(result.find("\"ok\":true") != std::string::npos, line + " refused: " + result);
}

std::string wait_working(Process& owner, const fs::path& log, std::size_t count) {
    return wait_status(owner, log, [&](const auto& status) {
        return occurrences(status, "\"state\":\"Working\"") == count &&
               status.find("\"reconstructing\":false") != std::string::npos;
    });
}

void verify_drained(Process& owner, const fs::path& log, std::size_t cancelled) {
    test::require(owner.wait() == 0, "offline drain did not exit cleanly: " + owner.errors());
    const auto journal = contents(log);
    test::require(occurrences(journal, "\"state\":\"Cancelled\"") >= cancelled &&
                      journal.find("\"outcome\":\"completed\"") != std::string::npos &&
                      journal.find("\"working_orders\":false") != std::string::npos &&
                      !fs::exists(log.string() + ".sock"),
                  "offline drain did not confirm terminal orders and remove its socket");
}

std::size_t trade_transactions(const fs::path& log, std::string_view event) {
    std::ifstream input(log);
    std::size_t count{};
    for (std::string line; std::getline(input, line);)
        if (line.find(event) != std::string::npos && line.find("\"stream\":\"FORTS_TRADE_REPL\"") != std::string::npos)
            ++count;
    return count;
}

void test_fol_outages(const std::string& executable, const test::RuntimeFixturePaths& fixture) {
    const fs::path log{"outages.ndjson"};
    const auto native = fixture.config_dir / "fol-native-posts.log";
    Process owner(arguments(executable, fixture, log, "fol_outages"), "outages-owner");
    wait_status(owner, log, writer_ready);
    require_accepted(executable, log, "place resting 1001 buy 1 102500 day", "outages-add");
    wait_working(owner, log, 1);
    const auto assert_resting = [&](std::string_view status) {
        test::require(status.find("\"order_id\":20003") != std::string_view::npos &&
                          status.find("\"state\":\"Working\"") != std::string_view::npos &&
                          occurrences(contents(native), "bytes=") == 1,
                      "outage lost/reposted the working order");
    };
    for (const std::string fault : {"router", "publisher"}) {
        const auto prior = occurrences(contents(log), "\"event\":\"link_lost\"");
        set_fake_control(fixture, fault + "-down");
        wait_status(owner, log,
                    [&](const auto&) { return occurrences(contents(log), "\"event\":\"link_lost\"") == prior + 1; });
        test::require(occurrences(contents(native), "bytes=") == 1, "native post during " + fault + " outage");
        set_fake_control(fixture, fault + "-up");
        const auto status = wait_status(owner, log, [&](const auto& value) {
            return writer_ready(value) && value.find("\"reconstructing\":false") != std::string::npos &&
                   occurrences(contents(log), "\"event\":\"link_restored\"") == prior + 1;
        });
        assert_resting(status);
    }

    unsigned index{};
    const auto blocked = [&](const std::string& event, const auto& wait_for) {
        set_fake_control(fixture, event);
        const auto status =
            wait_status(owner, log, [&](const auto& value) { return !writer_ready(value) && wait_for(value); });
        private_online(status);
        assert_resting(status);
        const auto line = "place blocked" + std::to_string(index) + " 1001 buy 1 102500 day";
        const auto response = command(executable, log, line, "outages-refused-" + std::to_string(index++));
        test::require(response.find("\"ok\":false") != std::string::npos &&
                          response.find("FullOrderLog Add") != std::string::npos &&
                          occurrences(contents(native), "bytes=") == 1,
                      "entry reached the publisher during " + event + ": " + response);
    };
    const auto book_invalid = [](std::string_view value) {
        return value.find("\"source\":\"full_order_log\",\"valid\":false") != std::string_view::npos &&
               value.find("\"crossed\":false") != std::string_view::npos;
    };
    blocked("fol-pause", book_invalid);
    set_fake_control(fixture, "fol-resume");
    assert_resting(wait_status(owner, log, writer_ready));
    blocked("fol-wrong-session", [&](std::string_view status) {
        return book_invalid(status) && status.find("\"source_session_id\":320") != std::string_view::npos &&
               status.find("\"session_terms\":{\"sess_id\":321") != std::string_view::npos;
    });
    set_fake_control(fixture, "fol-current-session");
    assert_resting(wait_status(owner, log, writer_ready));
    for (const std::string fault : {"fol-lifenum", "fol-clear"}) {
        blocked(fault, book_invalid);
        set_fake_control(fixture, "fol-current-session");
        assert_resting(wait_status(owner, log, writer_ready));
    }
    test::require(contents(log).find("\"close_reason_hex\":\"0xA006\"") != std::string::npos &&
                      contents(log).find("\"signed_value\":9223372036854775807") != std::string::npos,
                  "offline FOL outage evidence omitted native close reason or the full clear revision");

    require_accepted(executable, log, "cancel resting", "outages-cancel");
    const auto flat = wait_status(owner, log, [](const auto& status) {
        return status.find("\"state\":\"Cancelled\"") != std::string::npos &&
               status.find("\"positions\":[{\"isin_id\":1001,\"xpos\":0") != std::string::npos;
    });
    private_online(flat);
    owner.terminate();
    verify_drained(owner, log, 1);
    test::require(occurrences(contents(native), "AddOrder wire_valid=1") == 1 &&
                      occurrences(contents(native), "DelOrder wire_valid=1") == 1 &&
                      occurrences(contents(native), "bytes=") == 2,
                  "outage sequence must have exactly one Add and one Del");
}

void test_fol_restart(const std::string& executable, const test::RuntimeFixturePaths& fixture) {
    const auto native = fixture.config_dir / "fol-native-posts.log";
    for (std::size_t count = 0; count <= 2; ++count) {
        fs::remove(native);
        const auto tag = "term-" + std::to_string(count);
        const fs::path log{tag + ".ndjson"};
        Process owner(arguments(executable, fixture, log, "fol_" + tag), tag + "-owner");
        wait_status(owner, log, writer_ready);
        for (std::size_t i = 0; i < count; ++i) {
            const auto order = tag + "-" + std::to_string(i);
            require_accepted(executable, log, "place " + order + " 1001 buy 1 102500 day", order);
            wait_working(owner, log, i + 1);
        }
        owner.terminate();
        verify_drained(owner, log, count);
        test::require(occurrences(contents(native), "AddOrder wire_valid=1") == count &&
                          occurrences(contents(native), "DelOrder wire_valid=1") == count &&
                          occurrences(contents(native), "bytes=") == 2 * count,
                      "SIGTERM did not post exactly one Del per working order");
    }

    fs::remove(native);
    const fs::path log{"crash-open-transaction.ndjson"};
    auto request = arguments(executable, fixture, log, "fol_crash_transaction");
    {
        Process owner(request, "transaction-owner");
        wait_status(owner, log, writer_ready);
        require_accepted(executable, log, "place before_crash 1001 buy 1 102500 day", "transaction-add");
        const auto resting = wait_working(owner, log, 1);
        test::require(resting.find("\"client_order_id\":\"before_crash\",\"order_id\":20003,") != std::string::npos &&
                          resting.find("\"ext_id\":100") != std::string::npos,
                      "crash fixture must establish the resting order identity");
        constexpr std::string_view begin{"\"event\":\"transaction_begin\""};
        constexpr std::string_view commit{"\"event\":\"transaction_commit\""};
        const auto begins = trade_transactions(log, begin);
        const auto commits = trade_transactions(log, commit);
        set_fake_control(fixture, "trade-begin");
        wait_status(owner, log, [&](const auto&) { return trade_transactions(log, begin) == begins + 1; });
        test::require(trade_transactions(log, commit) == commits, "TRADE transaction committed before the crash");
        owner.crash(); // Only this owned offline child. The fake venue snapshot is staged below.
        test::require(fs::exists(log.string() + ".sock") && occurrences(contents(native), "bytes=") == 1,
                      "crash accidentally drained or removed the owned socket");
    }

    // The DSO lives inside each child. Seed its next private snapshot with the exact
    // fake exchange identity left Working above. Keep the journal/state unchanged.
    const auto ini = fixture.config_dir / "t1.ini";
    auto profile = contents(ini);
    const auto at = profile.find("profile=fresh");
    test::require(at != std::string::npos, "missing fake recovery profile seed");
    profile.replace(at, std::string_view("profile=fresh").size(), "profile=recovery");
    test::write_text_file(ini, profile);
    request.push_back("--cancel-only");
    Process recovered(request, "transaction-cancel-only");
    const auto status = wait_working(recovered, log, 1);
    private_online(status);
    test::require(status.find("\"execution_mode\":\"full_order_log_cancel_only\"") != std::string::npos &&
                      status.find("\"client_order_id\":\"recovered:321:20003\",\"order_id\":20003,"
                                  "\"instance_owned\":true,\"ext_id\":100") != std::string::npos,
                  "same-journal restart did not recover the owned exchange identity: " + status);
    const auto recovery_log = contents(log);
    for (const auto* service :
         {"FORTS_TRADE_REPL_MATCH3;", "FORTS_POS_REPL_MATCH3;", "FORTS_USERORDERBOOK_REPL_MATCH3;"})
        test::require(recovery_log.find(service) != std::string::npos,
                      std::string("writer/recovery did not open partitioned private service ") + service);
    const auto refused = command(executable, log, "move recovered:321:20003 1 102000", "transaction-move-refused");
    test::require(refused.find("cancel-only recovery refuses Move") != std::string::npos &&
                      occurrences(contents(native), "bytes=") == 1,
                  "cancel-only restart accepted Move");
    require_accepted(executable, log, "cancel recovered:321:20003", "transaction-cancel");
    wait_status(recovered, log, [](const auto& value) {
        return value.find("\"state\":\"Cancelled\"") != std::string::npos &&
               value.find("\"positions\":[{\"isin_id\":1001,\"xpos\":0") != std::string::npos;
    });
    recovered.terminate();
    verify_drained(recovered, log, 1);
    test::require(occurrences(contents(native), "AddOrder wire_valid=1") == 1 &&
                      occurrences(contents(native), "DelOrder wire_valid=1") == 1 &&
                      occurrences(contents(native), "bytes=") == 2,
                  "crash recovery resent entry or failed to cancel exactly once");
}
} // namespace
