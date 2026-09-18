#pragma once

#include "moex/plaza2/cgate/plaza2_runtime.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"

#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace moex::plaza2::observer {
using namespace cgate;

// Runtime has already converted exchange fixed strings exactly once.
inline std::string quote(std::string_view utf8) {
    return '"' + text::json_escape_utf8(utf8) + '"';
}

inline std::string diagnostic(std::string_view message) {
    // Do not expose settings-bearing diagnostics, regardless of embedded values.
    if (const char* secret = std::getenv("MOEX_PLAZA2_CGATE_SOFTWARE_KEY");
        secret && *secret && message.find(secret) != std::string_view::npos)
        return "[redacted secret-bearing diagnostic]";
    std::string normalized(message);
    for (auto& c : normalized)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto marker : {"key", "password", "credential", "p2tcp://", "ini="})
        if (normalized.find(marker) != std::string_view::npos)
            return "[redacted settings-bearing diagnostic]";
    return std::string(message.substr(0, 2048));
}

inline std::string hex_bytes(std::span<const std::byte> bytes) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto byte : bytes) {
        const auto c = std::to_integer<unsigned>(byte);
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}

class Journal {
  public:
    static constexpr std::size_t buffer_limit = 64 * 1024;
    static constexpr std::size_t record_limit = 1024 * 1024;
    static constexpr std::uint64_t hash_seed = 14695981039346656037ULL;
    enum class Durability { Buffered, Durable };
    struct Metrics {
        std::uint64_t bytes{}, write_calls{}, sync_calls{}, max_write_ns{}, max_sync_ns{};
        std::size_t peak_buffer_bytes{};
    };
    struct Stamp {
        std::int64_t wall, monotonic;
        static Stamp now() {
            return {std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count(),
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count()};
        }
    };
    explicit Journal(const std::filesystem::path& path) {
        buffer_.reserve(buffer_limit);
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd_ < 0)
            throw std::runtime_error("cannot create exclusive observer journal");
        const int parent = ::open(path.parent_path().empty() ? "." : path.parent_path().c_str(), O_RDONLY);
        if (parent < 0 || ::fsync(parent) != 0) {
            if (parent >= 0)
                ::close(parent);
            ::close(fd_);
            throw std::runtime_error("cannot persist journal directory");
        }
        ::close(parent);
    }
    ~Journal() {
        ::close(fd_);
    }
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    void append(std::string_view body, Stamp stamp = Stamp::now(), Durability durability = Durability::Buffered,
                std::uint64_t* row_hash = nullptr) {
        if (body.size() > record_limit - 256 || closed_)
            throw std::runtime_error("observer journal record exceeds bound or journal closed");
        std::string line =
            "{\"seq\":" + std::to_string(++sequence_) + ",\"receive_unix_ns\":" + std::to_string(stamp.wall) +
            ",\"receive_steady_ns\":" + std::to_string(stamp.monotonic) + "," + std::string(body) + "}\n";
        if (row_hash)
            for (const unsigned char byte : line) {
                *row_hash ^= byte;
                *row_hash *= 1099511628211ULL;
            }
        std::string_view remaining(line);
        while (!remaining.empty()) {
            const auto count = std::min(buffer_limit - buffer_.size(), remaining.size());
            buffer_.append(remaining.substr(0, count));
            remaining.remove_prefix(count);
            metrics_.peak_buffer_bytes = std::max(metrics_.peak_buffer_bytes, buffer_.size());
            if (buffer_.size() == buffer_limit)
                flush();
        }
        if (durability == Durability::Durable)
            sync();
    }
    void append_durable(std::string_view body, Stamp stamp = Stamp::now()) {
        append(body, stamp, Durability::Durable);
    }
    void sync() {
        flush();
        const auto start = Stamp::now().monotonic;
        ++metrics_.sync_calls;
        if (::fsync(fd_) != 0)
            throw std::runtime_error("observer evidence fsync failed");
        metrics_.max_sync_ns =
            std::max(metrics_.max_sync_ns, static_cast<std::uint64_t>(Stamp::now().monotonic - start));
    }
    void finish() {
        if (closed_)
            return;
        append_durable("\"kind\":\"clean_shutdown\"");
        closed_ = true;
    }
    [[nodiscard]] const Metrics& metrics() const noexcept {
        return metrics_;
    }

  private:
    void flush() {
        std::size_t done = 0;
        while (done < buffer_.size()) {
            const auto start = Stamp::now().monotonic;
            ++metrics_.write_calls;
            const auto n = ::write(fd_, buffer_.data() + done, buffer_.size() - done);
            metrics_.max_write_ns =
                std::max(metrics_.max_write_ns, static_cast<std::uint64_t>(Stamp::now().monotonic - start));
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                throw std::runtime_error("observer evidence write failed");
            done += static_cast<std::size_t>(n);
            metrics_.bytes += static_cast<std::uint64_t>(n);
        }
        buffer_.clear();
    }
    int fd_{-1};
    std::uint64_t sequence_{0};
    std::string buffer_;
    Metrics metrics_;
    bool closed_{false};
};

class Handler final : public Plaza2ListenerEventHandler {
  public:
    Handler(Journal& journal, generated::StreamCode stream, std::uint64_t generation)
        : journal_(journal), stream_(stream), generation_(generation) {}

    void on_plaza2_listener_error(const Plaza2Error& error) noexcept override {
        failed = true;
        try {
            journal_.append_durable(
                prefix() + "\"kind\":\"callback_error\",\"code\":" + std::to_string(static_cast<unsigned>(error.code)) +
                ",\"runtime_code\":" + std::to_string(error.runtime_code) +
                ",\"message\":" + quote(diagnostic(error.message)));
        } catch (...) {
        }
    }

    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override {
        try {
            accept(event);
            return {};
        } catch (...) {
            failed = true;
            return {Plaza2ErrorCode::CallbackFailed, 0, "observer journal or transaction failure"};
        }
    }

    bool failed{false};
    bool online{false};
    bool closed{false};
    bool recovery_requested{false};

  private:
    std::string prefix() const {
        return "\"stream\":" + quote(generated::FindStreamByCode(stream_)->stream_name) +
               ",\"generation\":" + std::to_string(generation_) + ",";
    }

    void accept(const Plaza2ListenerEvent& event) {
        using K = Plaza2ListenerEventKind;
        const auto received = Journal::Stamp::now();
        const bool aggr = stream_ == generated::StreamCode::kFortsAggrRepl;
        constexpr std::array names = {"open",   "close",   "transaction_begin", "transaction_commit", "row",
                                      "online", "lifenum", "clear_deleted",     "replstate",          "timeout"};
        if (event.kind == K::Timeout)
            return;
        if (event.kind == K::Open) {
            online = false;
            closed = false;
            transaction_ = 0;
            pending_begin_.clear();
        }
        if (event.kind == K::TransactionBegin) {
            if (transaction_ != 0)
                throw std::runtime_error("nested transaction");
            transaction_ = ++next_transaction_;
            invalidated_ = false;
            rows_ = 0;
            relevant_rows_ = 0;
            row_hash_ = Journal::hash_seed;
        }
        if (event.kind == K::Close || event.kind == K::LifeNum || event.kind == K::ClearDeleted) {
            invalidated_ = true;
            if (event.kind != K::ClearDeleted)
                online = false;
        }
        if (event.kind == K::LifeNum) {
            recovery_requested = recovery_requested || (lifenum_ != 0 && lifenum_ != event.unsigned_value);
            lifenum_ = event.unsigned_value;
        }
        if (event.kind == K::Close)
            closed = true;
        if (event.kind == K::StreamData) {
            if (transaction_ == 0)
                throw std::runtime_error("observer row outside transaction");
            ++rows_;
            // AGGR book traffic is intentionally excluded; retain every sys_event.
            if (aggr && event.table_code != generated::TableCode::kFortsAggrReplSysEvents)
                return;
            ++relevant_rows_;
            if (!pending_begin_.empty()) {
                journal_.append(pending_begin_, pending_stamp_);
                pending_begin_.clear();
            }
        }
        if (aggr && event.kind == K::TransactionCommit && relevant_rows_ == 0) {
            transaction_ = 0;
            pending_begin_.clear();
            return;
        }
        std::ostringstream out;
        out << prefix() << "\"kind\":" << quote(names.at(static_cast<unsigned>(event.kind)))
            << ",\"transaction_id\":" << transaction_ << ",\"online_before\":" << (online ? "true" : "false")
            << ",\"lifenum\":" << lifenum_;
        if (event.kind == K::StreamData) {
            const auto* table = generated::FindTableByCode(event.table_code);
            out << ",\"table\":" << quote(table ? table->table_name : "unknown")
                << ",\"transaction_row_index\":" << rows_ << ",\"fields\":{";
            bool first = true;
            for (const auto& field : event.fields) {
                if (!first)
                    out << ',';
                first = false;
                const auto* desc = generated::FindFieldByCode(field.field_code);
                out << quote(desc ? desc->field_name : "unknown") << ":{\"kind\":" << static_cast<unsigned>(field.kind)
                    << ",\"value\":";
                if (field.kind == Plaza2DecodedValueKind::SignedInteger)
                    out << field.signed_value;
                else if (field.kind == Plaza2DecodedValueKind::UnsignedInteger ||
                         field.kind == Plaza2DecodedValueKind::Timestamp)
                    out << field.unsigned_value;
                else
                    out << quote(field.text_value);
                out << ",\"raw_hex\":" << quote(hex_bytes(field.raw_value));
                if (field.kind == Plaza2DecodedValueKind::Timestamp && field.raw_value.size() == 10) {
                    const auto byte = [&](std::size_t i) { return std::to_integer<unsigned>(field.raw_value[i]); };
                    out << ",\"p2time_components\":{\"year\":" << (byte(0) | byte(1) << 8) << ",\"month\":" << byte(2)
                        << ",\"day\":" << byte(3) << ",\"hour\":" << byte(4) << ",\"minute\":" << byte(5)
                        << ",\"second\":" << byte(6) << ",\"millisecond\":" << (byte(8) | byte(9) << 8) << '}'
                        << ",\"decoded_time_policy\":\"timegm_calendar_seconds_timezone_unconfirmed\"";
                }
                out << '}';
            }
            out << "},\"raw_payload_hex\":" << quote(hex_bytes(event.raw_payload));
        } else {
            out << ",\"unsigned_value\":" << event.unsigned_value << ",\"signed_value\":" << event.signed_value
                << ",\"table_code\":" << static_cast<std::uint32_t>(event.table_code)
                << ",\"table_index\":" << event.table_index << ",\"clear_deleted_flags\":" << event.clear_deleted_flags
                << ",\"close_reason\":" << event.close_reason << ",\"text\":" << quote(event.text_value);
        }
        if (event.kind == K::TransactionCommit) {
            out << ",\"transaction_committed\":" << (transaction_ != 0 && !invalidated_ ? "true" : "false")
                << ",\"source_row_count\":" << rows_ << ",\"recorded_row_count\":" << relevant_rows_
                << ",\"recorded_row_fnv1a64\":\"" << row_hash_ << '"';
        }
        if (aggr && event.kind == K::TransactionBegin) {
            pending_begin_ = out.str() + ",\"deferred_begin\":true";
            pending_stamp_ = received;
            return;
        }
        const bool buffered =
            event.kind == K::StreamData || event.kind == K::TransactionBegin || event.kind == K::ReplState;
        journal_.append(out.str(), received, buffered ? Journal::Durability::Buffered : Journal::Durability::Durable,
                        event.kind == K::StreamData ? &row_hash_ : nullptr);
        if (event.kind == K::Online)
            online = true;
        if (event.kind == K::TransactionCommit || event.kind == K::Close || event.kind == K::LifeNum) {
            transaction_ = 0;
            pending_begin_.clear();
        }
    }

    Journal& journal_;
    generated::StreamCode stream_;
    std::uint64_t generation_, next_transaction_{0}, transaction_{0}, lifenum_{0}, rows_{0};
    bool invalidated_{false};
    std::uint64_t relevant_rows_{0};
    std::uint64_t row_hash_{Journal::hash_seed};
    std::string pending_begin_;
    Journal::Stamp pending_stamp_{};
};
} // namespace moex::plaza2::observer
