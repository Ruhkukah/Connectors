"""Fixed TEST quote caps: current book quotes, with REFDATA identity/grid/bounds.

Settlement price is a clearing reference, not a current executable quote.
Use the lower side of a fresh two-sided book so the reference is conservative.
Never increase a frozen cap during a session.
"""
import time

SCALE = 100000


def current_reference(state, target, now_ns=None):
    rows = [x for x in state['instruments'] if x['isin_id'] == target['isin_id']]
    if len(rows) != 1 or rows[0]['symbol'] != target['symbol']:
        raise ValueError('current_price_identity_unproved')
    row = rows[0]
    terms, book = row.get('session_terms'), row.get('book')
    if not terms or terms['sess_id'] != state['sess_id'] or not terms['interval_valid'] or terms['refdata_lifenum'] <= 0:
        raise ValueError('current_price_refdata_unproved')
    if not book or not book['valid']:
        raise ValueError('current_price_book_unproved')
    now_ns = time.time_ns() if now_ns is None else now_ns
    callback = book.get('stream_last_callback_utc_ns')
    if type(callback) is int and callback > 0:
        fresh = 0 <= now_ns - callback <= 10_000_000_000
    elif type(callback) is int and callback == 0:
        # Without profiling the callback clock is intentionally zero. Native
        # committed-book clocks are still populated and must BOTH be fresh.
        committed, exchange = book.get('book_commit_monotonic_ns'), book.get('exchange_moment_ns')
        fresh = (type(committed) is int and committed > 0
                 and 0 <= time.monotonic_ns() - committed <= 10_000_000_000
                 and type(exchange) is int and exchange > 0
                 and -1_000_000_000 <= now_ns - exchange <= 10_000_000_000)
    else:
        fresh = False
    if not fresh:
        raise ValueError('current_price_book_stale')
    bid, ask = book.get('best_bid'), book.get('best_ask')
    if not bid or not ask or bid['price_scaled'] > ask['price_scaled']:
        raise ValueError('current_price_two_sided_book_unproved')
    for level in (bid, ask):
        p, step = level['price_scaled'], terms['min_step_scaled']
        if not isinstance(p, int) or p <= 0 or level['quantity'] <= 0 or not step or p % step or not terms['lower_price_scaled'] <= p <= terms['upper_price_scaled']:
            raise ValueError('current_price_grid_or_bounds_unproved')
    return bid['price_scaled']


def bind_caps(state, targets):
    result = []
    for target in targets:
        reference = current_reference(state, target)
        row = next(x for x in state['instruments'] if x['isin_id'] == target['isin_id'])
        if row['session_terms']['reference_price_scaled'] != target['reference_price_scaled']:
            raise ValueError('current_price_settlement_provenance_changed')
        result.append(dict(target, quote_reference_scaled=reference,
                           quote_reference_source='fresh_best_bid',
                           quote_reference_captured_utc_ns=time.time_ns(),
                           quote_cap_scaled=2 * reference))
    return result


def approved_ceiling(target, terms):
    # Historical fixtures/bundles retain their narrower settlement-based cap.
    return 2 * target.get('quote_reference_scaled', terms['reference_price_scaled'])
