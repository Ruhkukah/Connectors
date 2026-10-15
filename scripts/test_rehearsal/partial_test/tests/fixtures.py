"""Native-shape state and journal fixtures; no product, network or subprocess."""
import copy
import json


NOW = 1_792_058_400_000_000_000  # 15 October 2026 13:00 MSK.
ISIN = 101
SYMBOL = 'CNY-12.26'
LIFENUM = 31
SESSION = 11729
STREAMS = (
    'FORTS_TRADE_REPL', 'FORTS_USERORDERBOOK_REPL', 'FORTS_POS_REPL',
    'FORTS_PART_REPL', 'FORTS_REFDATA_REPL', 'FORTS_SESSIONSTATE_REPL',
    'FORTS_INSTRUMENTSTATE_REPL', 'FORTS_AGGR20_REPL', 'p2mqreply',
)


def status(*, quantity_limit=10, ask_size=7, bid=990_000, ask=1_000_000):
    """One instrument, explicit complete same-epoch native reconstruction."""
    anchor = dict(trades_lifenum=LIFENUM)
    value = dict(
        sess_id=SESSION, queued=0, reconstructing=False, orders=[], positions=[],
        journal_failed=False, log_error=False, cancel_only=False,
        operator_action_required=False, cgate_key_check_failed=False,
        transport=dict(
            valid=True, connection=dict(state=3), publisher=dict(state=3),
            listeners=[dict(stream=name, online=True, snapshot_complete=True,
                            native=dict(state=3), error=dict(code=0))
                       for name in STREAMS]),
        trade_replay=dict(anchor_ready=True, accepted_lifenum=LIFENUM,
                          used_anchor=copy.deepcopy(anchor)),
        pos_anchor=dict(online=True, snapshot_complete=True, **anchor),
        configuration=dict(
            allow_orders=True, sole_instance=True, login_configured=True,
            ext_id_begin=100000, ext_id_end=199999,
            rate=5, risk=dict(max_quantity=quantity_limit, max_open_orders=1,
                              instruments=[dict(isin_id=ISIN,
                                                max_position=quantity_limit,
                                                max_quote_notional_scaled=10_000_000)])),
        instruments=[dict(
            symbol=SYMBOL, isin_id=ISIN, order_entry_ready=True,
            pending_fill_reservations=dict(proof_valid=True, cached_proof_dirty=False,
                buy_overflow=False, sell_overflow=False, buy_quantity=0, sell_quantity=0),
            session_terms=dict(sess_id=SESSION, interval_valid=True,
                               refdata_lifenum=21, min_step_scaled=1_000,
                               lower_price_scaled=500_000,
                               upper_price_scaled=2_000_000,
                               reference_price_scaled=1_000_000),
            book=dict(valid=True, stream_last_callback_utc_ns=NOW,
                      best_bid=dict(price_scaled=bid, quantity=70),
                      best_ask=dict(price_scaled=ask, quantity=ask_size)))],
    )
    return value


def append_event(path, event, data):
    with path.open('a') as out:
        out.write(json.dumps(dict(event=event, data=data)) + '\n')


def append_deal(path, *, client, order_id, quantity, deal=901, revision=101,
                commit=True, credit=True, duplicate=False, isin=ISIN, side='buy'):
    append_event(path, 'transaction_begin', dict(stream='FORTS_TRADE_REPL'))
    fields = dict(isin_id=isin, replAct=0, replRev=revision,
                  **{f'public_order_id_{side}': order_id, f'private_order_id_{side}': 0},
                  id_deal=deal, xamount=quantity)
    append_event(path, 'stream_row', dict(
        stream='FORTS_TRADE_REPL', table='user_deal',
        fields=[dict(name=name, signed=value) for name, value in fields.items()]))
    if commit:
        append_event(path, 'transaction_commit', dict(stream='FORTS_TRADE_REPL'))
    if credit:
        trade = dict(client_order_id=client, order_id=order_id,
                     deal_id=deal, quantity=quantity)
        append_event(path, 'trade', trade)
        if duplicate:
            append_event(path, 'trade', trade)
