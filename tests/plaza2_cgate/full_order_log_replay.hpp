#pragma once
// Deterministic TEST-only wire source. Payloads are prepared before timing;
// the measured generator invokes the actual dynamically loaded CGate callback.
namespace {
struct FullOrderLogReplay {
    std::uint64_t remaining{}, sequence{}, retained{};
    std::uint32_t batch{};
    std::vector<std::byte> payload;
    std::vector<std::uint64_t> callback_samples;
    const MessagePlan* plan{};
    std::size_t order{}, revision{}, remainder{}, price{}, direction{}, action{};
    std::array<moex::plaza2::public_wire::Bcd16_5, 64> prices{};
    void configure(std::uint64_t count, std::uint64_t orders, std::uint32_t per_commit) {
        remaining = count;
        retained = orders;
        batch = per_commit;
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
                    .fields = {
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogIsinId, .signed_value = 4001},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogPublicAction, .signed_value = 1},
                        {.field_code = FieldCode::kFortsTradeReplOrdersLogXstatus, .signed_value = 1},
                    }};
                payload = encode_message_payload(*plan, seed);
                for (const auto& f : plan->fields) {
                    using enum FieldCode;
                    if (f.field_code == kFortsTradeReplOrdersLogPublicOrderId)
                        order = f.offset;
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
                }
                for (std::size_t i = 0; i < prices.size(); ++i)
                    prices[i] = *encode_d16_5(std::to_string(100 + i) + ".00000");
            }
            if (auto error = emit_simple_message(*listener, kCgMsgTnBegin); error)
                return error;
            const auto count = std::min<std::uint64_t>(remaining, batch);
            for (std::uint64_t n = 0; n < count; ++n) {
                const std::int64_t id = 1000000 + sequence % retained;
                const std::int64_t rev = 1000000 + sequence;
                const std::int64_t qty = 1000 - sequence / retained;
                const std::int8_t update_action = sequence < retained ? 1 : 2;
                std::memcpy(payload.data() + action, &update_action, 1);
                std::memcpy(payload.data() + order, &id, 8);
                std::memcpy(payload.data() + revision, &rev, 8);
                std::memcpy(payload.data() + remainder, &qty, 8);
                const auto level = (id - 1000000) % prices.size();
                std::memcpy(payload.data() + price, prices[level].data(), prices[level].size());
                const std::int8_t dir = level < 32 ? 1 : 2;
                std::memcpy(payload.data() + direction, &dir, 1);
                CgMsgStreamData msg{.type = kCgMsgStreamData,
                                    .data_size = payload.size(),
                                    .data = payload.data(),
                                    .msg_index = plan->msg_index,
                                    .msg_name = plan->message_name.c_str(),
                                    .rev = rev};
                const auto start = std::chrono::steady_clock::now();
                const auto error = listener->callback(listener->connection, listener, &msg, listener->callback_data);
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

extern "C" void moex_fake_ordlog_callback_samples(std::vector<std::uint64_t>* out) {
    if (out)
        *out = full_order_log_replay.callback_samples;
}
