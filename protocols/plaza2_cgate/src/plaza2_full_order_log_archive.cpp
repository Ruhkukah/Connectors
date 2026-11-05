#include "full_order_log_capture_format.hpp"
#include <algorithm>
#include <fstream>
#include <set>
#include <map>
#include "moex/plaza2/cgate/plaza2_full_order_log_oi.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_refdata.hpp"
#include <zstd.h>

namespace moex::plaza2::cgate::capture_format {
std::string read_pinned_refdata(const std::filesystem::path& directory, std::string_view sha256) {
    const auto path = directory / kFullOrderLogRefdataFilename;
    require(std::filesystem::is_regular_file(std::filesystem::symlink_status(path)),
            "capture REFDATA sidecar missing or not a regular file");
    const auto size = std::filesystem::file_size(path);
    require(size > 0 && size <= kFullOrderLogRefdataMaxBytes, "capture REFDATA sidecar size invalid");
    std::ifstream file(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    require(file.gcount() == static_cast<std::streamsize>(size) && file.peek() == EOF && !file.bad(),
            "capture REFDATA sidecar read failed");
    require(plaza2_sha256_hex(text) == sha256, "capture REFDATA sidecar SHA-256 mismatch");
    return text;
}

std::vector<Plaza2RawTableBinding> decode_schema(std::span<const std::byte> bytes) {
    const auto integer = [&]() {
        const auto result = load<std::uint32_t>(bytes);
        bytes = bytes.subspan(4);
        return result;
    };
    const auto string = [&]() {
        const auto length = integer();
        require(length <= 4096 && length <= bytes.size(), "capture schema string invalid");
        std::string result(reinterpret_cast<const char*>(bytes.data()), length);
        bytes = bytes.subspan(length);
        return result;
    };
    const auto count = integer();
    require(count > 0 && count <= 4, "capture invalid table count");
    std::vector<Plaza2RawTableBinding> result;
    std::set<std::uint32_t> indices;
    std::set<std::string> names;
    for (std::uint32_t i = 0; i < count; ++i) {
        Plaza2RawTableBinding t;
        t.index = integer();
        t.row_size = integer();
        t.name = string();
        require(indices.insert(t.index).second && names.insert(t.name).second, "capture duplicate table");
        const auto fields = integer();
        require(fields <= 1024, "capture too many fields");
        for (std::uint32_t j = 0; j < fields; ++j) {
            Plaza2RawFieldBinding f;
            f.index = integer();
            f.offset = integer();
            f.size = integer();
            f.name = string();
            f.type_token = string();
            require(f.offset <= t.row_size && f.size <= t.row_size - f.offset, "capture field outside row");
            t.fields.push_back(std::move(f));
        }
        result.push_back(std::move(t));
    }
    require(bytes.empty(), "capture schema trailing bytes");
    return result;
}
} // namespace moex::plaza2::cgate::capture_format
namespace moex::plaza2::cgate {
using namespace capture_format;
FullOrderLogArchiveSummary
read_full_order_log_capture(const std::filesystem::path& directory,
                            const std::function<void(const FullOrderLogCaptureRecord&)>& visitor,
                            bool allow_incomplete) {
    std::vector<std::filesystem::path> files;
    for (const auto& p : std::filesystem::directory_iterator(directory))
        if (p.path().extension() == ".folz") {
            require(p.is_regular_file() && !p.is_symlink(), "capture segment is not a regular file");
            files.push_back(p.path());
        }
    std::sort(files.begin(), files.end());
    require(!files.empty(), "capture contains no segments");
    FullOrderLogArchiveSummary result;
    std::map<std::uint32_t, std::vector<Plaza2RawTableBinding>> schemas;
    std::uint64_t identity{}, segment{}, sequence{}, source{};
    bool ended = false;
    const auto exact = [](std::ifstream& file, void* data, std::size_t bytes) {
        file.read(static_cast<char*>(data), bytes);
        require(static_cast<std::size_t>(file.gcount()) == bytes, "capture truncated");
    };
    for (const auto& path : files) {
        std::ifstream file(path, std::ios::binary);
        FileHeader header;
        try {
            exact(file, &header, sizeof header);
            require(std::memcmp(header.magic, "MOEXFOL2", 8) == 0 && header.version == 2 &&
                        header.block_limit >= 4096 && header.block_limit <= 4 * 1024 * 1024,
                    "capture header invalid");
            if (!segment)
                result.capture_identity = identity = header.identity;
            require(header.identity == identity && header.segment == ++segment,
                    "capture segment missing/reordered/foreign");
        } catch (const std::exception&) {
            if (!allow_incomplete || !sequence)
                throw;
            result.truncated = true;
            break;
        }
        result.stored_bytes += std::filesystem::file_size(path);
        while (file.peek() != EOF) {
            require(!ended, "capture data after clean end");
            ChunkHeader chunk;
            std::vector<std::byte> raw;
            try {
                exact(file, &chunk, sizeof chunk);
                require(chunk.raw_bytes && chunk.raw_bytes <= header.block_limit && chunk.compressed_bytes &&
                            chunk.compressed_bytes <= ZSTD_compressBound(header.block_limit) &&
                            chunk.first_sequence == sequence + 1 && chunk.records,
                        "capture chunk invalid");
                std::vector<std::byte> compressed(chunk.compressed_bytes);
                raw.resize(chunk.raw_bytes);
                exact(file, compressed.data(), compressed.size());
                const auto n = ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size());
                require(!ZSTD_isError(n) && n == raw.size() && checksum(raw) == chunk.checksum,
                        "capture chunk integrity failed");
                // Verify the ENTIRE chunk before exposing any record to replay/export.
                std::span<const std::byte> verify = raw;
                std::uint64_t count{}, next = sequence;
                while (!verify.empty()) {
                    const auto h = load<RecordHeader>(verify);
                    verify = verify.subspan(sizeof h);
                    const std::uint64_t size =
                        static_cast<std::uint64_t>(h.payload_bytes) + h.null_bytes + h.schema_bytes;
                    require(h.sequence == ++next && size <= verify.size(), "capture record framing invalid");
                    verify = verify.subspan(size);
                    require(h.kind != end_kind || verify.empty(), "capture footer misplaced");
                    ++count;
                }
                require(count == chunk.records, "capture chunk record count mismatch");
            } catch (const std::exception&) {
                if (!allow_incomplete || !sequence)
                    throw;
                result.truncated = true;
                break;
            }
            std::span<const std::byte> rest = raw;
            std::uint64_t count{};
            while (!rest.empty()) {
                const auto h = load<RecordHeader>(rest);
                rest = rest.subspan(sizeof h);
                require(h.sequence == ++sequence, "capture sequence gap/duplicate");
                ++count;
                const std::uint64_t size = static_cast<std::uint64_t>(h.payload_bytes) + h.null_bytes + h.schema_bytes;
                require(size <= rest.size(), "capture record size invalid");
                const auto payload = rest.first(h.payload_bytes);
                const auto nulls = rest.subspan(h.payload_bytes, h.null_bytes);
                const auto schema = rest.subspan(h.payload_bytes + h.null_bytes, h.schema_bytes);
                rest = rest.subspan(size);
                if (h.kind == manifest_kind) {
                    require(sequence == 1 && h.source_sequence == 0 && h.schema_bytes == 0 && h.null_bytes == 0,
                            "capture manifest misplaced");
                    const auto ids = load<std::uint32_t>(payload);
                    const auto base_size = 44 + ids * 4;
                    require(ids > 0 && ids <= 64 &&
                                (payload.size() == base_size || payload.size() == base_size + recovery_cursor_bytes),
                            "capture universe invalid");
                    if (payload.size() > base_size)
                        result.recovery_cursor = read_recovery_cursor(payload.subspan(base_size));
                    for (std::uint32_t i = 0; i < ids; ++i)
                        result.isins.push_back(load<std::int32_t>(payload, 4 + i * 4));
                    result.source_sha.assign(reinterpret_cast<const char*>(payload.data() + 4 + ids * 4), 40);
                    while (!result.source_sha.empty() && result.source_sha.back() == 0)
                        result.source_sha.pop_back();
                    require(std::is_sorted(result.isins.begin(), result.isins.end()) && result.isins.front() > 0 &&
                                std::adjacent_find(result.isins.begin(), result.isins.end()) == result.isins.end(),
                            "capture universe invalid");
                    continue;
                }
                require(!result.isins.empty(), "capture missing manifest");
                if (h.kind == end_kind) {
                    require(h.unsigned_value <= 2 && h.source_sequence >= source && rest.empty() && nulls.empty() &&
                                schema.empty(),
                            "capture ended after validation error or invalid footer");
                    if (!payload.empty()) {
                        result.refdata_sha256.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
                        require(result.refdata_sha256.size() == 64 &&
                                    std::ranges::all_of(
                                        result.refdata_sha256,
                                        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
                                "capture REFDATA pin invalid");
                    }
                    result.end_status = static_cast<FullOrderLogCaptureEndStatus>(h.unsigned_value);
                    result.host_status = h.signed_value;
                    ended = true;
                    result.end_record_present = true;
                    continue;
                }
                require(
                    (h.kind <= static_cast<std::uint32_t>(Plaza2ListenerEventKind::Timeout) || h.kind == error_kind) &&
                        (h.stream == static_cast<std::uint32_t>(kFullOrderLogStreamCode) ||
                         h.stream == static_cast<std::uint32_t>(generated::StreamCode::kFortsDealsRepl)) &&
                        h.source_sequence > source,
                    "capture event identity invalid");
                source = h.source_sequence;
                FullOrderLogCaptureRecord record{h.sequence, h.source_sequence, h.utc_ns, h.monotonic_ns, {}};
                auto& e = record.event;
                e.kind = h.kind == error_kind ? Plaza2ListenerEventKind::Close
                                              : static_cast<Plaza2ListenerEventKind>(h.kind);
                record.listener_error = h.kind == error_kind;
                auto& coverage = h.stream == static_cast<std::uint32_t>(kFullOrderLogStreamCode)
                                     ? result.fol_coverage
                                     : result.deals_coverage;
                if (record.listener_error) {
                    ++result.listener_errors;
                    ++coverage.listener_errors;
                } else {
                    coverage.opens += e.kind == Plaza2ListenerEventKind::Open;
                    coverage.onlines += e.kind == Plaza2ListenerEventKind::Online;
                    coverage.closes += e.kind == Plaza2ListenerEventKind::Close;
                    coverage.life_notifications += e.kind == Plaza2ListenerEventKind::LifeNum;
                    coverage.clear_deleted += e.kind == Plaza2ListenerEventKind::ClearDeleted;
                }
                e.stream_code = static_cast<generated::StreamCode>(h.stream);
                e.table_code = static_cast<generated::TableCode>(h.table_code);
                e.table_index = h.table_index;
                e.unsigned_value = h.unsigned_value;
                e.signed_value = h.signed_value;
                e.clear_deleted_flags = h.clear_flags;
                e.close_reason = record.listener_error ? static_cast<std::uint32_t>(h.unsigned_value) : h.close_reason;
                e.native_data_size = h.native_data_size;
                e.raw_payload = payload;
                e.raw_nulls = {reinterpret_cast<const std::uint8_t*>(nulls.data()), nulls.size()};
                auto& tables = schemas[h.stream];
                if (e.kind == Plaza2ListenerEventKind::Open) {
                    tables = decode_schema(schema);
                    e.raw_tables = tables;
                } else
                    require(schema.empty(), "capture schema outside OPEN");
                if (e.kind == Plaza2ListenerEventKind::StreamData || e.kind == Plaza2ListenerEventKind::ClearDeleted) {
                    const auto t = std::find_if(tables.begin(), tables.end(),
                                                [&](const auto& v) { return v.index == e.table_index; });
                    require(t != tables.end(), "capture unknown table index");
                    e.raw_table = &*t;
                }
                if (e.kind == Plaza2ListenerEventKind::ReplState)
                    e.text_value = {reinterpret_cast<const char*>(payload.data()), payload.size()};
                ++result.records;
                if (e.kind == Plaza2ListenerEventKind::StreamData)
                    ++result.rows;
                if (e.kind == Plaza2ListenerEventKind::TransactionCommit)
                    ++result.commits;
                if (e.kind == Plaza2ListenerEventKind::StreamData &&
                    e.stream_code == generated::StreamCode::kFortsDealsRepl)
                    ++result.open_interest_rows;
                visitor(record);
            }
            require(count == chunk.records, "capture chunk record count mismatch");
            result.last_good_sequence = sequence;
        }
        if (result.truncated)
            break;
        require(!file.bad(), "capture read failed");
    }
    const auto stop_path = directory / "stop-status.folend";
    if (std::filesystem::exists(stop_path)) {
        require(!std::filesystem::is_symlink(stop_path) && std::filesystem::is_regular_file(stop_path),
                "capture stop record is not a regular file");
        std::ifstream stop(stop_path, std::ios::binary);
        StopRecord record;
        stop.read(reinterpret_cast<char*>(&record), sizeof record);
        const bool verified =
            stop.gcount() == sizeof record && stop.peek() == EOF && std::memcmp(record.magic, "MOEXEND2", 8) == 0 &&
            record.identity == identity && record.status <= 2 &&
            record.checksum == checksum(std::as_bytes(std::span(&record, 1)).first(offsetof(StopRecord, checksum)));
        if (verified) {
            result.end_record_present = true;
            require(!ended || (result.end_status == static_cast<FullOrderLogCaptureEndStatus>(record.status) &&
                               result.host_status == static_cast<int>(record.host_status)),
                    "capture conflicting end records");
            result.end_status = static_cast<FullOrderLogCaptureEndStatus>(record.status);
            result.host_status = static_cast<int>(record.host_status);
        } else {
            require(allow_incomplete, "capture missing/torn stop record");
            result.truncated = true;
        }
    }
    if (!result.refdata_sha256.empty()) {
        const auto rows = parse_full_order_log_refdata(read_pinned_refdata(directory, result.refdata_sha256));
        for (const auto& row : rows)
            require(std::ranges::binary_search(result.isins, row.isin_id) && row.effective_after_sequence > 0 &&
                        row.effective_after_sequence < result.last_good_sequence,
                    "capture REFDATA sidecar references an invalid instrument or capture sequence");
    } else if (ended) {
        require(!std::filesystem::exists(std::filesystem::symlink_status(directory / kFullOrderLogRefdataFilename)),
                "capture has an unpinned REFDATA sidecar");
    }
    result.truncated = result.truncated || !ended || result.listener_errors ||
                       result.end_status != FullOrderLogCaptureEndStatus::Clean;
    require(allow_incomplete || !result.truncated,
            "capture incomplete: missing clean end, nonzero status or listener error");
    return result;
}
FullOrderLogArchiveSummary replay_full_order_log_capture(const std::filesystem::path& directory,
                                                         bool allow_incomplete) {
    std::unique_ptr<Plaza2FullOrderLog> book;
    std::uint64_t valid{}, orders{};
    // Universe comes from the manifest; load it with the first OPEN's first pass.
    auto manifest = read_full_order_log_capture(directory, [](const auto&) {}, allow_incomplete);
    book = std::make_unique<Plaza2FullOrderLog>(manifest.isins);
    FullOrderLogOpenInterest oi(manifest.isins);
    std::uint64_t oi_updates{};
    oi.on_update = [&](const auto&) { ++oi_updates; };
    book->on_commit = [&](const auto& b) {
        ++valid;
        orders = b.order_count();
    };
    auto result = read_full_order_log_capture(
        directory,
        [&](const auto& record) {
            if (const auto error =
                    (record.event.stream_code == kFullOrderLogStreamCode ? book->on_plaza2_listener_event(record.event)
                                                                         : oi.on_plaza2_listener_event(record.event));
                error)
                throw std::runtime_error(error.message);
        },
        allow_incomplete);
    result.open_interest_updates = oi_updates;
    result.valid_commits = valid;
    result.last_valid_order_count = orders;
    return result;
}
} // namespace moex::plaza2::cgate
