#pragma once
#include "moex/plaza2/cgate/plaza2_full_order_log_capture.hpp"
#include <bit>
#include <unistd.h>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace moex::plaza2::cgate::capture_format {
static_assert(std::endian::native == std::endian::little);
inline constexpr std::uint32_t manifest_kind = 0xfffffffd, end_kind = 0xfffffffe, error_kind = 0xffffffff;
inline constexpr std::string_view recovery_cursor_magic = "ORDREC01";
inline constexpr std::size_t recovery_cursor_bytes = 44;
struct FileHeader {
    char magic[8]{'M', 'O', 'E', 'X', 'F', 'O', 'L', '2'};
    std::uint32_t version{2}, block_limit{};
    std::uint64_t identity{}, segment{};
};
struct RecordHeader {
    std::uint64_t sequence{}, source_sequence{}, utc_ns{}, monotonic_ns{}, unsigned_value{};
    std::int64_t signed_value{};
    std::uint32_t kind{}, stream{}, table_code{}, table_index{}, clear_flags{}, close_reason{};
    std::uint32_t payload_bytes{}, null_bytes{}, schema_bytes{}, native_data_size{};
};
struct StopRecord {
    char magic[8]{'M', 'O', 'E', 'X', 'E', 'N', 'D', '2'};
    std::uint64_t identity{}, status{}, host_status{}, checksum{};
};
static_assert(sizeof(StopRecord) == 40);
struct ChunkHeader {
    std::uint32_t raw_bytes{}, compressed_bytes{};
    std::uint64_t checksum{}, records{}, first_sequence{};
};
static_assert(sizeof(FileHeader) == 32 && sizeof(RecordHeader) == 88 && sizeof(ChunkHeader) == 32);
inline void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T load(std::span<const std::byte> bytes, std::size_t offset = 0) {
    require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "capture field truncated");
    T result;
    std::memcpy(&result, bytes.data() + offset, sizeof(result));
    return result;
}
template <class T> void append(std::vector<std::byte>& bytes, const T& value) {
    const auto at = bytes.size();
    bytes.resize(at + sizeof(value));
    std::memcpy(bytes.data() + at, &value, sizeof(value));
}
inline void append_bytes(std::vector<std::byte>& bytes, const void* data, std::size_t size) {
    const auto at = bytes.size();
    bytes.resize(at + size);
    if (size)
        std::memcpy(bytes.data() + at, data, size);
}
inline bool valid_recovery_cursor(const FullOrderLogRecoveryCursor& cursor) {
    return cursor.original_identity && cursor.reopen_sequence && cursor.life_number &&
           cursor.life_number <= UINT32_MAX && cursor.revision > 0 && cursor.matching_id > 0 &&
           cursor.matching_id <= INT8_MAX;
}
inline void append_recovery_cursor(std::vector<std::byte>& bytes, const FullOrderLogRecoveryCursor& cursor) {
    require(valid_recovery_cursor(cursor), "invalid ORDLOG recovery cursor");
    append_bytes(bytes, recovery_cursor_magic.data(), recovery_cursor_magic.size());
    append(bytes, cursor.original_identity);
    append(bytes, cursor.reopen_sequence);
    append(bytes, cursor.life_number);
    append(bytes, cursor.revision);
    append(bytes, cursor.matching_id);
}
inline FullOrderLogRecoveryCursor read_recovery_cursor(std::span<const std::byte> bytes) {
    require(bytes.size() == recovery_cursor_bytes &&
                std::memcmp(bytes.data(), recovery_cursor_magic.data(), recovery_cursor_magic.size()) == 0,
            "invalid ORDLOG recovery cursor manifest");
    FullOrderLogRecoveryCursor cursor{load<std::uint64_t>(bytes, 8), load<std::uint64_t>(bytes, 16),
                                      load<std::uint64_t>(bytes, 24), load<std::int64_t>(bytes, 32),
                                      load<std::int32_t>(bytes, 40)};
    require(valid_recovery_cursor(cursor), "invalid ORDLOG recovery cursor");
    return cursor;
}
inline bool valid_composite_open(std::string_view value) {
    constexpr std::string_view ordlog = "p2ordbook://FORTS_ORDLOG_REPL", ordbook = "snapshot=FORTS_ORDBOOK_REPL";
    const auto split = value.find(';');
    if (split == value.npos || !value.starts_with(ordlog) || split < ordlog.size())
        return false;
    const auto matching = value.substr(ordlog.size(), split - ordlog.size());
    if (!matching.empty()) {
        if (!matching.starts_with("_MATCH") || matching.size() < 7 || matching.size() > 9 || matching[6] < '1' ||
            matching[6] > '9' || matching.find_first_not_of("0123456789", 6) != value.npos)
            return false;
    }
    const auto snapshot = value.substr(split + 1);
    return snapshot.starts_with(ordbook) && snapshot.substr(ordbook.size()) == matching;
}
inline std::uint64_t checksum(std::span<const std::byte> bytes) {
    std::uint64_t result = 14695981039346656037ULL;
    for (auto b : bytes)
        result = (result ^ std::to_integer<unsigned char>(b)) * 1099511628211ULL;
    return result;
}
inline int sync_data(int fd) {
#ifdef __APPLE__
    return ::fsync(fd);
#else
    return ::fdatasync(fd);
#endif
}
inline std::uint64_t utc_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
inline std::uint64_t mono_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
inline std::vector<std::byte> encode_schema(std::span<const Plaza2RawTableBinding> tables) {
    std::vector<std::byte> out;
    append(out, static_cast<std::uint32_t>(tables.size()));
    const auto string = [&](const std::string& value) {
        require(value.size() <= 4096, "capture schema name too long");
        append(out, static_cast<std::uint32_t>(value.size()));
        append_bytes(out, value.data(), value.size());
    };
    for (const auto& t : tables) {
        append(out, static_cast<std::uint32_t>(t.index));
        append(out, static_cast<std::uint32_t>(t.row_size));
        string(t.name);
        append(out, static_cast<std::uint32_t>(t.fields.size()));
        for (const auto& f : t.fields) {
            append(out, static_cast<std::uint32_t>(f.index));
            append(out, static_cast<std::uint32_t>(f.offset));
            append(out, static_cast<std::uint32_t>(f.size));
            string(f.name);
            string(f.type_token);
        }
    }
    return out;
}
std::vector<Plaza2RawTableBinding> decode_schema(std::span<const std::byte> bytes);
std::string read_pinned_refdata(const std::filesystem::path& directory, std::string_view sha256);
} // namespace moex::plaza2::cgate::capture_format
