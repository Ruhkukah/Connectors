"""One sealed Oct16 TEST session. No action occurs on import.

All native startup, planned restart, cleanup and independent guardian proof are
the immutable predecessor's reviewed implementations. This adapter supplies
the new date, shorter scope and existing owned fault callbacks only.
"""
import argparse
import datetime as dt
import fcntl
import json
import os
from pathlib import Path
import sys
import time

import base_supervisor as base

DATE = '2026-10-16'
base.AUTHORIZED_DATE = DATE


def epoch(hour, minute=0):
    return dt.datetime(2026, 10, 16, hour, minute, tzinfo=base.MSK).timestamp()


def validate(cfg):
    base.require(cfg.get('authorized_date') == DATE and cfg.get('owner_authorized') is True
                 and cfg.get('timer_paused') is True and cfg.get('no_watchdog_sigkill') is True,
                 'short_owner_scope_unproved')
    base.require(cfg.get('source_git_sha') == base.HEAD and cfg.get('binary_sha256') == base.BINARY
                 and cfg.get('coverage_scope') == 'SHORT_INTERRUPTION_ONLY'
                 and cfg.get('closeout_mode') == 'flat_then_observe_clearing'
                 and cfg.get('latency_gating') is False
                 and cfg.get('partial_fill_enabled') is False
                 and cfg.get('predecessor_clean_terminal_verified') is True,
                 'short_candidate_or_prior_clean_terminal_unproved')
    for key, stamp in {
        'scheduled_start_utc': epoch(9), 'product_start_latest_utc': epoch(10, 10),
        'admission_latest_utc': epoch(10, 15), 'earliest_entry_utc': epoch(10),
        'hard_entry_deadline_utc': epoch(10, 45), 'flat_deadline_utc': epoch(10, 50),
        'guardian_trigger_utc': epoch(10, 55), 'hard_cleanup_deadline_utc': epoch(11),
    }.items():
        base.require(cfg.get(key) == stamp, 'short_sealed_window_changed')
    base.require(cfg.get('environment') == 'test' and cfg.get('quantity_limit') == 2
                 and cfg.get('position_limit') == 2 and cfg.get('open_order_limit') == 5
                 and cfg.get('rate_limit') == 5, 'short_native_risk_limits_changed')


class Supervisor(base.Supervisor):
    def start(self):
        # Two planned starts reserve monotonic blocks; neither can lower the
        # native 7,000-ID gate. There is no retry of an uncertain startup.
        product = super().start()
        base.require(self.rt.budget(self.meta) >= 8000,
                     'short_two_start_identifier_headroom_unproved')
        return product

    def risk_gate(self, product):
        base.require(time.time() >= self.cfg['earliest_entry_utc'],
                     'short_no_entry_before_friday_continuous_open')
        return super().risk_gate(product)

    def run(self):
        validate(self.cfg)
        self.rt.atomic(self.root / 'supervisor-owned.json', self.ops.process_identity(os.getpid()))
        self.publish()
        while time.time() < self.cfg['scheduled_start_utc']:
            base.require(not (self.root / 'stop-requested').exists(), 'prestart_stop_requested')
            base.require(self.rt.sha('/proc/sys/kernel/random/boot_id') == self.rt.BOOT,
                         'prestart_boot_changed')
            self.publish()
            time.sleep(min(5, self.cfg['scheduled_start_utc'] - time.time()))
        self.tick()
        base.require(time.time() < self.cfg['product_start_latest_utc'],
                     'short_product_start_window_missed')
        with (self.root / 'execution-claim.json').open('x') as out:
            json.dump(dict(claimed_utc_ns=time.time_ns(), source_git_sha=base.HEAD), out)
            out.flush()
            os.fsync(out.fileno())
        product = self.start()
        self.sequence, self.prices = self.driver.sequence, self.driver.price_caps
        faultcase = self.fault_bridge.build_faultcase(self, self.fault_core,
                                                     self.router_helper, self.upstream_helper)
        env = dict(account=self.environment['env']['MOEX_TEST_CLIENT'],
                   restart=self.restart, flatten=lambda p, deadline: self.flatten(p, deadline),
                   closeout=self.closeout, verify_owner=self.verify_owner,
                   tick=self.tick, faultcase=faultcase)
        result = self.driver.run(product, self.cfg, self.work, env, self.publish)
        self.publish(dict(result, state='complete', phase='terminal_closeout',
                          no_guardian_failure=True, qualification_result='SUPPLEMENTAL_TEST_COMPLETE'))
        return result

    def fail(self, error):
        pending = getattr(self, 'pending_fault_controller', None)
        if pending is not None:
            # Restoration owns the cross-controller lock until the helper
            # exits. Never trade/clean up through an uncertain router state.
            try:
                pending.wait_fault_worker()
                base.require(pending.fault_worker is None, 'short_fault_restore_pending')
            except BaseException:
                self.publish(dict(state='BLOCKED', phase='fault_restoration_pending',
                                  flat='UNKNOWN', terminal=False,
                                  initiating_failure=type(error).__name__,
                                  cleanup_error='independent_restore_still_pending'))
                raise
            os.close(pending.guard_control)
            pending.fault_lock.close()
            del self.pending_fault_controller
        return super().fail(error)


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
    bootstrap = load(cfg.get('bootstrap', 'bootstrap.py'), 'short_bootstrap')
    load('frozen_day_driver.py', 'day_driver')
    driver = load('short_session.py', 'short_day_driver')
    guardian = load('guardian.py', 'short_guardian')
    coverage = load(cfg.get('coverage_module', 'coverage_proof.py'), 'short_coverage')
    runner = Supervisor(args.root, cfg, runtime, ops, bootstrap, driver, guardian, coverage)
    runner.fault_bridge = load('short_fault_bridge.py', 'short_fault_bridge')
    runner.fault_core = load('working_fault_core.py', 'working_fault_core')
    runner.router_helper = load('router_recovery.py', 'short_router_recovery')
    runner.upstream_helper = load('upstream_recovery.py', 'short_upstream_recovery')
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
