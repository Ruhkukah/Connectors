"""Calendar authorization and sealed short-session window; no native actions."""
import datetime as dt

MSK = dt.timezone(dt.timedelta(hours=3))
DATES = frozenset(('2026-10-16', '2026-10-19'))
dates = DATES


def epoch(date, hour, minute=0):
    parsed = dt.date.fromisoformat(date)
    return dt.datetime.combine(parsed, dt.time(hour, minute), MSK).timestamp()


def expected(date):
    """One source for installer, runtime, supervisor and driver windows."""
    if date not in DATES:
        raise ValueError('short_authorized_calendar_date_unproved')
    friday = date == '2026-10-16'
    return dict(authorized_date=date, scheduled_start_utc=epoch(date, 9),
                earliest_entry_utc=epoch(date, 10 if friday else 9),
                product_start_latest_utc=epoch(date, 10, 10) if friday else epoch(date, 9, 30),
                admission_latest_utc=epoch(date, 12), hard_entry_deadline_utc=epoch(date, 14, 30),
                flat_deadline_utc=epoch(date, 14, 40), guardian_trigger_utc=epoch(date, 14, 55),
                hard_cleanup_deadline_utc=epoch(date, 15))


def window(cfg):
    result = expected(cfg.get('authorized_date'))
    if any(type(cfg.get(key)) not in (int, float) or cfg[key] != value
           for key, value in result.items() if key != 'authorized_date'):
        raise ValueError('short_sealed_window_changed')
    return result


validate = window
