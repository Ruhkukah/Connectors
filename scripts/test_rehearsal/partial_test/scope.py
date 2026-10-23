"""One immutable authorization shared by every partial-scenario guard.

Prices and notionals use the product's integer scale (100000). Private identity
values are compared locally; exceptions contain static failure codes only.
"""
from dataclasses import asdict, dataclass
from datetime import datetime, timezone, timedelta
import hashlib
import json
import re

MSK = timezone(timedelta(hours=3))
TERMINAL = {'Filled', 'Cancelled', 'Rejected'}
WORKING = {'Working', 'PartFilled'}
STREAMS = {'FORTS_TRADE_REPL', 'FORTS_USERORDERBOOK_REPL', 'FORTS_POS_REPL',
           'FORTS_PART_REPL', 'FORTS_REFDATA_REPL', 'FORTS_SESSIONSTATE_REPL',
           'FORTS_INSTRUMENTSTATE_REPL', 'FORTS_AGGR20_REPL', 'p2mqreply'}


class HarnessStop(RuntimeError):
    pass


def require(ok, code):
    if not ok:
        raise HarnessStop(code)


def positive(value):
    return type(value) is int and value > 0


@dataclass(frozen=True)
class FrozenScope:
    authorized_date: str
    source_head: str
    binary_sha256: str
    ci_run_id: int
    login: str
    ext_namespace: str
    instance_id: str
    instrument: str
    isin_id: int
    quantity_limit: int
    position_limit: int
    fixed_ask_budget_scaled: int
    rate: int = 5
    max_open_orders: int = 1
    max_attempts: int = 3
    timeout_seconds: int = 600
    review_ci_head: str = ''

    def __post_init__(self):
        require(re.fullmatch(r'\d{4}-\d{2}-\d{2}', self.authorized_date) is not None,
                'scope_date_invalid')
        try:
            datetime.strptime(self.authorized_date, '%Y-%m-%d')
        except ValueError as error:
            raise HarnessStop('scope_date_invalid') from error
        require(re.fullmatch('[0-9a-f]{40}', self.source_head) is not None and
                re.fullmatch('[0-9a-f]{64}', self.binary_sha256) is not None and
                positive(self.ci_run_id), 'scope_candidate_pins_invalid')
        require(self.review_ci_head == '' or
                re.fullmatch('[0-9a-f]{40}', self.review_ci_head) is not None,
                'scope_review_ci_head_invalid')
        require(all(isinstance(v, str) and v and not any(c.isspace() for c in v)
                    for v in (self.login, self.ext_namespace, self.instance_id)),
                'scope_private_identity_invalid')
        ext = re.fullmatch(r'([1-9][0-9]*):([1-9][0-9]*)', self.ext_namespace)
        require(ext is not None and int(ext[1]) <= int(ext[2]) < 2147483647,
                'scope_ext_id_range_invalid')
        require(self.instrument in {'ALRS-12.26', 'CNY-12.26'} and positive(self.isin_id),
                'scope_instrument_invalid')
        require(type(self.quantity_limit) is int and 2 <= self.quantity_limit <= 10 and
                type(self.position_limit) is int and
                self.quantity_limit <= self.position_limit <= 10 and
                positive(self.fixed_ask_budget_scaled), 'scope_risk_invalid')
        require(type(self.rate) is int and self.rate == 5 and
                type(self.max_open_orders) is int and self.max_open_orders == 1 and
                type(self.max_attempts) is int and 1 <= self.max_attempts <= 3 and
                type(self.timeout_seconds) is int and 1 <= self.timeout_seconds <= 600,
                'scope_budget_invalid')

    @property
    def digest(self):
        return hashlib.sha256(json.dumps(asdict(self), sort_keys=True,
                                        separators=(',', ':')).encode()).hexdigest()

    def launcher_arguments(self):
        """Reviewable exact argument vector; this function never starts a process."""
        value = self.fixed_ask_budget_scaled
        notional = f'{value // 100000}.{value % 100000:05d}'.rstrip('0').rstrip('.')
        return ('--environment', 'test', '--login-env', 'MOEX_LOGIN', '--ext-id-range',
                self.ext_namespace, '--instance-id', self.instance_id,
                '--max-quantity', str(self.quantity_limit), '--max-position',
                f'{self.isin_id}={self.position_limit}', '--max-open-orders', '1',
                '--max-commands-per-second', '5', '--max-notional',
                f'{self.isin_id}={notional}', '--isin-id', str(self.isin_id),
                '--allow-orders', '--sole-instance')


@dataclass(frozen=True)
class RuntimeProof:
    """Protected adapter's identity/ownership receipt, never derived from argv text."""
    scope_digest: str
    source_head: str
    binary_sha256: str
    login: str
    ext_namespace: str
    instance_id: str
    owned_product: bool = False
    sole_instance: bool = False
    remaining_ids: int = 0
    environment: str = 'TEST'
    observed_utc_ns: int = 0
    reservation_coverage: bool = False
    reservation_negative_control: bool = False
    original_allocation_unchanged: bool = False
    ci_run_id: int = 0
    ci_head: str = ''
    ci_jobs_passed: bool = False
    scripts_only_comparison: bool = False


def full_native_ready(state):
    transport, replay, pos = (state.get('transport', {}),
                              state.get('trade_replay', {}), state.get('pos_anchor') or {})
    listeners, anchor = transport.get('listeners', []), replay.get('used_anchor') or {}
    epoch = replay.get('accepted_lifenum')
    return (state.get('reconstructing') is False and
            not any(state.get(key) for key in ('journal_failed', 'log_error', 'cancel_only',
                    'operator_action_required', 'cgate_key_check_failed')) and
            transport.get('valid') is True and
            transport.get('connection', {}).get('state') == 3 and
            transport.get('publisher', {}).get('state') == 3 and
            len(listeners) == len(STREAMS) and
            {row.get('stream') for row in listeners} == STREAMS and
            all(row.get('online') is True and row.get('snapshot_complete') is True and
                row.get('native', {}).get('state') == 3 and row.get('error', {}).get('code') == 0
                for row in listeners) and replay.get('anchor_ready') is True and
            positive(epoch) and epoch == anchor.get('trades_lifenum') == pos.get('trades_lifenum') and
            pos.get('online') is True and pos.get('snapshot_complete') is True)


def validate_runtime(scope, state, proof, now_ns, require_flat=False, entry=True):
    require(datetime.fromtimestamp(now_ns / 1e9, MSK).date().isoformat() == scope.authorized_date,
            'scope_authorized_date_changed')
    require(isinstance(proof, RuntimeProof) and proof.scope_digest == scope.digest and
            proof.source_head == scope.source_head and proof.binary_sha256 == scope.binary_sha256 and
            proof.login == scope.login and proof.ext_namespace == scope.ext_namespace and
            proof.instance_id == scope.instance_id and proof.environment == 'TEST' and
            proof.owned_product is True and proof.sole_instance is True and
            type(proof.remaining_ids) is int and proof.remaining_ids >= 7000 and
            proof.reservation_coverage is True and proof.reservation_negative_control is True and
            proof.original_allocation_unchanged is True and
            proof.ci_run_id == scope.ci_run_id and
            proof.ci_head == (scope.review_ci_head or scope.source_head) and
            (proof.ci_head == scope.source_head or proof.scripts_only_comparison is True) and
            proof.ci_jobs_passed is True and
            0 <= now_ns - proof.observed_utc_ns <= 10_000_000_000,
            'scope_identity_ownership_or_ids_unproved')
    require(full_native_ready(state), 'partial_full_native_reconstruction_unproved')
    config = state.get('configuration', {})
    risk = config.get('risk', {})
    limits = risk.get('instruments', [])
    begin, end = (int(x) for x in scope.ext_namespace.split(':'))
    require(config.get('allow_orders') is True and config.get('sole_instance') is True and
            config.get('login_configured') is True and config.get('rate') == scope.rate and
            config.get('ext_id_begin') == begin and config.get('ext_id_end') == end and
            risk.get('max_quantity') == scope.quantity_limit and
            risk.get('max_open_orders') == scope.max_open_orders and len(limits) == 1 and
            limits[0].get('isin_id') == scope.isin_id and
            limits[0].get('max_position') == scope.position_limit and
            limits[0].get('max_quote_notional_scaled') == scope.fixed_ask_budget_scaled,
            'partial_effective_limits_outside_scope')
    require(state.get('queued') == 0, 'partial_queued_command_unresolved')
    live = [x for x in state.get('orders', []) if x.get('state') not in TERMINAL]
    require(len(live) <= 1 and all(x.get('instance_owned') is True and
            x.get('operator_action_required') is False and x.get('state') in WORKING and
            x.get('isin_id') == scope.isin_id for x in live), 'partial_live_order_scope_unproved')
    positions = state.get('positions', [])
    require(all(type(x.get('xpos')) is int and
                (x.get('isin_id') == scope.isin_id or x.get('xpos') == 0) and
                abs(x['xpos']) <= scope.position_limit for x in positions),
            'partial_position_scope_unproved')
    require(len({x.get('isin_id') for x in positions}) == len(positions),
            'partial_ambiguous_position')
    if require_flat:
        require(not live and not any(x['xpos'] for x in positions), 'partial_not_flat')
    rows = [x for x in state.get('instruments', []) if x.get('isin_id') == scope.isin_id]
    require(len(rows) == 1 and rows[0].get('symbol') == scope.instrument,
            'partial_refdata_identity_changed')
    row = rows[0]
    fills = row.get('pending_fill_reservations', {})
    require(fills.get('proof_valid') is True and fills.get('cached_proof_dirty') is False and
            fills.get('buy_overflow') is False and fills.get('sell_overflow') is False and
            type(fills.get('buy_quantity')) is int and fills['buy_quantity'] >= 0 and
            type(fills.get('sell_quantity')) is int and fills['sell_quantity'] >= 0,
            'partial_pending_fill_proof_unproved')
    if require_flat:
        require(fills['buy_quantity'] == fills['sell_quantity'] == 0,
                'partial_pending_fill_reservation_not_flat')
    if entry:
        book, terms = row.get('book') or {}, row.get('session_terms') or {}
        require(row.get('order_entry_ready') is True and book.get('valid') is True and
                terms.get('sess_id') == state.get('sess_id') and
                terms.get('interval_valid') is True and positive(terms.get('refdata_lifenum')),
                'partial_book_authority_unproved')
        require(0 <= now_ns - book.get('stream_last_callback_utc_ns', 0) <= 10_000_000_000,
                'partial_quote_stale')
        step, lower, upper = (terms.get('min_step_scaled'), terms.get('lower_price_scaled'),
                              terms.get('upper_price_scaled'))
        require(positive(step) and positive(lower) and positive(upper) and lower <= upper,
                'partial_terms_invalid')
        bid, ask = book.get('best_bid'), book.get('best_ask')
        require(all(x and positive(x.get('price_scaled')) and positive(x.get('quantity')) and
                    x['price_scaled'] % step == 0 and lower <= x['price_scaled'] <= upper
                    for x in (bid, ask)), 'partial_quote_grid_or_bounds_invalid')
        require(bid['price_scaled'] < ask['price_scaled'], 'partial_crossed_or_locked_quote')
    return row


def guardian_check(scope, state, proof, now_ns):
    """Same guard for guardian snapshots, including q10; no q2 bypass or alias."""
    return validate_runtime(scope, state, proof, now_ns, entry=False)


@dataclass(frozen=True)
class Candidate:
    quantity: int
    price_scaled: int
    step_scaled: int
    session_id: int
    trade_lifenum: int
    ask_size: int


def choose_candidate(scope, state, proof, now_ns):
    row = validate_runtime(scope, state, proof, now_ns, require_flat=True)
    ask = row['book']['best_ask']
    quantity, price = ask['quantity'] + 1, ask['price_scaled']
    if quantity > scope.quantity_limit or quantity * price > scope.fixed_ask_budget_scaled:
        return None
    return Candidate(quantity, price, row['session_terms']['min_step_scaled'],
                     state['sess_id'], state['trade_replay']['accepted_lifenum'], ask['quantity'])


def passive_move_price(scope, state, proof, now_ns):
    row = validate_runtime(scope, state, proof, now_ns)
    bid, terms = row['book']['best_bid']['price_scaled'], row['session_terms']
    # Buy Move must be passive even across a wide spread. ask-step is unsafe.
    below = bid - terms['min_step_scaled']
    price = below if below >= terms['lower_price_scaled'] else bid
    require(price <= bid and price < row['book']['best_ask']['price_scaled'] and
            price % terms['min_step_scaled'] == 0 and
            terms['lower_price_scaled'] <= price <= terms['upper_price_scaled'],
            'partial_passive_move_price_unavailable')
    return price
