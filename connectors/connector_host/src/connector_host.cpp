#include "moex/connector_host/connector_host.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <random>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace moex::connector_host {
namespace {
namespace cg = plaza2::cgate;
namespace ps = plaza2::private_state;
using namespace plaza2_trade;
using plaza2::generated::StreamCode;

cg::Plaza2Error invalid(std::string message) {
    return {.code = cg::Plaza2ErrorCode::InvalidConfiguration, .message = std::move(message)};
}

std::string quoted(std::string_view value) {
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

bool write_atomic_file(const std::filesystem::path& path, std::string_view contents, std::string& error) {
    if (path.empty() || path.parent_path().empty()) {
        error = "persistent session checkpoint requires a journal root";
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "failed to create persistent session directory: " + filesystem_error.message();
        return false;
    }
    std::random_device random;
    const auto temporary = path.string() + ".tmp-" + std::to_string(random());
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        error = "checkpoint exclusive temporary creation failed";
        return false;
    }
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const auto count = ::write(fd, contents.data() + offset, contents.size() - offset);
        if (count <= 0) {
            ::close(fd);
            ::unlink(temporary.c_str());
            error = "checkpoint write failed";
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    const bool synced = ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (!synced || !closed || ::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        error = "checkpoint durable publication failed";
        return false;
    }
    const int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (directory < 0) {
        error = "checkpoint directory open failed";
        return false;
    }
    const bool durable = ::fsync(directory) == 0;
    ::close(directory);
    if (!durable) {
        error = "checkpoint directory fsync failed";
        return false;
    }
    return true;
}

std::optional<std::string> checkpoint_string_field(std::string_view text, std::string_view key) {
    const auto marker = std::string("\"") + std::string(key) + "\": \"";
    const auto begin = text.find(marker);
    if (begin == std::string_view::npos)
        return std::nullopt;
    const auto value_begin = begin + marker.size();
    std::string value;
    for (std::size_t index = value_begin; index < text.size(); ++index) {
        const auto character = text[index];
        if (character == '"')
            return value;
        if (character == '\\' && index + 1 < text.size()) {
            const auto escaped = text[++index];
            switch (escaped) {
            case '"':
                value.push_back('"');
                break;
            case '\\':
                value.push_back('\\');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                return std::nullopt;
            }
        } else {
            value.push_back(character);
        }
    }
    return std::nullopt;
}

template <class Integer> std::optional<Integer> checkpoint_integer_field(std::string_view text, std::string_view key) {
    const auto marker = std::string("\"") + std::string(key) + "\": ";
    const auto begin = text.find(marker);
    if (begin == std::string_view::npos)
        return std::nullopt;
    const auto value_begin = begin + marker.size();
    const auto value_end = text.find_first_of(",\n}", value_begin);
    const auto value = text.substr(value_begin, value_end == std::string_view::npos ? std::string_view::npos
                                                                                    : value_end - value_begin);
    Integer parsed{};
    const auto [pointer, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || pointer != value.data() + value.size())
        return std::nullopt;
    return parsed;
}

struct PersistentSessionCheckpoint {
    std::string schema{"moex.connector_host.persistent_session.v2"};
    std::string account_sha256;
    std::int32_t sess_id{0};
    std::string session_binding_sha256;
    std::uint64_t authorized_generation{0};
    std::int64_t known_order_id{0};
    std::int64_t known_remaining{-1};
    std::int32_t lifecycle_state{0};
    std::uint32_t next_recovered_user_id{std::numeric_limits<std::uint32_t>::max()};
    std::string phase;
    std::uint64_t epoch_counter{0};
    std::string base_run_id;
    std::string profile_id;
    std::string profile_fingerprint;
    std::string run_id;
    std::int32_t ext_id{0};
    std::uint32_t add_user_id{0};
    std::uint32_t cancel_user_id{0};
    std::uint32_t recovery_user_id{0};
    Plaza2TradeSide side{Plaza2TradeSide::Buy};
    std::string price;
    std::string base_contract_code;
    std::string comment;
    std::int32_t quantity{1};
    std::int32_t isin_id{0};
    std::int8_t instrument_mask{0};
    std::string plan_sha256;
    std::string add_payload_sha256;
    std::string recovery_payload_sha256;
};

std::string checkpoint_json(const PersistentSessionCheckpoint& checkpoint) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": " << quoted(std::string_view(checkpoint.schema)) << ",\n"
        << "  \"account_sha256\": " << quoted(checkpoint.account_sha256) << ",\n"
        << "  \"sess_id\": " << checkpoint.sess_id << ",\n"
        << "  \"session_binding_sha256\": " << quoted(checkpoint.session_binding_sha256) << ",\n"
        << "  \"authorized_generation\": " << checkpoint.authorized_generation << ",\n"
        << "  \"known_order_id\": " << checkpoint.known_order_id << ",\n"
        << "  \"known_remaining\": " << checkpoint.known_remaining << ",\n"
        << "  \"lifecycle_state\": " << checkpoint.lifecycle_state << ",\n"
        << "  \"next_recovered_user_id\": " << checkpoint.next_recovered_user_id << ",\n"
        << "  \"phase\": " << quoted(checkpoint.phase) << ",\n"
        << "  \"epoch_counter\": " << checkpoint.epoch_counter << ",\n"
        << "  \"base_run_id\": " << quoted(checkpoint.base_run_id) << ",\n"
        << "  \"profile_id\": " << quoted(checkpoint.profile_id) << ",\n"
        << "  \"profile_fingerprint\": " << quoted(checkpoint.profile_fingerprint) << ",\n"
        << "  \"run_id\": " << quoted(std::string_view(checkpoint.run_id)) << ",\n"
        << "  \"ext_id\": " << checkpoint.ext_id << ",\n"
        << "  \"add_user_id\": " << checkpoint.add_user_id << ",\n"
        << "  \"cancel_user_id\": " << checkpoint.cancel_user_id << ",\n"
        << "  \"recovery_user_id\": " << checkpoint.recovery_user_id << ",\n"
        << "  \"side\": " << quoted(checkpoint.side == Plaza2TradeSide::Buy ? "buy" : "sell") << ",\n"
        << "  \"price\": " << quoted(checkpoint.price) << ",\n"
        << "  \"base_contract_code\": " << quoted(checkpoint.base_contract_code) << ",\n"
        << "  \"comment\": " << quoted(checkpoint.comment) << ",\n"
        << "  \"quantity\": " << checkpoint.quantity << ",\n"
        << "  \"isin_id\": " << checkpoint.isin_id << ",\n"
        << "  \"instrument_mask\": " << static_cast<int>(checkpoint.instrument_mask) << ",\n"
        << "  \"plan_sha256\": " << quoted(checkpoint.plan_sha256) << ",\n"
        << "  \"add_payload_sha256\": " << quoted(checkpoint.add_payload_sha256) << ",\n"
        << "  \"recovery_payload_sha256\": " << quoted(checkpoint.recovery_payload_sha256) << "\n"
        << "}\n";
    return out.str();
}

bool parse_checkpoint(std::string_view text, PersistentSessionCheckpoint& checkpoint, std::string& error) {
    const auto schema = checkpoint_string_field(text, "schema");
    const auto phase = checkpoint_string_field(text, "phase");
    const auto epoch = checkpoint_integer_field<std::uint64_t>(text, "epoch_counter");
    const auto base_run_id = checkpoint_string_field(text, "base_run_id");
    const auto profile_id = checkpoint_string_field(text, "profile_id");
    const auto profile_fingerprint = checkpoint_string_field(text, "profile_fingerprint");
    const auto run_id = checkpoint_string_field(text, "run_id");
    const auto ext_id = checkpoint_integer_field<std::int32_t>(text, "ext_id");
    const auto add_user_id = checkpoint_integer_field<std::uint32_t>(text, "add_user_id");
    const auto cancel_user_id = checkpoint_integer_field<std::uint32_t>(text, "cancel_user_id");
    const auto recovery_user_id = checkpoint_integer_field<std::uint32_t>(text, "recovery_user_id");
    const auto side = checkpoint_string_field(text, "side");
    const auto price = checkpoint_string_field(text, "price");
    const auto base_contract_code = checkpoint_string_field(text, "base_contract_code");
    const auto comment = checkpoint_string_field(text, "comment");
    const auto quantity = checkpoint_integer_field<std::int32_t>(text, "quantity");
    const auto isin_id = checkpoint_integer_field<std::int32_t>(text, "isin_id");
    const auto instrument_mask = checkpoint_integer_field<std::int32_t>(text, "instrument_mask");
    const auto plan_sha256 = checkpoint_string_field(text, "plan_sha256");
    const auto add_payload_sha256 = checkpoint_string_field(text, "add_payload_sha256");
    const auto recovery_payload_sha256 = checkpoint_string_field(text, "recovery_payload_sha256");
    if (!schema ||
        (*schema != "moex.connector_host.persistent_session.v1" &&
         *schema != "moex.connector_host.persistent_session.v2") ||
        !phase || !epoch || !base_run_id || !profile_id || !profile_fingerprint || !run_id || !ext_id || !add_user_id ||
        !cancel_user_id || !recovery_user_id || !side || !price || !base_contract_code || !comment || !quantity ||
        !isin_id || !instrument_mask || !plan_sha256 || !add_payload_sha256 || !recovery_payload_sha256 ||
        (*side != "buy" && *side != "sell")) {
        error = "persistent session checkpoint is incomplete or has an unsupported schema";
        return false;
    }
    if (*phase != "idle" && *phase != "authorized" && *phase != "add_may_have_been_sent") {
        error = "persistent session checkpoint has an unsupported phase";
        return false;
    }
    checkpoint.schema = *schema;
    if (*schema == "moex.connector_host.persistent_session.v1") {
        // V1 also lacks a durable recovered-reply-ID high-water mark when idle.
        error = "v1 checkpoint lacks safe restart identity and reply-ID reservation history";
        return false;
    } else {
        const auto account = checkpoint_string_field(text, "account_sha256");
        const auto session = checkpoint_integer_field<std::int32_t>(text, "sess_id");
        const auto binding = checkpoint_string_field(text, "session_binding_sha256");
        const auto generation = checkpoint_integer_field<std::uint64_t>(text, "authorized_generation");
        const auto known = checkpoint_integer_field<std::int64_t>(text, "known_order_id");
        const auto remaining = checkpoint_integer_field<std::int64_t>(text, "known_remaining");
        const auto state = checkpoint_integer_field<std::int32_t>(text, "lifecycle_state");
        const auto next = checkpoint_integer_field<std::uint32_t>(text, "next_recovered_user_id");
        if (!account || !session || !binding || !generation || !known || !remaining || !state || !next ||
            account->size() != 64 || *session <= 0 || *known < 0 || *remaining < -1 ||
            (*phase != "idle" &&
             (binding->size() != 64 || plan_sha256->size() != 64 || add_payload_sha256->size() != 64))) {
            error = "v2 checkpoint recovery identity incomplete";
            return false;
        }
        checkpoint.account_sha256 = *account;
        checkpoint.sess_id = *session;
        checkpoint.session_binding_sha256 = *binding;
        checkpoint.authorized_generation = *generation;
        checkpoint.known_order_id = *known;
        checkpoint.known_remaining = *remaining;
        checkpoint.lifecycle_state = *state;
        checkpoint.next_recovered_user_id = *next;
    }
    checkpoint.phase = *phase;
    checkpoint.epoch_counter = *epoch;
    checkpoint.base_run_id = *base_run_id;
    checkpoint.profile_id = *profile_id;
    checkpoint.profile_fingerprint = *profile_fingerprint;
    checkpoint.run_id = *run_id;
    checkpoint.ext_id = *ext_id;
    checkpoint.add_user_id = *add_user_id;
    checkpoint.cancel_user_id = *cancel_user_id;
    checkpoint.recovery_user_id = *recovery_user_id;
    checkpoint.side = *side == "buy" ? Plaza2TradeSide::Buy : Plaza2TradeSide::Sell;
    checkpoint.price = *price;
    checkpoint.base_contract_code = *base_contract_code;
    checkpoint.comment = *comment;
    checkpoint.quantity = *quantity;
    checkpoint.isin_id = *isin_id;
    if (*instrument_mask < std::numeric_limits<std::int8_t>::min() ||
        *instrument_mask > std::numeric_limits<std::int8_t>::max()) {
        error = "persistent session checkpoint instrument mask is out of range";
        return false;
    }
    checkpoint.instrument_mask = static_cast<std::int8_t>(*instrument_mask);
    checkpoint.plan_sha256 = *plan_sha256;
    checkpoint.add_payload_sha256 = *add_payload_sha256;
    checkpoint.recovery_payload_sha256 = *recovery_payload_sha256;
    if (checkpoint.schema.ends_with(".v2") && checkpoint_json(checkpoint) != text) {
        error = "v2 checkpoint is not canonical (duplicate, unknown or malformed fields)";
        return false;
    }
    return true;
}

std::optional<std::int32_t> checked_i32_add(std::int32_t base, std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
        return std::nullopt;
    const auto value = static_cast<std::int64_t>(base) + static_cast<std::int64_t>(offset);
    if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
        return std::nullopt;
    return static_cast<std::int32_t>(value);
}

std::optional<std::uint32_t> checked_u32_add(std::uint32_t base, std::uint64_t offset) {
    if (offset > std::numeric_limits<std::uint32_t>::max() - base)
        return std::nullopt;
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(base) + offset);
}

std::optional<std::uint64_t> checked_u64_mul(std::uint64_t left, std::uint64_t right) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
        return std::nullopt;
    return left * right;
}
} // namespace

std::string_view host_state_name(ConnectorHostState state) noexcept {
    switch (state) {
    case ConnectorHostState::Created:
        return "Created";
    case ConnectorHostState::Started:
        return "Started";
    case ConnectorHostState::Ready:
        return "Ready";
    case ConnectorHostState::Stopping:
        return "Stopping";
    case ConnectorHostState::Stopped:
        return "Stopped";
    case ConnectorHostState::Recovering:
        return "Recovering";
    case ConnectorHostState::Failed:
        return "Failed";
    }
    return "Failed";
}

struct ConnectorHost::Impl {
    explicit Impl(Plaza2HostConfig value)
        : config(std::move(value)), transport(config.transport), epoch_base_run_id(config.order.run_id),
          epoch_base_ext_id(config.order.ext_id), epoch_base_add_user_id(config.order.add_user_id),
          epoch_base_cancel_user_id(config.order.cancel_user_id),
          epoch_base_recovery_user_id(config.order.recovery_user_id),
          checkpoint_path(config.order.journal_root / "persistent_session.json") {
        std::random_device random;
        process_identity = cg::plaza2_sha256_hex(std::to_string(random()) + ":" + std::to_string(random()) + ":" +
                                                 std::to_string(random()) + ":" + std::to_string(random()));
        load_checkpoint();
        transport.configure_recovered_reservations(next_recovered_user_id,
                                                   [this](std::uint32_t next, std::string& error) {
                                                       next_recovered_user_id = next;
                                                       auto checkpoint = retained_checkpoint;
                                                       if (!checkpoint) {
                                                           error = "durable epoch checkpoint required for reservation";
                                                           return false;
                                                       }
                                                       checkpoint->next_recovered_user_id = next;
                                                       if (!persist_checkpoint(*checkpoint, error))
                                                           return false;
                                                       return true;
                                                   });
    }
    ~Impl() {
        if (checkpoint_lock_fd >= 0)
            ::close(checkpoint_lock_fd);
    }
    int checkpoint_lock_fd{-1};
    bool acquire_checkpoint_lock(std::string& error) {
        if (checkpoint_lock_fd >= 0)
            return true;
        std::error_code ec;
        std::filesystem::create_directories(checkpoint_path.parent_path(), ec);
        if (ec) {
            error = "checkpoint directory unavailable";
            return false;
        }
        const int fd = ::open((checkpoint_path.string() + ".lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
        if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            if (fd >= 0)
                ::close(fd);
            error = "persistent checkpoint already owned or lock unavailable";
            return false;
        }
        checkpoint_lock_fd = fd;
        return true;
    }
    Plaza2HostConfig config;
    Plaza2TestTradeTransport transport;
    SystemOrderLifecycleClock clock;
    std::unique_ptr<PersistentOrderController> persistent;
    std::uint64_t epoch_counter{0};
    std::string epoch_base_run_id;
    std::int32_t epoch_base_ext_id{0};
    std::uint32_t epoch_base_add_user_id{0};
    std::uint32_t epoch_base_cancel_user_id{0};
    std::uint32_t epoch_base_recovery_user_id{0};
    std::filesystem::path checkpoint_path;
    std::string process_identity;
    std::string restart_checkpoint_sha;
    std::optional<PersistentSessionCheckpoint> retained_checkpoint;
    std::uint32_t next_recovered_user_id{std::numeric_limits<std::uint32_t>::max()};
    bool restart_recovery_only{false};
    bool first_order_fill_incident{false};
    bool checkpoint_blocked{false};
    bool recovered_epoch_active{false};
    std::string checkpoint_error;
    std::optional<PreSendPlan> persistent_plan;
    std::optional<OrderLifecycleConfig> persistent_order;
    ConnectorHostState state{ConnectorHostState::Created};
    std::string error;
    cg::Plaza2Error causal_error;
    std::uint64_t causal_error_time_ns{};
    std::string causal_operation;
    Plaza2TransportHealth causal_health;
    std::string causal_callback_error;
    std::string authorized_sha;
    bool submitted{false};
    std::optional<OrderLifecycleResult> result;

    ConnectorHostOrderRequest request_from_config() const {
        return {.side = config.order.side,
                .price = config.order.price,
                .base_contract_code = config.order.base_contract_code,
                .comment = config.order.comment,
                .quantity = config.order.quantity};
    }

    OrderLifecycleConfig order_for_request(const ConnectorHostOrderRequest& request) const {
        auto value = config.order;
        value.side = request.side;
        value.price = request.price;
        value.base_contract_code = request.base_contract_code;
        value.comment = request.comment;
        value.quantity = request.quantity;
        value.order_type = Plaza2TradeOrderType::Limit;
        return value;
    }

    bool assign_active_identity(OrderLifecycleConfig& order, std::uint64_t epoch, std::string& error) const {
        if (epoch == 0) {
            error = "persistent epoch counter is exhausted";
            return false;
        }
        const auto suffix = epoch - 1;
        const auto user_offset = checked_u64_mul(suffix, 16);
        const auto ext_id = checked_i32_add(epoch_base_ext_id, suffix);
        const auto add_user_id = user_offset ? checked_u32_add(epoch_base_add_user_id, *user_offset) : std::nullopt;
        const auto cancel_user_id =
            user_offset ? checked_u32_add(epoch_base_cancel_user_id, *user_offset) : std::nullopt;
        const auto recovery_user_id =
            user_offset ? checked_u32_add(epoch_base_recovery_user_id, *user_offset) : std::nullopt;
        if (!user_offset || !ext_id || !add_user_id || !cancel_user_id || !recovery_user_id) {
            error = "persistent identifier space exhausted";
            return false;
        }
        order.run_id = epoch_base_run_id + "-epoch-" + std::to_string(epoch);
        order.ext_id = *ext_id;
        order.add_user_id = *add_user_id;
        order.cancel_user_id = *cancel_user_id;
        order.recovery_user_id = *recovery_user_id;
        return true;
    }

    bool assign_next_identity(OrderLifecycleConfig& order, std::string& error) const {
        const auto user_offset = checked_u64_mul(epoch_counter, 16);
        const auto ext_id = checked_i32_add(epoch_base_ext_id, epoch_counter);
        const auto add_user_id = user_offset ? checked_u32_add(epoch_base_add_user_id, *user_offset) : std::nullopt;
        const auto cancel_user_id =
            user_offset ? checked_u32_add(epoch_base_cancel_user_id, *user_offset) : std::nullopt;
        const auto recovery_user_id =
            user_offset ? checked_u32_add(epoch_base_recovery_user_id, *user_offset) : std::nullopt;
        if (!user_offset || !ext_id || !add_user_id || !cancel_user_id || !recovery_user_id) {
            error = "persistent identifier space exhausted";
            return false;
        }
        order.run_id = epoch_base_run_id;
        order.ext_id = *ext_id;
        order.add_user_id = *add_user_id;
        order.cancel_user_id = *cancel_user_id;
        order.recovery_user_id = *recovery_user_id;
        return true;
    }

    PersistentSessionCheckpoint checkpoint_for(std::string phase, const OrderLifecycleConfig& order,
                                               const PreSendPlan* plan) const {
        PersistentSessionCheckpoint checkpoint;
        checkpoint.phase = std::move(phase);
        checkpoint.account_sha256 = cg::plaza2_sha256_hex(order.broker_code + order.client_code);
        checkpoint.sess_id = config.transport.target_session_id;
        checkpoint.next_recovered_user_id = next_recovered_user_id;
        if (order.session_price_binding) {
            checkpoint.session_binding_sha256 =
                cg::plaza2_sha256_hex(session_price_binding_json(*order.session_price_binding));
            checkpoint.authorized_generation = order.session_price_binding->transport_generation;
        }
        if (persistent) {
            const auto& result = persistent->last_result();
            checkpoint.lifecycle_state = static_cast<std::int32_t>(result.state);
            if (result.add_reply && result.add_reply->accepted && result.add_reply->order_id)
                checkpoint.known_order_id = *result.add_reply->order_id;
            if (result.observation && !result.observation->identity_conflict) {
                checkpoint.known_order_id = result.observation->private_order_id;
                checkpoint.known_remaining = result.observation->remaining_quantity;
            }
        }
        checkpoint.epoch_counter = epoch_counter;
        checkpoint.base_run_id = epoch_base_run_id;
        checkpoint.profile_id = order.profile_id;
        checkpoint.profile_fingerprint = order.profile_fingerprint;
        checkpoint.run_id = order.run_id;
        checkpoint.ext_id = order.ext_id;
        checkpoint.add_user_id = order.add_user_id;
        checkpoint.cancel_user_id = order.cancel_user_id;
        checkpoint.recovery_user_id = order.recovery_user_id;
        checkpoint.side = order.side;
        checkpoint.price = order.price;
        checkpoint.base_contract_code = order.base_contract_code;
        checkpoint.comment = order.comment;
        checkpoint.quantity = order.quantity;
        checkpoint.isin_id = order.isin_id;
        checkpoint.instrument_mask = order.instrument_mask;
        if (plan != nullptr) {
            checkpoint.plan_sha256 = plan->sha256;
            checkpoint.add_payload_sha256 = cg::plaza2_sha256_hex(plan->add_command.payload);
            checkpoint.recovery_payload_sha256 = cg::plaza2_sha256_hex(plan->exact_ext_id_recovery_command.payload);
        }
        return checkpoint;
    }

    bool write_checkpoint(std::string phase, const OrderLifecycleConfig& order, const PreSendPlan* plan,
                          std::string& error) {
        const auto checkpoint = checkpoint_for(std::move(phase), order, plan);
        return persist_checkpoint(checkpoint, error);
    }

    bool persist_checkpoint(const PersistentSessionCheckpoint& checkpoint, std::string& error) {
        if (!acquire_checkpoint_lock(error)) {
            checkpoint_blocked = true;
            checkpoint_error = error;
            return false;
        }
        if (!std::filesystem::exists(checkpoint_path.string() + ".required") &&
            !write_atomic_file(checkpoint_path.string() + ".required", "checkpoint must remain present\n", error)) {
            checkpoint_blocked = true;
            checkpoint_error = error;
            return false;
        }
        if (!write_atomic_file(checkpoint_path, checkpoint_json(checkpoint), error)) {
            checkpoint_blocked = true;
            checkpoint_error = error;
            return false;
        }
        retained_checkpoint = checkpoint;
        checkpoint_blocked = false;
        checkpoint_error.clear();
        return true;
    }

    void load_checkpoint() {
        if (checkpoint_path.empty() || !std::filesystem::exists(checkpoint_path)) {
            if (std::filesystem::exists(checkpoint_path.string() + ".required")) {
                checkpoint_blocked = true;
                checkpoint_error = "required durable checkpoint is missing";
            }
            return;
        }
        if (!acquire_checkpoint_lock(checkpoint_error)) {
            checkpoint_blocked = true;
            return;
        }
        struct stat metadata;
        if (::lstat(checkpoint_path.c_str(), &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
            (metadata.st_mode & 077) != 0 || metadata.st_size > 65536) {
            checkpoint_blocked = true;
            checkpoint_error = "checkpoint must be bounded protected regular file";
            return;
        }
        std::ifstream input(checkpoint_path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (!input && text.empty()) {
            checkpoint_blocked = true;
            checkpoint_error = "persistent session checkpoint could not be read";
            return;
        }
        PersistentSessionCheckpoint checkpoint;
        std::string parse_error;
        if (!parse_checkpoint(text, checkpoint, parse_error) || checkpoint.base_run_id != epoch_base_run_id ||
            checkpoint.profile_id != config.order.profile_id ||
            checkpoint.profile_fingerprint != config.order.profile_fingerprint) {
            checkpoint_blocked = true;
            checkpoint_error =
                parse_error.empty() ? "persistent session checkpoint identity does not match host" : parse_error;
            return;
        }
        retained_checkpoint = checkpoint;
        restart_checkpoint_sha = cg::plaza2_sha256_hex(text);
        next_recovered_user_id = checkpoint.next_recovered_user_id;
        epoch_counter = checkpoint.epoch_counter;
        const bool active_checkpoint = checkpoint.phase != "idle";
        if (active_checkpoint && checkpoint.epoch_counter == 0) {
            checkpoint_blocked = true;
            checkpoint_error = "persistent session checkpoint has an invalid active epoch counter";
            return;
        }
        const auto suffix = active_checkpoint ? checkpoint.epoch_counter - 1 : checkpoint.epoch_counter;
        const auto user_offset = checked_u64_mul(suffix, 16);
        const auto expected_ext = checked_i32_add(epoch_base_ext_id, suffix);
        const auto expected_add = user_offset ? checked_u32_add(epoch_base_add_user_id, *user_offset) : std::nullopt;
        const auto expected_cancel =
            user_offset ? checked_u32_add(epoch_base_cancel_user_id, *user_offset) : std::nullopt;
        const auto expected_recovery =
            user_offset ? checked_u32_add(epoch_base_recovery_user_id, *user_offset) : std::nullopt;
        const auto expected_run = active_checkpoint
                                      ? epoch_base_run_id + "-epoch-" + std::to_string(checkpoint.epoch_counter)
                                      : epoch_base_run_id;
        if (!user_offset || !expected_ext || !expected_add || !expected_cancel || !expected_recovery ||
            checkpoint.run_id != expected_run || checkpoint.ext_id != *expected_ext ||
            checkpoint.add_user_id != *expected_add || checkpoint.cancel_user_id != *expected_cancel ||
            checkpoint.recovery_user_id != *expected_recovery) {
            checkpoint_blocked = true;
            checkpoint_error = "persistent session checkpoint identifier arithmetic does not match the host";
            return;
        }
        if (checkpoint.phase == "idle") {
            config.order.run_id = checkpoint.base_run_id;
            config.order.ext_id = checkpoint.ext_id;
            config.order.add_user_id = checkpoint.add_user_id;
            config.order.cancel_user_id = checkpoint.cancel_user_id;
            config.order.recovery_user_id = checkpoint.recovery_user_id;
            return;
        }
        if (checkpoint.quantity != config.order.quantity || checkpoint.quantity != 1 ||
            checkpoint.isin_id != config.order.isin_id || checkpoint.sess_id != config.transport.target_session_id ||
            checkpoint.account_sha256 != cg::plaza2_sha256_hex(config.order.broker_code + config.order.client_code) ||
            checkpoint.side != config.order.side || checkpoint.price != config.order.price ||
            checkpoint.instrument_mask != config.order.instrument_mask) {
            checkpoint_blocked = true;
            checkpoint_error = "persistent session checkpoint target identity does not match host";
            return;
        }
        config.order.run_id = checkpoint.run_id;
        config.order.ext_id = checkpoint.ext_id;
        config.order.add_user_id = checkpoint.add_user_id;
        config.order.cancel_user_id = checkpoint.cancel_user_id;
        config.order.recovery_user_id = checkpoint.recovery_user_id;
        config.order.side = checkpoint.side;
        config.order.price = checkpoint.price;
        config.order.base_contract_code = checkpoint.base_contract_code;
        config.order.comment = checkpoint.comment;
        config.order.quantity = checkpoint.quantity;
        restart_recovery_only = true;
        recovered_epoch_active = true;
    }

    ConnectorHostSnapshot snapshot() const {
        ConnectorHostSnapshot out;
        const auto& host = transport.host();
        const auto& data = host.private_state();
        out.state = host.recovering() ? ConnectorHostState::Recovering : state;
        out.recovery = transport.host().recovery_status();
        out.environment = config.transport.host.runtime.environment;
        out.mode = config.transport.host.mode;
        out.target_isin_id = config.transport.target_isin_id;
        out.session_id = config.transport.target_session_id;
        out.runtime_compatibility = cg::plaza2_compatibility_name(host.probe_report().compatibility);
        out.runtime_scheme_sha256 = host.probe_report().scheme_drift.runtime_scheme_sha256;
        out.transport_health = host.runtime_health();
        const auto& health = out.transport_health;
        const bool live = host.started() && state != ConnectorHostState::Failed &&
                          state != ConnectorHostState::Recovering && state != ConnectorHostState::Stopping &&
                          state != ConnectorHostState::Stopped && health.all_active();
        out.publisher_handle_open = host.publisher_open();
        out.reply_handle_open = host.p2mqreply_open();
        out.publisher_ready = live && out.publisher_handle_open && health.publisher == 3;
        out.reply_ready = live && out.reply_handle_open && health.reply == 3;
        out.aggr_snapshot_state_ready = host.aggr_online() && host.aggr_snapshot_complete();
        out.aggr_ready = live && health.aggr == 3 && out.aggr_snapshot_state_ready;
        out.publisher_calls = host.publisher_call_counts();
        out.streams.assign(data.stream_health().begin(), data.stream_health().end());
        constexpr std::array required{StreamCode::kFortsTradeRepl,
                                      StreamCode::kFortsUserorderbookRepl,
                                      StreamCode::kFortsPosRepl,
                                      StreamCode::kFortsPartRepl,
                                      StreamCode::kFortsRefdataRepl,
                                      StreamCode::kFortsSessionstateRepl,
                                      StreamCode::kFortsInstrumentstateRepl};
        out.private_snapshot_state_ready = std::all_of(required.begin(), required.end(), [&](auto code) {
            return std::count_if(out.streams.begin(), out.streams.end(), [&](const auto& row) {
                       return row.stream_code == code && row.online && row.snapshot_complete;
                   }) == 1;
        });
        out.private_streams_ready = live && health.private_active && out.private_snapshot_state_ready;
        for (const auto& row : out.streams) {
            if (row.stream_code == StreamCode::kFortsUserorderbookRepl)
                out.uob_periodic_consistent = row.periodic_snapshot_consistent;
        }
        const auto evidence = transport.inspect_target_evidence(config.order.client_code);
        out.refdata_lifenum = evidence.target_refdata_lifenum;
        out.target_refdata_provenance_ready = evidence.target_refdata_provenance_ready;
        out.fut_instruments_provenance = evidence.target_fut_instruments_provenance;
        out.fut_sess_contents_provenance = evidence.target_fut_sess_contents_provenance;
        out.session_provenance = evidence.target_session_provenance;
        out.position_evidence_class = evidence.position_evidence_class;
        out.zero_starting_position_proven = evidence.zero_starting_position_proven;
        out.pos_trades_rev = evidence.position_trades_rev;
        out.pos_trades_lifenum = evidence.position_trades_lifenum;
        out.trade_anchor = evidence.trade_replay_anchor_used;
        out.trade_replay_complete = evidence.trade_replay_complete;
        out.active_own_order_count = evidence.active_own_order_count;
        bool membership = false;
        for (const auto& row : data.instruments()) {
            if (row.isin_id != out.target_isin_id)
                continue;
            out.target = row.isin;
            out.min_step = row.min_step;
            if (row.has_current_status)
                out.instrument_status = row.current_status;
            membership = row.kind == ps::InstrumentKind::kFuture && row.current_session_member &&
                         row.sess_id == out.session_id && row.trade_mode_id != 0 && !row.min_step.empty();
        }
        for (const auto& row : data.sessions()) {
            if (row.sess_id == out.session_id && row.has_current_status)
                out.session_status = row.current_status;
        }
        const auto participant = config.order.broker_code + config.order.client_code;
        const auto limit = data.find_limit_by_code(participant);
        out.participant_limit_row_present = limit.match_count != 0;
        out.participant_identity_exact = limit.match_count == 1 && limit.exact != nullptr &&
                                         limit.exact->participant_kind == ps::LimitParticipantKind::Client;
        out.exchange_money_limit_check_enabled = limit.exact != nullptr && limit.exact->limits_set;
        out.limits_set = out.participant_identity_exact && out.exchange_money_limit_check_enabled;
        out.order_epoch_active = recovered_epoch_active || (persistent != nullptr && persistent->active());
        out.order_authorized = persistent != nullptr && persistent->authorized();
        out.order_submission_attempted =
            recovered_epoch_active || (persistent != nullptr && persistent->submission_attempted());
        if (const auto bbo = host.aggr20_projector().snapshot_for_isin(out.target_isin_id)) {
            if (bbo->top_bid)
                out.bid = bbo->top_bid->price;
            if (bbo->top_ask)
                out.ask = bbo->top_ask->price;
            out.target_aggr20_uncrossed =
                bbo->top_bid && bbo->top_ask && bbo->top_bid->price_scaled < bbo->top_ask->price_scaled;
            out.bbo_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                   bbo->committed_at)
                                 .count();
        }
        const auto max_age = config.order.policy.max_aggr20_age_ms;
        out.observation_ready = host.started() && state != ConnectorHostState::Failed && out.publisher_ready &&
                                out.reply_ready && host.probe_report().trading_capable && out.private_streams_ready &&
                                out.uob_periodic_consistent && out.target_refdata_provenance_ready && membership &&
                                out.session_status == 1 && out.instrument_status == 1 && out.limits_set &&
                                out.trade_replay_complete && out.zero_starting_position_proven &&
                                out.active_own_order_count == 0 && out.aggr_ready && out.target_aggr20_uncrossed &&
                                out.bbo_age_ms >= 0 && max_age > 0 && max_age <= 5000 &&
                                static_cast<std::uint64_t>(out.bbo_age_ms) <= max_age;
        const auto price_gate = inspect_session_price(
            data.find_future_session_terms(out.target_isin_id), out.target_isin_id, out.session_id, out.refdata_lifenum,
            out.private_streams_ready && out.target_refdata_provenance_ready, config.order.price);
        out.session_terms_present = price_gate.session_terms_present;
        out.session_terms_current = price_gate.session_terms_current;
        out.session_price_bounds_valid = price_gate.session_price_bounds_valid;
        out.order_price_within_exchange_bounds = price_gate.order_price_within_exchange_bounds;
        out.order_price_tick_aligned = price_gate.order_price_tick_aligned;
        out.new_order_allowed = config.purpose == HostPurpose::OrderTest && out.observation_ready &&
                                price_gate.allowed() && persistent == nullptr && !recovered_epoch_active &&
                                !checkpoint_blocked && !restart_recovery_only && !first_order_fill_incident &&
                                authorized_sha.empty() && !submitted &&
                                epoch_counter != std::numeric_limits<std::uint64_t>::max();
        if (config.order.policy.version == kFirstOrderDeepPassiveVersion) {
            const auto terms = data.find_future_session_terms(out.target_isin_id);
            const auto price = ps::parse_session_decimal(config.order.price);
            const auto bid = ps::parse_session_decimal(out.bid), ask = ps::parse_session_decimal(out.ask);
            out.new_order_allowed &=
                terms && price && bid && ask &&
                deep_passive_price_allowed(*terms, price->units, static_cast<int>(config.order.side), bid->units,
                                           ask->units, out.bbo_age_ms);
        }
        out.last_error = error;
        out.causal_error = causal_error;
        out.causal_error_time_ns = causal_error_time_ns;
        out.causal_operation = causal_operation;
        out.causal_health = causal_health;
        out.causal_callback_error = causal_callback_error;
        if (out.state == ConnectorHostState::Ready && !out.observation_ready)
            out.state = ConnectorHostState::Started;
        if (out.last_error.empty() && first_order_fill_incident)
            out.last_error = "FIRST_ORDER_FILLED: new-order authority frozen; preserve actual position";
        if (out.last_error.empty() && !checkpoint_error.empty())
            out.last_error = checkpoint_error;
        if (out.last_error.empty() && recovered_epoch_active)
            out.last_error = "RestartOrderRecoveryRequired";
        if (out.last_error.empty() && host.started() && !out.observation_ready)
            out.last_error = "OBSERVATION_NOT_READY";
        if (persistent != nullptr) {
            const auto& current = persistent->last_result();
            out.lifecycle_state = persistent->active() ? std::optional(current.state) : std::nullopt;
            out.add_reply = current.add_reply;
            out.cancel_reply = current.cancel_reply;
            out.market_safe = current.market_safe_terminal;
            out.evidence_consistent = current.evidence_consistent;
            if (current.observation) {
                const auto& row = *current.observation;
                out.order_id = row.public_order_id != 0 ? row.public_order_id : row.private_order_id;
                out.original_quantity = row.original_quantity;
                out.remaining_quantity = row.remaining_quantity;
                out.executed_quantity = row.executed_quantity;
            } else if (current.add_reply && current.add_reply->order_id.has_value()) {
                out.order_id = *current.add_reply->order_id;
            }
        } else if (result) {
            out.lifecycle_state = result->state;
            out.add_reply = result->add_reply;
            out.cancel_reply = result->cancel_reply;
            out.market_safe = result->market_safe_terminal;
            out.evidence_consistent = result->evidence_consistent;
            if (result->observation) {
                const auto& row = *result->observation;
                out.order_id = row.public_order_id != 0 ? row.public_order_id : row.private_order_id;
                out.original_quantity = row.original_quantity;
                out.remaining_quantity = row.remaining_quantity;
                out.executed_quantity = row.executed_quantity;
            }
        }
        return out;
    }

    RecoveredOrderKey recovered_key() const {
        const auto& c = persistent_order ? *persistent_order : config.order;
        RecoveredOrderKey key;
        key.epoch = c.run_id;
        key.account = c.broker_code + c.client_code;
        key.isin_id = c.isin_id;
        key.sess_id = config.transport.target_session_id;
        key.ext_id = c.ext_id;
        key.side = c.side;
        key.price = c.price;
        key.quantity = c.quantity;
        if (retained_checkpoint)
            key.known_exchange_id = retained_checkpoint->known_order_id;
        if (persistent) {
            const auto& result = persistent->last_result();
            if (result.add_reply && result.add_reply->accepted && result.add_reply->order_id)
                key.known_exchange_id = *result.add_reply->order_id;
            else if (result.observation)
                key.known_exchange_id = result.observation->private_order_id;
        }
        return key;
    }

    OrderLifecycleConfig current_order() const {
        return current_order(request_from_config());
    }

    OrderLifecycleConfig current_order(const ConnectorHostOrderRequest& request) const {
        auto value = order_for_request(request);
        const auto view = snapshot();
        value.require_session_price_binding = true;
        value.session_price_binding.reset();
        if (view.private_streams_ready && view.target_refdata_provenance_ready) {
            if (const auto terms = transport.host().private_state().find_future_session_terms(
                    value.isin_id, view.session_id, view.refdata_lifenum))
                value.session_price_binding = SessionPriceBinding{view.recovery.generation, *terms};
        }
        value.first_order_bbo.reset();
        if (value.policy.version == kFirstOrderDeepPassiveVersion) {
            if (const auto book = transport.host().aggr20_projector().snapshot_for_isin(value.isin_id);
                book && book->top_bid && book->top_ask &&
                std::chrono::steady_clock::now() - book->committed_at <=
                    std::chrono::milliseconds(kFirstOrderBboMaxAgeMs)) {
                const auto bid = ps::parse_session_decimal(book->top_bid->price);
                const auto ask = ps::parse_session_decimal(book->top_ask->price);
                if (bid && ask)
                    value.first_order_bbo = DeepPassiveBboBinding{
                        bid->units,
                        ask->units,
                        book->last_repl_id,
                        book->last_repl_rev,
                        std::chrono::duration_cast<std::chrono::nanoseconds>(book->committed_at.time_since_epoch())
                            .count(),
                        book->exchange_moment,
                        book->exchange_moment_ns};
            }
        }
        value.smoke = {};
        value.smoke.instrument_exists = view.target_refdata_provenance_ready;
        value.smoke.tradable_session = view.session_status == 1 && view.instrument_status == 1;
        value.smoke.aggr20_two_sided = view.target_aggr20_uncrossed;
        value.smoke.limits_snapshot_applicable = view.limits_set;
        value.smoke.tick_size = view.min_step;
        value.smoke.top_bid = view.bid;
        value.smoke.top_ask = view.ask;
        value.smoke.aggr20_age_ms = view.bbo_age_ms < 0 ? 5001 : static_cast<std::uint64_t>(view.bbo_age_ms);
        value.smoke.market_data_source = "FORTS_AGGR20_REPL";
        if (const auto bbo = transport.host().aggr20_projector().snapshot_for_isin(view.target_isin_id)) {
            value.smoke.aggr20_source_sequence = bbo->last_repl_id;
            value.smoke.aggr20_source_revision = bbo->last_repl_rev;
        }
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm utc{};
        gmtime_r(&now, &utc);
        std::ostringstream timestamp;
        timestamp << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
        value.smoke.aggr20_observed_at_utc = timestamp.str();
        // Date of this observation, not an inferred exchange session ID.
        value.smoke.trading_day = timestamp.str().substr(0, 10);
        value.smoke.session_id = std::to_string(view.session_id);
        value.smoke.session_state = "current-status";
        value.smoke.refdata_source = "FORTS_REFDATA_REPL.target_rows";
        value.smoke.refdata_source_sequence = view.refdata_lifenum;
        value.smoke.refdata_source_revision = view.fut_instruments_provenance.repl_rev;
        value.smoke.limits_source = "FORTS_PART_REPL";
        for (const auto& stream : view.streams) {
            if (stream.stream_code == StreamCode::kFortsPartRepl)
                value.smoke.limits_commit_sequence = stream.last_commit_sequence;
        }
        return value;
    }

    cg::Plaza2Error authorize_candidate(const PreSendPlan& candidate, const OrderLifecycleConfig& order_config,
                                        std::string_view canonical, std::string_view sha) {
        if (candidate.canonical_json != canonical || candidate.sha256 != sha) {
            error = "exact current canonical plan and authorization SHA required";
            return invalid(error);
        }
        const auto& c = order_config;
        Plaza2AuthorizedOrderIntent intent;
        intent.session_price_binding = c.session_price_binding;
        intent.first_order_bbo = c.first_order_bbo;
        intent.sha256 = candidate.sha256;
        intent.canonical_json = candidate.canonical_json;
        intent.profile_id = c.profile_id;
        intent.profile_fingerprint = c.profile_fingerprint;
        intent.add_payload_sha256 = cg::plaza2_sha256_hex(candidate.add_command.payload);
        intent.recovery_payload_sha256 = cg::plaza2_sha256_hex(candidate.exact_ext_id_recovery_command.payload);
        intent.isin_id = c.isin_id;
        intent.base_contract_code = c.base_contract_code;
        intent.side = c.side;
        intent.quantity = c.quantity;
        intent.price = c.price;
        intent.comment = c.comment;
        intent.ext_id = c.ext_id;
        intent.add_user_id = c.add_user_id;
        intent.cancel_user_id = c.cancel_user_id;
        intent.recovery_user_id = c.recovery_user_id;
        intent.instrument_mask = c.instrument_mask;
        intent.broker_code = c.broker_code;
        intent.client_code = c.client_code;
        intent.broker_code_sha256 = cg::plaza2_sha256_hex(c.broker_code);
        intent.client_code_sha256 = cg::plaza2_sha256_hex(c.client_code);
        intent.policy_version = c.policy.version;
        intent.policy_sha256 = c.policy.sha256;
        intent.max_distance_ticks = c.policy.max_distance_ticks;
        intent.max_aggr20_age_ms = c.policy.max_aggr20_age_ms;
        intent.require_zero_starting_position = c.policy.require_zero_starting_position;
        if (auto value = transport.install_authorized_intent(std::move(intent)))
            return value;
        if (auto value = transport.bind_authorized_plan(candidate))
            return value;
        authorized_sha = candidate.sha256;
        error.clear();
        return {};
    }
};

ConnectorHost::ConnectorHost(Plaza2HostConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
ConnectorHost::~ConnectorHost() {
    (void)stop();
}

cg::Plaza2Error ConnectorHost::start() {
    auto& p = *impl_;
    if (p.state != ConnectorHostState::Created)
        return invalid("host start is one-shot");
    if (p.checkpoint_blocked)
        return invalid(p.checkpoint_error.empty() ? "persistent session checkpoint blocks startup"
                                                  : p.checkpoint_error);
    const auto& c = p.config;
    const auto& arms = c.transport.host.arm_state;
    if (c.transport.host.runtime.environment != cg::Plaza2Environment::Test ||
        c.order.environment != cg::Plaza2Environment::Test || c.transport.target_isin_id != c.order.isin_id ||
        c.order.isin_id <= 0 || c.transport.target_session_id <= 0 || c.transport.authorized_intent ||
        !c.transport.host.trade_replay_from_pos_anchor ||
        c.transport.observation_client_code != c.order.broker_code + c.order.client_code ||
        c.order.broker_code.empty() || c.order.client_code.empty()) {
        p.state = ConnectorHostState::Failed;
        p.error = "invalid TEST host target/account/anchor configuration";
        return invalid(p.error);
    }
    if ((c.purpose == HostPurpose::Qualify &&
         (arms.test_order_send_armed || c.transport.host.mode == Plaza2TestSessionHostMode::LiveTestAuthorizedSend)) ||
        (c.purpose == HostPurpose::OrderTest &&
         (c.transport.host.mode != Plaza2TestSessionHostMode::LiveTestAuthorizedSend || !arms.test_network_armed ||
          !arms.test_session_armed || !arms.test_plaza2_armed || !arms.test_order_send_armed))) {
        p.state = ConnectorHostState::Failed;
        p.error = "host purpose/send arms mismatch";
        return invalid(p.error);
    }
    if (p.restart_recovery_only) {
        const auto& checkpoint = *p.retained_checkpoint;
        Plaza2AuthorizedOrderIntent identity;
        identity.sha256 = checkpoint.plan_sha256;
        identity.profile_id = checkpoint.profile_id;
        identity.profile_fingerprint = checkpoint.profile_fingerprint;
        identity.broker_code = c.order.broker_code;
        identity.client_code = c.order.client_code;
        identity.isin_id = checkpoint.isin_id;
        identity.ext_id = checkpoint.ext_id;
        identity.side = checkpoint.side;
        identity.price = checkpoint.price;
        identity.quantity = checkpoint.quantity;
        identity.add_user_id = checkpoint.add_user_id;
        identity.cancel_user_id = checkpoint.cancel_user_id;
        identity.recovery_user_id = checkpoint.recovery_user_id;
        p.transport.restore_recovery_epoch(
            std::move(identity),
            "{\"schema\":" + quoted(std::string_view(checkpoint.schema)) +
                ",\"checkpoint_sha256\":" + quoted(std::string_view(p.restart_checkpoint_sha)) +
                ",\"epoch\":" + quoted(std::string_view(checkpoint.run_id)) +
                ",\"process_identity\":" + quoted(std::string_view(p.process_identity)) + "}",
            checkpoint.known_order_id);
        auto restored_order = c.order;
        restored_order.journal_root /= "restart-" + p.process_identity;
        p.persistent = std::make_unique<PersistentOrderController>(restored_order, p.transport, p.clock);
        if (auto error = p.persistent->restore_recovery_only(checkpoint.add_payload_sha256,
                                                             checkpoint.recovery_payload_sha256)) {
            p.checkpoint_blocked = true;
            p.checkpoint_error = error.message;
            return error;
        }
        p.persistent_order = c.order;
    }
    if (auto error = p.transport.host().start()) {
        p.state = ConnectorHostState::Failed;
        p.error = error.message;
        p.causal_error = error;
        p.causal_error_time_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        p.causal_health = p.transport.host().runtime_health();
        p.causal_callback_error = p.transport.host().last_callback_error();
        p.causal_operation = "start";
        return error;
    }
    p.state = ConnectorHostState::Started;
    return {};
}

cg::Plaza2Error ConnectorHost::poll() {
    auto& p = *impl_;
    if (p.state != ConnectorHostState::Started && p.state != ConnectorHostState::Ready &&
        p.state != ConnectorHostState::Recovering)
        return invalid("poll requires a running host");
    if (auto error = p.transport.host().poll()) {
        p.state = ConnectorHostState::Failed;
        p.error = error.message;
        p.causal_error = error;
        p.causal_error_time_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        const auto& failure = p.transport.host().recovery_status();
        p.causal_health = failure.cause ? failure.health : p.transport.host().runtime_health();
        p.causal_callback_error = p.transport.host().last_callback_error();
        p.causal_operation = "poll";
        return error;
    }
    const auto& recovery = p.transport.host().recovery_status();
    if (recovery.cause) {
        p.causal_error = recovery.cause;
        p.causal_error_time_ns = recovery.error_time_ns;
        p.causal_health = recovery.health;
        p.causal_operation = "transport rebootstrap";
    }
    p.state = p.transport.host().recovering() ? ConnectorHostState::Recovering : ConnectorHostState::Started;
    if (p.snapshot().observation_ready)
        p.state = ConnectorHostState::Ready;
    return {};
}

cg::Plaza2Error ConnectorHost::stop() {
    auto& p = *impl_;
    if (p.state == ConnectorHostState::Stopped)
        return {};
    if ((p.persistent != nullptr && p.persistent->active()) || p.recovered_epoch_active)
        return invalid("finish or resolve the current order epoch before stopping the session host");
    p.state = ConnectorHostState::Stopping;
    if (auto error = p.transport.host().stop()) {
        p.state = ConnectorHostState::Failed;
        p.error = "runtime stop failed";
        return invalid(p.error);
    }
    p.state = ConnectorHostState::Stopped;
    if (p.checkpoint_lock_fd >= 0) {
        ::close(p.checkpoint_lock_fd);
        p.checkpoint_lock_fd = -1;
    }
    return {};
}

DeepPassiveProposal ConnectorHost::first_order_price_proposal() const {
    const auto order = impl_->current_order();
    if (!order.session_price_binding || !order.first_order_bbo)
        return {};
    return propose_deep_passive(order.session_price_binding->terms, order.first_order_bbo->bid_units,
                                order.first_order_bbo->ask_units, static_cast<std::int64_t>(order.smoke.aggr20_age_ms));
}

ConnectorHostSnapshot ConnectorHost::snapshot() const {
    return impl_->snapshot();
}

ConnectorHostQualificationSnapshot ConnectorHost::qualification_snapshot(bool private_identity) const {
    const auto& host = impl_->transport.host();
    const auto instruments = host.private_state().instruments();
    const auto positions = host.private_state().positions();
    ConnectorHostQualificationSnapshot out{.book = host.aggr20_projector().snapshot(),
                                           .instruments = {instruments.begin(), instruments.end()},
                                           .positions = {positions.begin(), positions.end()},
                                           .rate = host.publisher_rate_metrics(),
                                           .aggr_online = host.aggr_online(),
                                           .aggr_snapshot_complete = host.aggr_snapshot_complete()};
    for (const auto& row : host.private_state().own_orders()) {
        if (row.identity_conflict || ((row.from_user_book || row.from_current_day) &&
                                      (row.public_amount_rest > 0 || row.private_amount_rest > 0)))
            out.active_orders.push_back(row);
    }
    const auto& data = host.private_state();
    out.visible_limit_rows = data.limit_row_count();
    out.unknown_limit_rows = data.unknown_limit_row_count();
    const auto broker = data.find_limit_by_code(impl_->config.order.broker_code);
    const auto client = data.find_limit_by_code(impl_->config.order.broker_code + impl_->config.order.client_code);
    out.matching_broker_limit_rows = broker.match_count;
    out.matching_client_limit_rows = client.match_count;
    out.client_code_is_brokerage_account = impl_->config.order.client_code == "000";
    if (broker.exact)
        out.broker_limit = *broker.exact;
    if (client.exact)
        out.client_limit = *client.exact;
    for (const auto& row : data.limits())
        out.limit_diagnostics.push_back(
            {row.repl_id, row.participant_kind, row.account_code.size(),
             row.account_code == impl_->config.order.broker_code,
             row.account_code == impl_->config.order.broker_code + impl_->config.order.client_code, row.limits_set,
             row.is_auto_update_limit, row.money_free, row.money_blocked, row.money_amount,
             private_identity ? row.account_code : std::string{}});
    return out;
}

PreSendPlan ConnectorHost::plan() const {
    return plan_order(impl_->request_from_config());
}

PreSendPlan ConnectorHost::plan_order(const ConnectorHostOrderRequest& request) const {
    const auto& p = *impl_;
    if (p.checkpoint_blocked || p.restart_recovery_only || p.first_order_fill_incident || p.recovered_epoch_active ||
        p.persistent != nullptr) {
        return {.failure = PreSendFailure::JournalFailure,
                .message = p.checkpoint_blocked ? "PERSISTENT_SESSION_CHECKPOINT_BLOCKED"
                                                : "PERSISTENT_SESSION_EPOCH_REQUIRES_RECONCILIATION"};
    }
    if (p.epoch_counter == std::numeric_limits<std::uint64_t>::max())
        return {.failure = PreSendFailure::JournalFailure, .message = "PERSISTENT_IDENTIFIER_SPACE_EXHAUSTED"};
    if (!p.snapshot().observation_ready)
        return {.failure = PreSendFailure::SessionNotTradable, .message = "OBSERVATION_NOT_READY"};
    if (request.quantity != 1)
        return {.failure = PreSendFailure::InvalidQuantity, .message = "TEST V1 quantity must be exactly 1"};
    auto config = p.current_order(request);
    config.dry_run = true;
    config.send_test_order = false;
    config.any_arm_flag = false;
    config.authorized_plan_sha256.clear();
    return build_pre_send_plan(config);
}

cg::Plaza2Error ConnectorHost::authorize(std::string_view canonical, std::string_view sha) {
    auto& p = *impl_;
    if (p.config.purpose != HostPurpose::OrderTest || p.submitted || p.persistent != nullptr ||
        p.recovered_epoch_active || p.restart_recovery_only || p.first_order_fill_incident || p.checkpoint_blocked ||
        !p.authorized_sha.empty())
        return invalid("authorization is unavailable for this host");
    const auto candidate = plan();
    if (!candidate.ok) {
        p.error = candidate.message.empty() ? "OBSERVATION_NOT_READY" : candidate.message;
        return invalid(p.error);
    }
    return p.authorize_candidate(candidate, p.current_order(), canonical, sha);
}

OrderLifecycleResult ConnectorHost::submit() {
    auto& p = *impl_;
    if (p.config.purpose != HostPurpose::OrderTest || p.authorized_sha.empty() || p.submitted ||
        p.persistent != nullptr || p.recovered_epoch_active || p.restart_recovery_only || p.first_order_fill_incident ||
        p.checkpoint_blocked)
        return {.message = "authorized one-shot order-test required"};
    if (poll() || !p.snapshot().observation_ready)
        return {.message = "OBSERVATION_NOT_READY"};
    p.submitted = true;
    auto config = p.current_order();
    config.dry_run = false;
    config.send_test_order = true;
    config.any_arm_flag = true;
    config.authorized_plan_sha256 = p.authorized_sha;
    SystemOrderLifecycleClock clock;
    OrderLifecycleController controller(std::move(config), p.transport, clock);
    p.result = controller.run();
    if (!p.result->ok) {
        p.state = ConnectorHostState::Failed;
        p.error = "order lifecycle failed; inspect local journal";
    } else
        p.state = ConnectorHostState::Started;
    return *p.result;
}

cg::Plaza2Error ConnectorHost::begin_order(const ConnectorHostOrderRequest& request, std::string_view canonical_plan,
                                           std::string_view sha256) {
    auto& p = *impl_;
    if (p.config.purpose != HostPurpose::OrderTest || p.submitted || p.persistent != nullptr ||
        p.recovered_epoch_active || p.restart_recovery_only || p.first_order_fill_incident || p.checkpoint_blocked ||
        !p.authorized_sha.empty()) {
        return invalid("a persistent order epoch is already active or unavailable for this host");
    }
    if (p.state != ConnectorHostState::Started && p.state != ConnectorHostState::Ready) {
        return invalid("begin_order requires a started host");
    }
    if (!p.snapshot().observation_ready) {
        return invalid("begin_order requires current observation readiness");
    }
    if (request.quantity != 1)
        return invalid("persistent TEST V1 quantity must be exactly 1");
    if (p.epoch_counter == std::numeric_limits<std::uint64_t>::max())
        return invalid("persistent epoch counter is exhausted");
    const auto next_epoch = p.epoch_counter + 1;
    auto order = p.current_order(request);
    std::string identity_error;
    if (!p.assign_active_identity(order, next_epoch, identity_error))
        return invalid(identity_error);
    order.dry_run = true;
    order.send_test_order = false;
    order.any_arm_flag = false;
    const auto candidate = build_pre_send_plan(order);
    if (!candidate.ok) {
        return invalid(candidate.message.empty() ? "OBSERVATION_NOT_READY" : candidate.message);
    }
    if (const auto authorization = p.authorize_candidate(candidate, order, canonical_plan, sha256)) {
        return authorization;
    }
    // Persistent epochs share one ConnectorHost, so their execution-safety
    // evidence must follow the same unique run identity as the journal.  The
    // transport keeps this operation private and accepts it only before any
    // AddOrder attempt or uncertainty exists.
    const auto epoch_receipt_path = order.journal_root / order.run_id / "execution_safety.json";
    if (const auto receipt_path = p.transport.set_execution_safety_receipt_path_for_epoch(epoch_receipt_path)) {
        p.transport.mark_order_epoch_terminal();
        (void)p.transport.reset_order_epoch();
        p.authorized_sha.clear();
        return receipt_path;
    }
    std::string checkpoint_error;
    auto staged_checkpoint = order;
    const auto previous_epoch = p.epoch_counter;
    p.epoch_counter = next_epoch;
    if (!p.write_checkpoint("authorized", staged_checkpoint, &candidate, checkpoint_error)) {
        p.epoch_counter = previous_epoch;
        (void)p.transport.mark_order_epoch_terminal();
        (void)p.transport.reset_order_epoch();
        return invalid(checkpoint_error);
    }
    auto controller = std::make_unique<PersistentOrderController>(order, p.transport, p.clock);
    if (const auto begin = controller->begin(candidate)) {
        p.epoch_counter = previous_epoch;
        auto idle = p.config.order;
        (void)p.assign_next_identity(idle, checkpoint_error);
        std::string idle_error;
        (void)p.write_checkpoint("idle", idle, nullptr, idle_error);
        (void)p.transport.mark_order_epoch_terminal();
        (void)p.transport.reset_order_epoch();
        p.authorized_sha.clear();
        return begin;
    }
    p.epoch_counter = next_epoch;
    p.config.order = std::move(order);
    p.persistent_order = p.config.order;
    p.persistent_plan = candidate;
    p.persistent = std::move(controller);
    return {};
}

cg::Plaza2Error ConnectorHost::begin_order(std::string_view canonical_plan, std::string_view sha256) {
    return begin_order(impl_->request_from_config(), canonical_plan, sha256);
}

OrderLifecycleResult ConnectorHost::submit_order() {
    auto& p = *impl_;
    if (p.persistent == nullptr || p.recovered_epoch_active || p.restart_recovery_only || p.first_order_fill_incident ||
        p.checkpoint_blocked)
        return {.message = "begin_order with an exact authorization is required"};
    if (!p.persistent_plan || !p.persistent_order)
        return {.message = "persistent epoch authorization is incomplete"};
    std::string checkpoint_error;
    if (!p.write_checkpoint("add_may_have_been_sent", *p.persistent_order, &*p.persistent_plan, checkpoint_error)) {
        OrderLifecycleResult blocked;
        blocked.state = OrderLifecycleState::Authorized;
        blocked.message = "persistent submission checkpoint failed; no publisher allocation/post: " + checkpoint_error;
        return blocked;
    }
    return p.persistent->submit_order();
}

OrderLifecycleResult ConnectorHost::poll_order() {
    auto& p = *impl_;
    if (p.persistent == nullptr)
        return {.message = "begin_order with an exact authorization is required"};
    auto result = p.persistent->poll_order();
    if (p.config.order.policy.version == kFirstOrderDeepPassiveVersion && result.observation &&
        (result.observation->state == OrderLifecycleState::Filled || result.observation->executed_quantity > 0)) {
        p.first_order_fill_incident = true;
        p.error = "FIRST_ORDER_FILLED: new-order authority frozen; preserve actual position, no automatic flatten";
    }
    if (p.retained_checkpoint) {
        auto checkpoint = *p.retained_checkpoint;
        checkpoint.lifecycle_state = static_cast<std::int32_t>(result.state);
        if (result.add_reply && result.add_reply->accepted && result.add_reply->order_id)
            checkpoint.known_order_id = *result.add_reply->order_id;
        if (result.observation && !result.observation->identity_conflict) {
            checkpoint.known_order_id = result.observation->private_order_id;
            checkpoint.known_remaining = result.observation->remaining_quantity;
        }
        std::string error;
        if (checkpoint_json(checkpoint) != checkpoint_json(*p.retained_checkpoint) &&
            !p.persist_checkpoint(checkpoint, error)) {
            result.ok = false;
            result.message = error;
        }
    }
    return result;
}

OrderLifecycleResult ConnectorHost::cancel_current_order() {
    auto& p = *impl_;
    if (p.persistent == nullptr)
        return {.message = "begin_order with an exact authorization is required"};
    if (p.restart_recovery_only)
        return {.message = "restart requires fresh explicit recovered Cancel authorization"};
    return p.persistent->cancel_order();
}

RecoveredOrderReconciliation ConnectorHost::reconcile_recovered_order() {
    auto& p = *impl_;
    if (!p.persistent || !p.persistent->active() || !p.persistent->submission_attempted() || p.checkpoint_blocked)
        return {.outcome = RecoveredOrderOutcome::GenerationNotFresh};
    if (poll())
        return {.outcome = RecoveredOrderOutcome::PrivateStreamNotCurrent};
    auto reconciliation = p.transport.inspect_recovered_order(p.recovered_key());
    if (reconciliation.outcome == RecoveredOrderOutcome::TerminalAlready && reconciliation.observation)
        (void)p.persistent->accept_recovered_terminal(*reconciliation.observation);
    return reconciliation;
}

RecoveredCancelPlan ConnectorHost::prepare_recovered_cancel(const std::filesystem::path& artifact) {
    auto& p = *impl_;
    const auto reconciliation = reconcile_recovered_order();
    if (reconciliation.outcome != RecoveredOrderOutcome::ExactlyOneWorkingMatch)
        return {.reconciliation = reconciliation,
                .error = std::string(recovered_order_outcome_name(reconciliation.outcome))};
    // Retain the old journal before any later cancel/reply replaces its current
    // fields. This is evidence only; it cannot authorize a publisher call.
    const auto journal_path = p.persistent->last_result().journal_path;
    std::ifstream journal(journal_path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(journal), {}};
    std::string error;
    if (!journal || bytes.empty() ||
        !write_recovered_artifact(artifact.string() + ".journal-before.json", bytes, error))
        return {.reconciliation = reconciliation, .error = "prior epoch journal could not be preserved: " + error};
    return p.transport.prepare_recovered_cancel(p.recovered_key(), artifact);
}

OrderLifecycleResult ConnectorHost::cancel_recovered_order(const std::filesystem::path& artifact,
                                                           std::string_view sha) {
    auto& p = *impl_;
    const auto reconciliation = reconcile_recovered_order();
    if (reconciliation.outcome == RecoveredOrderOutcome::TerminalAlready && p.persistent) {
        auto terminal = p.persistent->last_result();
        terminal.cancel_submission = {}; // No command was issued by this call.
        return terminal;
    }
    if (reconciliation.outcome != RecoveredOrderOutcome::ExactlyOneWorkingMatch || !reconciliation.observation)
        return {.message = std::string(recovered_order_outcome_name(reconciliation.outcome))};
    std::optional<cg::Plaza2PublisherMessageResult> attempt;
    auto result = p.persistent->explicit_recovered_cancel(
        *reconciliation.observation, p.transport.recovered_cancel_user_id(artifact, sha), [&] {
            attempt = p.transport.execute_recovered_cancel(artifact, sha);
            return *attempt;
        });
    result.cancel_submission = attempt.value_or(cg::Plaza2PublisherMessageResult{});
    return result;
}

cg::Plaza2Error ConnectorHost::finish_order_epoch() {
    auto& p = *impl_;
    if (p.persistent == nullptr)
        return invalid("no active persistent order epoch");
    if (const auto finish = p.persistent->finish_order_epoch())
        return finish;
    p.transport.mark_order_epoch_terminal();
    if (const auto reset = p.transport.reset_order_epoch())
        return reset;
    auto next_order = p.config.order;
    std::string identity_error;
    if (!p.assign_next_identity(next_order, identity_error)) {
        p.checkpoint_blocked = true;
        p.checkpoint_error = identity_error;
        p.persistent.reset();
        return invalid(identity_error);
    }
    p.config.order = next_order;
    std::string checkpoint_error;
    if (!p.write_checkpoint("idle", p.config.order, nullptr, checkpoint_error)) {
        p.persistent.reset();
        p.persistent_plan.reset();
        p.persistent_order.reset();
        p.error = checkpoint_error;
        return invalid(checkpoint_error);
    }
    p.persistent.reset();
    p.persistent_plan.reset();
    p.persistent_order.reset();
    p.recovered_epoch_active = false;
    p.authorized_sha.clear();
    p.result.reset();
    p.error.clear();
    return {};
}

RestartReconciliationResult ConnectorHost::reconcile() {
    if (impl_->restart_recovery_only) {
        const auto current = reconcile_recovered_order();
        RestartReconciliationResult result;
        result.run_found = true;
        result.ok = current.outcome == RecoveredOrderOutcome::ExactlyOneWorkingMatch ||
                    current.outcome == RecoveredOrderOutcome::TerminalAlready;
        result.message = std::string(recovered_order_outcome_name(current.outcome));
        if (current.observation)
            result.state = current.observation->state;
        if (current.outcome == RecoveredOrderOutcome::TerminalAlready) {
            const auto error = finish_order_epoch();
            result.resolved = !error;
            if (error) {
                result.ok = false;
                result.message = error.message;
            }
        }
        return result;
    }

    if (impl_->checkpoint_blocked)
        return {.ok = false, .message = impl_->checkpoint_error};
    if (impl_->persistent != nullptr && impl_->persistent->active())
        return {.ok = false, .message = "reconciliation requires a closed in-process order epoch"};
    if (poll())
        return {.ok = false, .message = "reconciliation requires a healthy running host"};
    const auto view = snapshot();
    if (!view.private_streams_ready || !view.trade_replay_complete)
        return {.ok = false, .message = "reconciliation requires current private replication and anchored TRADE"};
    // Rebuild the non-persisted market evidence from the current host before
    // validating the historical journal.  The checkpoint owns epoch identity
    // and application terms; smoke observations must be fresh on restart.
    auto reconciliation_config = impl_->current_order();
    const auto& state = impl_->transport.host().private_state();
    auto result = reconcile_unfinished_run(reconciliation_config, state.own_orders(), state.own_trades());
    if (impl_->recovered_epoch_active && result.resolved) {
        auto next_order = impl_->config.order;
        std::string identity_error;
        if (!impl_->assign_next_identity(next_order, identity_error)) {
            impl_->checkpoint_blocked = true;
            impl_->checkpoint_error = identity_error;
            result.ok = false;
            result.message = identity_error;
            return result;
        }
        std::string checkpoint_error;
        if (!impl_->write_checkpoint("idle", next_order, nullptr, checkpoint_error)) {
            result.ok = false;
            result.message = checkpoint_error;
            return result;
        }
        impl_->config.order = std::move(next_order);
        impl_->recovered_epoch_active = false;
    }
    return result;
}

std::string render_snapshot(const ConnectorHostSnapshot& s, bool json) {
    std::ostringstream out;
    out << std::boolalpha;
    if (!json) {
        out << "state=" << host_state_name(s.state) << " target=" << s.target << " session=" << s.session_id
            << "\nobservation_ready=" << s.observation_ready << " publisher=" << s.publisher_ready
            << " reply=" << s.reply_ready << "\nbbo=" << s.bid << '/' << s.ask << " age_ms=" << s.bbo_age_ms
            << "\nposition=" << position_evidence_class_name(s.position_evidence_class)
            << " active_own_orders=" << s.active_own_order_count << " uob_periodic=" << s.uob_periodic_consistent
            << "\norder_epoch_active=" << s.order_epoch_active << " order_authorized=" << s.order_authorized
            << " order_submission_attempted=" << s.order_submission_attempted
            << " new_order_allowed=" << s.new_order_allowed << "\nmarket_safe=" << s.market_safe
            << " evidence_consistent=" << s.evidence_consistent << "\nlast_error=" << s.last_error << '\n';
        return out.str();
    }
    out << "{\"schema\":\"moex.connector-host.v1\",\"state\":" << quoted(host_state_name(s.state))
        << ",\"environment\":" << quoted(s.environment == cg::Plaza2Environment::Test ? "test" : "prod")
        << ",\"mode\":" << static_cast<unsigned>(s.mode)
        << ",\"runtime_compatibility\":" << quoted(s.runtime_compatibility)
        << ",\"runtime_scheme_sha256\":" << quoted(s.runtime_scheme_sha256)
        << ",\"publisher_handle_open\":" << s.publisher_handle_open << ",\"reply_handle_open\":" << s.reply_handle_open
        << ",\"private_snapshot_state_ready\":" << s.private_snapshot_state_ready
        << ",\"aggr_snapshot_state_ready\":" << s.aggr_snapshot_state_ready << ",\"aggr_ready\":" << s.aggr_ready
        << ",\"participant_limit_row_present\":" << s.participant_limit_row_present
        << ",\"participant_identity_exact\":" << s.participant_identity_exact
        << ",\"exchange_money_limit_check_enabled\":" << s.exchange_money_limit_check_enabled
        << ",\"causal_error\":{\"connector_code\":" << static_cast<unsigned>(s.causal_error.code)
        << ",\"runtime_code\":" << (s.causal_error.runtime_code ? std::to_string(s.causal_error.runtime_code) : "null")
        << ",\"message\":" << quoted(s.causal_error.message) << "}";
    out << ",\"session_terms_present\":" << s.session_terms_present
        << ",\"session_terms_current\":" << s.session_terms_current
        << ",\"session_price_bounds_valid\":" << s.session_price_bounds_valid
        << ",\"order_price_within_exchange_bounds\":" << s.order_price_within_exchange_bounds
        << ",\"order_price_tick_aligned\":" << s.order_price_tick_aligned;
    out << ",\"recovery_generation\":" << s.recovery.generation << ",\"recovery_attempts\":" << s.recovery.attempts
        << ",\"recovery_transitions\":" << s.recovery.transitions
        << ",\"recovery_deadline_exhausted\":" << s.recovery.deadline_exhausted;
    const auto failure_detail = [&](std::string_view name, const cg::Plaza2Error& error) {
        out << "," << quoted(name) << ":{\"connector_code\":" << static_cast<unsigned>(error.code)
            << ",\"runtime_code\":" << (error.runtime_code ? std::to_string(error.runtime_code) : "null")
            << ",\"message\":" << quoted(error.message) << "}";
    };
    static constexpr std::array origin_names{
        "Unknown",        "ConnectionProcess", "ConnectionState", "ListenerState",    "Publisher",
        "Callback",       "Bootstrap",         "EnvironmentOpen", "ConnectionCreate", "ConnectionOpen",
        "ListenerCreate", "ListenerOpen",      "PublisherCreate", "PublisherOpen"};
    out << ",\"failure_origin\":" << quoted(origin_names.at(static_cast<std::size_t>(s.recovery.origin)));
    failure_detail("first_recovery_cause", s.recovery.first_cause);
    failure_detail("connection_process_cause", s.recovery.process_cause);
    failure_detail("state_query_cause", s.recovery.state_query_cause);
    out << ",\"publisher_ready\":" << s.publisher_ready << ",\"reply_ready\":" << s.reply_ready
        << ",\"private_streams_ready\":" << s.private_streams_ready << ",\"observation_ready\":" << s.observation_ready
        << ",\"target\":" << quoted(s.target) << ",\"target_isin_id\":" << s.target_isin_id
        << ",\"session_id\":" << s.session_id
        << ",\"session_status\":" << (s.session_status ? std::to_string(*s.session_status) : "null")
        << ",\"instrument_status\":" << (s.instrument_status ? std::to_string(*s.instrument_status) : "null")
        << ",\"min_step\":" << quoted(s.min_step) << ",\"bid\":" << quoted(s.bid) << ",\"ask\":" << quoted(s.ask)
        << ",\"bbo_age_ms\":" << s.bbo_age_ms << ",\"refdata_lifenum\":" << s.refdata_lifenum
        << ",\"target_refdata_provenance_ready\":" << s.target_refdata_provenance_ready
        << ",\"target_aggr20_uncrossed\":" << s.target_aggr20_uncrossed
        << ",\"position_evidence_class\":" << quoted(position_evidence_class_name(s.position_evidence_class))
        << ",\"zero_starting_position_proven\":" << s.zero_starting_position_proven
        << ",\"pos_trades_rev\":" << s.pos_trades_rev << ",\"pos_trades_lifenum\":" << s.pos_trades_lifenum
        << ",\"trade_replay_complete\":" << s.trade_replay_complete
        << ",\"active_own_order_count\":" << s.active_own_order_count
        << ",\"uob_periodic_consistent\":" << s.uob_periodic_consistent << ",\"limits_set\":" << s.limits_set
        << ",\"order_epoch_active\":" << s.order_epoch_active << ",\"order_authorized\":" << s.order_authorized
        << ",\"order_submission_attempted\":" << s.order_submission_attempted
        << ",\"new_order_allowed\":" << s.new_order_allowed << ",\"lifecycle_state\":"
        << (s.lifecycle_state ? quoted(order_lifecycle_state_name(*s.lifecycle_state)) : "null")
        << ",\"order_id\":" << s.order_id << ",\"original_quantity\":" << s.original_quantity
        << ",\"remaining_quantity\":" << s.remaining_quantity << ",\"executed_quantity\":" << s.executed_quantity
        << ",\"market_safe\":" << s.market_safe << ",\"evidence_consistent\":" << s.evidence_consistent
        << ",\"cg_pub_msgnew\":" << s.publisher_calls.msgnew << ",\"cg_pub_post\":" << s.publisher_calls.post
        << ",\"last_error\":" << quoted(s.last_error) << ",\"trade_anchor\":";
    if (s.trade_anchor)
        out << "{\"trades_rev\":" << s.trade_anchor->trades_rev
            << ",\"trades_lifenum\":" << s.trade_anchor->trades_lifenum
            << ",\"server_time\":" << s.trade_anchor->server_time << '}';
    else
        out << "null";
    out << ",\"target_provenance\":[";
    bool first = true;
    for (const auto& row : {s.fut_instruments_provenance, s.fut_sess_contents_provenance, s.session_provenance}) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"present\":" << row.present << ",\"table_code\":" << static_cast<unsigned>(row.table_code)
            << ",\"lifenum\":" << row.lifenum << ",\"repl_rev\":" << row.repl_rev << '}';
    }
    out << "],\"causal_operation\":" << quoted(s.causal_operation)
        << ",\"causal_monotonic_ns\":" << s.causal_error_time_ns
        << ",\"causal_callback_error\":" << quoted(s.causal_callback_error);
    const auto render_health = [&](std::string_view label, const Plaza2TransportHealth& h) {
        out << ',' << quoted(label) << ":{\"connection\":" << h.connection << ",\"publisher\":" << h.publisher
            << ",\"reply\":" << h.reply << ",\"aggr\":" << h.aggr << ",\"private\":[";
        for (std::size_t i = 0; i < h.private_count; ++i) {
            if (i)
                out << ',';
            out << '[' << static_cast<unsigned>(h.private_streams[i]) << ',' << h.private_states[i] << ']';
        }
        out << "]}";
    };
    render_health("transport_health", s.transport_health);
    render_health("causal_transport_health", s.causal_health);
    out << ",\"streams\":[";
    first = true;
    for (const auto& row : s.streams) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"name\":" << quoted(row.stream_name) << ",\"online\":" << row.online
            << ",\"snapshot_complete\":" << row.snapshot_complete
            << ",\"periodic_snapshot_consistent\":" << row.periodic_snapshot_consistent << '}';
    }
    out << "]}\n";
    return out.str();
}

} // namespace moex::connector_host
