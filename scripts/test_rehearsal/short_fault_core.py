"""Reviewable fixes around the immutable, hash-pinned interruption controller.

``install`` composes with the real frozen core. It performs no I/O, fork, signal
or command. The supervisor alone supplies an owned product and authorization.
"""
from pathlib import Path
import json
import os
import signal
import subprocess
import time
import traceback
import uuid


def native_outage(state):
    """Use all three native link legs, including the reply listener."""
    transport = state['transport']
    reply = next((row for row in transport['listeners'] if row['stream'] == 'p2mqreply'), None)
    return (transport['connection']['state'] != 3 or transport['publisher']['state'] != 3 or
            reply is None or reply['native']['state'] != 3)


def install(core):
    """Replace only the controller class; leave frozen source and helpers intact."""
    if getattr(core.Controller, '_short_fault_corrected', False):
        return core
    original = core.Controller

    class Controller(original):
        _short_fault_corrected = True

        def one_working(self, state):
            if not getattr(self, 'dry_run_no_order', False):
                return super().one_working(state)
            core.require(self.ops.flat(state), 'dry_run_requires_complete_native_flatness')
            return None

        def preserve_order(self, before, after):
            if getattr(self, 'dry_run_no_order', False):
                core.require(self.ops.flat(before) and self.ops.flat(after),
                             'dry_run_inventory_changed')
            else:
                super().preserve_order(before, after)

        def raw_refusal_request(self, line):
            """One CLI attempt; a timeout needs positive native refusal proof.

            No posts/IDs by themselves cannot prove that an accepted command
            will not execute later. Only the exact completed native kill-guard
            refusal can resolve this timeout, and the guard must remain current.
            """
            core.require(self.ops.process_identity(self.product.child.pid) == self.product_identity,
                         'outage_probe_owner_changed')
            core.require(core.sha(self.binary) == self.cfg['binary_sha256'], 'outage_probe_binary_changed')
            guard = getattr(self.product, 'guard', None)
            core.require(guard is not None, 'outage_probe_current_native_kill_guard_missing')
            guard.prove()
            path = self.product.work / 'interaction.ndjson'
            offset = path.stat().st_size
            highwaters = core.checkpoint_highwaters(self.environment['state'])
            with self.ops.lock(self.product.work / 'command.lock'):
                with (self.product.work / 'operator-private.ndjson').open('a') as out:
                    out.write(json.dumps(dict(utc_ns=time.time_ns(), intent=line,
                                             expected_local_refusal=True)) + '\n')
                    out.flush()
                    os.fsync(out.fileno())
                child = subprocess.Popen([str(self.binary), 'plaza2', 'cmd', '--command-socket',
                                          str(self.product.work / 'cmd.sock'), line],
                    cwd=self.product.work, env=self.environment['env'],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                owner = self.ops.process_identity(child.pid)
                timed_out = False
                try:
                    stdout, stderr = child.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    try:
                        self.ops.checked_signal(owner, signal.SIGTERM)
                    except BaseException as error:
                        raise core.Refused('outage_probe_uncertain_cli_shutdown_unproved_never_retried') from error
                    try:
                        stdout, stderr = child.communicate(timeout=2)
                    except subprocess.TimeoutExpired as error:
                        raise core.Refused('outage_probe_uncertain_cli_exit_unproved_never_retried') from error
                with (self.product.work / 'operator-private.ndjson').open('a') as out:
                    out.write(json.dumps(dict(utc_ns=time.time_ns(), line=line, exit=child.returncode,
                        stdout=stdout.decode(errors='replace'), stderr=stderr.decode(errors='replace'),
                        cli_timeout_seconds=10, cli_timed_out=timed_out)) + '\n')
                if not timed_out:
                    try:
                        value = json.loads(stdout)
                    except (ValueError, UnicodeError) as error:
                        raise core.Refused('outage_probe_uncertain_invalid_cli_response_never_retried') from error
                    core.require(isinstance(value, dict) and child.returncode != 0 and value.get('ok') is False and
                                 value.get('error') == 'kill switch enabled',
                                 'outage_probe_uncertain_not_definite_local_kill_switch_refusal')
                    self.last_outage_probe_cli = dict(timed_out=False, timeout_seconds=10,
                                                       outcome='DEFINITE_LOCAL_REFUSAL', attempts=1)
                    return
                until = min(self.cfg['cleanup_deadline_utc'], time.time() + 5)
                while True:
                    core.require(self.ops.process_identity(self.product.child.pid) == self.product_identity,
                                 'outage_probe_uncertain_owner_changed_never_retried')
                    try:
                        guard.prove()
                    except BaseException as error:
                        raise core.Refused('outage_probe_uncertain_native_guard_unproved_never_retried') from error
                    rows = list(core.complete_events(path, offset))
                    completed = any(event['event'] == 'local_refusal' and
                                    event['data'].get('line') == line for event in rows)
                    if completed:
                        try:
                            proof = core.verify_local_probe(rows, line, highwaters,
                                                           core.checkpoint_highwaters(self.environment['state']))
                        except BaseException as error:
                            raise core.Refused('outage_probe_uncertain_native_refusal_proof_failed_never_retried') from error
                        self.last_outage_probe_cli = dict(timed_out=True, timeout_seconds=10,
                            outcome='NOT_SENT_NATIVE_REFUSAL_COMPLETED', attempts=1,
                            native_completion=proof)
                        return
                    core.require(time.time() < until, 'outage_probe_uncertain_never_retried')
                    self.touch()
                    time.sleep(.02)

        def bounded_outage_probe(self, name, folder, observed_state):
            # Do not fetch status again after observing a brief native outage.
            core.require(name not in self.probe_attempts, 'outage_probe_must_not_be_retried')
            fault = json.loads((folder / 'fault.json').read_text())
            core.require(fault.get('rollback_armed') is True and
                         (fault.get('signal_sent') is True or fault.get('upstream_fault') is True and
                          fault.get('interrupted_socket_count', 0) > 0), 'actual_fault_receipt_missing')
            core.require(native_outage(observed_state), 'native_outage_not_observed')
            core.require(observed_state['configuration']['risk']['kill_switch'] is True,
                         'outage_probe_kill_switch_not_enabled')
            self.one_working(observed_state)
            line = (f"place q-outage-{uuid.uuid4().hex[:12]} {self.passive_target['isin_id']} buy 1 "
                    f"{self.sequence._price(self.passive_price)} day")
            offset = (self.product.work / 'interaction.ndjson').stat().st_size
            before = core.checkpoint_highwaters(self.environment['state'])
            self.probe_attempts.add(name)  # One intent, including failure/timeout; never retry.
            self.raw_refusal_request(line)
            after = core.checkpoint_highwaters(self.environment['state'])
            until = min(self.cfg['cleanup_deadline_utc'], time.time() + 5)
            while True:
                rows = list(core.complete_events(self.product.work / 'interaction.ndjson', offset))
                if any(e['event'] == 'local_refusal' and e['data'].get('line') == line for e in rows):
                    break
                core.require(time.time() < until, 'local_refusal_journal_flush_deadline')
                self.touch()
                time.sleep(.02)
            proof = core.verify_local_probe(rows, line, before, after)
            proof.update(actual_fault_observed=True, probe_attempts=1,
                         observed_outage_utc_ns=getattr(self, 'observed_outage_utc_ns', None))
            if getattr(self, 'last_outage_probe_cli', None) is not None:
                proof['cli'] = self.last_outage_probe_cli
            return proof

        def wait_ready(self, flat=False, timeout=300):
            # A key check refusal is expected while the exact owned router is
            # unavailable. It cannot authorize entry, and must clear by deadline.
            deadline = min(self.cfg['cleanup_deadline_utc'], time.time() + timeout)
            while time.time() < deadline:
                self.touch()
                core.require(self.product.child.poll() is None, 'product_exited')
                state = self.product.status()
                core.require(not any(state.get(key) for key in
                             ('journal_failed', 'cancel_only', 'log_error', 'operator_action_required')),
                             'product_safety_alert')
                if self.ops.ready(state, self.targets, entry=False) and (not flat or self.ops.flat(state)):
                    self.inspect()
                    self.configuration(state)
                    self.monotonic_ids()
                    return state
                time.sleep(.1)
            raise core.Refused('fault_readiness_or_flatness_deadline')

        def working_fault(self, name, kind):
            before = self.entry_gate()
            self.one_working(before)
            core.require(self.cfg['cleanup_deadline_utc'] - time.time() >= 150,
                         'fault_restore_and_cleanup_budget_insufficient')
            core.require(before['configuration']['risk']['kill_switch'] is True,
                         'fault_requires_kill_switch_guard')
            if getattr(self, 'dry_run_no_order', False):
                # The Place is blocked by the proven native kill switch; the
                # ordinary fixed cap and selected TEST symbol remain in scope.
                self.passive_price = min(self.passive_target['quote_cap_scaled'], 1)
            else:
                self.passive_price = self.sequence._units(next(row['price'] for row in before['orders']
                                                        if row['client_order_id'] == self.passive_cid))
            offset = (self.product.work / 'interaction.ndjson').stat().st_size
            folder = self.work / name
            folder.mkdir(mode=0o700)
            self.public['phase'] = name
            self.touch()
            started = time.monotonic()
            worker = os.fork()
            if worker == 0:
                os.close(self.guard_control)
                identity, ops = self.product_identity, self.ops

                class OwnedProcess:
                    pid = identity['pid']

                    def poll(child):
                        try:
                            core.require(ops.process_identity(child.pid) == identity,
                                         'fault_worker_product_identity_changed')
                            return None
                        except (FileNotFoundError, ProcessLookupError):
                            return 0

                self.product.child = OwnedProcess()
                try:
                    auth = self.fault_authorization()
                    if kind == 'router':
                        result = self.router.restart(self.probe, auth, self.product.child.pid, folder,
                            arm=lambda *args: self.router.arm_rollback(*args, keep_fd=self.fault_lock.fileno()))
                        witness = offset
                    else:
                        witness = [None]

                        def acknowledge():
                            ready = self.wait_ready(timeout=40)
                            self.preserve_order(before, ready)
                            witness[0] = (self.product.work / 'interaction.ndjson').stat().st_size

                        result = self.upstream.interrupt(self.probe, auth, self.product.child.pid, folder,
                                                        self.fault_lock.fileno(), acknowledge=acknowledge)
                        witness = witness[0]
                    core.atomic(folder / 'helper-result-private.json', dict(result=result, witness_offset=witness))
                    os._exit(0)
                except BaseException as error:
                    (folder / 'helper-error-private.txt').write_text(traceback.format_exc())
                    core.atomic(folder / 'helper-error-public.json',
                                dict(error_class=type(error).__name__, fault_result='UNKNOWN'))
                    os._exit(2)
            self.fault_worker = worker
            until = min(self.cfg['entry_deadline_utc'], time.time() + 150)
            refusal = status = exited_at = None
            try:
                while time.time() < until:
                    self.touch()
                    if (folder / 'helper-error-public.json').exists():
                        raise core.Refused('independent_fault_helper_failed')
                    if refusal is None and (folder / 'fault.json').exists():
                        fault = json.loads((folder / 'fault.json').read_text())
                        if fault.get('signal_sent') is True or fault.get('upstream_fault') is True:
                            state = self.product.status()
                            if native_outage(state):
                                self.observed_outage_utc_ns = time.time_ns()
                                refusal = self.bounded_outage_probe(name, folder, state)
                    if self.fault_worker is not None:
                        done, code = os.waitpid(worker, os.WNOHANG)
                        if done:
                            status, exited_at, self.fault_worker = code, time.time(), None
                    if status is not None:
                        if status != 0 or refusal is not None:
                            break
                        # Journal/native state may lag the returning helper.
                        if time.time() >= min(until, exited_at + 10):
                            break
                    time.sleep(.02)
                core.require(status == 0 and refusal is not None,
                             'fault_or_single_refusal_not_completed_within_bound')
            finally:
                self.wait_fault_worker()
            saved = json.loads((folder / 'helper-result-private.json').read_text())
            after = self.wait_ready()
            self.one_working(after)
            self.preserve_order(before, after)
            proof_until = min(self.cfg['entry_deadline_utc'], time.time() + 10)
            while True:
                events = list(core.complete_events(self.product.work / 'interaction.ndjson', saved['witness_offset']))
                if any(e['event'] == 'transaction_commit' and e['data'].get('stream') == 'FORTS_TRADE_REPL'
                       for e in events):
                    break
                core.require(time.time() < proof_until, 'postfault_trade_commit_deadline')
                self.touch()
                time.sleep(.05)
            witness = core.selective_recovery(events, after, self.ops.REQUIRED, kind)
            self.public['cases'].append(dict(case=name, status='PASS',
                dry_run_no_order=getattr(self, 'dry_run_no_order', False),
                working_order_preserved=not getattr(self, 'dry_run_no_order', False),
                zero_orders_verified=getattr(self, 'dry_run_no_order', False),
                local_refusal=refusal, recovery=saved['result'], native_witness=witness,
                journal_offset_begin=offset,
                journal_offset_end=(self.product.work / 'interaction.ndjson').stat().st_size,
                duration_ms=round((time.monotonic() - started) * 1000), ready_utc_ns=time.time_ns()))
            self.touch()

    core.Controller = Controller
    return core
