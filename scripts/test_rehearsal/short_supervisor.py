"""Scoped TEST control adapter. Importing never creates a native process.

The owner-authorized short session retains the frozen product, restart and
independent closeout implementations. All transient retries below happen before
trading intent, or in publisher-free preflight observations after verified exit.
"""
import argparse
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import base_supervisor as base
import short_scope as scope


def epoch(hour, minute=0, date='2026-10-16'):
    return scope.epoch(date, hour, minute)


def validate(cfg):
    scope.window(cfg)
    base.AUTHORIZED_DATE = cfg['authorized_date']
    base.require(cfg.get('owner_authorized') is True and cfg.get('timer_paused') is True
                 and cfg.get('no_watchdog_sigkill') is True, 'short_owner_scope_unproved')
    base.require(cfg.get('source_git_sha') == base.HEAD and cfg.get('binary_sha256') == base.BINARY
                 and cfg.get('coverage_scope') == 'SHORT_INTERRUPTION_ONLY'
                 and cfg.get('closeout_mode') == 'flat_then_observe_clearing'
                 and cfg.get('latency_gating') is False and cfg.get('partial_fill_enabled') is False
                 and cfg.get('predecessor_clean_terminal_verified') is True,
                 'short_candidate_or_prior_clean_terminal_unproved')
    base.require(cfg.get('environment') == 'test' and cfg.get('quantity_limit') == 2
                 and cfg.get('position_limit') == 2 and cfg.get('open_order_limit') == 5
                 and cfg.get('rate_limit') == 5, 'short_native_risk_limits_changed')


class PreIntentNotSent(base.Refused):
    """The guard failed before Product persisted or dispatched an intent."""
    not_sent = True
    def __init__(self, code):
        self.code = code
        super().__init__(code)


def error_codes(error):
    result, seen = [], set()
    while error is not None and id(error) not in seen:
        seen.add(id(error))
        result.append(str(getattr(error, 'code', error)))
        error = error.__cause__ or error.__context__
    return result


def uncertain_or_mismatched(error):
    if getattr(error, 'not_sent', False):
        return False
    return any(any(word in code for word in ('command_outcome_uncertain',
        'product_command_refused_or_uncertain', 'order_identity_unresolved',
        'owned_order_or_position_changed', 'reconciliation_mismatch',
        'working_order_or_fill_baseline_changed', 'position_baseline_changed',
        'identifier_rollback')) for code in error_codes(error))


class Supervisor(base.Supervisor):
    def _cheap_owner(self, product):
        base.require(product is self.product and self.meta is not None
                     and product.child.poll() is None
                     and self.ops.process_identity(product.child.pid) == self.meta['identity'],
                     'short_owned_product_identity_changed')
        self.tick()

    def risk_snapshot(self, product, selected=None, *, verify=True):
        """One fresh snapshot grants selected-book entry; full account stays required."""
        if verify:
            self.verify_owner(product)
        self.rt.day_check(self.cfg, entry=True)
        base.require(time.time() >= self.cfg['earliest_entry_utc'],
                     'short_no_entry_before_continuous_open')
        base.require(not self.closing and not (self.work / 'stop-requested').exists(),
                     'short_new_entry_stopped')
        selected = self.targets if selected is None else selected
        base.require(selected and all(t in self.targets for t in selected),
                     'short_selected_instrument_outside_scope')
        deadline = min(time.time() + 30, self.cfg['hard_entry_deadline_utc'])
        import price_caps
        transient = {'current_price_book_unproved', 'current_price_book_stale',
                     'current_price_two_sided_book_unproved', 'passive_quote_unavailable'}
        while time.time() < deadline:
            self._cheap_owner(product)
            state = product.status()
            base.require(not any(state.get(k) for k in ('journal_failed', 'cancel_only',
                         'log_error', 'operator_action_required')), 'short_native_safety_alert')
            if self.ops.ready(state, self.targets, entry=False):
                self.rt.configuration_gate(state, self.targets, self.cfg)
                rows = {row['isin_id']: row for row in state['instruments']}
                books_ready = True
                for target in selected:
                    row = rows[target['isin_id']]
                    if not row.get('order_entry_ready') or not row.get('book', {}).get('valid'):
                        books_ready = False
                        break
                    try:
                        bid = price_caps.current_reference(state, target)
                    except ValueError as error:
                        if str(error) not in transient:
                            raise
                        books_ready = False
                        break
                    base.require(target['quote_cap_scaled'] <= 2 * bid,
                                 'frozen_conservative_cap_exceeds_twice_current_bid')
                if books_ready:
                    return state
            time.sleep(min(.2, max(0, deadline - time.time())))
        raise PreIntentNotSent('short_preintent_readiness_deadline_not_sent')

    def risk_gate(self, product):
        if getattr(self, '_fast_product', None) is product:
            self._cheap_owner(product)
            self.rt.day_check(self.cfg, entry=True)
            base.require(time.monotonic() < self._fast_deadline,
                         'short_burst_dispatch_lease_expired_not_sent')
            return
        return self.risk_snapshot(product, getattr(self, '_selected_targets', None),
                                  verify=getattr(self, '_dispatch_owner_product', None) is not product)

    def _command_isolation(self, product):
        if getattr(self, '_fast_product', None) is product or getattr(self, '_dispatch_owner_product', None) is product:
            return self._cheap_owner(product)
        return self.verify_owner(product)

    def product_class(self):
        owner, ops = self, self.ops
        class ProtectedProduct(ops.Product):
            def command(self, line, permit_risk=True):
                if line == 'status':
                    return super().command(line, permit_risk=permit_risk)
                try:
                    if getattr(owner, '_fast_product', None) is self:
                        owner._cheap_owner(self)
                    else:
                        owner.verify_owner(self)
                except Exception as error:
                    raise PreIntentNotSent(getattr(error, 'code', 'short_preintent_owner_not_sent')) from error
                owner._dispatch_owner_product = self
                previous = getattr(owner, '_selected_targets', None)
                if line.startswith(('place ', 'move ')):
                    fields = line.split()
                    if line.startswith('place '):
                        owner._selected_targets = [t for t in owner.targets if str(t['isin_id']) == fields[2]]
                    owner.publish({'orders_started': True})
                try:
                    # The unchanged Product persists one intent, invokes actual
                    # CLI once and keeps native risk/ownership/rate enforcement.
                    return super().command(line, permit_risk=permit_risk)
                finally:
                    owner._dispatch_owner_product = None
                    owner._selected_targets = previous
            def close(self):
                raise base.Refused('legacy_product_close_forbidden')
            def crash(self):
                raise base.Refused('legacy_product_crash_forbidden')
        return ProtectedProduct

    def attach(self, product):
        super().attach(product)
        product.isolation = lambda: self._command_isolation(product)
        def preintent_gate():
            try:
                return self.risk_gate(product)
            except PreIntentNotSent:
                raise
            except Exception as error:
                raise PreIntentNotSent(getattr(error, 'code', 'short_preintent_guard_not_sent')) from error
        product.risk_guard = preintent_gate
        return product

    @contextmanager
    def fast_dispatch(self, product, targets):
        base.require(getattr(self, '_fast_product', None) is None, 'short_nested_burst_forbidden')
        self.risk_snapshot(product, selected=targets)
        self._fast_product = product
        self._fast_deadline = min(time.monotonic() + 30,
            time.monotonic() + self.cfg['hard_entry_deadline_utc'] - time.time())
        try:
            yield
        finally:
            self._fast_product = None
            self._fast_deadline = 0

    def _preflight_deadline(self):
        if not hasattr(self, '_read_only_preflight_until'):
            self._read_only_preflight_until = min(time.time() + 1800, self.cfg['product_start_latest_utc'])
        return self._read_only_preflight_until

    def _preflight_pause(self):
        self.tick()
        base.require(time.time() + 2 < self._preflight_deadline(), 'short_readonly_preflight_deadline')
        time.sleep(min(2, max(0, self._preflight_deadline() - time.time())))

    def resolve(self):
        attempt = 0
        while True:
            try:
                return super().resolve()
            except Exception as error:
                # Only the isolated resolver's known completed refusal can be
                # observed again. An uncertain/live child never gets a retry.
                child = self.readonly_child
                if (str(error) not in ('fresh_refdata_resolver_refused', 'fresh_refdata_capture_unproved')
                        or child is None or child.poll() is None or self.readonly_meta is None):
                    raise
                args = self.readonly_meta['args']
                base.require('--allow-orders' not in args and '--state' not in args
                             and self.rt.sha(self.environment['state']) == self.before_live_sha,
                             'short_resolver_retry_no_order_boundary_unproved')
                inventory = self.rt.inventory(self.readonly_meta)
                base.require(inventory['client_count'] == 0 and inventory['upstreams'] == 4,
                             'short_resolver_retry_clients_or_router_unproved')
                folder = self.root / 'fresh-refdata'
                folder.rename(self.root / ('fresh-refdata-refused-' + str(attempt)))
                self.publish({'preflight_resolver_refusals': attempt + 1,
                              'preflight_resolver_retry_no_publisher': True})
                self.readonly_child = self.readonly_meta = None
                attempt += 1
                self._preflight_pause()

    def initial_flat_probe(self):
        attempt = 0
        while True:
            try:
                return super().initial_flat_probe()
            except Exception as error:
                if str(error) != 'fresh_independent_flatness_unproved':
                    raise
                folder = self.root / 'before-native-independent-flat'
                observation = json.loads((folder / 'snapshot-observation-public.json').read_text())
                good = (type(observation.get('exit')) is int and observation['exit'] in (0, 5)
                    and observation.get('stop_ok') is True
                    and type(observation.get('publisher_posts')) is int and observation['publisher_posts'] == 0
                    and type(observation.get('message_allocations')) is int and observation['message_allocations'] == 0
                    and observation.get('checkpoint_unchanged_by_probe') is True
                    and observation.get('after', {}).get('client_count') == 0
                    and observation.get('after', {}).get('upstreams') == 4)
                base.require(good, 'short_preflight_probe_retry_boundary_unproved')
                folder.rename(self.root / ('before-native-independent-flat-refused-' + str(attempt)))
                self.publish({'preflight_flat_refusals': attempt + 1,
                              'preflight_probe_retry_publisher_posts': 0})
                attempt += 1
                self._preflight_pause()

    def clock(self):
        while True:
            try:
                tracking = subprocess.run(['chronyc', 'tracking'], capture_output=True, timeout=10)
                base.require(tracking.returncode == 0, 'native_clock_tracking_unavailable')
                return self.ops.clock_proof(tracking.stdout.decode())
            except (subprocess.TimeoutExpired, self.ops.Stop, base.Refused):
                self._preflight_pause()

    def prestart_headroom(self):
        meta = dict(state=str(self.environment['state']), ext_id_range=self.cfg['ext_id_range'],
                    instance=self.cfg['instance'])
        base.require(self.rt.budget(meta) >= 9000, 'short_two_start_identifier_headroom_unproved')

    def start(self):
        # Exactly one trading start. CI/clock and publisher-free observations
        # may wait before it; live-state starts are never retried.
        self.publish({'state': 'preflight', 'phase': 'preflight'})
        self._preflight_deadline()
        self.rt.verify_ci(self.root, self.cfg, tick=self.tick)
        self.tick()
        self.work.mkdir(mode=0o700)
        folder = self.root / 'native-environment'
        folder.mkdir(mode=0o700)
        self.environment = self.bootstrap.setup(folder, json.loads((self.root / 'inputs.json').read_text()))
        base.require(self.rt.sha(self.environment['binary']) == base.PRODUCTION,
                     'frozen_production_bootstrap_binary_changed')
        base.require(self.cfg['source_git_sha'] == base.HEAD and self.cfg['binary_sha256'] == base.BINARY
                     and self.rt.sha(self.cfg['binary']) == base.BINARY, 'exact_candidate_binary_changed')
        self.environment['binary'] = self.cfg['binary']
        self.before_live_sha = self.rt.sha(self.environment['state'])
        self.resolve()
        self.initial_flat_probe()
        clock = self.clock()
        base.require(time.time() < self.cfg['product_start_latest_utc'], 'short_product_start_deadline')
        self.prestart_headroom()
        args = self.arguments(clock['offset_us'])
        self.ops.CLEANUP = self.cfg['hard_entry_deadline_utc']
        self.ops.END = self.cfg['hard_cleanup_deadline_utc'] - 300
        product = self.product_class()(self.work, self.cfg['binary'], self.environment['env'], args, self.targets, self.tick)
        product.start()
        self.attach(product)
        base.require(self.rt.budget(self.meta) >= 8000, 'short_two_start_identifier_headroom_unproved')
        self.publish({'state': 'initial_native_readiness', 'phase': 'initial_native_readiness',
                      'normal_native_started_utc_ns': time.time_ns(), 'clock': clock})
        initial = product.wait_ready(self.cfg['admission_latest_utc'], require_flat=True, entry=False)
        self.coverage_gate(initial)
        return product

    def _wait_fault_restoration(self):
        pending = getattr(self, 'pending_fault_controller', None)
        if pending is None and not getattr(self, 'fault_restore_required', False):
            return
        # A worker may already have exited with failure while its independent
        # watchdog is still restoring the router. Worker exit is not restoration.
        self.fault_restore_required = True
        until = min(time.time() + 180, self.cfg['hard_cleanup_deadline_utc'] - 20)
        while True:
            self.publish({'phase': 'fault_restoration_pending', 'terminal': False, 'flat': 'UNKNOWN'})
            base.require(time.time() < until, 'short_fault_restore_pending_no_safe_cleanup')
            self.tick()
            if pending is not None and pending.fault_worker is not None:
                try:
                    pending.wait_fault_worker()
                except Exception:
                    if pending.fault_worker is not None:
                        base.require(time.time() < until, 'short_fault_restore_pending_no_safe_cleanup')
                        time.sleep(min(.2, max(0, until - time.time())))
                        continue
            try:
                inventory = self.verify_owner(self.product)
                base.require(type(inventory.get('client_count')) is int and inventory['client_count'] == 1
                             and type(inventory.get('upstreams')) is int and inventory['upstreams'] == 4,
                             'short_fault_restore_router_inventory_unproved')
                state = self.product.status()
                base.require(self.ops.ready(state, self.targets, entry=False),
                             'short_fault_restore_full_native_reconstruction_pending')
                self.rt.configuration_gate(state, self.targets, self.cfg)
            except Exception:
                base.require(time.time() < until, 'short_fault_restore_pending_no_safe_cleanup')
                time.sleep(min(.2, max(0, until - time.time())))
                continue
            break
        # Retain the canonical cross-controller lock until both the worker and
        # actual native/router restoration are proved, then perform cleanup.
        if pending is not None:
            os.close(pending.guard_control)
            pending.fault_lock.close()
            del self.pending_fault_controller
        self.fault_restore_required = False

    def safe_case_failure(self, product, error, name):
        self._wait_fault_restoration()
        base.require(not uncertain_or_mismatched(error), 'short_unsafe_case_failure_stops_session')
        self.verify_owner(product)
        result = self.flatten(product, min(time.time() + 180, self.cfg['flat_deadline_utc']), final=False)
        state = product.status()
        base.require(result.get('cancel_confirmed') is True and result.get('flat') is True
                     and self.ops.ready(state, self.targets, entry=False) and self.ops.flat(state),
                     'short_case_failure_flatness_unproved')
        inventory = self.rt.verify_owner(self.meta)
        base.require(inventory['client_count'] == 1 and inventory['upstreams'] == 4,
                     'short_case_failure_router_restoration_unproved')
        return dict(cancel_confirmed=True, flat=True, restored=True, upstreams=4,
                    client_count=1, complete_reconstruction=True, failed_case=name)

    def run(self):
        validate(self.cfg)
        self.fault_bridge.authorized(self.fault_core, self, self.cfg, preflight=True)
        self.rt.atomic(self.root / 'supervisor-owned.json', self.ops.process_identity(os.getpid()))
        self.publish()
        while time.time() < self.cfg['scheduled_start_utc']:
            base.require(not (self.root / 'stop-requested').exists(), 'prestart_stop_requested')
            base.require(self.rt.sha('/proc/sys/kernel/random/boot_id') == self.rt.BOOT, 'prestart_boot_changed')
            self.publish()
            time.sleep(max(0, min(5, self.cfg['scheduled_start_utc'] - time.time())))
        self.tick()
        # The canonical helper validates pristine config and caches its resolved
        # whitelist before any order; later scans still validate all endpoints.
        probe, guard, upstream = self.rt._probe()
        launch, health = probe.inspect({'environment': 'test'})
        base.require(health['client_count'] == 0, 'short_preflight_competing_client')
        white = upstream.pristine_whitelist(probe, launch, self.rt.WHITE)
        base.require(len(guard.strict_upstreams(upstream, probe, launch.pid, white)) == 4,
                     'short_preflight_four_whitelisted_upstreams_unproved')
        base.require(time.time() < self.cfg['product_start_latest_utc'], 'short_product_start_window_missed')
        with (self.root / 'execution-claim.json').open('x') as out:
            json.dump(dict(claimed_utc_ns=time.time_ns(), source_git_sha=base.HEAD), out)
            out.flush(); os.fsync(out.fileno())
        product = self.start()
        self.sequence, self.prices = self.driver.sequence, self.driver.price_caps
        faultcase = self.fault_bridge.build_faultcase(self, self.fault_core, self.router_helper, self.upstream_helper)
        env = dict(account=self.environment['env']['MOEX_TEST_CLIENT'], restart=self.restart,
                   flatten=lambda p, deadline: self.flatten(p, deadline), closeout=self.closeout,
                   verify_owner=self.verify_owner, tick=self.tick, faultcase=faultcase,
                   fast_dispatch=self.fast_dispatch, safe_case_failure=self.safe_case_failure)
        result = self.driver.run(product, self.cfg, self.work, env, self.publish)
        self.publish(dict(result, state='complete', phase='terminal_closeout', no_guardian_failure=True,
                          qualification_result='SUPPLEMENTAL_TEST_COMPLETE'))
        return result

    def fail(self, error):
        try:
            self._wait_fault_restoration()
        except Exception:
            self.publish(dict(state='BLOCKED', phase='fault_restoration_pending', flat='UNKNOWN',
                              terminal=False, initiating_failure=type(error).__name__,
                              cleanup_error='independent_restore_still_pending'))
            raise
        return super().fail(error)


def load_fault_helpers(load, runtime, cfg):
    """Install one canonical module/class before any historical probe imports."""
    router = load('router_recovery.py', 'router_recovery')
    upstream = load('upstream_recovery.py', 'upstream_recovery')
    base.require(runtime.sha(router.__file__) == cfg['router_helper_sha256']
                 and runtime.sha(upstream.__file__) == cfg['upstream_helper_sha256'],
                 'short_canonical_helper_bytes_changed')
    helpers = load('short_fault_helpers.py', 'short_fault_helpers')
    helpers.install(router, upstream, preflight_deadline=cfg['product_start_latest_utc'])
    return router, upstream


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--execute', action='store_true')
    args = parser.parse_args()
    base.require(args.execute, 'short_execution_not_requested')
    os.umask(0o077)
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.root))
    import runtime
    import ops
    cfg = runtime.load_cfg(args.root)
    validate(cfg)
    load = lambda name, logical: base.module_from(args.root, name, logical)
    # Canonical names precede _probe's historical imports. In particular every
    # router user must see the SAME Refused class for bounded rollback retries.
    router, upstream = load_fault_helpers(load, runtime, cfg)
    bootstrap = load(cfg.get('bootstrap', 'bootstrap.py'), 'short_bootstrap')
    load('frozen_day_driver.py', 'day_driver')
    driver = load('short_session.py', 'short_day_driver')
    guardian = load('guardian.py', 'short_guardian')
    coverage = load(cfg.get('coverage_module', 'coverage_proof.py'), 'short_coverage')
    runner = Supervisor(args.root, cfg, runtime, ops, bootstrap, driver, guardian, coverage)
    runner.fault_bridge = load('short_fault_bridge.py', 'short_fault_bridge')
    core = load('working_fault_core.py', 'working_fault_core')
    runner.fault_core = load('short_fault_core.py', 'short_fault_core').install(core)
    runner.router_helper, runner.upstream_helper = router, upstream
    with (args.root / 'supervisor.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            runner.run()
        except BaseException as error:
            runner.fail(error)
            return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
