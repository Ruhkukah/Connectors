// Qualification-only read-only resolver; built from the same reviewed native source.
// No publisher, reply listener, message allocation or order API is created.
#include "moex/connector_host/operator_config.hpp"
#include "moex/plaza2_trade/cgate_session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace cg = moex::plaza2::cgate;
namespace ch = moex::connector_host;
namespace pt = moex::plaza2_trade;
namespace ps = moex::plaza2::private_state;
namespace gen = moex::plaza2::generated;
using Clock = std::chrono::steady_clock;
static volatile std::sig_atomic_t interrupted = 0;
static void interrupt(int) {
    interrupted = 1;
}

static int fail(std::string_view code, int exit_code, std::uint32_t runtime_code = 0) {
    std::cout << "{\"ok\":false,\"error\":\"" << code << "\",\"runtime_code\":" << runtime_code << "}\n";
    return exit_code;
}

struct Target {
    std::string_view symbol;
    std::int32_t isin_id;
    ps::FutureSessionTerms terms;
    ps::SourceRowProvenance definition;
};

static std::optional<std::array<Target, 2>> resolve(const ps::Plaza2PrivateStateProjector& data, std::int32_t session,
                                                    std::uint64_t life) {
    constexpr std::array<std::string_view, 2> symbols{"ALRS-12.26", "CNY-12.26"};
    std::array<Target, 2> result{};
    for (std::size_t index = 0; index < symbols.size(); ++index) {
        const ps::InstrumentSnapshot* found = nullptr;
        for (const auto& row : data.instruments()) {
            if (row.isin != symbols[index] || !ps::is_outright_future(row) || !row.current_session_member ||
                row.sess_id != session)
                continue;
            if (found) // Ambiguous identity must never choose an arbitrary row.
                return std::nullopt;
            found = &row;
        }
        if (!found || found->isin_id <= 0)
            return std::nullopt;
        auto terms = data.find_future_session_terms(found->isin_id, session, life);
        const auto definition =
            data.instrument_source_provenance(gen::TableCode::kFortsRefdataReplFutInstruments, found->isin_id);
        if (!terms || !definition || !definition->present || definition->lifenum != life ||
            terms->isin_id != found->isin_id || terms->sess_id != session || !terms->source.present ||
            terms->source.lifenum != life || !terms->settlement_price || terms->settlement_price->units <= 0 ||
            terms->settlement_price->units > std::numeric_limits<std::int64_t>::max() / 2 || !terms->min_step ||
            terms->min_step->units <= 0 || !terms->bounds.interval_valid || !terms->bounds.lower ||
            !terms->bounds.upper)
            return std::nullopt;
        result[index] = Target{symbols[index], found->isin_id, *terms, *definition};
    }
    if (result[0].isin_id == result[1].isin_id)
        return std::nullopt;
    return result;
}

static bool committed_streams(std::span<const ps::StreamHealthSnapshot> streams) {
    // The projector's last_commit_sequence is a touched-view marker, not the
    // native bridge's committed-row counter. ONLINE + complete + committed rows
    // are checked here; exact target source epochs and terms remain separate gates.
    return streams.size() == 3 && std::all_of(streams.begin(), streams.end(), [](const auto& row) {
               return row.online && row.snapshot_complete && row.committed_row_count > 0;
           });
}

// Private qualification diagnostics contain only states/counts and the two approved symbols.
// No account, identifier, source row, path, credential or software-key value is emitted.
static void diagnose(const pt::CgateSession& session) {
    const auto health = session.runtime_health();
    const auto& data = session.private_state();
    const auto life = data.refdata_lifenum();
    const auto current = data.current_session_id();
    const auto* row = data.find_session(current);
    const auto source = data.session_source_provenance(gen::TableCode::kFortsRefdataReplSession, current);
    std::cerr << std::boolalpha << "{\"event\":\"resolver_gate_diagnostic\",\"health\":{\"valid\":" << health.valid
              << ",\"private_count\":" << health.private_count << ",\"private_active\":" << health.private_active
              << ",\"connection\":" << health.connection << ",\"publisher\":" << health.publisher
              << ",\"reply\":" << health.reply << "},\"stream_count\":" << data.stream_health().size()
              << ",\"streams\":[";
    bool comma = false;
    for (const auto& stream : data.stream_health()) {
        if (comma)
            std::cerr << ',';
        comma = true;
        const auto found = std::find(health.private_streams.begin(),
                                     health.private_streams.begin() + health.private_count, stream.stream_code);
        const auto native =
            found != health.private_streams.begin() + health.private_count
                ? health.private_states[static_cast<std::size_t>(found - health.private_streams.begin())]
                : 0;
        std::cerr << "{\"stream\":\"" << stream.stream_name << "\",\"online\":" << stream.online
                  << ",\"snapshot_complete\":" << stream.snapshot_complete
                  << ",\"commits\":" << stream.last_commit_sequence
                  << ",\"committed_rows\":" << stream.committed_row_count << ",\"native_state\":" << native << '}';
    }
    std::cerr << "],\"session\":{\"current_present\":" << (current > 0) << ",\"ref_life_present\":" << life.has_value()
              << ",\"ref_life_positive\":" << (life && *life > 0) << ",\"session_count\":" << data.sessions().size()
              << ",\"row_present\":" << (row != nullptr)
              << ",\"has_current_status\":" << (row && row->has_current_status)
              << ",\"source_available\":" << source.has_value() << ",\"source_present\":" << (source && source->present)
              << ",\"source_matches_ref_life\":" << (source && life && source->lifenum == *life) << "},\"targets\":[";
    comma = false;
    for (const std::string_view symbol : {"ALRS-12.26", "CNY-12.26"}) {
        std::size_t count{}, future{}, member{}, session_match{}, eligible{}, definition_available{},
            definition_present{}, definition_epoch{}, terms_available{}, terms_present{}, terms_epoch{},
            reference_positive{}, grid_positive{}, bounds_valid{}, status{}, status_bound{};
        for (const auto& instrument : data.instruments()) {
            if (instrument.isin != symbol)
                continue;
            ++count;
            future += ps::is_outright_future(instrument);
            member += instrument.current_session_member;
            session_match += instrument.sess_id == current;
            eligible += ps::is_outright_future(instrument) && instrument.current_session_member &&
                        instrument.sess_id == current && instrument.isin_id > 0;
            status += instrument.has_current_status;
            status_bound += instrument.current_status_refdata_bound;
            const auto definition =
                data.instrument_source_provenance(gen::TableCode::kFortsRefdataReplFutInstruments, instrument.isin_id);
            definition_available += definition.has_value();
            definition_present += definition && definition->present;
            definition_epoch += definition && life && definition->lifenum == *life;
            const auto terms = life ? data.find_future_session_terms(instrument.isin_id, current, *life) : std::nullopt;
            terms_available += terms.has_value();
            terms_present += terms && terms->source.present;
            terms_epoch += terms && life && terms->source.lifenum == *life && terms->sess_id == current &&
                           terms->isin_id == instrument.isin_id;
            reference_positive += terms && terms->settlement_price && terms->settlement_price->units > 0 &&
                                  terms->settlement_price->units <= std::numeric_limits<std::int64_t>::max() / 2;
            grid_positive += terms && terms->min_step && terms->min_step->units > 0;
            bounds_valid += terms && terms->bounds.interval_valid && terms->bounds.lower && terms->bounds.upper;
        }
        if (comma)
            std::cerr << ',';
        comma = true;
        std::cerr << "{\"symbol\":\"" << symbol << "\",\"count\":" << count << ",\"regular_future_count\":" << future
                  << ",\"current_member_count\":" << member << ",\"current_session_match_count\":" << session_match
                  << ",\"eligible_count\":" << eligible << ",\"status_count\":" << status
                  << ",\"status_bound_count\":" << status_bound
                  << ",\"definition_available_count\":" << definition_available
                  << ",\"definition_present_count\":" << definition_present
                  << ",\"definition_epoch_match_count\":" << definition_epoch
                  << ",\"terms_available_count\":" << terms_available << ",\"terms_present_count\":" << terms_present
                  << ",\"terms_epoch_match_count\":" << terms_epoch
                  << ",\"reference_positive_count\":" << reference_positive
                  << ",\"grid_positive_count\":" << grid_positive << ",\"bounds_valid_count\":" << bounds_valid << '}';
    }
    std::cerr << "]}\n" << std::flush;
}

int main(int argc, char** argv) {
    std::signal(SIGTERM, interrupt);
    std::signal(SIGINT, interrupt);
    std::signal(SIGHUP, SIG_IGN);
    std::signal(SIGPIPE, SIG_IGN);
    try {
        std::vector<std::string_view> arguments;
        for (int index = 1; index < argc; ++index)
            arguments.emplace_back(argv[index]);
        auto request = ch::parse_operator_arguments(arguments);
        auto config = request.config.transport.host;
        if (request.help || request.command != "run" || !request.config.read_only_market_data ||
            config.runtime.environment != cg::Plaza2Environment::Test || !config.read_only_market_data ||
            config.allow_orders || !config.publisher_settings.empty() || !config.p2mqreply_settings.empty() ||
            !config.public_deals_stream.settings.empty() || request.wait_ms > 900000)
            return fail("test_readonly_configuration_required", 2);

        // The parser requires a positive --isin-id, but this discovery does not
        // subscribe to a book or use that placeholder as a target identity.
        config.aggr20_stream = {};
        config.market_data_isin_ids.clear();
        config.publisher_name.clear();
        config.publisher_open_settings.clear();
        config.p2mqreply_open_settings.clear();
        config.trade_replay_from_pos_anchor = false;
        config.event_log = {};
        config.listener_event_log = {};
        config.collect_poll_metrics = false;
        config.collect_post_metrics = false;
        if (config.private_streams.size() != 1 ||
            config.private_streams.front().stream_code != gen::StreamCode::kFortsRefdataRepl ||
            config.status_streams.size() != 2)
            return fail("refdata_and_status_profile_required", 2);

        pt::CgateSession session(std::move(config));
        struct Stop {
            pt::CgateSession& session;
            ~Stop() {
                (void)session.stop();
            }
        } stop{session};
        if (auto error = session.start())
            return fail("native_start_failed", 3, error.runtime_code);

        const auto deadline = Clock::now() + std::chrono::milliseconds(request.wait_ms);
        while (!interrupted && Clock::now() < deadline) {
            if (auto error = session.poll(false))
                return fail("native_poll_failed", 3, error.runtime_code);
            if (!session.last_callback_error().empty())
                return fail("native_callback_failed", 3);
            for (const auto code : {gen::StreamCode::kFortsRefdataRepl, gen::StreamCode::kFortsSessionstateRepl,
                                    gen::StreamCode::kFortsInstrumentstateRepl})
                if (auto error = session.listener_error(code))
                    return fail("native_listener_failed", 3, error.runtime_code);

            const auto health = session.runtime_health();
            const auto& data = session.private_state();
            const auto life = data.refdata_lifenum();
            const auto current_session = data.current_session_id();
            const auto streams = data.stream_health();
            const bool committed = committed_streams(streams);
            const auto session_row = data.find_session(current_session);
            const auto source =
                data.session_source_provenance(gen::TableCode::kFortsRefdataReplSession, current_session);
            if (health.valid && health.connection == 3 && health.private_count == 3 && health.private_active &&
                health.publisher == 0 && health.reply == 0 && committed && life && *life > 0 && current_session > 0 &&
                session_row && session_row->has_current_status && source && source->present &&
                source->lifenum == *life) {
                if (auto targets = resolve(data, current_session, *life)) {
                    const auto calls = session.publisher_call_counts();
                    if (session.publisher_open() || session.p2mqreply_open() || calls.msgnew || calls.post)
                        return fail("unexpected_trading_capability", 4);
                    const auto captured = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count();
                    // Stop and validate cleanup before printing the success receipt.
                    if (auto error = session.stop())
                        return fail("native_stop_failed", 3, error.runtime_code);
                    std::cout << "{\"ok\":true,\"read_only\":true,\"committed_refdata\":true,"
                                 "\"publisher_created\":false,\"message_allocations\":0,\"publisher_posts\":0,"
                                 "\"captured_utc_ns\":"
                              << captured << ",\"sess_id\":" << current_session << ",\"refdata_lifenum\":" << *life
                              << ",\"session_repl_rev\":" << source->repl_rev << ",\"targets\":[";
                    bool comma = false;
                    for (const auto& target : *targets) {
                        if (comma)
                            std::cout << ',';
                        comma = true;
                        std::cout << "{\"symbol\":\"" << target.symbol << "\",\"isin_id\":" << target.isin_id
                                  << ",\"reference_price_scaled\":" << target.terms.settlement_price->units
                                  << ",\"quote_cap_scaled\":" << 2 * target.terms.settlement_price->units
                                  << ",\"min_step_scaled\":" << target.terms.min_step->units
                                  << ",\"lower_price_scaled\":" << target.terms.bounds.lower->units
                                  << ",\"upper_price_scaled\":" << target.terms.bounds.upper->units
                                  << ",\"definition_repl_rev\":" << target.definition.repl_rev
                                  << ",\"terms_repl_rev\":" << target.terms.source.repl_rev << '}';
                    }
                    std::cout << "]}\n";
                    return 0;
                }
            }
            (void)session.take_private_row_changes();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        diagnose(session);
        return fail(interrupted ? "interrupted" : "fresh_committed_targets_not_resolved", 5);
    } catch (const std::exception&) {
        return fail("invalid_configuration_or_native_exception", 9);
    }
}
