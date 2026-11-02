"""Bounded TEST scenarios: venue commands only through the supplied product CLI sequence.

The caller owns current account/epoch/build/clock/namespace gates, process lifetime,
flattening and final protection. No uncertain command is retried here.
"""
import json
import time

from sequence import SequenceStop, TERMINAL, WORKING, _price, _units
from native_evidence import CommittedDealObserver, EvidenceError


def _outcome(seq, name, status, reason, **values):
    result = dict(status=status, reason=reason, **values)
    seq.record(name, result)
    return result


def _events(seq, offset):
    with seq.journal.open() as source:
        source.seek(offset)
        for line in source:
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                return  # An asynchronously buffered partial line proves nothing.


def _credited_native(seq, offset, client_id, order_ids, isin_id, executed):
    """Require credited-once trades backed by committed native user_deal rows."""
    observer = CommittedDealObserver(seq.journal, offset, client_id, order_ids, isin_id)
    return _observe(observer, executed)


def _observe(observer, executed):
    try:
        observer.update()
        return observer.proved(executed)
    except EvidenceError as error:
        raise SequenceStop(str(error)) from error


def _native_move(seq, offset, old_order_id, new_order_id):
    commands = set()
    for event in _events(seq, offset):
        data = event.get('data', {})
        if event.get('event') == 'command' and data.get('name') == 'MoveOrder':
            fields = data.get('fields', {})
            if fields.get('order_id1') == old_order_id:
                if fields.get('regime') != 3:
                    raise SequenceStop('native_move_regime_not_three')
                commands.add(data.get('user_id'))
        elif event.get('event') == 'reply' and data.get('user_id') in commands and data.get('msgid') == 176 and data.get('code') == 0 and data.get('order_id1') == new_order_id:
            return True
    return False


def concurrent_cancel(seq):
    """Three far-passive orders; individual ALRS cancel, then mass CNY/ALRS proof."""
    state = seq.snapshot()
    seq.assert_flat(state)
    cny = next(x for x in seq.targets if x['symbol'] == 'CNY-12.26')
    alrs = next(x for x in seq.targets if x['symbol'] == 'ALRS-12.26')
    planned = [(cny, seq.passive(state, cny, 1)), (cny, seq.passive(state, cny, 1)), (alrs, seq.passive(state, alrs, 1))]
    limits = state['configuration']['risk']
    if limits['max_open_orders'] < 3:
        raise SequenceStop('concurrent_order_capacity_unavailable')
    for target in (cny, alrs):
        price_total = sum(price for row, price in planned if row['isin_id'] == target['isin_id'])
        limit = next(x for x in limits['instruments'] if x['isin_id'] == target['isin_id'])
        if price_total > limit['max_quote_notional_scaled']:
            raise SequenceStop('concurrent_combined_quote_cap_unavailable')
    placed = []
    try:
        for target, price in planned:
            client = seq.next_id(target)
            seq.send(f"place {client} {target['isin_id']} buy 1 {_price(price)} day")
            placed.append(client)
            seq.accepted_order(target, client)
        current = seq.snapshot()
        if sum(seq.order(current, key)['state'] in WORKING for key in placed) != 3:
            raise SequenceStop('three_concurrent_orders_not_proven')
        seq.cancel(placed[-1])  # Leaves only CNY working: existing mass_cancel is account-wide in its wait.
        seq.mass_cancel(cny)
        seq.mass_cancel(alrs)
        seq.assert_flat(seq.snapshot())
        return _outcome(seq, 'concurrent_cancel', 'PASS', 'three_live_then_individual_and_mass_cancel', concurrent_orders=3)
    finally:
        # Unexpected fills/errors stop new risk. Caller performs broader protection/flattening.
        for client in placed:
            state = seq.snapshot()
            row = seq.order(state, client)
            if row and row['state'] in WORKING:
                seq.cancel(client, allowed_fill=2)


def partial_move(seq, target, timeout_seconds=600):
    """One natural 1-of-2 fill must be committed and credited before a regime-3 Move.

    Returns NOT_OBSERVED for unavailable prices, zero/full fills, or a definite local
    Move refusal. Cancels any known remainder. Does not flatten the resulting POS.
    """
    if not 0 < timeout_seconds <= 600:
        raise ValueError('partial observation must be bounded to 600 seconds')
    state = seq.snapshot()
    seq.assert_flat(state)
    row = seq.target(state, target)
    terms, book = row['session_terms'], row['book']
    limit = next(x for x in state['configuration']['risk']['instruments'] if x['isin_id'] == target['isin_id'])
    step = terms['min_step_scaled']
    bid, ask = book.get('best_bid'), book.get('best_ask')
    if not step or not bid or not ask:
        return _outcome(seq, 'partial_move', 'NOT_OBSERVED', 'near_passive_quote_unavailable')
    ceiling = limit['max_quote_notional_scaled'] // 2
    price = min(bid['price_scaled'], ceiling // step * step)
    # A far price is not a credible partial-fill attempt; never exceed the approved cap.
    if price < bid['price_scaled'] - step or price >= ask['price_scaled']:
        return _outcome(seq, 'partial_move', 'NOT_OBSERVED', 'near_passive_price_outside_cap')
    try:
        seq.valid_price(state, target, price, 2)
        seq.valid_price(state, target, price - step, 2)
    except SequenceStop as error:
        if error.code not in ('price_outside_native_limits', 'quote_cap_exceeded'):
            raise
        return _outcome(seq, 'partial_move', 'NOT_OBSERVED', 'passive_move_grid_or_bounds_unavailable')
    client, offset = seq.next_id(target), seq.journal.stat().st_size
    placed = False
    try:
        seq.send(f"place {client} {target['isin_id']} buy 2 {_price(price)} day")
        placed = True
        deadline = time.monotonic() + timeout_seconds
        prefilled = None
        state = seq.snapshot()
        current = seq.order(state, client)
        if not current or current['order_id'] <= 0:
            current = seq.accepted_order(target, client)
        observer = CommittedDealObserver(seq.journal, offset, client, {current['order_id']}, target['isin_id'])
        next_status = 0
        while time.monotonic() < deadline:
            # Read appended complete events every 20 ms. Refresh CLI status at
            # 200 ms, or immediately on a committed/credited one-lot trigger.
            triggered = _observe(observer, 1)
            if triggered or time.monotonic() >= next_status:
                state = seq.snapshot()
                next_status = time.monotonic() + 0.2
            current = seq.order(state, client)
            if current and current['order_id'] > 0:
                observer.order_ids.add(current['order_id'])
            if current and current['state'] == 'Rejected':
                return _outcome(seq, 'partial_move', 'NOT_OBSERVED', 'two_lot_add_rejected')
            if current and current['executed'] == 2:
                return _outcome(seq, 'partial_move', 'NOT_OBSERVED', 'fully_filled_before_move', executed=2)
            if current and current['state'] in WORKING and current['executed'] == 1 and current['remaining'] == 1 and current['order_id'] > 0 and _observe(observer, 1):
                prefilled = current
                break
            time.sleep(0.02)
        if not prefilled:
            return _outcome(seq, 'partial_move', 'NOT_OBSERVED', 'committed_partial_fill_not_observed')
        # Refresh immediately before Move; the market may have finished the original order.
        state = seq.snapshot()
        current = seq.order(state, client)
        if not current or current['state'] not in WORKING or current['executed'] != 1 or current['remaining'] != 1:
            return _outcome(seq, 'partial_move', 'NOT_OBSERVED', 'partial_disappeared_before_move')
        if not _observe(observer, 1):
            return _outcome(seq, 'partial_move', 'NOT_OBSERVED', 'partial_credit_changed_before_move')
        row = seq.target(state, target, require_ready=False)
        if state.get('reconstructing') or not row['order_entry_ready']:
            return _outcome(seq, 'partial_move', 'NOT_OBSERVED', 'move_authority_unavailable')
        replacement = price - row['session_terms']['min_step_scaled']
        seq.valid_price(state, target, replacement, 2)
        if not row['book'].get('best_ask') or replacement >= row['book']['best_ask']['price_scaled']:
            return _outcome(seq, 'partial_move', 'NOT_OBSERVED', 'replacement_no_longer_passive')
        old_id, move_offset = current['order_id'], seq.journal.stat().st_size
        try:
            seq.send(f"move {client} 2 {_price(replacement)}")
        except SequenceStop as error:
            if error.code != 'command_refused':
                raise  # An uncertain Move is not safe to classify or retry.
            return _outcome(seq, 'partial_move', 'NOT_OBSERVED', 'move_definitively_refused')
        def moved(now):
            value = seq.order(now, client)
            return value if value and value['order_id'] > 0 and value['order_id'] != old_id and value['quantity'] == 2 and _units(value['price']) == replacement and _native_move(seq, move_offset, old_id, value['order_id']) else False
        _, result = seq.wait(moved, 'partial_move_replacement_not_proven')
        if result['executed'] != 1 or result['remaining'] != 1 or result['state'] not in WORKING:
            return _outcome(seq, 'partial_move', 'NOT_OBSERVED', 'replacement_raced_full_fill', executed=result['executed'])
        observer.order_ids.add(result['order_id'])
        terminal = seq.cancel(client, allowed_fill=2)
        seq.wait(lambda now: _observe(observer, seq.order(now, client)['executed']), 'partial_move_fill_accounting_not_proven', timeout=10)
        return _outcome(seq, 'partial_move', 'PASS', 'real_partial_before_regime_three_move', partial_before_move=True, native_regime=3, executed=terminal['executed'])
    finally:
        if placed:
            state = seq.snapshot()
            current = seq.order(state, client)
            if current and current['state'] in WORKING:
                seq.cancel(client, allowed_fill=2)
