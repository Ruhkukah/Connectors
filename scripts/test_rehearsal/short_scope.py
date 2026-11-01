"""Calendar authorization and sealed short-session window; no native actions."""
import datetime as dt
import math

MSK = dt.timezone(dt.timedelta(hours=3))
DATES = frozenset(('2026-10-16', '2026-10-19'))
dates = DATES


def epoch(date, hour, minute=0):
    parsed = dt.date.fromisoformat(date)
    return dt.datetime.combine(parsed, dt.time(hour, minute), MSK).timestamp()


def expected(date, *, continuation_start_utc=None):
    """One source for installer, runtime, supervisor and driver windows."""
    if date not in DATES:
        raise ValueError('short_authorized_calendar_date_unproved')
    friday = date == '2026-10-16'
    result = dict(authorized_date=date, scheduled_start_utc=epoch(date, 9),
                earliest_entry_utc=epoch(date, 10 if friday else 9),
                product_start_latest_utc=epoch(date, 10, 10) if friday else epoch(date, 9, 30),
                admission_latest_utc=epoch(date, 12), hard_entry_deadline_utc=epoch(date, 14, 30),
                flat_deadline_utc=epoch(date, 14, 40), guardian_trigger_utc=epoch(date, 14, 55),
                hard_cleanup_deadline_utc=epoch(date, 15))
    if continuation_start_utc is not None:
        if (date != '2026-10-16' or type(continuation_start_utc) not in (int, float)
                or not epoch(date, 9) <= continuation_start_utc <= epoch(date, 11, 30)
                or not math.isfinite(continuation_start_utc)):
            raise ValueError('short_continuation_start_unproved')
        result['scheduled_start_utc'] = continuation_start_utc
        result['product_start_latest_utc'] = min(continuation_start_utc + 1800,
                                                epoch(date, 11, 50))
    return result


def window(cfg):
    continuation = cfg.get('same_day_continuation', False)
    if continuation is not True and continuation is not False:
        raise ValueError('short_continuation_authority_unproved')
    if continuation:
        predecessor_verified = (cfg.get('predecessor_clean_terminal_verified') is True
            if cfg.get('burst_only') is True else
            cfg.get('predecessor_blocked_preentry_verified') is True)
        if (cfg.get('owner_authorized') is not True or
                not predecessor_verified or
                cfg.get('continuation_start_utc') is None):
            raise ValueError('short_continuation_authority_unproved')
        result = expected(cfg.get('authorized_date'),
                          continuation_start_utc=cfg['continuation_start_utc'])
    else:
        if 'continuation_start_utc' in cfg:
            raise ValueError('short_continuation_authority_unproved')
        result = expected(cfg.get('authorized_date'))
    if any(type(cfg.get(key)) not in (int, float) or cfg[key] != value
           for key, value in result.items() if key != 'authorized_date'):
        raise ValueError('short_sealed_window_changed')
    return result


validate = window
