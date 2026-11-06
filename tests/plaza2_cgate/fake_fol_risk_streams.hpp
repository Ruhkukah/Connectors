// Included within the fake runtime implementation to share its wire fixtures.
bool is_fol_risk_stream(StreamCode code) {
    return code == fixture::kFolRiskPart || code == fixture::kFolRiskProhibition || code == fixture::kFolRiskUser ||
           code == fixture::kFolRiskEnvironment;
}

bool configure_fol_risk_stream(FakeListener& listener) {
    struct Field {
        const char* name;
        const char* type;
        std::size_t size;
    };
    constexpr Field common[]{{"replID", "i8", 8}, {"replRev", "i8", 8}, {"replAct", "i8", 8}};
    constexpr Field part[]{{"client_code", "c7", 8},
                           {"money_free", "d26.2", 15},
                           {"money_blocked", "d26.2", 15},
                           {"money_amount", "d26.2", 15}};
    constexpr Field prohibition[]{{"client_code", "c7", 8}, {"type", "i4", 4}};
    constexpr Field user[]{{"login", "c20", 21},
                           {"sma_flags", "i4", 4},
                           {"start_date", "t", sizeof(CgTime)},
                           {"end_date", "t", sizeof(CgTime)}};
    constexpr Field environment[]{{"environment_id", "i4", 4}, {"environment_name", "c8", 9}};
    std::span<const Field> fields;
    const char* name{};
    if (listener.settings == "p2repl://FORTS_PART_REPL;tables=part") {
        listener.stream_code = fixture::kFolRiskPart;
        name = "part";
        fields = part;
    } else if (listener.settings == "p2repl://FORTS_PROHIBITION_REPL;tables=prohibition") {
        listener.stream_code = fixture::kFolRiskProhibition;
        name = "prohibition";
        fields = prohibition;
    } else if (listener.settings == "p2repl://FORTS_REFDATA_REPL;tables=user") {
        listener.stream_code = fixture::kFolRiskUser;
        name = "user";
        fields = user;
    } else if (listener.settings == "p2repl://FORTS_REFDATA_REPL;tables=spectra_environment") {
        listener.stream_code = fixture::kFolRiskEnvironment;
        name = "spectra_environment";
        fields = environment;
    } else
        return false;

    auto message = std::make_unique<OwnedMessage>();
    message->name = name;
    message->desc.id = 1;
    message->desc.name = message->name.data();
    const auto add_field = [&](Field field) {
        auto item = std::make_unique<OwnedField>();
        item->name = field.name;
        item->type_token = field.type;
        item->desc.name = item->name.data();
        item->desc.type = item->type_token.data();
        item->desc.size = field.size;
        item->desc.offset = message->desc.size;
        message->desc.size += field.size;
        if (!message->fields.empty())
            message->fields.back()->desc.next = &item->desc;
        message->fields.push_back(std::move(item));
    };
    for (const auto field : common)
        add_field(field);
    for (const auto field : fields)
        add_field(field);
    message->desc.fields = &message->fields.front()->desc;
    message->desc.num_fields = message->fields.size();
    listener.scheme = std::make_unique<OwnedScheme>();
    listener.scheme->desc.num_messages = 1;
    listener.scheme->desc.messages = &message->desc;
    listener.scheme->messages.push_back(std::move(message));
    return true;
}

std::uint32_t emit_fol_risk_snapshot(FakeListener& listener) {
    const std::string_view profile = option(Option::FolRiskStreams);
    const auto& table = *listener.scheme->messages.front();
    std::vector<std::byte> payload(table.desc.size);
    const auto store = [&](std::string_view field, const void* bytes, std::size_t size) {
        const auto found =
            std::ranges::find_if(table.fields, [field](const auto& item) { return item->name == field; });
        std::memcpy(payload.data() + (*found)->desc.offset, bytes, size);
    };
    const auto number = [&](std::string_view field, auto value) { store(field, &value, sizeof(value)); };
    const auto text = [&](std::string_view field, std::string_view value) { store(field, value.data(), value.size()); };
    const auto emit = [&](std::int64_t id, std::int64_t revision) {
        number("replID", id);
        number("replRev", revision);
        CgMsgStreamData event{.type = kCgMsgStreamData,
                              .data_size = payload.size(),
                              .data = payload.data(),
                              .msg_id = 1,
                              .msg_name = table.name.c_str(),
                              .rev = revision};
        return listener.callback(listener.connection, &listener, &event, listener.callback_data);
    };
    const bool t1_environment = listener.stream_code == fixture::kFolRiskEnvironment && profile == "t1-environment";
    CgDataLifeNum life{.life_number = t1_environment ? 131140477u : 7u};
    if (const auto result = emit_simple_message(listener, kCgMsgP2replLifenum, &life, sizeof(life)); result)
        return result;
    bool row = true, two = false;
    if (listener.stream_code == fixture::kFolRiskPart) {
        text("client_code", "CLIENT1");
        constexpr std::array<std::uint8_t, 15> balance{2, 26, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 12, 34, 56};
        for (const auto field : {"money_free", "money_blocked", "money_amount"})
            store(field, balance.data(), balance.size());
    } else if (listener.stream_code == fixture::kFolRiskProhibition) {
        row = profile == "prohibited";
        text("client_code", "CLIENT1");
        number("type", std::int32_t{1});
    } else if (listener.stream_code == fixture::kFolRiskEnvironment) {
        row = profile != "no-environment";
        two = profile == "two-environments";
        const bool wrong = profile == "wrong-environment";
        number("environment_id", std::int32_t{t1_environment ? 7 : wrong ? 1 : 2});
        text("environment_name", t1_environment ? "MXFOT1" : wrong ? "MXFOPROD" : "MXFOPREP");
    } else {
        two = profile == "two-logins";
        text("login", profile == "other-login" ? "other-login" : "risk-login");
        number("sma_flags", std::int32_t{profile == "cod-enabled" ? 3 : 2});
        if (profile != "null-start")
            number("start_date", CgTime{.year = 2000, .month = 1, .day = 1});
    }
    if (row)
        if (const auto result = emit(1, t1_environment ? 42 : 1); result)
            return result;
    if (two)
        if (const auto result = emit(2, 2); result)
            return result;
    return emit_simple_message(listener, kCgMsgP2replOnline);
}
