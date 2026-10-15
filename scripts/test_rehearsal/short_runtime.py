"""Oct16 TEST scope over an immutable, previously reviewed native runtime.

The deployed package supplies frozen_runtime.py; imports do not start anything.
Native provenance and review/CI provenance deliberately have separate pins.
"""
from pathlib import Path
import http.client
import json
import re
import time
import urllib.request
import urllib.error

import short_scope as scope

import frozen_runtime as base

AUTHORIZED_DATE = '2026-10-16'
base.AUTHORIZED_DATE = AUTHORIZED_DATE
HEAD, BINARY, BOOT = base.HEAD, base.BINARY, base.BOOT
CI_RUN_ID = base.CI_RUN_ID
REVIEW_ONLY = False
JOBS = {'connector-validation', 'component-sanitizers'}


def __getattr__(name):
    return getattr(base, name)


def fetch(suffix):
    request = urllib.request.Request(
        'https://api.github.com/repos/Ruhkukah/Connectors/' + suffix,
        headers={'User-Agent': 'MOEX-TEST-qualification'})
    with urllib.request.urlopen(request, timeout=20) as result:
        return json.load(result)


def successful_run(run, pin, sha):
    return (type(pin) is int and run.get('id') == pin and run.get('head_sha') == sha
            and run.get('status') == 'completed' and run.get('conclusion') == 'success')


def _verify_ci_once(root, cfg):
    require = base.require
    review = cfg.get('review_git_sha', '')
    review_ci = cfg.get('review_ci_run_id')
    require(re.fullmatch('[0-9a-f]{40}', review) is not None,
            'short_review_exact_head_missing')
    require(cfg.get('ci_run_id') == CI_RUN_ID and cfg.get('source_git_sha') == HEAD,
            'short_native_ci_pin_changed')
    require(successful_run(fetch('actions/runs/' + str(CI_RUN_ID)), CI_RUN_ID, HEAD),
            'short_native_exact_ci_not_green')
    require(successful_run(fetch('actions/runs/' + str(review_ci)), review_ci, review),
            'short_review_exact_ci_not_green')
    pr = fetch('pulls/68')
    require(pr.get('state') == 'open' and pr.get('draft') is True
            and pr['head']['sha'] == review, 'short_protected_review_head_changed')
    comparison = fetch('compare/' + HEAD + '...' + review)
    files = comparison.get('files', [])
    require(comparison.get('merge_base_commit', {}).get('sha') == HEAD
            and comparison.get('status') in ('ahead', 'identical')
            and type(comparison.get('total_commits')) is int
            and comparison['total_commits'] <= 250 and len(files) < 300,
            'short_native_review_comparison_incomplete')
    require(all(row.get('status') != 'renamed' and
                (row.get('filename', '').startswith('scripts/test_rehearsal/')
                 or row.get('filename') in ('CERTIFICATION.md', 'CERTIFICATION_DAY_RUNBOOK.md'))
                for row in files), 'short_review_contains_native_or_unapproved_changes')
    checks = fetch('commits/' + review + '/check-runs')['check_runs']
    latest = {}
    for item in checks:
        if item.get('name') in JOBS:
            latest.setdefault(item['name'], item)
    require(set(latest) == JOBS and all(row['status'] == 'completed'
            and row['conclusion'] == 'success' for row in latest.values()),
            'short_review_both_ci_jobs_not_green')
    base.atomic(Path(root) / 'ci-public.json', dict(
        source_git_sha=HEAD, native_ci_run_id=CI_RUN_ID,
        review_git_sha=review, review_ci_run_id=review_ci,
        scripts_and_documentation_only=True,
        jobs={name: row['conclusion'] for name, row in latest.items()},
        checked_utc_ns=time.time_ns()))


def preflight_deadline(cfg, now=None):
    now = time.time() if now is None else now
    return min(now + 1800, cfg['product_start_latest_utc'])


def transient_network(error):
    if isinstance(error, urllib.error.HTTPError):
        return error.code == 429 or 500 <= error.code <= 599
    return isinstance(error, (urllib.error.URLError, TimeoutError, ConnectionError,
                              http.client.IncompleteRead))


def verify_ci(root, cfg, tick=None):
    # Only a transport failure may retry. A real pin, CI, schema or review
    # refusal remains fatal. No native product is created by this loop.
    deadline = preflight_deadline(cfg)
    while True:
        try:
            return _verify_ci_once(root, cfg)
        except Exception as error:
            if not transient_network(error) or time.time() + 2 >= deadline:
                raise
            if tick is not None:
                tick()
            time.sleep(min(2, max(0, deadline - time.time())))


def load_cfg(root):
    # Scope validation precedes selecting the one owner-authorized date; the
    # immutable base then verifies every manifest file and all native pins.
    candidate = json.loads((Path(root) / 'configuration-private.json').read_text())
    scope.window(candidate)
    base.AUTHORIZED_DATE = candidate['authorized_date']
    return base.load_cfg(root)


def day_check(cfg, entry=False):
    scope.window(cfg)
    base.AUTHORIZED_DATE = cfg['authorized_date']
    return base.day_check(cfg, entry=entry)


base.verify_ci = verify_ci
