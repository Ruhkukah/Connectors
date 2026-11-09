#pragma once
#include "moex/plaza2/cgate/plaza2_full_order_log_capture.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_oi.hpp"
#include <charconv>
#include <iostream>
namespace moex::connector_host {
struct FullOrderLogCaptureFlags {
    plaza2::cgate::FullOrderLogCaptureOptions options;
    bool budget_set{}, open_interest{};
    bool consume(std::string_view key, int& i, int argc, char** argv) {
        if (key == "--open-interest") {
            if (open_interest)
                throw std::invalid_argument("duplicate --open-interest");
            open_interest = true;
            return true;
        }
        if (key != "--capture-dir" && key != "--capture-max-bytes" && key != "--capture-min-free-bytes")
            return false;
        if (++i == argc)
            throw std::invalid_argument("missing capture option value");
        if (key == "--capture-dir") {
            if (!options.directory.empty())
                throw std::invalid_argument("duplicate capture directory");
            options.directory = argv[i];
            if (options.directory.empty())
                throw std::invalid_argument("empty capture directory");
        } else {
            budget_set = true;
            std::uint64_t value{};
            const auto text = std::string_view(argv[i]);
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                (!value && key == "--capture-max-bytes"))
                throw std::invalid_argument("invalid capture byte budget");
            (key == "--capture-max-bytes" ? options.max_bytes : options.min_free_bytes) = value;
        }
        return true;
    }
    void validate() const {
        if (budget_set && options.directory.empty())
            throw std::invalid_argument("capture budgets require --capture-dir");
    }
};
inline void report_full_order_log_open_interest(const plaza2::cgate::FullOrderLogOpenInterest& oi,
                                                std::span<const std::int32_t> ids) {
    for (auto id : ids) {
        const auto v = oi.value(id);
        std::cout << "oi_isin=" << id << " oi_online=" << oi.online() << " oi_available=" << v.has_value();
        if (v)
            std::cout << " oi_quantity=" << v->quantity << " oi_session=" << v->session
                      << " oi_moment_ns=" << v->moment_ns;
        std::cout << '\n' << std::flush;
    }
}
inline void report_full_order_log_capture(const plaza2::cgate::FullOrderLogCapture& recorder) {
    const auto m = recorder.metrics();
    std::cout << "capture_records=" << m.records << " capture_filtered=" << m.filtered
              << " capture_logical_bytes=" << m.logical_bytes << " capture_stored_bytes=" << m.stored_bytes
              << " capture_syncs=" << m.syncs << " capture_sync_max_ns=" << m.sync_max_ns
              << " capture_queue_peak_blocks=" << m.queue_peak << " capture_failed=" << m.failed << '\n'
              << std::flush;
}
} // namespace moex::connector_host
