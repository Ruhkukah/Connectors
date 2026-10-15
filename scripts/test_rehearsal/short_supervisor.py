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
    if getattr(error, 'rate_only', False) is True and type(error).__name__ == 'RateCheckFailed':
        return False
    if getattr(error, 'not_sent', False):
        return False
    return any(any(word in code for word in ('command_outcome_uncertain',
        'product_command_refused_or_uncertain', 'order_identity_unresolved',
        'owned_order_or_position_changed', 'reconciliation_mismatch',
        'working_order_or_fill_baseline_changed', 'position_baseline_changed',
        'identifier_rollback')) for code in error_codes(error))


def supplemental_qualification(result):
    rows = result.get('scenarios', {})
    return ('SUPPLEMENTAL_TEST_COMPLETE' if result.get('qualification_result') == 'SUPPLEMENTAL_TEST_COMPLETE'
            and result.get('terminal') is True and result.get('execution_result') == 'PASS'
            and isinstance(rows,dict) and rows and all(row.get('status') == 'PASS' for row in rows.values())
            else 'SUPPLEMENTAL_TEST_INCOMPLETE')


def cancel_scope_proof(rows, isin, complete_flat):
    """Only post-request native empty evidence or reply/later commit can confirm."""
    commands, acknowledged, requested = set(), None, False
    for row in rows:
        event, data = row['event'], row['event'].get('data',{})
        if (event.get('event') == 'operator_input' and data.get('line') == 'cancel-all '+str(isin)
                and data.get('channel') == 'command_socket'):
            requested = True
        if not requested:
            continue
        if (event.get('event') == 'cancel_all_empty' and data.get('isin_id') == isin
                and data.get('snapshot_confirmed') is True and complete_flat):
            return dict(confirmed=True,mode='native_complete_empty_scope',native_offset=row['begin'])
        if (event.get('event') == 'command' and data.get('name') == 'DelUserOrders'
                and data.get('fields',{}).get('isin_id') == isin):
            commands.add(data.get('user_id'))
        elif (event.get('event') == 'reply' and data.get('user_id') in commands
                and data.get('msgid') == 186):
            if type(data.get('code')) is not int or data['code'] != 0:
                return dict(confirmed=False,conflicting_native_reply=True)
            acknowledged = row['end']
        elif (acknowledged is not None and row['begin'] >= acknowledged
                and event.get('event') == 'transaction_commit' and data.get('stream') == 'FORTS_TRADE_REPL'):
            return dict(confirmed=True,mode='matching_code0_reply_and_later_TRADE_commit',native_offset=row['end'])
    return dict(confirmed=False)


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
            def start(self):
                # One native start only. Capture its exact ownership before any
                # metadata write/attachment can fail; never create a third boot.
                if ops.sha(self.binary) != self.expected_sha:
                    raise ops.Stop('product_binary_changed')
                stdout=(self.work/'product-stdout-private.log').open('ab')
                stderr=(self.work/'product-stderr-private.log').open('ab')
                try:
                    self.child=ops.subprocess.Popen(self.args,cwd=self.work,env=self.env,
                        stdin=subprocess.DEVNULL,stdout=stdout,stderr=stderr,start_new_session=True)
                finally:
                    stdout.close();stderr.close()
                for _ in range(20):
                    try:
                        identity=ops.process_identity(self.child.pid)
                        break
                    except FileNotFoundError:
                        time.sleep(.01)
                else:
                    raise ops.Stop('product_start_failed')
                base.require(Path(identity['executable']).resolve() == Path(self.binary).resolve(),
                             'product_binary_identity_mismatch')
                owner.latest_native_product,owner.latest_native_identity=self,identity
                ops.atomic(self.work/'product-owned.json',identity)
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
                if (observation.get('fresh_private_snapshot_ready') is True
                        and (type(observation.get('working_orders')) is int and observation['working_orders'] > 0
                             or type(observation.get('nonzero_positions')) is int and observation['nonzero_positions'] > 0)):
                    raise base.Refused('short_preflight_actual_nonflat_account')
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
        was_closing = self.closing
        if getattr(self,'fault_restore_required',False) or getattr(self,'pending_fault_controller',None) is not None:
            proof = self.emergency_cancel()  # Dispatch both risk-reducing requests before waiting.
            base.require(proof.get('cancel_confirmed') is True and proof.get('flat') is True,
                         'short_fault_emergency_cancellation_or_flatness_unproved')
        try:
            self._wait_fault_restoration()
        except Exception:
            self.restore_wait_failed = True
            raise
        self.closing = was_closing
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
                          qualification_result=supplemental_qualification(result)))
        return result

    def restart(self, product, saved):
        # Keep an on-host ownership witness before the planned crash. This is
        # evidence only; the unchanged base performs the sole planned restart.
        self.last_restart_working_count = len(saved)
        try:
            self.rt.atomic(self.root/'planned-restart-working-private.json',
                           dict(saved=saved,observed_utc_ns=time.time_ns()))
        except OSError:
            raise base.Refused('planned_restart_order_witness_not_persisted')
        return super().restart(product,saved)

    def _emergency_owner(self):
        product = getattr(self,'latest_native_product',None)
        identity = getattr(self,'latest_native_identity',None)
        if product is None:
            product,identity = self.product,(self.meta or {}).get('identity')
        base.require(product is not None and identity is not None and product.child.poll() is None
                     and self.ops.process_identity(product.child.pid) == identity
                     and Path(identity['executable']).resolve() == Path(product.binary).resolve()
                     and self.rt.sha(product.binary) == base.BINARY
                     and product.args.count('--command-socket') == 1
                     and Path(product.args[product.args.index('--command-socket')+1]) == self.work/'cmd.sock',
                     'emergency_exact_owned_product_or_socket_unproved')
        return product,identity

    def emergency_cancel(self):
        """Acquire the same cleanup lock as the guardian; never race another owner."""
        product,identity = self._emergency_owner()
        self.publish(dict(emergency_product_pid=identity['pid'],
                          emergency_product_start_ticks=identity['start_ticks']))
        try:
            self.rt.atomic(self.root/'emergency-product-owned-private.json',identity)
        except OSError:
            self.publish(dict(emergency_identity_receipt_write_failed=True))
        try:
            lock=self.ops.lock(self.work/'cleanup.lock',nonblocking=True)
        except BlockingIOError:
            result=dict(cancel_confirmed=False,flat='UNKNOWN',cleanup_lock_held=True,
                        no_duplicate_cancel_sent=True,no_new_native_process=True)
            self.publish(dict(state='BLOCKED',phase='another_cleanup_owner_active',terminal=False,
                              emergency_cleanup=result,flat='UNKNOWN'))
            return result
        with lock:
            return self._emergency_cancel_owned(product,identity)

    def _emergency_cancel_owned(self,product,identity):
        """Try both native scopes once, even when four upstreams are unproved."""
        base.require(len(self.targets) == 2 and {t['symbol'] for t in self.targets}
                     == {'ALRS-12.26','CNY-12.26'} and len({t['isin_id'] for t in self.targets}) == 2,
                     'emergency_cancel_two_TEST_scopes_unproved')
        self.closing = True
        journal=self.work/'interaction.ndjson'
        attempts=getattr(self,'emergency_cancel_attempts',{})
        self.emergency_cancel_attempts=attempts
        deadline=min(time.time()+180,self.cfg['hard_cleanup_deadline_utc']-1)
        for target in self.targets:
            isin=target['isin_id']
            if isin in attempts:
                continue  # Claimed requests, including uncertain ones, never retry.
            base.require(time.time()<deadline,'emergency_cancel_hard_deadline')
            self._emergency_owner()
            row=dict(isin_id=isin,attempted=True,request_utc_ns=time.time_ns(),
                     journal_begin=journal.stat().st_size,confirmed=False)
            attempts[isin]=row  # Claim before actual CLI; never resend this scope.
            line='cancel-all '+str(isin)
            try:
                with (self.work/'operator-private.ndjson').open('a') as output:
                    output.write(json.dumps(dict(utc_ns=time.time_ns(),intent=line,
                        emergency_risk_reducing=True,owned_pid=identity['pid']))+'\n')
                    output.flush();os.fsync(output.fileno())
            except OSError:
                row['intent_receipt_write_failed']=True
            try:
                result=subprocess.run([product.binary,'plaza2','cmd','--command-socket',
                    str(self.work/'cmd.sock'),line],cwd=self.work,env=product.env,
                    capture_output=True,timeout=max(.1,min(10,deadline-time.time())))
                data=json.loads(result.stdout)
                row['cli_accepted']=(result.returncode==0 and isinstance(data,dict) and data.get('ok') is True)
                if not row['cli_accepted']:
                    row['command_outcome']='REFUSED_OR_UNCERTAIN_NEVER_RETRIED'
            except Exception as error:
                row['command_outcome']='UNCERTAIN_NEVER_RETRIED'
                row['error_class']=type(error).__name__
        complete_flat=False
        while time.time()<deadline:
            self.publish(dict(state='BLOCKED',phase='emergency_cancel_confirmation',terminal=False,
                              emergency_cancel=list(attempts.values()),flat='UNKNOWN'))
            self._emergency_owner()
            try:
                response=subprocess.run([product.binary,'plaza2','cmd','--command-socket',
                    str(self.work/'cmd.sock'),'status'],cwd=self.work,env=product.env,
                    capture_output=True,timeout=max(.1,min(5,deadline-time.time())))
                state=json.loads(response.stdout)
                self.rt.configuration_gate(state,self.targets,self.cfg)
                complete_flat=(response.returncode==0 and self.ops.ready(state,self.targets,entry=False)
                               and self.ops.flat(state))
                for isin,row in attempts.items():
                    rows=[]
                    with journal.open('rb') as source:
                        source.seek(row['journal_begin'])
                        while True:
                            begin=source.tell();line=source.readline()
                            if not line or not line.endswith(b'\n'):break
                            rows.append(dict(begin=begin,end=source.tell(),event=json.loads(line)))
                    row.update(cancel_scope_proof(rows,isin,complete_flat))
                if complete_flat and all(row.get('confirmed') is True for row in attempts.values()):
                    break
            except Exception:
                pass  # Read-only confirmation may catch up; commands never retry.
            time.sleep(min(.5,max(0,deadline-time.time())))
        proof=dict(cancel_confirmed=all(row.get('confirmed') is True for row in attempts.values()),
                   flat=complete_flat,attempts=list(attempts.values()),
                   no_new_native_process=True,router_restoration_not_required_for_cancel_attempt=True)
        self.publish(dict(state='BLOCKED',phase='emergency_cancel_result',terminal=False,
                          emergency_cleanup=proof,flat=True if complete_flat else 'UNKNOWN'))
        return proof

    def _no_product_alarm(self,error):
        self.publish(dict(state='BLOCKED',phase='orders_working_no_product',terminal=False,
            orders_working_no_product=True,last_known_working_order_count=getattr(self,'last_restart_working_count',None),
            initiating_failure=getattr(error,'code',type(error).__name__),
            flat='UNKNOWN',cancel_confirmed=False,replacement_attachment_unproved=True,
            recovery_required=True,no_third_native_boot_authorized=True,qualification_result='SUPPLEMENTAL_TEST_INCOMPLETE'))

    def _stop_unattached_after_flat(self,proof):
        product,identity=self._emergency_owner()
        if proof.get('cancel_confirmed') is not True or proof.get('flat') is not True:
            self.publish(dict(state='BLOCKED',phase='unattached_product_requires_operator',terminal=False,
                flat='UNKNOWN',cancel_confirmed=False,emergency_product_pid=identity['pid'],
                emergency_product_start_ticks=identity['start_ticks'],no_third_native_boot_authorized=True))
            return
        meta=self.rt.metadata(self.root,self.cfg,self.environment,product)
        base.require(meta['identity']==identity,'emergency_metadata_identity_changed')
        # Retrying a metadata write starts nothing and restores guardian visibility.
        self.product,self.meta=product,meta
        try:
            self.rt.atomic(self.root/'active-product-private.json',meta)
        except OSError:
            self.publish(dict(emergency_metadata_write_failed=True))
        base.require(not self.shutdown_sent,'emergency_single_SIGTERM_only')
        self.shutdown_sent=True  # Uncertain signals are never repeated.
        self.rt.owned_sigterm_once(meta)
        product.child.wait(timeout=max(.1,min(20,self.cfg['hard_cleanup_deadline_utc']-time.time())))
        result=dict(product_exit=product.child.returncode,cancel_confirmed=True,flat=True,
                    no_third_native_boot_authorized=True)
        try:
            result['after']=self.rt.inventory(meta)
        except Exception as error:
            result['inventory_error']=type(error).__name__
        self.publish(dict(state='BLOCKED',phase='unattached_replacement_closeout',terminal=False,
                          emergency_replacement_closeout=result))

    def fail(self, error):
        self.publish(dict(state='BLOCKED',initiating_failure=getattr(error,'code',type(error).__name__),
                          qualification_result='SUPPLEMENTAL_TEST_INCOMPLETE'))
        emergency=None
        try:
            emergency=self.emergency_cancel()  # Both requests precede any restoration wait.
        except Exception as cleanup_error:
            self.publish(dict(emergency_cleanup_error=getattr(cleanup_error,'code',type(cleanup_error).__name__)))
        product=getattr(self,'latest_native_product',self.product)
        unattached=(getattr(self,'recovery_needed',False) and (product is None or product.child.poll() is not None
                    or self.meta is None or self.meta.get('identity') != getattr(self,'latest_native_identity',self.meta.get('identity'))))
        if unattached:
            if emergency is None:
                self._no_product_alarm(error)
            else:
                try:self._stop_unattached_after_flat(emergency)
                except Exception as cleanup_error:
                    self.publish(dict(state='BLOCKED',phase='unattached_product_requires_operator',terminal=False,
                        flat='UNKNOWN',cleanup_error=getattr(cleanup_error,'code',type(cleanup_error).__name__),
                        no_third_native_boot_authorized=True))
            return
        try:
            if getattr(self,'restore_wait_failed',False):
                raise base.Refused('prior_restore_wait_failed')
            self._wait_fault_restoration()
        except Exception as restore_error:
            self.publish(dict(restoration_error=getattr(restore_error,'code',type(restore_error).__name__),
                              flat='UNKNOWN',terminal=False))
            return
        if emergency is not None and (emergency.get('cancel_confirmed') is not True or emergency.get('flat') is not True):
            return  # Never repeat an uncertain/unconfirmed emergency cancellation.
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
