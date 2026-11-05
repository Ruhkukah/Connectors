#pragma once

namespace {
void test_fol_dtc_worker(const std::string& executable, const test::RuntimeFixturePaths& fixture) {
    using namespace dtc_test_wire;
    namespace dtc = moex::connector_host::dtc;
    const auto reserve_port = ::socket(AF_INET, SOCK_STREAM, 0);
    test::require(reserve_port >= 0, "reserve test loopback port");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    test::require(::bind(reserve_port, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
                  "bind test loopback port");
    socklen_t length = sizeof(address);
    test::require(::getsockname(reserve_port, reinterpret_cast<sockaddr*>(&address), &length) == 0,
                  "read test loopback port");
    ::close(reserve_port);
    const fs::path log{"dtc-worker.ndjson"};
    const fs::path secret{"dtc-worker.secret"};
    const auto ini = fixture.config_dir / "t1.ini";
    test::write_text_file(ini, contents(ini) + "profile=matrix-dtc\n");
    test::write_text_file(secret, "offline-dtc-fixture\n");
    ::chmod(secret.c_str(), 0600);
    auto request = arguments(executable, fixture, log, "fol_dtc_worker");
    request.insert(request.end(), {"--dtc-trading-port", std::to_string(ntohs(address.sin_port)),
                                   "--dtc-trading-secret-file", secret.string()});
    auto owner = std::make_unique<Process>(request, "dtc-worker-owner");
    wait_status(*owner, log, writer_ready);
    int fd = -1;
    const auto disconnect = [&] {
        if (fd >= 0)
            ::close(std::exchange(fd, -1));
    };
    host::ScopeExit close_fd(disconnect);
    const auto send = [&](const Bytes& packet) {
        for (std::size_t offset = 0; offset < packet.size();) {
            const auto count = ::send(fd, packet.data() + offset, packet.size() - offset, 0);
            test::require(count > 0, "send DTC worker fixture request");
            offset += static_cast<std::size_t>(count);
        }
    };
    dtc::DtcFrameDecoder decoder;
    std::vector<dtc::DtcFrame> frames;
    const auto receive = [&](unsigned type, auto accept, unsigned alternate = 0) -> WireMessage {
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        std::string observed;
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto it = frames.begin(); it != frames.end();) {
                if (it->message_type == type || it->message_type == 302 || it->message_type == 307 ||
                    it->message_type == 509 || it->message_type == 5) {
                    const auto received_type = it->message_type;
                    WireMessage message(it->payload);
                    observed += " type=" + std::to_string(received_type);
                    for (auto field : {1U, 2U, 27U})
                        if (const auto f = message.fields.find(field); f != message.fields.end() && f->second.wire == 2)
                            observed += " " + f->second.text;
                    it = frames.erase(it);
                    if (received_type == alternate || (received_type == type && accept(message)))
                        return message;
                } else {
                    ++it;
                }
            }
            pollfd descriptor{fd, POLLIN, 0};
            if (::poll(&descriptor, 1, 40) <= 0)
                continue;
            std::array<std::uint8_t, 8192> data{};
            const auto count = ::recv(fd, data.data(), data.size(), 0);
            test::require(count > 0, "DTC worker closed before expected response: " + owner->errors());
            std::string error;
            test::require(decoder.append(std::span(data).first(static_cast<std::size_t>(count)), frames, error), error);
            test::require(frames.size() < 256, "unexpectedly many DTC fixture frames");
        }
        throw std::runtime_error("missing DTC worker response " + std::to_string(type) + observed + ": " +
                                 owner->errors());
    };
    const auto any = [](const auto&) { return true; };
    constexpr std::string_view symbol = "RTS-6.26";
    const auto connect = [&] {
        decoder.reset();
        frames.clear();
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        test::require(fd >= 0, "create DTC worker fixture client");
        test::require(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
                      "connect actual FOL DTC worker");
        send({16, 0, 6, 0, 8, 0, 0, 0, 4, 0, 0, 0, 'D', 'T', 'C', 0});
        // The negotiation reply has a binary, not protobuf, payload.
        std::array<std::uint8_t, 16> negotiation{};
        for (std::size_t offset = 0; offset < negotiation.size();) {
            pollfd descriptor{fd, POLLIN, 0};
            test::require(::poll(&descriptor, 1, 3000) > 0, "DTC worker did not negotiate");
            const auto count = ::recv(fd, negotiation.data() + offset, negotiation.size() - offset, 0);
            test::require(count > 0, "DTC worker negotiation closed");
            offset += static_cast<std::size_t>(count);
        }
        test::require(negotiation[2] == 7, "unexpected DTC worker negotiation reply");
        Bytes logon;
        put_integer(logon, 1, 8);
        put_text(logon, 2, "kairos");
        put_text(logon, 3, "offline-dtc-fixture");
        put_integer(logon, 7, 10);
        send(packet(1, logon));
        test::require(receive(2, any).number(2) == 1, "DTC worker authentication failed");
        Bytes definition;
        put_integer(definition, 1, 1);
        put_text(definition, 2, symbol);
        put_text(definition, 3, dtc::kDtcMoexSpectraExchange);
        send(packet(506, definition));
        receive(507, any);
        Bytes query;
        put_integer(query, 1, 2);
        const auto reconcile = [&](unsigned request_type, unsigned reply_type, unsigned reject_type, unsigned field) {
            for (auto n = 0; n < 40; ++n) {
                send(packet(request_type, query));
                if (receive(reply_type, any, reject_type).has(field))
                    return;
                std::this_thread::sleep_for(25ms);
            }
            throw std::runtime_error("DTC private snapshot stayed unavailable " + std::to_string(request_type));
        };
        reconcile(300, 301, 302, 10);
        reconcile(305, 306, 307, 6);
    };
    connect();
    const auto put_double = [](Bytes& out, unsigned field, double number) {
        put_varint(out, field * 8 + 1);
        const auto bits = std::bit_cast<std::uint64_t>(number);
        for (unsigned i = 0; i < 8; ++i)
            out.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    };
    const auto place = [&](std::string_view key, std::string_view price) {
        Bytes add;
        put_text(add, 1, symbol);
        put_text(add, 2, dtc::kDtcMoexSpectraExchange);
        put_text(add, 4, key);
        put_integer(add, 5, 2);
        put_integer(add, 6, 1);
        put_double(add, 9, 1);
        put_integer(add, 10, 1);
        put_text(add, 17, price);
        send(packet(208, add));
    };
    const auto order = [&](std::string_view key, unsigned state, std::string_view native = {}) {
        return receive(301, [&](const auto& m) {
            return m.has(8) && m.string(8) == key && m.number(10) == state &&
                   (native.empty() || (m.has(9) && m.string(9) == native));
        });
    };
    place("dtc-worker-one", "102500");
    const auto working = order("dtc-worker-one", 4);
    const auto stable = std::string(working.string(7));
    test::require(stable == "100" && working.string(9) == "20003", "DTC Add did not use persisted native identity");
    Bytes move;
    put_text(move, 1, stable);
    put_text(move, 2, "dtc-worker-one");
    put_integer(move, 6, 1);
    put_text(move, 13, "102250");
    send(packet(204, move));
    const auto moved = order("dtc-worker-one", 4, "20004");
    test::require(moved.string(7) == stable, "DTC Move changed the stable ServerOrderID");
    place("dtc-worker-one", "102500");
    const auto duplicate = receive(
        301, [&](const auto& m) { return m.has(27) && m.string(27) == "duplicate ClientOrderID; no new order"; });
    test::require(duplicate.string(7) == stable && duplicate.number(11) == 3, "duplicate lost active order state");
    Bytes cancel;
    put_text(cancel, 1, stable);
    put_text(cancel, 2, "dtc-worker-one");
    send(packet(203, cancel));
    order("dtc-worker-one", 8);
    place("dtc-worker-fill", "103000");
    order("dtc-worker-fill", 7);
    place("dtc-worker-recover", "102500");
    const auto before_restart = std::string(order("dtc-worker-recover", 4, "20006").string(7));
    Bytes recovery_move;
    put_text(recovery_move, 1, before_restart);
    put_text(recovery_move, 2, "dtc-worker-recover");
    put_integer(recovery_move, 6, 1);
    put_text(recovery_move, 13, "102250");
    send(packet(204, recovery_move));
    test::require(order("dtc-worker-recover", 4, "20007").string(7) == before_restart, "restart fixture Move lost ID");
    owner->crash();
    disconnect();
    test::write_text_file(ini, contents(ini) + "profile=matrix-dtc-recovery\n");
    owner = std::make_unique<Process>(request, "dtc-worker-restarted");
    wait_status(*owner, log, writer_ready);
    connect();
    place("dtc-worker-recover", "102500");
    const auto restored = receive(
        301, [&](const auto& m) { return m.has(27) && m.string(27) == "duplicate ClientOrderID; no new order"; });
    test::require(restored.string(7) == before_restart && restored.string(9) == "20007",
                  "restart did not recover the original DTC and venue identity");
    disconnect();
    std::this_thread::sleep_for(1s);
    connect();
    disconnect();
    const auto latest_loss = std::chrono::steady_clock::now();
    const auto posts_path = fixture.config_dir / "fol-native-posts.log";
    while (occurrences(contents(posts_path), "DelOrder wire_valid=1") < 2 &&
           std::chrono::steady_clock::now() - latest_loss < 14s)
        std::this_thread::sleep_for(25ms);
    const auto elapsed = std::chrono::steady_clock::now() - latest_loss;
    test::require(elapsed >= 9500ms && elapsed < 14s, "client loss sweep did not reset and rearm after reconnect");
    owner->terminate();
    test::require(owner->wait() == 0, "DTC worker did not stop with fake owner");
    const auto posts = contents(posts_path);
    test::require(occurrences(posts, "AddOrder wire_valid=1") == 3 &&
                      occurrences(posts, "MoveOrder wire_valid=1") == 2 &&
                      occurrences(posts, "DelOrder wire_valid=1") == 2 &&
                      contents(log).find("\"event\":\"trade\"") != std::string::npos,
                  "DTC worker did not exercise native Add/Move/Del and user_deal exactly once");
}
} // namespace
