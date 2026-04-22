#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr std::uint32_t kCgErrOk = 0;
constexpr std::uint32_t kCgRangeBegin = 131072;
constexpr std::uint32_t kCgErrInvalidArgument = kCgRangeBegin + 1;
constexpr std::uint32_t kCgErrTimeout = kCgRangeBegin + 3;
constexpr std::uint32_t kCgErrIncorrectState = kCgRangeBegin + 5;

constexpr std::uint32_t kStateClosed = 0;
constexpr std::uint32_t kStateOpening = 1;
constexpr std::uint32_t kStateActive = 2;
constexpr std::uint32_t kStateError = 3;

bool g_env_open = false;

struct FakeConnection {
    std::uint32_t state{kStateClosed};
    std::string settings;
};

struct FakeListener {
    std::uint32_t state{kStateClosed};
    std::string settings;
};

using CgListenerCallback = std::uint32_t (*)(void* conn, void* listener, void* msg, void* data);

} // namespace

extern "C" {

std::uint32_t cg_env_open(const char* settings) {
    if (settings == nullptr || *settings == '\0') {
        return kCgErrInvalidArgument;
    }
    g_env_open = true;
    return kCgErrOk;
}

std::uint32_t cg_env_close() {
    if (!g_env_open) {
        return kCgErrIncorrectState;
    }
    g_env_open = false;
    return kCgErrOk;
}

std::uint32_t cg_conn_new(const char* settings, void** connptr) {
    if (!g_env_open || settings == nullptr || connptr == nullptr) {
        return !g_env_open ? kCgErrIncorrectState : kCgErrInvalidArgument;
    }
    auto* connection = new FakeConnection{};
    connection->settings = settings;
    *connptr = connection;
    return kCgErrOk;
}

std::uint32_t cg_conn_destroy(void* conn) {
    if (conn == nullptr) {
        return kCgErrInvalidArgument;
    }
    delete static_cast<FakeConnection*>(conn);
    return kCgErrOk;
}

std::uint32_t cg_conn_open(void* conn, const char*) {
    if (!g_env_open || conn == nullptr) {
        return !g_env_open ? kCgErrIncorrectState : kCgErrInvalidArgument;
    }
    auto* connection = static_cast<FakeConnection*>(conn);
    if (connection->state == kStateActive) {
        connection->state = kStateError;
        return kCgErrIncorrectState;
    }
    connection->state = kStateActive;
    return kCgErrOk;
}

std::uint32_t cg_conn_close(void* conn) {
    if (conn == nullptr) {
        return kCgErrInvalidArgument;
    }
    auto* connection = static_cast<FakeConnection*>(conn);
    connection->state = kStateClosed;
    return kCgErrOk;
}

std::uint32_t cg_conn_process(void* conn, std::uint32_t, void*) {
    if (conn == nullptr) {
        return kCgErrInvalidArgument;
    }
    auto* connection = static_cast<FakeConnection*>(conn);
    if (connection->state != kStateActive) {
        return kCgErrIncorrectState;
    }
    return kCgErrTimeout;
}

std::uint32_t cg_conn_getstate(void* conn, std::uint32_t* state) {
    if (conn == nullptr || state == nullptr) {
        return kCgErrInvalidArgument;
    }
    *state = static_cast<FakeConnection*>(conn)->state;
    return kCgErrOk;
}

std::uint32_t cg_lsn_new(void* conn, const char* settings, CgListenerCallback callback, void* data, void** lsnptr) {
    if (!g_env_open || conn == nullptr || settings == nullptr || callback == nullptr || lsnptr == nullptr) {
        return !g_env_open ? kCgErrIncorrectState : kCgErrInvalidArgument;
    }
    auto* listener = new FakeListener{};
    listener->settings = settings;
    *lsnptr = listener;
    callback(conn, listener, data, nullptr);
    return kCgErrOk;
}

std::uint32_t cg_lsn_destroy(void* listener) {
    if (listener == nullptr) {
        return kCgErrInvalidArgument;
    }
    delete static_cast<FakeListener*>(listener);
    return kCgErrOk;
}

std::uint32_t cg_lsn_open(void* listener, const char*) {
    if (listener == nullptr) {
        return kCgErrInvalidArgument;
    }
    auto* typed = static_cast<FakeListener*>(listener);
    typed->state = kStateActive;
    return kCgErrOk;
}

std::uint32_t cg_lsn_close(void* listener) {
    if (listener == nullptr) {
        return kCgErrInvalidArgument;
    }
    auto* typed = static_cast<FakeListener*>(listener);
    typed->state = kStateClosed;
    return kCgErrOk;
}

std::uint32_t cg_lsn_getstate(void* listener, std::uint32_t* state) {
    if (listener == nullptr || state == nullptr) {
        return kCgErrInvalidArgument;
    }
    *state = static_cast<FakeListener*>(listener)->state;
    return kCgErrOk;
}

} // extern "C"
