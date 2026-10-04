#include "moex/connector_host/full_order_log_dtc.hpp"
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace moex::connector_host::dtc {
DtcFullOrderLogSource::DtcFullOrderLogSource(const plaza2::cgate::Plaza2FullOrderLog& book, std::int32_t isin,
                                             DtcMarketDataSnapshot metadata, std::size_t max_changes)
    : book_(book), isin_(isin) {
    changes_.reserve(max_changes);
    for (auto* sides : {&published_, &scratch_})
        for (auto& side : *sides)
            side.reserve(max_changes / 2);
    update_metadata(std::move(metadata));
}
void DtcFullOrderLogSource::update_metadata(DtcMarketDataSnapshot metadata) {
    metadata.levels.clear();
    metadata.isin_id = isin_;
    metadata.full_order_log = true;
    metadata.order_entry_allowed = false;
    metadata_ = std::move(metadata);
}
void DtcFullOrderLogSource::load_top(bool bid, std::vector<Level>& into) const {
    into.clear();
    const auto& levels = book_.levels(isin_, bid);
    if (bid) {
        for (auto row = levels.rbegin(); row != levels.rend() && into.size() < depth_; ++row)
            into.emplace_back(*row);
    } else {
        for (auto row = levels.begin(); row != levels.end() && into.size() < depth_; ++row)
            into.emplace_back(*row);
    }
}
void DtcFullOrderLogSource::configure_depth_limit(std::size_t depth) {
    if (!depth || depth > published_[0].capacity())
        throw std::invalid_argument("FullOrderLog DTC depth exceeds capacity");
    depth_ = depth;
    configured_ = true;
    changes_.clear();
    load_top(true, published_[0]);
    load_top(false, published_[1]);
    published_revision_ = book_.revision(isin_);
    published_epoch_ = book_.epoch();
    wire_prices_valid_ = true;
}
void DtcFullOrderLogSource::committed() {
    changes_.clear();
    if (!configured_ || (published_revision_ == book_.revision(isin_) && published_epoch_ == book_.epoch()))
        return;
    wire_prices_valid_ = true;
    const auto wire = [](std::int64_t price) { return static_cast<float>(static_cast<double>(price) / 100000.0); };
    for (unsigned side = 0; side < 2; ++side) {
        load_top(side == 0, scratch_[side]);
        auto& before = published_[side];
        const auto& after = scratch_[side];
        for (std::size_t i = 0; i < std::max(before.size(), after.size()); ++i) {
            if (i < after.size()) {
                wire_prices_valid_ &= std::isfinite(wire(after[i].first)) &&
                                      (!i || (side == 0 ? wire(after[i - 1].first) > wire(after[i].first)
                                                        : wire(after[i - 1].first) < wire(after[i].first)));
                if (i < before.size() && before[i] == after[i])
                    continue;
            }
            const auto level = i < after.size() ? after[i] : Level{before[i].first, 0};
            changes_.push_back({.price_scaled = level.first,
                                .volume = level.second,
                                .side = side == 0 ? DtcDepthSide::Bid : DtcDepthSide::Ask,
                                .depth_level = static_cast<std::uint32_t>(i + 1)});
        }
        before.swap(scratch_[side]);
    }
    published_revision_ = book_.revision(isin_);
    published_epoch_ = book_.epoch();
}
DtcMarketDataSnapshot DtcFullOrderLogSource::status_snapshot() const {
    auto out = metadata_;
    out.stream_epoch = out.market_data_authority_epoch = book_.epoch();
    out.source_snapshot_version = out.snapshot_watermark = book_.revision(isin_);
    out.committed_at = book_.committed_at();
    out.source_online = out.aggr_online = out.snapshot_complete = out.book_snapshot_current = book_.valid();
    out.market_data_display_allowed =
        out.transport_active && out.refdata_metadata_current && book_.valid() && wire_prices_valid_;
    out.source_consistent = out.target_authoritative = out.valid = out.market_data_display_allowed;
    out.source_snapshot_hash = 0;
    out.order_entry_allowed = false;
    out.session_ready_witness_kind = SessionReadyWitnessKind::None;
    return out;
}
DtcMarketDataSnapshot DtcFullOrderLogSource::snapshot() const {
    auto out = status_snapshot();
    for (const bool bid : {true, false}) {
        std::vector<Level> rows;
        load_top(bid, rows);
        for (const auto& [price, quantity] : rows)
            out.levels.push_back(
                {.price_scaled = price, .volume = quantity, .side = bid ? DtcDepthSide::Bid : DtcDepthSide::Ask});
    }
    out.snapshot_level_count = out.levels.size();
    return out;
}
DtcReadOnlyCapabilities DtcFullOrderLogSource::capabilities() const noexcept {
    return {.market_depth = true, .security_definitions = true};
}
} // namespace moex::connector_host::dtc
