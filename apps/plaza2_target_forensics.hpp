#pragma once

#include "moex/plaza2/cgate/plaza2_runtime.hpp"
#include <iomanip>
#include <chrono>
#include <optional>
#include <sstream>

// Opt-in target-only evidence. No transport calls, price policy, or disk I/O in callbacks.
class Plaza2TargetForensics {
    using Row = moex::plaza2::cgate::Plaza2ForensicRow;
    using Event = moex::plaza2::cgate::Plaza2ListenerEvent;
    using Kind = moex::plaza2::cgate::Plaza2ListenerEventKind;
    struct Captured {
        Row row;
        std::uint64_t life{};
        std::int64_t revision{};
        std::int64_t commit_utc_ns{}, commit_monotonic_ns{};
    };
    std::vector<Captured> pending_, committed_;
    std::uint64_t life_{};
    std::int64_t last_revision_{-1};
    bool transaction_{}, identity_verified_{};
    static std::string quoted(std::string_view value) {
        std::string result = "\"";
        constexpr char hex[] = "0123456789abcdef";
        for (const unsigned char ch : value) {
            if (ch == '"' || ch == '\\') {
                result += '\\';
                result += static_cast<char>(ch);
            } else if (ch < 32) {
                result += "\\u00";
                result += hex[ch >> 4];
                result += hex[ch & 15];
            } else
                result += static_cast<char>(ch);
        }
        return result + '"';
    }

    static std::string value(const Row& row, std::string_view name) {
        for (const auto& f : row.fields)
            if (f.name == name)
                return f.independent_value;
        return {};
    }
    static std::string hex(std::span<const std::byte> bytes) {
        constexpr char digits[] = "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (auto b : bytes) {
            auto c = std::to_integer<unsigned>(b);
            out += digits[c >> 4];
            out += digits[c & 15];
        }
        return out;
    }

  public:
    std::int64_t isin{}, session{};
    std::string expected_symbol;
    bool failed{};
    bool wants(const Event& e) const noexcept {
        using F = moex::plaza2::generated::FieldCode;
        if (isin <= 0 || e.table_code != moex::plaza2::generated::TableCode::kFortsRefdataReplFutSessContents)
            return false;
        std::int64_t i{}, s{}, rev{-1};
        for (const auto& f : e.fields) {
            if (f.field_code == F::kFortsRefdataReplFutSessContentsIsinId)
                i = f.signed_value;
            if (f.field_code == F::kFortsRefdataReplFutSessContentsSessId)
                s = f.signed_value;
            if (f.field_code == F::kFortsRefdataReplFutSessContentsReplRev)
                rev = f.signed_value;
        }
        return i == isin && s == session && rev != last_revision_;
    }
    void capture(Row row) noexcept {
        try {
            if (!transaction_ || pending_.size() + committed_.size() >= 16) {
                failed = true;
                return;
            }
            identity_verified_ = false;
            const auto rev = std::stoll(value(row, "replRev"));
            for (const auto& field : row.fields)
                if (!field.equal)
                    failed = true;
            if (value(row, "isin_id") != std::to_string(isin) || value(row, "sess_id") != std::to_string(session))
                failed = true;
            for (const auto& old : pending_)
                if (old.life == life_ && old.revision == rev)
                    return;
            pending_.push_back({std::move(row), life_, rev});
        } catch (...) {
            failed = true;
        }
    }
    void observe(const Event& e) noexcept {
        if (e.stream_code != moex::plaza2::generated::StreamCode::kFortsRefdataRepl)
            return;
        try {
            if (e.kind == Kind::LifeNum || e.kind == Kind::Close || e.kind == Kind::Open) {
                pending_.clear();
                identity_verified_ = false;
                transaction_ = false;
                last_revision_ = -1;
                if (e.kind == Kind::LifeNum)
                    life_ = e.unsigned_value;
            } else if (e.kind == Kind::ClearDeleted &&
                       e.table_code == moex::plaza2::generated::TableCode::kFortsRefdataReplFutSessContents) {
                identity_verified_ = false;
            } else if (e.kind == Kind::TransactionBegin) {
                if (transaction_)
                    failed = true;
                pending_.clear();
                transaction_ = true;
            } else if (e.kind == Kind::TransactionCommit) {
                if (!transaction_) {
                    failed = true;
                    return;
                }
                for (auto& row : pending_) {
                    last_revision_ = row.revision;
                    row.commit_utc_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count();
                    row.commit_monotonic_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::chrono::steady_clock::now().time_since_epoch())
                                                  .count();
                    committed_.push_back(std::move(row));
                }
                pending_.clear();
                transaction_ = false;
            }
        } catch (...) {
            failed = true;
        }
    }
    bool identity_verified() const noexcept {
        return identity_verified_ && !failed && !transaction_;
    }
    bool has_committed() const noexcept {
        return !committed_.empty();
    }
    std::string drain(bool aggr_target_present) {
        std::ostringstream out;
        for (const auto& captured : committed_) {
            const auto& r = captured.row;
            const bool identity =
                value(r, "isin_id") == std::to_string(isin) && value(r, "sess_id") == std::to_string(session);
            const bool symbol = !expected_symbol.empty() &&
                                (value(r, "isin") == expected_symbol || value(r, "short_isin") == expected_symbol);
            if (captured.life == life_ && captured.revision == last_revision_)
                identity_verified_ = identity && symbol && aggr_target_present;
            out << "{\"stream\":" << static_cast<unsigned>(r.stream_code)
                << ",\"table\":" << static_cast<unsigned>(r.table_code) << ",\"table_index\":" << r.table_index
                << ",\"message_name\":" << quoted(r.message_name) << ",\"message_size\":" << r.message_size
                << ",\"lifenum\":" << captured.life << ",\"repl_rev\":" << captured.revision
                << ",\"commit_utc_ns\":" << captured.commit_utc_ns
                << ",\"commit_monotonic_ns\":" << captured.commit_monotonic_ns
                << ",\"numeric_identity_exact\":" << identity << ",\"symbol_identity_exact\":" << symbol
                << ",\"aggr_target_present\":" << aggr_target_present << ",\"payload_hex\":" << quoted(hex(r.payload))
                << ",\"null_map_hex\":" << quoted(hex(std::as_bytes(std::span(r.nulls)))) << ",\"fields\":[";
            bool comma = false;
            for (const auto& f : r.fields) {
                if (comma)
                    out << ',';
                comma = true;
                out << "{\"name\":" << quoted(f.name) << ",\"type\":" << quoted(f.type) << ",\"offset\":" << f.offset
                    << ",\"size\":" << f.size << ",\"ordinal\":" << f.ordinal << ",\"is_null\":" << f.is_null
                    << ",\"generic\":" << quoted(f.generic_value) << ",\"independent\":" << quoted(f.independent_value)
                    << ",\"conversion_result\":" << f.conversion_result << ",\"equal\":" << f.equal
                    << ",\"raw_hex\":" << quoted(hex(std::span(r.payload).subspan(f.offset, f.size))) << '}';
            }
            out << "]}\n";
        }
        committed_.clear();
        return out.str();
    }
};
