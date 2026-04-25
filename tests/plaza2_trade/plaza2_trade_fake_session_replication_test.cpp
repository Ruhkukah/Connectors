#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include "moex/plaza2_trade/plaza2_trade_fake_session.hpp"

#include "plaza2_trade_test_support.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using moex::plaza2::fake::CommitListener;
using moex::plaza2::fake::EngineState;
using moex::plaza2::fake::EventSpec;
using moex::plaza2::fake::Plaza2FakeEngine;
using moex::plaza2::fake::RowSpec;
using moex::plaza2::fake::ScenarioSpec;
using moex::plaza2::private_state::Plaza2PrivateStateProjector;
using moex::plaza2_trade::Plaza2TradeCommandRequest;
using moex::plaza2_trade::Plaza2TradeFakeOutcomeStatus;
using moex::plaza2_trade::Plaza2TradeFakeSession;
using moex::plaza2_trade::test_support::make_add_order;
using moex::plaza2_trade::test_support::make_del_order;
using moex::plaza2_trade::test_support::require;

class CommitBoundaryProbe final : public CommitListener {
  public:
    explicit CommitBoundaryProbe(Plaza2PrivateStateProjector& projector) : projector_(projector) {}

    void on_event(const ScenarioSpec& scenario, const EventSpec& event, const EngineState& state) override {
        projector_.on_event(scenario, event, state);
    }

    void on_stream_row(const ScenarioSpec& scenario, const EventSpec& event, const RowSpec& row,
                       std::span<const moex::plaza2::fake::FieldValueSpec> fields, const EngineState& state) override {
        projector_.on_stream_row(scenario, event, row, fields, state);
        saw_uncommitted_row_ = true;
        if (projector_.own_orders().empty()) {
            hidden_before_commit_ = true;
        }
    }

    void on_transaction_commit(const ScenarioSpec& scenario, const EventSpec& commit_event,
                               const EngineState& state) override {
        projector_.on_transaction_commit(scenario, commit_event, state);
        visible_after_commit_ = !projector_.own_orders().empty();
    }

    [[nodiscard]] bool saw_uncommitted_row() const noexcept {
        return saw_uncommitted_row_;
    }

    [[nodiscard]] bool hidden_before_commit() const noexcept {
        return hidden_before_commit_;
    }

    [[nodiscard]] bool visible_after_commit() const noexcept {
        return visible_after_commit_;
    }

  private:
    Plaza2PrivateStateProjector& projector_;
    bool saw_uncommitted_row_{false};
    bool hidden_before_commit_{false};
    bool visible_after_commit_{false};
};

void project_batch(const moex::plaza2_trade::Plaza2TradeFakeReplicationBatch& batch,
                   Plaza2PrivateStateProjector& projector) {
    Plaza2FakeEngine engine;
    const auto result = engine.run(batch.view(), &projector);
    if (result.error) {
        throw std::runtime_error("fake replication bridge failed: " + result.error.message);
    }
}

void test_add_order_confirmation_projects_private_state() {
    Plaza2TradeFakeSession session;
    session.establish();
    const auto accepted = session.submit(Plaza2TradeCommandRequest{make_add_order()});
    require(accepted.status == Plaza2TradeFakeOutcomeStatus::Accepted, "AddOrder should accept");

    Plaza2PrivateStateProjector projector;
    project_batch(accepted.replication, projector);

    require(projector.connector_health().commit_count == 1, "fake confirmation should commit once");
    require(projector.own_orders().size() == 1, "fake confirmation should project one own order");
    const auto& order = projector.own_orders()[0];
    require(order.private_order_id == *accepted.generated_order_id, "projected order id should correlate to reply");
    require(order.client_code == "C01", "projected client code should match fake command");
    require(order.price == "101.25", "projected price should match fake command");
    require(order.private_amount_rest == 10, "projected remaining quantity should match fake command");
}

void test_commit_boundary_visibility() {
    Plaza2TradeFakeSession session;
    session.establish();
    const auto accepted = session.submit(Plaza2TradeCommandRequest{make_add_order()});

    Plaza2PrivateStateProjector projector;
    CommitBoundaryProbe probe(projector);
    Plaza2FakeEngine engine;
    const auto result = engine.run(accepted.replication.view(), &probe);
    if (result.error) {
        throw std::runtime_error("commit-boundary replay failed: " + result.error.message);
    }

    require(probe.saw_uncommitted_row(), "test should observe staged row before commit");
    require(probe.hidden_before_commit(), "projected order must not be visible before TN_COMMIT");
    require(probe.visible_after_commit(), "projected order must become visible after TN_COMMIT");
}

void test_cancel_and_fill_confirmations_project() {
    Plaza2TradeFakeSession session;
    session.establish();
    const auto accepted = session.submit(Plaza2TradeCommandRequest{make_add_order()});

    Plaza2PrivateStateProjector projector;
    project_batch(accepted.replication, projector);

    const auto fill = session.simulate_fill(*accepted.generated_order_id, 91001, 4);
    require(fill.status == Plaza2TradeFakeOutcomeStatus::Accepted, "fake fill should accept");
    project_batch(fill.replication, projector);
    require(projector.own_trades().size() == 1, "fake fill should project one own trade");
    require(projector.own_trades()[0].id_deal == 91001, "projected trade should carry fake deal id");

    auto cancel = make_del_order();
    cancel.order_id = *accepted.generated_order_id;
    const auto canceled = session.submit(Plaza2TradeCommandRequest{cancel});
    require(canceled.status == Plaza2TradeFakeOutcomeStatus::Accepted, "cancel should accept after partial fill");
    project_batch(canceled.replication, projector);
    require(!projector.own_orders().empty(), "cancel confirmation should keep order visible for read side");
    require(projector.own_orders()[0].private_amount_rest == 0, "cancel confirmation should zero remaining quantity");
}

void test_rejected_command_does_not_project_active_order() {
    Plaza2TradeFakeSession session;
    session.establish();
    auto invalid = make_add_order();
    invalid.amount = 0;
    const auto rejected = session.submit(Plaza2TradeCommandRequest{invalid});
    require(rejected.status == Plaza2TradeFakeOutcomeStatus::ValidationFailed, "invalid AddOrder should reject");
    require(rejected.replication.empty(), "rejected AddOrder should not emit active-order confirmation");
    require(session.orders().empty(), "rejected AddOrder should not mutate fake order state");
}

} // namespace

int main() {
    try {
        test_add_order_confirmation_projects_private_state();
        test_commit_boundary_visibility();
        test_cancel_and_fill_confirmations_project();
        test_rejected_command_does_not_project_active_order();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
