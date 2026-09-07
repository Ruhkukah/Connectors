// Read-only SDK experiment. Run ONLY in a --network none container; loopback port 1 has no Access Server.
#include <cgate.h>
#include <cstdio>
#include <string>
static unsigned callbacks = 0;
static CG_RESULT callback(cg_conn_t*, cg_listener_t*, cg_msg_t* message, void*) {
    ++callbacks;
    std::printf("callback=%u\n", message->type);
    return 0;
}
int main() {
    const auto env = cg_env_open("key=00000000");
    std::printf("env=%u\n", env);
    if (env)
        return 1;
    cg_conn_t* connection = nullptr;
    const auto created = cg_conn_new("p2tcp://127.0.0.1:1;app_name=c2_offline", &connection);
    std::printf("connection_new=%u\n", created);
    if (created) {
        cg_env_close();
        return 1;
    }
    const auto connection_open = cg_conn_open(connection, nullptr);
    unsigned connection_state = 0;
    for (unsigned i = 0; i < 32; ++i) {
        cg_conn_process(connection, 1000, nullptr);
        cg_conn_getstate(connection, &connection_state);
        if (connection_state == CG_STATE_ERROR)
            break;
    }
    std::printf("loopback_connection_open=%u state=%u; network=none\n", connection_open, connection_state);
    for (int pair = 0; pair < 2; ++pair) {
        std::string url = "p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL;name=c2_probe";
        url += ";online.scheme=|FILE|/evidence/ordLog_trades.ini|CustReplScheme";
        url += ";snapshot.scheme=|FILE|/evidence/ordbook.ini|CustReplScheme";
        if (pair)
            url += ";snapshot.data=multileg_orders;online.data=multileg_orders_log";
        cg_listener_t* listener = nullptr;
        const auto made = cg_lsn_new(connection, url.c_str(), callback, nullptr, &listener);
        std::printf("pair=%s new=%u\n", pair ? "multileg" : "regular", made);
        if (made)
            continue;
        // Deliberate pre-OPEN diagnostic; not a negotiated-schema qualification.
        cg_scheme_desc_t* scheme = nullptr;
        const auto got = cg_lsn_getscheme(listener, &scheme);
        std::printf("pre_open_getscheme=%u present=%d\n", got, scheme != nullptr);
        if (!got && scheme) {
            std::size_t index = 0;
            for (auto* message = scheme->messages; message; message = message->next)
                std::printf("index=%zu name=%s size=%zu\n", index++, message->name, message->size);
        }
        const auto opened = cg_lsn_open(listener, nullptr);
        unsigned state = 0;
        cg_lsn_getstate(listener, &state);
        std::printf("listener_open=%u state=%u callbacks=%u\n", opened, state, callbacks);
        cg_lsn_destroy(listener);
    }
    cg_conn_destroy(connection);
    cg_env_close();
}
