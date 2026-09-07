#include "plaza2_c2_capture_abi.hpp"
#include "plaza2_public_wire.hpp"
#include "moex/plaza2/cgate/plaza2_runtime.hpp"
#include <atomic>
#include <mutex>
#include <algorithm>
#include <bit>
#include <chrono>
#include <charconv>
#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <span>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using moex::plaza2::cgate::plaza2_sha256_hex;
#ifndef MOEX_SOURCE_GIT_SHA
#define MOEX_SOURCE_GIT_SHA "unknown"
#endif
namespace {
std::uint64_t now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
std::uint64_t number(std::string_view value) {
    std::uint64_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::runtime_error("invalid numeric option");
    return result;
}
volatile std::sig_atomic_t stopped = 0;
void stop(int) {
    stopped = 1;
}
std::string quote(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c < 32 || c > 126) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += c;
    }
    return out + '"';
}
std::string hash_file(const fs::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::runtime_error("cannot hash input file");
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        ::close(fd);
        throw std::runtime_error("hash input is not a regular file");
    }
    void* memory = st.st_size ? mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : nullptr;
    ::close(fd);
    if (memory == MAP_FAILED)
        throw std::runtime_error("hash mapping failed");
    const auto digest =
        plaza2_sha256_hex(std::span{static_cast<const std::byte*>(memory), static_cast<std::size_t>(st.st_size)});
    if (memory)
        munmap(memory, st.st_size);
    return digest;
}
void json_file(const fs::path& p, const std::string& content) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content << '\n';
    f.flush();
    if (!f)
        throw std::runtime_error("evidence index write failed");
}
struct Config {
    std::map<std::string, std::string> values;
    std::vector<std::string> secrets;
    explicit Config(const fs::path& path) {
        std::ifstream input(path);
        if (!input)
            throw std::runtime_error("config unavailable");
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty() || line.front() == '#')
                continue;
            const auto split = line.find('=');
            if (split == std::string::npos || !values.emplace(line.substr(0, split), line.substr(split + 1)).second)
                throw std::runtime_error("invalid or duplicate config key");
        }
        for (const auto& [key, value] : values) {
            if (key != "runtime" && key != "env" && key != "connection" && key != "regular" && key != "multileg" &&
                key != "env_file" && key != "ordbook_scheme" && key != "ordlog_scheme")
                throw std::runtime_error("unknown config key");
            collect(value);
        }
        if (values.contains("env_file")) {
            std::ifstream f(values.at("env_file"));
            if (!f)
                throw std::runtime_error("environment file unavailable");
            while (std::getline(f, line))
                collect(line);
        }
        for (const char* key : {"runtime", "env", "connection", "regular", "ordbook_scheme", "ordlog_scheme"})
            if (!values.contains(key) || values.at(key).empty())
                throw std::runtime_error("missing config key");
        if (!values.at("connection").starts_with("p2tcp://") && !values.at("connection").starts_with("p2lrpcq://") &&
            !values.at("connection").starts_with("p2sys://"))
            throw std::runtime_error("unapproved connection transport");
        if (values.at("connection").find('@') != std::string::npos)
            throw std::runtime_error("userinfo connection URL unsupported");
        if (values.at("env").find("log=") != std::string::npos ||
            values.at("env").find("minloglevel=") != std::string::npos)
            throw std::runtime_error("capture controls vendor logging");
        const std::regex ini_pattern(R"((?:^|;)ini=([^;]+))");
        std::smatch ini;
        if (std::regex_search(values.at("env"), ini, ini_pattern) &&
            (!values.contains("env_file") || values.at("env_file") != ini[1]))
            throw std::runtime_error("environment INI must be declared for hashing and redaction");
        for (const char* key : {"regular", "multileg"})
            if (values.contains(key)) {
                const auto& url = values.at(key);
                if (!url.starts_with("p2ordbook://FORTS_ORDLOG_REPL;") ||
                    url.find("snapshot=FORTS_ORDBOOK_REPL") == std::string::npos ||
                    url.find("replstate") != std::string::npos || url.find("FORTS_TRADE") != std::string::npos ||
                    url.find("USER") != std::string::npos)
                    throw std::runtime_error("only anonymous composite listeners permitted");
                // No alternate stream or embedded URL can escape the public subscription.
                std::size_t start = url.find(';') + 1;
                std::map<std::string, std::string> parts;
                while (start < url.size()) {
                    const auto end = url.find(';', start);
                    const auto token = url.substr(start, end - start);
                    const auto equal = token.find('=');
                    if (equal == std::string::npos)
                        throw std::runtime_error("invalid listener setting");
                    const auto name = token.substr(0, equal);
                    const auto value = token.substr(equal + 1);
                    if (!parts.emplace(name, value).second)
                        throw std::runtime_error("duplicate listener setting");
                    if (name == "snapshot") {
                        if (value != "FORTS_ORDBOOK_REPL")
                            throw std::runtime_error("private snapshot forbidden");
                    } else if (name == "snapshot.data") {
                        if (value != (std::string_view(key) == "regular" ? "orders" : "multileg_orders"))
                            throw std::runtime_error("invalid snapshot dataset");
                    } else if (name == "online.data") {
                        if (value != (std::string_view(key) == "regular" ? "orders_log" : "multileg_orders_log"))
                            throw std::runtime_error("invalid online dataset");
                    } else if (name == "snapshot.bind") {
                        if (value != "info.trades_rev")
                            throw std::runtime_error("unqualified bound");
                    } else if (name == "online.scheme" || name == "snapshot.scheme") {
                        const auto filekey = name == "online.scheme" ? "ordlog_scheme" : "ordbook_scheme";
                        if (!values.contains(filekey) || value != "|FILE|" + values.at(filekey) + "|CustReplScheme")
                            throw std::runtime_error("explicit scheme must have a hashed source");
                    } else if (name != "name")
                        throw std::runtime_error("unapproved listener parameter");
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
            }
    }
    void collect(const std::string& text) {
        static const std::regex pattern(
            R"((?:^|[;\s])(?:key|password|passwd|pass|pwd|token|login|username|user|local_pass)\s*=\s*([^;\r\n]+))",
            std::regex::icase);
        for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern); it != std::sregex_iterator(); ++it)
            secrets.push_back((*it)[1]);
    }
    std::string clean(std::string text) const {
        for (const char* key : {"env", "connection"})
            if (values.contains(key)) {
                const auto& value = values.at(key);
                std::size_t pos = 0;
                while (!value.empty() && (pos = text.find(value, pos)) != std::string::npos) {
                    text.replace(pos, value.size(), "[REDACTED_CONFIG]");
                    pos += 17;
                }
            }
        for (const auto& secret : secrets)
            if (!secret.empty()) {
                std::size_t pos = 0;
                while ((pos = text.find(secret, pos)) != std::string::npos) {
                    text.replace(pos, secret.size(), "[REDACTED]");
                    pos += 10;
                }
            }
        return text;
    }
    bool sensitive(std::span<const std::byte> bytes) const {
        const std::string_view raw(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return std::any_of(secrets.begin(), secrets.end(),
                           [&](const auto& s) { return !s.empty() && raw.find(s) != std::string_view::npos; });
    }
};
// This is the entire dynamic API allowlist. No publisher, reply or transaction function pointer exists.
struct Api {
    void* module;
    template <class T> T get(const char* name) {
        auto ptr = reinterpret_cast<T>(dlsym(module, name));
        if (!ptr)
            throw std::runtime_error("required CGate API missing");
        return ptr;
    }
    std::uint32_t (*env_open)(const char*);
    std::uint32_t (*env_close)();
    std::uint32_t (*conn_new)(const char*, void**);
    std::uint32_t (*conn_open)(void*, const char*);
    std::uint32_t (*conn_close)(void*);
    std::uint32_t (*conn_destroy)(void*);
    std::uint32_t (*process)(void*, std::uint32_t, void*);
    std::uint32_t (*conn_state)(void*, std::uint32_t*);
    std::uint32_t (*lsn_new)(void*, const char*, CgListenerCallback, void*, void**);
    std::uint32_t (*lsn_open)(void*, const char*);
    std::uint32_t (*lsn_close)(void*);
    std::uint32_t (*lsn_destroy)(void*);
    std::uint32_t (*lsn_state)(void*, std::uint32_t*);
    std::uint32_t (*scheme)(void*, void**);
    const char* (*error)(std::uint32_t);
    std::uint32_t (*version)(const char*, int*, int*, int*);
    explicit Api(const std::string& path) : module(dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)) {
        if (!module)
            throw std::runtime_error("cannot load runtime");
#define API(member, name) member = get<decltype(member)>(name)
        API(env_open, "cg_env_open");
        API(env_close, "cg_env_close");
        API(conn_new, "cg_conn_new");
        API(conn_open, "cg_conn_open");
        API(conn_close, "cg_conn_close");
        API(conn_destroy, "cg_conn_destroy");
        API(process, "cg_conn_process");
        API(conn_state, "cg_conn_getstate");
        API(lsn_new, "cg_lsn_new");
        API(lsn_open, "cg_lsn_open");
        API(lsn_close, "cg_lsn_close");
        API(lsn_destroy, "cg_lsn_destroy");
        API(lsn_state, "cg_lsn_getstate");
        API(scheme, "cg_lsn_getscheme");
        API(error, "cg_err_getstr");
        API(version, "cg_env_getcomp_ver");
#undef API
    }
    const char* error_text(std::uint32_t code) const {
        const auto* text = error(code);
        return text ? text : "";
    }
    ~Api() {
        dlclose(module);
    }
};
// Error-only SDK output is drained independently, bounded, and redacted before entering the binary evidence.
// No logging operation is performed for market data; callback ordering remains independent of diagnostics.
struct Diagnostics {
    int saved{-1}, saved_error{-1};
    int reader{-1};
    std::thread worker;
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, std::string>> lines;
    std::atomic<bool> overflow{false};
    Diagnostics() {
        int pipefd[2];
        if (::pipe(pipefd))
            throw std::runtime_error("diagnostic pipe failed");
        saved = ::dup(STDOUT_FILENO);
        saved_error = ::dup(STDERR_FILENO);
        reader = pipefd[0];
        if (saved < 0 || saved_error < 0 || ::dup2(pipefd[1], STDOUT_FILENO) < 0 ||
            ::dup2(pipefd[1], STDERR_FILENO) < 0) {
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            if (saved >= 0) {
                ::dup2(saved, STDOUT_FILENO);
                ::close(saved);
            }
            if (saved_error >= 0) {
                ::dup2(saved_error, STDERR_FILENO);
                ::close(saved_error);
            }
            throw std::runtime_error("diagnostic redirection failed");
        }
        ::close(pipefd[1]);
        worker = std::thread([this] {
            std::string line;
            std::array<char, 4096> bytes{};
            const auto record = [&] {
                std::lock_guard lock(mutex);
                if (lines.size() == 256)
                    overflow = true;
                else
                    lines.emplace_back(now(), std::move(line));
                line.clear();
            };
            for (;;) {
                const auto count = ::read(reader, bytes.data(), bytes.size());
                if (count < 0) {
                    if (errno == EINTR)
                        continue;
                    overflow = true;
                    break;
                }
                if (!count)
                    break;
                for (std::ptrdiff_t i = 0; i < count; ++i) {
                    if (bytes[i] == '\n')
                        record();
                    else if (line.size() < 65536)
                        line += bytes[i];
                    else
                        overflow = true;
                }
            }
            if (!line.empty())
                record();
        });
    }
    void stop() {
        if (saved >= 0) {
            std::fflush(stdout);
            ::dup2(saved, STDOUT_FILENO);
            ::close(saved);
            saved = -1;
        }
        if (saved_error >= 0) {
            std::fflush(stderr);
            ::dup2(saved_error, STDERR_FILENO);
            ::close(saved_error);
            saved_error = -1;
        }
        if (worker.joinable())
            worker.join();
        if (reader >= 0) {
            ::close(reader);
            reader = -1;
        }
    }
    auto take() {
        std::lock_guard lock(mutex);
        std::vector<std::pair<std::uint64_t, std::string>> result;
        result.swap(lines);
        return result;
    }
    ~Diagnostics() {
        stop();
    }
};
struct Metrics {
    std::uint64_t callbacks{}, records{}, bytes{}, high_water{}, overflow{}, writes{}, descriptors{}, unknown{},
        first_lost{}, lost_listener{}, redactions{};
    bool invalid{};
    std::string json() const {
        return "{\"status\":" + quote(invalid ? "CAPTURE_INVALID" : "CAPTURE_COMPLETE") +
               ",\"callbacks_received\":" + std::to_string(callbacks) +
               ",\"records_captured\":" + std::to_string(records) + ",\"bytes_captured\":" + std::to_string(bytes) +
               ",\"queue_high_water_bytes\":" + std::to_string(high_water) +
               ",\"capture_overflow\":" + std::to_string(overflow) + ",\"write_failures\":" + std::to_string(writes) +
               ",\"descriptor_failures\":" + std::to_string(descriptors) +
               ",\"unknown_callbacks\":" + std::to_string(unknown) +
               ",\"first_lost_ordinal\":" + std::to_string(first_lost) +
               ",\"first_lost_listener\":" + std::to_string(lost_listener) +
               ",\"redaction_failures\":" + std::to_string(redactions) + "}";
    }
};
struct Writer {
    const Config& config;
    fs::path directory;
    int fd{-1};
    std::vector<std::byte> buffer;
    std::size_t used{}, queued_records{};
    Metrics metrics;
    std::uint64_t buffered_first{}, buffered_listener{};
    Writer(const Config& c, fs::path dir, std::size_t capacity)
        : config(c), directory(std::move(dir)), buffer(capacity) {
        if (!fs::create_directory(directory))
            throw std::runtime_error("output directory must be new");
        fs::permissions(directory, fs::perms::owner_all);
        fd = ::open((directory / "capture.part").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
            throw std::runtime_error("cannot create trace");
        const char magic[] = "P2CAP001";
        std::memcpy(buffer.data(), magic, 8);
        used = 8;
    }
    ~Writer() {
        if (fd >= 0)
            ::close(fd);
    }
    void invalidate(std::uint64_t listener, std::uint64_t ordinal) {
        metrics.invalid = true;
        if (!metrics.first_lost) {
            metrics.first_lost = buffered_first ? buffered_first : ordinal;
            metrics.lost_listener = buffered_first ? buffered_listener : listener;
        }
    }
    void put(std::uint64_t n, unsigned size) {
        for (unsigned i = 0; i < size; ++i) {
            buffer[used++] = std::byte(n & 255);
            n >>= 8;
        }
    }
    bool frame(std::uint32_t kind, std::uint32_t listener, std::uint64_t ordinal, std::uint64_t poll,
               std::uint64_t epoch, std::uint32_t type, std::uint32_t id, std::uint64_t index, std::int64_t rev,
               std::int64_t owner, std::uint64_t user, std::span<const std::byte> payload,
               std::span<const std::uint8_t> nulls = {}) {
        if (metrics.invalid)
            return false;
        if (config.sensitive(payload)) {
            ++metrics.redactions;
            invalidate(listener, ordinal);
            return false;
        }
        if (payload.size() + nulls.size() + 92 > buffer.size() - used) {
            ++metrics.overflow;
            invalidate(listener, ordinal);
            return false;
        }
        if (ordinal && !buffered_first) {
            buffered_first = ordinal;
            buffered_listener = listener;
        }
        put(88 + payload.size() + nulls.size(), 4);
        put(kind, 4);
        put(listener, 4);
        put(ordinal, 8);
        put(poll, 8);
        put(now(), 8);
        put(epoch, 8);
        put(type, 4);
        put(id, 4);
        put(index, 8);
        put(rev, 8);
        put(owner, 8);
        put(user, 8);
        put(nulls.size(), 4);
        put(payload.size(), 4);
        if (!payload.empty())
            std::memcpy(buffer.data() + used, payload.data(), payload.size());
        used += payload.size();
        if (!nulls.empty())
            std::memcpy(buffer.data() + used, nulls.data(), nulls.size());
        used += nulls.size();
        metrics.high_water = std::max<std::uint64_t>(metrics.high_water, used);
        ++queued_records;
        return true;
    }
    bool text(std::uint32_t kind, std::uint32_t listener, std::uint64_t ordinal, std::uint64_t poll,
              std::uint64_t epoch, const std::string& s) {
        const auto cleaned = kind == 2 ? s : config.clean(s);
        return frame(kind, listener, ordinal, poll, epoch, 0, 0, UINT64_MAX, 0, 0, 0,
                     std::as_bytes(std::span{cleaned.data(), cleaned.size()}));
    }
    bool flush() {
        if (metrics.invalid)
            return false;
        std::size_t offset = 0;
        while (offset < used) {
            const auto wrote = ::write(fd, buffer.data() + offset, used - offset);
            if (wrote < 0 && errno == EINTR)
                continue;
            if (wrote <= 0) {
                ++metrics.writes;
                invalidate(buffered_listener, buffered_first);
                return false;
            }
            offset += wrote;
            metrics.bytes += wrote;
        }
        metrics.records += queued_records;
        queued_records = 0;
        used = 0;
        buffered_first = 0;
        return true;
    }
    std::string finish() {
        flush();
        const auto prefix = metrics.invalid ? std::string{} : hash_file(directory / "capture.part");
        text(5, 0, 0, 0, 0, "{\"prefix_sha256\":" + quote(prefix) + ",\"metrics\":" + metrics.json() + "}");
        flush();
        if (fsync(fd)) {
            ++metrics.writes;
            invalidate(buffered_listener, buffered_first);
        }
        ::close(fd);
        fd = -1;
        std::string digest;
        if (!metrics.invalid) {
            fs::rename(directory / "capture.part", directory / "capture.bin");
            fs::permissions(directory / "capture.bin", fs::perms::owner_read);
            digest = hash_file(directory / "capture.bin");
            json_file(directory / "capture.bin.sha256", digest);
        }
        json_file(directory / "metrics.json", metrics.json());
        json_file(directory / "manifest.json",
                  "{\"format\":1,\"source_sha256\":" + quote(digest) +
                      ",\"status\":" + quote(metrics.invalid ? "CAPTURE_INVALID" : "CAPTURE_COMPLETE") + "}");
        return digest;
    }
};
struct Listener {
    Api& api;
    Writer& writer;
    std::uint32_t id;
    std::string url;
    void* handle{};
    std::uint64_t ordinal{}, epoch{}, poll{}, online_at{}, reopen_at{};
    std::uint32_t last_state{UINT32_MAX};
    bool online{}, attempted{}, reopened{}, expected_descriptor{};
    std::size_t table_count{};
    std::array<std::int64_t, 512> frontier{}, pending_frontier{};
    void status(const std::string& text) {
        writer.text(4, id, 0, poll, epoch, text);
    }
    void result(std::string_view operation, std::uint32_t code) {
        status("{\"operation\":" + quote(operation) + ",\"code\":" + std::to_string(code) +
               ",\"text\":" + quote(api.error_text(code)) + "}");
    }
    bool describe() {
        CgSchemeDesc* descriptor{};
        const auto code = api.scheme(handle, reinterpret_cast<void**>(&descriptor));
        if (code || !descriptor || descriptor->num_messages > 512) {
            result("getscheme", code);
            return false;
        }
        std::string s = "{\"scheme_type\":" + std::to_string(descriptor->scheme_type) +
                        ",\"features\":" + std::to_string(descriptor->features) + ",\"tables\":[";
        auto* message = descriptor->messages;
        bool snapshot = false, log = false, expected = true;
        table_count = descriptor->num_messages;
        const auto string = [](const char* p) {
            if (!p || strnlen(p, 4097) > 4096)
                throw std::runtime_error("descriptor string invalid");
            return quote(p);
        };
        for (std::size_t index = 0; index < descriptor->num_messages; ++index) {
            if (!message || message->num_fields > 4096 || message->size > 64 * 1024 * 1024)
                return false;
            if (index)
                s += ',';
            s += "{\"index\":" + std::to_string(index) + ",\"id\":" + std::to_string(message->id) +
                 ",\"name\":" + string(message->name) + ",\"size\":" + std::to_string(message->size) +
                 ",\"align\":" + std::to_string(message->align) + ",\"fields\":[";
            snapshot |= std::string_view(message->name) == (id == 0 ? "orders" : "multileg_orders");
            log |= std::string_view(message->name) == (id == 0 ? "orders_log" : "multileg_orders_log");
            const moex::plaza2::public_wire::Table* qualified = nullptr;
            for (const auto& table : moex::plaza2::public_wire::kTables)
                if (table.name == message->name)
                    qualified = &table;
            if (qualified && (qualified->size != message->size || qualified->fields.size() != message->num_fields))
                expected = false;
            auto* field = message->fields;
            for (std::size_t f = 0; f < message->num_fields; ++f) {
                if (!field || field->offset > message->size || field->size > message->size - field->offset)
                    return false;
                if (f)
                    s += ',';
                s += "{\"name\":" + string(field->name) + ",\"type\":" + string(field->type) +
                     ",\"size\":" + std::to_string(field->size) + ",\"offset\":" + std::to_string(field->offset) + "}";
                if (qualified && f < qualified->fields.size()) {
                    const auto& q = qualified->fields[f];
                    if (q.name != field->name || q.type != field->type || q.size != field->size ||
                        q.offset != field->offset)
                        expected = false;
                }
                if (s.size() > writer.buffer.size())
                    return false;
                field = field->next;
            }
            if (field)
                return false;
            s += "]}";
            message = message->next;
        }
        if (message)
            return false;
        s += "],\"outcome\":" +
             quote((expected_descriptor = snapshot && log && expected) ? "SUCCESS_NEGOTIATED"
                                                                       : "OPENED_BUT_UNEXPECTED_DESCRIPTOR") +
             "}";
        return writer.text(2, id, ordinal, poll, epoch, s);
    }
    static std::uint32_t callback(void*, void*, void* raw, void* data) noexcept {
        auto& l = *static_cast<Listener*>(data);
        auto& w = l.writer;
        ++l.ordinal;
        ++w.metrics.callbacks;
        try {
            const auto& m = *static_cast<CgMsg*>(raw);
            std::uint64_t index = UINT64_MAX, user = 0;
            std::int64_t rev = 0;
            std::uint32_t msgid = 0;
            std::span<const std::uint8_t> nulls;
            if (m.type == 0x120) {
                const auto& stream = *static_cast<CgMsgStreamData*>(raw);
                index = stream.msg_index;
                rev = stream.rev;
                msgid = stream.msg_id;
                user = stream.user_id;
                if (stream.num_nulls && !stream.nulls)
                    throw std::runtime_error("null map unavailable");
                nulls = {stream.nulls, stream.num_nulls};
            } else if (m.type == 0x110) {
                const auto& message = *static_cast<CgMsgData*>(raw);
                index = message.msg_index;
                msgid = message.msg_id;
                user = message.user_id;
            }
            if ((m.data_size && !m.data) || m.data_size > 64 * 1024 * 1024 || nulls.size() > 4096)
                throw std::runtime_error("invalid native payload");
            if (!w.frame(3, l.id, l.ordinal, l.poll, l.epoch, m.type, msgid, index, rev, m.owner_id, user,
                         {static_cast<const std::byte*>(m.data), m.data_size}, nulls))
                return 1;
            if (m.type == 0x100 && !l.describe()) {
                ++w.metrics.descriptors;
                w.invalidate(l.id, l.ordinal);
                return 1;
            }
            if (m.type == 0x200)
                l.pending_frontier = l.frontier;
            if (m.type == 0x120 && index < l.table_count)
                l.pending_frontier[index] = rev;
            if (m.type == 0x210)
                l.frontier = l.pending_frontier;
            if (m.type == 0x1110) {
                l.frontier = {};
                l.pending_frontier = {};
            }
            if (m.type == 0x1112) {
                l.online = true;
                l.online_at = now();
            }
            if (m.type == 0x101 || m.type == 0x1110)
                l.online = false;
            if (m.type != 0x100 && m.type != 0x101 && m.type != 0x110 && m.type != 0x120 && m.type != 0x200 &&
                m.type != 0x210 && m.type != 0x1110 && m.type != 0x1111 && m.type != 0x1112 && m.type != 0x1115)
                ++w.metrics.unknown;
            return 0;
        } catch (...) {
            ++w.metrics.descriptors;
            w.invalidate(l.id, l.ordinal);
            return 1;
        }
    }
    void create(void* connection) {
        attempted = true;
        ++epoch;
        last_state = UINT32_MAX;
        online = false;
        expected_descriptor = false;
        frontier = {};
        pending_frontier = {};
        auto code = api.lsn_new(connection, url.c_str(), &callback, this, &handle);
        result("listener_new", code);
        if (!code) {
            code = api.lsn_open(handle, nullptr);
            result("listener_open", code);
        }
        if (code)
            status("{\"outcome\":\"OPEN_REJECTED_WITH_EXACT_ERROR\"}");
    }
    void close() {
        online = false;
        if (handle) {
            result("listener_close", api.lsn_close(handle));
            result("listener_destroy", api.lsn_destroy(handle));
            handle = nullptr;
        }
    }
};
} // namespace
int main(int argc, char** argv) {
    try {
        fs::path config_path, output;
        std::uint64_t duration = 1000, reopen = 0, capacity = 4 * 1024 * 1024;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 == argc)
                throw std::runtime_error("expected option value");
            const std::string_view option = argv[i];
            if (option == "--config")
                config_path = argv[i + 1];
            else if (option == "--output")
                output = argv[i + 1];
            else if (option == "--duration-ms")
                duration = number(argv[i + 1]);
            else if (option == "--reopen-ms")
                reopen = number(argv[i + 1]);
            else if (option == "--buffer-bytes")
                capacity = number(argv[i + 1]);
            else
                throw std::runtime_error("unknown option");
        }
        if (config_path.empty() || output.empty() || !duration || duration > 3600000 || capacity < 512 ||
            capacity > 64 * 1024 * 1024 || reopen > duration)
            throw std::runtime_error("invalid capture arguments");
        static_assert(std::endian::native == std::endian::little);
        Config config(config_path);
        Api api(config.values.at("runtime"));
        Writer writer(config, output, capacity);
        std::string metadata =
            "{\"format\":1,\"endianness\":\"little\",\"pointer_bits\":" + std::to_string(sizeof(void*) * 8) +
            ",\"architecture\":" +
            quote(
#if defined(__aarch64__) || defined(__arm64__)
                "arm64"
#else
                "x86_64"
#endif
                ) +
            ",\"connector_commit\":" + quote(MOEX_SOURCE_GIT_SHA) +
            ",\"config_sha256\":" + quote(hash_file(config_path)) +
            ",\"executable_sha256\":" + quote(hash_file(fs::absolute(argv[0]))) +
            ",\"runtime_sha256\":" + quote(hash_file(config.values.at("runtime"))) +
            ",\"duration_ms\":" + std::to_string(duration) + ",\"reopen_ms\":" + std::to_string(reopen) +
            ",\"vendor_log\":\"redacted_error_pipe\",\"connection\":\"[REDACTED]\",\"sources\":{";
        bool comma = false;
        for (const char* key : {"env_file", "ordbook_scheme", "ordlog_scheme"})
            if (config.values.contains(key)) {
                if (comma)
                    metadata += ',';
                comma = true;
                metadata += quote(key) + ':' + quote(hash_file(config.values.at(key)));
            }
        metadata += "},\"listeners\":[";
        for (int id = 0; id < 2; ++id) {
            const char* key = id ? "multileg" : "regular";
            if (!config.values.contains(key))
                continue;
            if (id)
                metadata += ',';
            auto url = config.values.at(key);
            for (const char* filekey : {"ordbook_scheme", "ordlog_scheme"})
                if (config.values.contains(filekey)) {
                    const auto pos = url.find(config.values.at(filekey));
                    if (pos != std::string::npos)
                        url.replace(pos, config.values.at(filekey).size(), "[HASHED_SOURCE]");
                }
            metadata += "{\"id\":" + std::to_string(id) + ",\"url\":" + quote(config.clean(url)) + "}";
        }
        metadata += "]}";
        writer.text(1, 0, 0, 0, 0, metadata);
        writer.flush();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::signal(SIGXFSZ, SIG_IGN);
        Diagnostics diagnostics;
        const auto drain_diagnostics = [&](std::uint64_t poll) {
            if (diagnostics.overflow) {
                ++writer.metrics.overflow;
                writer.invalidate(0, 0);
            }
            for (const auto& [received, line] : diagnostics.take())
                writer.text(6, 0, 0, poll, 0,
                            "{\"sdk_error_text\":" + quote(config.clean(line)) +
                                ",\"diagnostic_monotonic_ns\":" + std::to_string(received) + "}");
        };
        const auto environment = config.values.at("env") + ";log=std;minloglevel=error";
        const auto env_code = api.env_open(environment.c_str());
        writer.text(4, 0, 0, 0, 0,
                    "{\"operation\":\"env_open\",\"code\":" + std::to_string(env_code) +
                        ",\"text\":" + quote(api.error_text(env_code)) + "}");
        void* connection = nullptr;
        std::array<Listener, 2> listeners{
            {{api, writer, 0, config.values.at("regular")},
             {api, writer, 1, config.values.contains("multileg") ? config.values.at("multileg") : ""}}};
        if (!env_code && !writer.metrics.invalid) {
            int major{}, minor{}, patch{};
            const auto version = api.version("cgate", &major, &minor, &patch);
            writer.text(4, 0, 0, 0, 0,
                        "{\"runtime_version_code\":" + std::to_string(version) + ",\"runtime_version\":" +
                            quote(std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(patch)) +
                            "}");
            auto code = api.conn_new(config.values.at("connection").c_str(), &connection);
            if (!code)
                code = api.conn_open(connection, nullptr);
            writer.text(4, 0, 0, 0, 0,
                        "{\"operation\":\"connection_open\",\"code\":" + std::to_string(code) +
                            ",\"text\":" + quote(api.error_text(code)) + "}");
            const auto deadline = now() + duration * 1000000;
            std::uint64_t poll = 0;
            std::uint32_t last = UINT32_MAX;
            while (!code && !stopped && now() < deadline && !writer.metrics.invalid) {
                ++poll;
                std::uint32_t state{};
                code = api.conn_state(connection, &state);
                if (code)
                    listeners[0].result("connection_state", code);
                if (state != last) {
                    writer.text(4, 0, 0, poll, 0, "{\"connection_state\":" + std::to_string(state) + "}");
                    last = state;
                }
                if (code || state == 1)
                    break;
                for (auto& l : listeners) {
                    l.poll = poll;
                    if (l.url.empty())
                        continue;
                    if (state == 3 && !l.attempted)
                        l.create(connection);
                    if (l.handle) {
                        std::uint32_t ls{};
                        const auto lc = api.lsn_state(l.handle, &ls);
                        if (lc)
                            l.result("listener_state", lc);
                        if (ls != l.last_state) {
                            l.status("{\"listener_state\":" + std::to_string(ls) + "}");
                            l.last_state = ls;
                        }
                        if (lc || ls == 1) {
                            l.online = false;
                            l.status("{\"outcome\":\"STATE_ERROR\",\"state_error\":true}");
                        }
                        if (!l.id && reopen && l.online && l.expected_descriptor && !l.reopened) {
                            l.close();
                            l.reopened = true;
                            l.reopen_at = now() + reopen * 1000000;
                        }
                    }
                    if (l.reopen_at && now() >= l.reopen_at) {
                        l.reopen_at = 0;
                        l.create(connection);
                    }
                }
                const auto processed = api.process(connection, 0, nullptr);
                if (processed && processed != 131075) {
                    listeners[0].result("connection_process", processed);
                    break;
                }
                drain_diagnostics(poll);
                if (processed == 131075 || writer.used >= std::min<std::size_t>(65536, writer.buffer.size() / 2))
                    writer.flush();
                if (processed == 131075)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        for (auto& l : listeners) {
            if (l.url.empty())
                continue;
            std::string summary = "{\"committed_callback_frontier\":[";
            for (std::size_t i = 0; i < l.table_count; ++i) {
                if (i)
                    summary += ',';
                summary += std::to_string(l.frontier[i]);
            }
            summary += "]}";
            l.status(summary);
            l.close();
        }
        if (connection) {
            api.conn_close(connection);
            api.conn_destroy(connection);
        }
        if (!env_code)
            api.env_close();
        diagnostics.stop();
        drain_diagnostics(0);
        writer.finish();
        std::cout << (writer.metrics.invalid ? "CAPTURE_INVALID" : "CAPTURE_COMPLETE (observations only)") << '\n';
        return writer.metrics.invalid ? 2 : 0;
    } catch (...) {
        std::cerr << "CAPTURE_INVALID: configuration, runtime or output failure (details suppressed for secrecy)\n";
        return 2;
    }
}
