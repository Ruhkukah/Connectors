#include "full_order_log_capture_format.hpp"
#include <algorithm>
#include <cstdio>
#include <atomic>
#include <array>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>
#include <zstd.h>

namespace moex::plaza2::cgate {
using namespace capture_format;
struct FullOrderLogCapture::Impl {
    struct Block {
        std::vector<std::byte> bytes;
        std::uint64_t count{}, first{};
    };
    FullOrderLogCaptureOptions options;
    Plaza2ListenerEventHandler& downstream;
    std::vector<std::int32_t> isins;
    std::vector<Block> queue;
    std::atomic<std::uint64_t> head{}, tail{}, stored{}, syncs{}, sync_max{};
    std::atomic_bool closing{}, failed{};
    std::mutex error_mutex;
    std::string error;
    std::thread worker;
    std::uint64_t sequence{}, source_sequence{}, filtered{}, logical{}, queue_peak{}, last_flush{}, identity{};
    int directory_fd{-1}, fd{-1};
    std::uint64_t segment{}, segment_size{};
    bool finished{}, validation_failed{};
    Impl(FullOrderLogCaptureOptions o, std::span<const std::int32_t> ids, Plaza2ListenerEventHandler& d)
        : options(std::move(o)), downstream(d), isins(ids.begin(), ids.end()) {
        require(!isins.empty() && isins.size() <= 64, "capture requires 1..64 selected instruments");
        std::sort(isins.begin(), isins.end());
        require(isins.front() > 0 && std::adjacent_find(isins.begin(), isins.end()) == isins.end(),
                "invalid capture instruments");
        require(options.block_bytes >= 4096 && options.block_bytes <= 4 * 1024 * 1024 && options.queue_blocks >= 2 &&
                    options.queue_blocks <= 64 && options.segment_bytes >= options.block_bytes * 2 &&
                    options.segment_bytes <= 1ULL << 30 && options.max_bytes >= options.segment_bytes &&
                    options.flush_interval.count() > 0 && options.sync_interval.count() > 0,
                "invalid capture limits");
        queue.resize(options.queue_blocks);
        for (auto& b : queue)
            b.bytes.reserve(options.block_bytes);
        require(std::filesystem::create_directory(options.directory), "capture directory must be new");
        if (::chmod(options.directory.c_str(), 0700))
            throw std::runtime_error("capture directory mode failed");
        directory_fd = ::open(options.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory_fd < 0)
            throw std::runtime_error("capture directory open failed");
        try {
            const auto parent =
                options.directory.has_parent_path() ? options.directory.parent_path() : std::filesystem::path(".");
            const int p = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            const bool synced = p >= 0 && ::fsync(p) == 0;
            if (p >= 0)
                ::close(p);
            if (!synced) {
                ::close(directory_fd);
                throw std::runtime_error("capture parent sync failed");
            }
            space();
            identity = utc_ns() ^ (static_cast<std::uint64_t>(::getpid()) << 32);
            last_flush = mono_ns();
            std::vector<std::byte> metadata;
            append(metadata, static_cast<std::uint32_t>(isins.size()));
            for (auto id : isins)
                append(metadata, id);
            std::array<char, 40> sha{};
#ifdef MOEX_CAPTURE_SOURCE_SHA
            const std::string_view source = MOEX_CAPTURE_SOURCE_SHA;
#else
            const std::string_view source = "unknown";
#endif
            std::memcpy(sha.data(), source.data(), std::min(sha.size(), source.size()));
            append_bytes(metadata, sha.data(), sha.size());
            enqueue({.kind = manifest_kind}, metadata, {}, {});
            worker = std::thread([this] { run(); });
        } catch (...) {
            ::close(directory_fd);
            directory_fd = -1;
            throw;
        }
    }
    ~Impl() {
        drain();
        if (directory_fd >= 0)
            ::close(directory_fd);
    }
    void fail(std::string message) noexcept {
        try {
            std::lock_guard lock(error_mutex);
            if (error.empty())
                error = std::move(message);
        } catch (...) {
        }
        failed.store(true, std::memory_order_release);
    }
    void check() {
        if (failed.load(std::memory_order_acquire)) {
            std::lock_guard lock(error_mutex);
            throw std::runtime_error(error.empty() ? "FullOrderLog capture failed" : error);
        }
    }
    void publish() {
        const auto h = head.load(std::memory_order_relaxed);
        if (h - tail.load(std::memory_order_acquire) >= queue.size())
            throw std::runtime_error("FullOrderLog capture queue full");
        if (!queue[h % queue.size()].count)
            return;
        head.store(h + 1, std::memory_order_release);
        queue_peak = std::max(queue_peak, h + 1 - tail.load(std::memory_order_acquire));
        last_flush = mono_ns();
    }
    void enqueue(RecordHeader r, std::span<const std::byte> payload, std::span<const std::uint8_t> nulls,
                 std::span<const std::byte> schema) {
        check();
        require(!finished, "capture already finished");
        const auto bytes = sizeof(r) + payload.size() + nulls.size() + schema.size();
        require(bytes <= options.block_bytes, "capture event exceeds block capacity");
        auto h = head.load(std::memory_order_relaxed);
        require(h - tail.load(std::memory_order_acquire) < queue.size(), "FullOrderLog capture queue full");
        auto* b = &queue[h % queue.size()];
        if (b->bytes.size() + bytes > options.block_bytes) {
            publish();
            h = head.load(std::memory_order_relaxed);
            require(h - tail.load(std::memory_order_acquire) < queue.size(), "FullOrderLog capture queue full");
            b = &queue[h % queue.size()];
        }
        r.sequence = ++sequence;
        r.source_sequence = source_sequence;
        r.utc_ns = utc_ns();
        r.monotonic_ns = mono_ns();
        r.payload_bytes = payload.size();
        r.null_bytes = nulls.size();
        r.schema_bytes = schema.size();
        if (!b->count)
            b->first = r.sequence;
        ++b->count;
        append(b->bytes, r);
        append_bytes(b->bytes, payload.data(), payload.size());
        append_bytes(b->bytes, nulls.data(), nulls.size());
        append_bytes(b->bytes, schema.data(), schema.size());
        logical += bytes;
    }
    void drain() noexcept {
        if (!worker.joinable())
            return;
        try {
            if (!failed.load())
                publish();
        } catch (const std::exception& e) {
            fail(e.what());
        }
        closing.store(true, std::memory_order_release);
        worker.join();
    }
    void space() {
        struct statvfs s {};
        if (::fstatvfs(directory_fd, &s) || static_cast<long double>(s.f_bavail) * s.f_frsize < options.min_free_bytes)
            throw std::runtime_error("FullOrderLog capture free-space limit");
    }
    void write_all(const void* data, std::size_t length) {
        require(length <= options.max_bytes - stored.load(), "FullOrderLog capture disk budget exhausted");
        const auto* p = static_cast<const char*>(data);
        while (length) {
            auto n = ::write(fd, p, length);
            if (n < 0 && errno == EINTR)
                continue;
            require(n > 0, "FullOrderLog capture write failed");
            p += n;
            length -= n;
            stored.fetch_add(n, std::memory_order_relaxed);
            segment_size += n;
        }
    }
    void sync() {
        const auto start = mono_ns();
        require(sync_data(fd) == 0, "FullOrderLog capture fdatasync failed");
        syncs.fetch_add(1, std::memory_order_relaxed);
        sync_max.store(std::max(sync_max.load(), mono_ns() - start), std::memory_order_relaxed);
    }
    void open_segment() {
        space();
        char name[64];
        std::snprintf(name, sizeof name, "capture-%06llu.folz", static_cast<unsigned long long>(++segment));
        fd = ::openat(directory_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        require(fd >= 0, "FullOrderLog capture segment open failed");
        segment_size = 0;
        FileHeader h;
        h.block_limit = options.block_bytes;
        h.identity = identity;
        h.segment = segment;
        write_all(&h, sizeof h);
        require(::fsync(directory_fd) == 0, "FullOrderLog capture directory sync failed");
    }
    void run() noexcept {
        auto* context = ZSTD_createCCtx();
        try {
            require(context, "FullOrderLog capture Zstd allocation failed");
            std::vector<std::byte> compressed(ZSTD_compressBound(options.block_bytes));
            open_segment();
            auto synced_at = mono_ns();
            for (;;) {
                const auto t = tail.load(std::memory_order_relaxed);
                if (t == head.load(std::memory_order_acquire)) {
                    if (closing.load(std::memory_order_acquire))
                        break;
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                } else {
                    auto& b = queue[t % queue.size()];
                    const auto size = ZSTD_compressCCtx(context, compressed.data(), compressed.size(), b.bytes.data(),
                                                        b.bytes.size(), 1);
                    require(!ZSTD_isError(size), "FullOrderLog capture compression failed");
                    if (segment_size + sizeof(ChunkHeader) + size > options.segment_bytes) {
                        sync();
                        require(::close(fd) == 0, "capture close failed");
                        fd = -1;
                        open_segment();
                    }
                    space();
                    ChunkHeader h{static_cast<std::uint32_t>(b.bytes.size()), static_cast<std::uint32_t>(size),
                                  checksum(b.bytes), b.count, b.first};
                    write_all(&h, sizeof h);
                    write_all(compressed.data(), size);
                    b.bytes.clear();
                    b.count = 0;
                    tail.store(t + 1, std::memory_order_release);
                }
                if (mono_ns() - synced_at >= static_cast<std::uint64_t>(options.sync_interval.count()) * 1000000) {
                    sync();
                    synced_at = mono_ns();
                }
            }
            sync();
            require(::fsync(directory_fd) == 0, "capture final directory sync failed");
            require(::close(fd) == 0, "capture final close failed");
            fd = -1;
        } catch (const std::exception& e) {
            fail(e.what());
        } catch (...) {
            fail("FullOrderLog capture worker failed");
        }
        if (fd >= 0)
            ::close(fd);
        fd = -1;
        ZSTD_freeCCtx(context);
    }
};
FullOrderLogCapture::FullOrderLogCapture(FullOrderLogCaptureOptions options, std::span<const std::int32_t> ids,
                                         Plaza2ListenerEventHandler& downstream)
    : impl_(std::make_unique<Impl>(std::move(options), ids, downstream)) {}
FullOrderLogCapture::~FullOrderLogCapture() noexcept = default;
bool FullOrderLogCapture::wants_raw_replication(std::string_view table) const noexcept {
    return impl_->downstream.wants_raw_replication(table);
}
bool FullOrderLogCapture::should_log_listener_event(const Plaza2ListenerEvent& e) const noexcept {
    return impl_->downstream.should_log_listener_event(e);
}
bool FullOrderLogCapture::needs_fresh_snapshot() const noexcept {
    return impl_->failed.load() || impl_->downstream.needs_fresh_snapshot();
}
Plaza2Error FullOrderLogCapture::record(const Plaza2ListenerEvent& e) {
    auto& p = *impl_;
    try {
        require(e.stream_code == kFullOrderLogStreamCode || e.stream_code == generated::StreamCode::kFortsDealsRepl,
                "capture accepts only public FOL/DEALS streams");
        ++p.source_sequence;
        bool selected = true;
        if (e.kind == Plaza2ListenerEventKind::StreamData && e.raw_table &&
            (e.raw_table->name == "orders" || e.raw_table->name == "orders_log" || e.raw_table->name == "deal")) {
            const auto& fields = e.raw_table->fields;
            const auto f =
                std::find_if(fields.begin(), fields.end(), [](const auto& v) { return v.name == "isin_id"; });
            // Malformed identity rows are retained as evidence and left to the book validator.
            if (f != fields.end() && f->size == 4 && f->offset <= e.raw_payload.size() &&
                4 <= e.raw_payload.size() - f->offset &&
                (e.raw_nulls.empty() || (f->index < e.raw_nulls.size() && !e.raw_nulls[f->index]))) {
                const auto id = load<std::int32_t>(e.raw_payload, f->offset);
                selected = std::binary_search(p.isins.begin(), p.isins.end(), id);
            }
        }
        if (selected) {
            std::vector<std::byte> schema;
            if (e.kind == Plaza2ListenerEventKind::Open)
                schema = encode_schema(e.raw_tables);
            const auto payload = e.kind == Plaza2ListenerEventKind::ReplState
                                     ? std::as_bytes(std::span(e.text_value.data(), e.text_value.size()))
                                     : e.raw_payload;
            require(e.table_index <= UINT32_MAX && e.native_data_size <= UINT32_MAX, "capture event metadata overflow");
            p.enqueue({.unsigned_value = e.unsigned_value,
                       .signed_value = e.signed_value,
                       .kind = static_cast<std::uint32_t>(e.kind),
                       .stream = static_cast<std::uint32_t>(e.stream_code),
                       .table_code = static_cast<std::uint32_t>(e.table_code),
                       .table_index = static_cast<std::uint32_t>(e.table_index),
                       .clear_flags = e.clear_deleted_flags,
                       .close_reason = e.close_reason,
                       .native_data_size = static_cast<std::uint32_t>(e.native_data_size)},
                      payload, e.raw_nulls, schema);
        } else
            ++p.filtered;
    } catch (const std::exception& error) {
        p.fail(error.what());
        p.downstream.on_plaza2_listener_error(
            {.code = Plaza2ErrorCode::CallbackFailed, .message = "FullOrderLog capture failed"});
        return {.code = Plaza2ErrorCode::CallbackFailed, .message = error.what()};
    }
    return {};
}
Plaza2Error FullOrderLogCapture::on_plaza2_listener_event(const Plaza2ListenerEvent& e) {
    if (auto error = record(e); error)
        return error;
    const auto result = impl_->downstream.on_plaza2_listener_event(e);
    if (result)
        on_plaza2_listener_error(result);
    return result;
}
void FullOrderLogCapture::record_error(generated::StreamCode stream, const Plaza2Error& error) noexcept {
    auto& p = *impl_;
    p.validation_failed = true;
    ++p.source_sequence;
    try {
        p.enqueue(
            {.unsigned_value = error.runtime_code, .kind = error_kind, .stream = static_cast<std::uint32_t>(stream)},
            {}, {}, {});
    } catch (const std::exception& e) {
        p.fail(e.what());
    }
}
void FullOrderLogCapture::on_plaza2_listener_error(const Plaza2Error& error) noexcept {
    record_error(kFullOrderLogStreamCode, error);
    impl_->downstream.on_plaza2_listener_error(error);
}
void FullOrderLogCapture::poll() {
    auto& p = *impl_;
    p.check();
    if (mono_ns() - p.last_flush >= static_cast<std::uint64_t>(p.options.flush_interval.count()) * 1000000) {
        try {
            p.publish();
        } catch (const std::exception& e) {
            p.fail(e.what());
            throw;
        }
    }
}
void FullOrderLogCapture::finish() {
    auto& p = *impl_;
    if (p.finished) {
        p.check();
        return;
    }
    p.check();
    p.enqueue({.unsigned_value = p.validation_failed ? 1ULL : 0ULL, .kind = end_kind}, {}, {}, {});
    p.finished = true;
    p.drain();
    p.check();
}
FullOrderLogCaptureMetrics FullOrderLogCapture::metrics() const noexcept {
    const auto& p = *impl_;
    return {p.sequence,     p.filtered,        p.logical,    p.stored.load(),
            p.syncs.load(), p.sync_max.load(), p.queue_peak, p.failed.load()};
}
} // namespace moex::plaza2::cgate
