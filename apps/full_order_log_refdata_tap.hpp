#pragma once

#include "moex/connector_host/connector_host.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_refdata.hpp"
#include <algorithm>
#include <map>
#include <stdexcept>
#include <tuple>

namespace moex::connector_host {

// Supplements committed host metadata with the exact decoded short code and
// presence of roundto. The shared projector stores absent roundto as zero.
class FullOrderLogRefdataTap {
  public:
    explicit FullOrderLogRefdataTap(std::span<const std::int32_t> selected) : selected_(selected) {}

    void on_event(const plaza2::cgate::Plaza2ListenerEvent& event) {
        using plaza2::cgate::Plaza2DecodedValueKind;
        using plaza2::cgate::Plaza2ListenerEventKind;
        using plaza2::generated::FieldCode;
        using plaza2::generated::StreamCode;
        using plaza2::generated::TableCode;
        if (event.stream_code != StreamCode::kFortsRefdataRepl)
            return;
        if (event.kind == Plaza2ListenerEventKind::LifeNum) {
            if (life_ != event.unsigned_value)
                fields_.clear();
            life_ = event.unsigned_value;
        } else if (event.kind == Plaza2ListenerEventKind::Open || event.kind == Plaza2ListenerEventKind::Close) {
            fields_.clear();
        }
        if (event.kind != Plaza2ListenerEventKind::StreamData || !life_ || event.signed_value <= 0)
            return;
        const bool instrument = event.table_code == TableCode::kFortsRefdataReplFutInstruments;
        if (!instrument && event.table_code != TableCode::kFortsRefdataReplFutSessContents)
            return;
        const auto find = [&](FieldCode code) {
            return std::ranges::find(event.fields, code, &plaza2::cgate::Plaza2DecodedFieldValue::field_code);
        };
        const auto id = find(instrument ? FieldCode::kFortsRefdataReplFutInstrumentsIsinId
                                        : FieldCode::kFortsRefdataReplFutSessContentsIsinId);
        if (id == event.fields.end() || id->kind != Plaza2DecodedValueKind::SignedInteger ||
            std::ranges::find(selected_, id->signed_value) == selected_.end())
            return;
        const auto short_code = find(instrument ? FieldCode::kFortsRefdataReplFutInstrumentsShortIsin
                                                : FieldCode::kFortsRefdataReplFutSessContentsShortIsin);
        const auto decimals = find(instrument ? FieldCode::kFortsRefdataReplFutInstrumentsRoundto
                                              : FieldCode::kFortsRefdataReplFutSessContentsRoundto);
        SourceFields fields;
        if (short_code != event.fields.end() && short_code->kind == Plaza2DecodedValueKind::String)
            fields.short_code = short_code->text_value;
        if (decimals != event.fields.end() && decimals->kind == Plaza2DecodedValueKind::SignedInteger &&
            decimals->signed_value >= 0 && decimals->signed_value <= 18)
            fields.decimals = static_cast<std::int32_t>(decimals->signed_value);
        const auto key = Key{static_cast<std::int32_t>(id->signed_value), event.table_code, event.signed_value};
        if (fields_.size() >= 65536 && !fields_.contains(key))
            throw std::runtime_error("FullOrderLog REFDATA source row limit");
        fields_.insert_or_assign(key, std::move(fields));
    }

    [[nodiscard]] plaza2::cgate::FullOrderLogRefdataValue value(const ConnectorHostMarketDataSnapshot& snapshot) const {
        plaza2::cgate::FullOrderLogRefdataValue value{.isin_id = static_cast<std::int32_t>(snapshot.target_isin_id)};
        const auto& instrument = snapshot.definition_source_provenance;
        const auto& session = snapshot.session_provenance;
        if (!snapshot.refdata_metadata_current || !instrument.present || !session.present || !life_ ||
            instrument.lifenum != life_ || session.lifenum != life_ || session.repl_rev <= 0)
            return value;
        const auto fields = fields_.find(Key{value.isin_id, instrument.table_code, instrument.repl_rev});
        if (fields == fields_.end())
            return value;
        value.available = true;
        value.isin = snapshot.symbol;
        value.short_code = fields->second.short_code;
        value.min_step = snapshot.min_step;
        value.decimals = fields->second.decimals;
        value.sess_id = snapshot.target_session_id;
        value.instrument_source = instrument;
        value.session_source = session;
        return value;
    }

  private:
    using Key = std::tuple<std::int32_t, plaza2::generated::TableCode, std::int64_t>;
    struct SourceFields {
        std::string short_code;
        std::optional<std::int32_t> decimals;
    };
    std::span<const std::int32_t> selected_;
    std::uint64_t life_{};
    std::map<Key, SourceFields> fields_;
};

} // namespace moex::connector_host
