#pragma once
// TEST-only wire source: 90% unconfigured ISINs; admitted rows are 40% adds,
// 40% cancels, 20% trades. Ephemeral levels appear and disappear each cycle.
namespace {
struct FullOrderLogReplay {
    std::uint64_t remaining{}, sequence{}, retained{}, phase_start{};
    std::uint32_t batch{};
    bool mixed{};
    void (*allocation_window)(bool){};
    std::vector<std::byte> payload;
    std::vector<std::uint64_t> callback_samples;
    const MessagePlan* plan{};
    std::size_t order{}, row_id{}, revision{}, remainder{}, price{}, direction{}, action{}, isin{}, moment{};
    std::array<moex::plaza2::public_wire::Bcd16_5, 192> prices{};
    void configure(std::uint64_t count, std::uint64_t orders, std::uint32_t per_commit, bool churn = false) {
        remaining = count;
        retained = orders;
        batch = per_commit;
        mixed = churn;
        phase_start = sequence;
        callback_samples.clear();
        callback_samples.reserve(count);
    }
    std::uint32_t emit(FakeConnection& connection) {
        for (auto* listener : connection.listeners) {
            if (listener->stream_code != kFullOrderLogStreamCode || !listener->script_emitted ||
                listener->state != kStateActive)
                continue;
            if (!plan) {
                for (const auto& p : listener->message_plans)
                    if (p.message_name == "orders_log")
                        plan = &p;
                if (!plan || !retained || !batch)
                    return kCgErrInvalidArgument;
                FakeMessageScript seed{
                    .table_code = plan->table_code,
                    .fields = {{.field_code = FieldCode::kFortsTradeReplOrdersLogXstatus, .signed_value = 1}}};
                payload = encode_message_payload(*plan, seed);
                for (const auto& f : plan->fields) {
                    using enum FieldCode;
                    if (f.field_code == kFortsTradeReplOrdersLogPublicOrderId)
                        order = f.offset;
                    if (f.field_code == kFortsTradeReplOrdersLogReplId)
                        row_id = f.offset;
                    if (f.field_code == kFortsTradeReplOrdersLogReplRev)
                        revision = f.offset;
                    if (f.field_code == kFortsTradeReplOrdersLogPublicAmountRest)
                        remainder = f.offset;
                    if (f.field_code == kFortsTradeReplOrdersLogPrice)
                        price = f.offset;
                    if (f.field_code == kFortsTradeReplOrdersLogDir)
                        direction = f.offset;
                    if (f.field_code == kFortsTradeReplOrdersLogPublicAction)
                        action = f.offset;
                    if (f.field_code == kFortsTradeReplOrdersLogIsinId)
                        isin = f.offset;
                    if (f.field_code == kFortsTradeReplOrdersLogMomentNs)
                        moment = f.offset;
                }
                for (std::size_t i = 0; i < prices.size(); ++i) {
                    const auto value = i < 64    ? 100000 + i * 250
                                       : i < 128 ? 80000 + (i - 64) * 250
                                                 : 120000 + (i - 128) * 250;
                    prices[i] = *encode_d16_5(std::to_string(value) + ".00000");
                }
            }
            if (auto error = emit_simple_message(*listener, kCgMsgTnBegin); error)
                return error;
            const auto count = std::min<std::uint64_t>(remaining, batch);
            const auto utc = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            for (std::uint64_t n = 0; n < count; ++n) {
                const auto index = sequence - phase_start;
                std::int64_t id = 1000000 + index % retained, qty = 1000;
                std::size_t level = index % 64;
                std::int32_t instrument = 1001;
                std::int8_t update_action = 1, dir = level < 32 ? 1 : 2;
                if (mixed) {
                    if (index % 10 != 0)
                        instrument = 1002 + index % 4999;
                    const auto active = index / 10, phase = active % 5, cycle = active / 5;
                    if (phase == 4) {
                        id = 1000000 + cycle % retained;
                        qty = 999 - cycle / retained;
                        level = (id - 1000000) % 64;
                        dir = level < 32 ? 1 : 2;
                        update_action = 2;
                    } else {
                        id = 1000000000 + cycle * 2 + phase % 2;
                        level = 64 + (phase % 2) * 64 + cycle % 64;
                        dir = phase % 2 == 0 ? 1 : 2;
                        update_action = phase < 2 ? 1 : 0;
                        qty = phase < 2 ? 100 : 0;
                    }
                }
                const std::int64_t rev = 1000000 + sequence;
                std::memcpy(payload.data() + action, &update_action, 1);
                std::memcpy(payload.data() + direction, &dir, 1);
                std::memcpy(payload.data() + order, &id, 8);
                std::memcpy(payload.data() + row_id, &rev, 8);
                std::memcpy(payload.data() + revision, &rev, 8);
                std::memcpy(payload.data() + remainder, &qty, 8);
                std::memcpy(payload.data() + isin, &instrument, 4);
                std::memcpy(payload.data() + moment, &utc, 8);
                std::memcpy(payload.data() + price, prices[level].data(), prices[level].size());
                CgMsgStreamData msg{.type = kCgMsgStreamData,
                                    .data_size = payload.size(),
                                    .data = payload.data(),
                                    .msg_index = plan->msg_index,
                                    .msg_name = plan->message_name.c_str(),
                                    .rev = rev};
                const auto start = std::chrono::steady_clock::now();
                if (allocation_window)
                    allocation_window(true);
                const auto error = listener->callback(listener->connection, listener, &msg, listener->callback_data);
                if (allocation_window)
                    allocation_window(false);
                callback_samples.push_back(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                        .count());
                if (error)
                    return error;
                ++sequence;
                --remaining;
            }
            return emit_simple_message(*listener, kCgMsgTnCommit);
        }
        return kCgErrIncorrectState;
    }
} full_order_log_replay;
} // namespace
extern "C" void moex_fake_ordlog_replay(std::uint64_t rows, std::uint64_t retained, std::uint32_t batch) {
    full_order_log_replay.configure(rows, retained, batch);
}
extern "C" void moex_fake_ordlog_mixed_replay(std::uint64_t rows, std::uint64_t retained, std::uint32_t batch) {
    full_order_log_replay.configure(rows, retained, batch, true);
}
extern "C" void moex_fake_ordlog_remaining(std::uint64_t* out) {
    if (out)
        *out = full_order_log_replay.remaining;
}
extern "C" void moex_fake_ordlog_callback_samples(std::vector<std::uint64_t>* out) {
    if (out)
        *out = full_order_log_replay.callback_samples;
}

extern "C" void moex_fake_ordlog_allocation_window(void (*window)(bool)) {
    full_order_log_replay.allocation_window = window;
}
