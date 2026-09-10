"""Structural proof: an order.request cannot reach an order API in this runner."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
main = source[source.index('int main('):]
for forbidden in ('order.request', '.plan_order(', '.begin_order(', '.submit_order(',
                  '.cancel_current_order(', '.poll_order(', '.finish_order_epoch('):
    assert forbidden not in main, forbidden
assert 'observation_authorized(date, auth, std::getenv("MOEX_AGGR_T1_ORDER_AUTH"))' in main
assert 'observation_config(request.config)' in main
assert 'participant_json(participants, row)' in main
assert 'row.repl_id' in source
print('observation-only entry point: no request ingestion or order API calls')

assert 'std::signal(' not in main
assert main.index('Plaza2QualificationStop stop;') < main.index('ch::ConnectorHost host(')
assert 'while (!failed && !stop.requested() &&' in main
assert 'failed = static_cast<bool>(host.poll());' in main
assert 'private_account_code' not in source[source.index('void participant_json'):source.index('std::optional<std::int64_t> exact_price')]
