"""Held, date-specific 22Oct manual full-day owner overlay.

No executable entry point and no import-time work. Root seals an explicitly
reviewed fresh composition. Reuses reviewed owner/bootstrap/guardian routines;
this module contains no scenario loop and never generates exposure commands.
"""
from __future__ import annotations
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import stat
import time

REVIEW_ONLY = True
DATE = '2026-10-22'
SOURCE = 'b1911621318603bf3b6d7278073a0266dc1ee0e3'
BINARY = 'c63d3021207fe80e4a62d265757294d8db3bff2cd4ccac9619b1edbcbe650c38'
MSK = dt.timezone(dt.timedelta(hours=3))
FROZEN_CAPS = {'ALRS-12.26': 301200000, 'CNY-12.26': 2400400}

class Refused(ValueError):
    pass

def require(value, code):
    if not value:
        raise Refused(code)

def epoch(hour, minute=0):
    return dt.datetime(2026, 10, 22, hour, minute, tzinfo=MSK).timestamp()

def schedule():
    return dict(scheduled_start_utc=epoch(6,20), normal_product_target_utc=epoch(6,40),
                product_start_latest_utc=epoch(7), admission_latest_utc=epoch(13,30),
                earliest_entry_utc=epoch(10,20), manual_entry_start_utc=epoch(10,20),
                manual_entry_deadline_utc=epoch(13,50), hard_entry_deadline_utc=epoch(14),
                flat_deadline_utc=epoch(14), final_clearing_utc=epoch(16),
                normal_sigterm_utc=epoch(16,5), guardian_trigger_utc=epoch(16,7),
                hard_cleanup_deadline_utc=epoch(16,10))

def validate(cfg, *, now=None, entry=False, held=True):
    require(held or not REVIEW_ONLY, 'manual_owner_held_unarmed')
    require(cfg.get('authorized_date') == DATE and cfg.get('source_git_sha') == SOURCE
            and cfg.get('binary_sha256') == BINARY, 'manual_native_date_pin')
    require(cfg.get('environment') == 'test' and cfg.get('target_venue') == 'T0'
            and cfg.get('timer_paused') is True and cfg.get('owner_authorized') is True
            and cfg.get('manual_commands_enabled') is True and type(cfg.get('planned_starts')) is int
            and cfg['planned_starts'] == 1,
            'manual_owner_scope')
    require(all(cfg.get(key) == value for key,value in schedule().items()), 'manual_calendar_changed')
    require(all(type(cfg.get(key)) is int and cfg[key] == value for key,value in
                (('quantity_limit',2),('position_limit',2),('open_order_limit',5),('rate_limit',5))),
            'manual_risk_limits_changed')
    require(cfg.get('frozen_quote_caps') == FROZEN_CAPS and cfg.get('no_watchdog_sigkill') is True,
            'manual_caps_or_shutdown_scope')
    require(all(cfg.get(key) is False for key in ('fault_scenarios_authorized',
                'router_fault_authorized','upstream_fault_authorized','restart_scenario_authorized',
                'partial_fill_enabled','latency_gating','profiling_enabled')),
            'manual_unapproved_scenario')
    if now is not None:
        require(dt.datetime.fromtimestamp(now,MSK).date().isoformat() == DATE, 'manual_date_changed')
        require(now < cfg['hard_cleanup_deadline_utc'], 'manual_day_expired')
        if entry:
            require(cfg['manual_entry_start_utc'] <= now < cfg['manual_entry_deadline_utc'],
                    'manual_outside_entry_window')
    return cfg

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def checkpoint(path):
    raw = Path(path).read_bytes()
    fields = raw.split()
    require(len(fields) == 8 and fields[0] == b'MOEXJ2'
            and all(value.isdigit() for value in fields[1:]), 'manual_checkpoint_format')
    value = 14695981039346656037
    for byte in b' '.join(fields[:7]):
        value = ((value ^ byte) * 1099511628211) & ((1<<64)-1)
    require(value == int(fields[7]), 'manual_checkpoint_checksum')
    return tuple(map(int,fields[1:7]))

def private(path, *, directory=False):
    path = Path(path)
    info = path.lstat()
    require(path.is_absolute() and not path.is_symlink() and info.st_uid == os.getuid()
            and not info.st_mode & 0o077
            and (stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)),
            'manual_private_file_identity')
    return path

def option(args, name):
    require(args.count(name) == 1 and args.index(name)+1 < len(args), 'manual_argument_missing')
    return args[args.index(name)+1]

def check_arguments(args, cfg):
    require(args[:3] == [cfg['binary'],'plaza2','run'] and args.count('--allow-orders') == 1
            and args.count('--sole-instance') == 1 and option(args,'--environment') == 'test'
            and option(args,'--router') == '127.0.0.1:4101'
            and option(args,'--state') == cfg['state']
            and option(args,'--instance-id') == cfg['instance']
            and option(args,'--ext-id-range') == cfg['ext_id_range'], 'manual_product_arguments')
    require(not any('profil' in x.lower() or x == '--measure-timings' for x in args),
            'manual_profiling_forbidden')
    for name,value in (('--max-commands-per-second','5'),('--max-quantity','2'),('--max-open-orders','5')):
        require(option(args,name) == value, 'manual_native_argument_limits')
    return args

def claim_start_once(root, cfg, args):
    """Durable before the sole Popen; never clear after startup uncertainty."""
    validate(cfg, now=time.time(), held=False)
    root = private(root,directory=True)
    require(time.time() < cfg['product_start_latest_utc'], 'manual_product_start_window_missed')
    check_arguments(args,cfg)
    state = private(cfg['state'])
    baseline_path = private(root/cfg['start_baseline_receipt'])
    manifest = json.loads(private(root/'manifest.json').read_text())
    require(manifest.get('source_git_sha') == SOURCE and manifest.get('binary_sha256') == BINARY
            and manifest.get('files',{}).get('configuration-private.json') == sha(private(root/'configuration-private.json'))
            and json.loads((root/'configuration-private.json').read_text()) == cfg, 'manual_configuration_not_sealed')
    require(manifest.get('files',{}).get(baseline_path.name) == cfg['start_baseline_receipt_sha256'] == sha(baseline_path),
            'manual_baseline_not_sealed')
    baseline = json.loads(baseline_path.read_text())
    begin,end = map(int,cfg['ext_id_range'].split(':'))
    expected = (begin+3000,3001)
    highwaters = checkpoint(state)
    require(baseline.get('authorized_date') == DATE and baseline.get('baseline_blocks') == 3
            and baseline.get('source_git_sha') == SOURCE and baseline.get('binary_sha256') == BINARY
            and baseline.get('checkpoint_highwaters') == dict(ext=expected[0],user=expected[1])
            and highwaters[3:5] == expected and sha(state) == baseline.get('checkpoint_sha256')
            and baseline.get('state_path_sha256') == hashlib.sha256(str(state).encode()).hexdigest()
            and baseline.get('root_path_sha256') == hashlib.sha256(str(root).encode()).hexdigest()
            and baseline.get('instance') == cfg['instance']
            and baseline.get('ext_id_range') == cfg['ext_id_range']
            and baseline.get('assignment_sha256') == cfg['assignment_sha256']
            and baseline.get('account_mapping_sha256') == cfg['account_mapping_sha256']
            and baseline.get('old_exact_owners_stopped') is True
            and baseline.get('no_native_product_present') is True, 'manual_baseline_changed')
    require(sha(private(cfg['binary'])) == BINARY and sha(private(cfg['assignment'])) == cfg['assignment_sha256'],
            'manual_binary_or_assignment_changed')
    assignment = json.loads(Path(cfg['assignment']).read_text())
    require(assignment.get('instance') == cfg['instance'] and assignment.get('ext_id_begin') == begin
            and assignment.get('ext_id_end') == end and end-highwaters[3]+1 >= 9000,
            'manual_assignment_or_headroom')
    # Pin old receipt bytes as well as the derived baseline; no prior file is edited.
    previous = Path(baseline.get('predecessor_root',''))
    pins = baseline.get('predecessor_files',{})
    require(previous.is_absolute() and previous != root and pins
            and all(sha(private(previous/name)) == pin for name,pin in pins.items()),
            'manual_predecessor_changed')
    receipt = dict(schema='MOEX_22OCT_MANUAL_ONE_ATTEMPT_V1',source_git_sha=SOURCE,binary_sha256=BINARY,
                   instance=cfg['instance'],ext_id_range=cfg['ext_id_range'],assignment_sha256=cfg['assignment_sha256'],
                   account_mapping_sha256=cfg['account_mapping_sha256'],state_path_sha256=hashlib.sha256(str(state).encode()).hexdigest(),
                   authorized_date=DATE,planned_starts=1,purpose='initial',checkpoint_sha256=sha(state),
                   checkpoint_highwaters=dict(ext=highwaters[3],user=highwaters[4]),
                   baseline_sha256=sha(baseline_path),args_sha256=hashlib.sha256(
                       json.dumps(args,separators=(',',':')).encode()).hexdigest(),claimed_utc_ns=time.time_ns())
    flags = os.O_WRONLY|os.O_CREAT|os.O_EXCL|getattr(os,'O_NOFOLLOW',0)
    fd = os.open(root/'normal-product-attempt-private.json',flags,0o600)
    try:
        raw = json.dumps(receipt,sort_keys=True).encode()+b'\n'
        require(os.write(fd,raw) == len(raw), 'manual_attempt_receipt_write_uncertain')
        os.fsync(fd)
    finally:
        os.close(fd)
    fd = os.open(root,os.O_RDONLY|os.O_DIRECTORY)
    try: os.fsync(fd)
    finally: os.close(fd)
    return receipt

def install_runtime_scope(runtime):
    """Fresh composition only; preserve pinned legacy file verification, CI and IO."""
    frozen = getattr(runtime,'base',runtime)
    frozen.AUTHORIZED_DATE = DATE
    def day_check(cfg,entry=False):
        return validate(cfg,now=time.time(),entry=entry,held=False)
    original_load = frozen.load_cfg
    def load_cfg(root):
        candidate = json.loads((Path(root)/'configuration-private.json').read_text())
        validate(candidate,held=False)
        return original_load(root)
    runtime.day_check = frozen.day_check = day_check
    runtime.load_cfg = load_cfg
    return runtime

def pre_popen_freshness(owner):
    """Refresh ownership after any wait; only a current strict flat probe admits boot."""
    inventory = owner.environment['inventory']()
    require(type(inventory.get('client_count')) is int and inventory['client_count'] == 0
            and type(inventory.get('upstreams')) is int and inventory['upstreams'] == 4,
            'manual_prestart_current_inventory_unproved')
    proof = owner.public.get('preentry_independent_flatness') or {}
    stamp = proof.get('completed_utc_ns')
    require(type(stamp) is int and 0 <= time.time_ns()-stamp <= 120_000_000_000
            and all(type(proof.get(key)) is int and proof[key] == value for key,value in
                    (('complete_replication_streams',8),('working_orders',0),('nonzero_positions',0),('exit',0)))
            and type((proof.get('after') or {}).get('client_count')) is int
            and proof['after']['client_count'] == 0,
            'manual_prestart_fresh_flatness_unproved')
    budget = owner.rt.budget(dict(state=str(owner.environment['state']),
                      ext_id_range=owner.cfg['ext_id_range'],instance=owner.cfg['instance']))
    require(type(budget) is int and budget >= 9000,'manual_prestart_id_headroom')
    return inventory

def metadata_targets(resolved, now_ns):
    """Metadata resolves identity pre-open; it never grants order admission."""
    require(resolved.get('ok') is True and resolved.get('read_only') is True
            and resolved.get('committed_refdata') is True and resolved.get('publisher_created') is False
            and type(resolved.get('message_allocations')) is int and resolved['message_allocations'] == 0
            and type(resolved.get('publisher_posts')) is int and resolved['publisher_posts'] == 0,
            'manual_resolver_publisherfree_unproved')
    captured = resolved.get('captured_utc_ns')
    require(type(captured) is int and 0 <= now_ns-captured <= 120_000_000_000
            and all(type(resolved.get(k)) is int and resolved[k] > 0
                    for k in ('sess_id','refdata_lifenum')), 'manual_refdata_capture_stale')
    targets = resolved.get('targets')
    require(type(targets) is list and len(targets) == 2
            and {t.get('symbol') for t in targets} == set(FROZEN_CAPS)
            and len({t.get('isin_id') for t in targets}) == 2, 'manual_resolved_symbol_scope')
    result = []
    for target in targets:
        lower,upper,step = (target.get(k) for k in ('lower_price_scaled','upper_price_scaled','min_step_scaled'))
        require(all(type(x) is int for x in (lower,upper,step)) and 0 < lower <= upper and step > 0
                and lower % step == 0 and upper % step == 0
                and all(type(target.get(k)) is int and target[k] > 0
                        for k in ('isin_id','definition_repl_rev','terms_repl_rev','reference_price_scaled')),
                'manual_committed_terms_invalid')
        cap = FROZEN_CAPS[target['symbol']]
        # Preserve the already approved reference/cap; fresh request prices
        # still require today's own ready book, grid, bounds and <=2*fresh bid.
        result.append(dict(target,quote_reference_scaled=cap//2,quote_cap_scaled=cap,
                 quote_reference_source='prior_frozen_owner_approved_cap',
                 cap_source='prior_frozen_owner_approved_cap',quote_reference_captured_utc_ns=captured))
    return result

def kill_on_witness(path, begin):
    """Only native post-command event order proves startup protection."""
    requested = None
    with Path(path).open('rb') as stream:
        stream.seek(begin)
        while True:
            offset = stream.tell();line = stream.readline()
            if not line or not line.endswith(b'\n'):
                break
            event = json.loads(line);data = event.get('data',{})
            require(event.get('event') != 'command','manual_startup_unexpected_exchange_post')
            if event.get('event') == 'operator_input' and data.get('line') == 'kill on' and data.get('channel') == 'command_socket':
                requested = offset
            if requested is not None and event.get('event') == 'kill_switch' and data.get('enabled') is True:
                return dict(operator_offset=requested,kill_switch_offset=offset,native_enabled=True,exchange_posts=0)
    return None

def owner_class(short_module):
    """Inject reviewed components. Importing this module starts nothing."""
    base = short_module.base
    class ManualDayOwner(short_module.Supervisor):
        def publish(self,value=None):
            if value:
                self.public.update(base.public_value(value))
            now = time.time()
            self.public['ownership_lease_until_utc'] = min(now+100,self.cfg['hard_cleanup_deadline_utc'])
            if self.public.get('phase') in ('preflight','resolve_refdata'):
                self.public['ownership_lease_until_utc'] = min(
                    self.public['ownership_lease_until_utc'],self.cfg['admission_latest_utc'])
            self.public['updated_utc_ns'] = time.time_ns()
            return self.rt.atomic(self.root/'public.json',self.public)

        def arguments(self,offset):
            args = base.Supervisor.arguments(self,offset)
            args = [x for x in args if x != '--measure-timings']
            return check_arguments(args,self.cfg)

        def resolve(self):
            # Reuse the exact pinned process/ownership/receipt path; only the
            # metadata->frozen-cap binding changes in this fresh composition.
            previous = base.conservative_caps
            base.conservative_caps = metadata_targets
            try:
                return short_module.Supervisor.resolve(self)
            finally:
                base.conservative_caps = previous

        def product_class(self):
            owner,ops = self,self.ops
            protected = base.Supervisor.product_class(self)
            class OneProduct(protected):
                def start(self):
                    pre_popen_freshness(owner)
                    claim_start_once(owner.root,owner.cfg,self.args)
                    try:
                        return ops.Product.start(self)
                    finally:
                        if self.child is not None:
                            owner.latest_native_product = self
                            try: owner.latest_native_identity = ops.process_identity(self.child.pid)
                            except (FileNotFoundError,ProcessLookupError): pass
            return OneProduct

        def risk_gate(self,product):
            validate(self.cfg,now=time.time(),entry=True,held=False)
            require(not self.closing and not (self.root/'stop-requested').exists()
                    and not (self.work/'stop-requested').exists(),'manual_new_entry_stopped')
            return self.risk_snapshot(product,verify=True)

        def restart(self,*args,**kwargs):
            raise Refused('manual_second_product_forbidden')

        def start(self):
            validate(self.cfg,now=time.time(),held=False)
            self.publish({'state':'preflight','phase':'preflight'})
            self.rt.verify_ci(self.root,self.cfg,tick=self.tick)
            self.work.mkdir(mode=0o700)
            folder = self.root/'native-environment'
            folder.mkdir(mode=0o700)
            self.environment = self.bootstrap.setup(folder,json.loads((self.root/'inputs.json').read_text()))
            require(self.rt.sha(self.cfg['binary']) == BINARY and str(self.environment['state']) == self.cfg['state'],
                    'manual_candidate_or_state_changed')
            self.environment['binary'] = self.cfg['binary']
            self.before_live_sha = self.rt.sha(self.environment['state'])
            # Resolve and probe immediately before the sole product start,
            # rather than letting a06:20 proof age through the06:40 wait.
            observe_until = self.cfg['normal_product_target_utc']-60
            while time.time() < observe_until:
                self.tick();time.sleep(min(1,observe_until-time.time()))
            self.resolve()
            self.initial_flat_probe()  # Reviewed bounded publisher-free retries, strict complete flatness.
            clock = self.clock()
            while time.time() < self.cfg['normal_product_target_utc']:
                self.tick();time.sleep(min(1,self.cfg['normal_product_target_utc']-time.time()))
            require(time.time() < self.cfg['product_start_latest_utc'],'manual_preopen_start_missed')
            self.ops.CLEANUP = self.cfg['hard_entry_deadline_utc']
            self.ops.END = self.cfg['hard_cleanup_deadline_utc']-300
            product = self.product_class()(self.work,self.cfg['binary'],self.environment['env'],
                                           self.arguments(clock['offset_us']),self.targets,self.tick)
            product.start()
            self.attach(product)
            product.wait_status(min(time.time()+30,self.cfg['product_start_latest_utc']))
            journal = self.work/'interaction.ndjson'
            offset = journal.stat().st_size
            product.command('kill on',permit_risk=False)
            until = min(time.time()+5,self.cfg['product_start_latest_utc'])
            witness = None
            while time.time() < until:
                witness = kill_on_witness(journal,offset)
                if witness is not None:
                    break
                self.tick();time.sleep(.02)
            require(witness is not None,'manual_native_startup_kill_witness_missing')
            self.publish({'startup_protection':witness})
            require(self.rt.budget(self.meta) >= 8000,'manual_poststart_id_headroom')
            self.publish({'state':'observing_preopen','phase':'observing_preopen',
                          'normal_native_started_utc_ns':time.time_ns(),'clock':clock,
                          'manual_commands_enabled':False,'planned_restarts':0})
            return product

        def run(self):
            validate(self.cfg,held=False)
            self.rt.atomic(self.root/'supervisor-owned.json',self.ops.process_identity(os.getpid()))
            self.startup_lease_active = True
            self.publish({'state':'scheduled','phase':'preflight'})
            self.await_startup_guardian()
            self.await_installation_barrier()
            while time.time() < self.cfg['scheduled_start_utc']:
                self.tick();time.sleep(min(5,self.cfg['scheduled_start_utc']-time.time()))
            product = self.start()
            covered = False
            exercise_closed = False
            while time.time() < self.cfg['normal_sigterm_utc']:
                self.tick()
                require(product.child.poll() is None,'manual_product_exited_before_closeout')
                state = product.status()
                ready = self.ops.ready(state,self.targets,entry=False)
                if ready:
                    self.rt.configuration_gate(state,self.targets,self.cfg)
                    if not covered:
                        self.coverage_gate(state)
                        covered = True
                    now = time.time()
                    outside = now < self.cfg['manual_entry_start_utc'] or now >= self.cfg['manual_entry_deadline_utc']
                    if outside and (now < self.cfg['manual_entry_start_utc'] or exercise_closed):
                        require(base.strict_flat(state,self.ops,self.targets),'manual_nonflat_outside_exercise')
                now = time.time()
                if now >= self.cfg['manual_entry_deadline_utc'] and not exercise_closed:
                    self.closing = True
                    self.publish({'phase':'final_cleanup','manual_commands_enabled':False})
                    self.flatten(product,min(self.cfg['flat_deadline_utc'],now+180),final=True)
                    exercise_closed = True
                    self.publish({'phase':'readonly_through_final_clearing','flat_at_exercise_end':True})
                permitted = covered and ready and not self.closing and (
                    self.cfg['manual_entry_start_utc'] <= now < self.cfg['manual_entry_deadline_utc'])
                phase = 'manual_main_exercise' if permitted else ('readonly_through_final_clearing'
                        if exercise_closed else 'observing_native_sessions')
                self.publish({'state':'observing','phase':phase,
                              'manual_commands_enabled':permitted,'native_reconstruction_ready':ready})
                time.sleep(min(5,max(0,self.cfg['normal_sigterm_utc']-time.time())))
            self.closing = True
            result = self.closeout(product)
            self.publish(dict(result,state='complete',phase='terminal_closeout',execution_result='PASS',
                              manual_scenario_verdict='REPORT_FROM_NATIVE_EVIDENCE',no_guardian_failure=True))
            return result

        def closeout(self,product):
            self.closing = True
            self.publish({'phase':'final_cleanup','manual_commands_enabled':False})
            require((self.work/'stop-requested').exists(),'manual_cleanup_stop_barrier_missing')
            # Normal manual exercise already ended and stopped at13:50. Hold
            # the reviewed guardian's cleanup lock through SIGTERM/closeout;
            # probe command.lock only, because Product.command acquires it too.
            with self.ops.lock(self.work/'cleanup.lock',nonblocking=True):
                with self.ops.lock(self.work/'command.lock',nonblocking=True):
                    pass
                return base.Supervisor.closeout(self,product)

        def fail(self,error):
            if isinstance(error, Refused):
                error = base.Refused(str(error))  # Retain the static initiating refusal code.
            self.recovery_needed = False
            if self.product is None and getattr(self,'latest_native_product',None) is not None:
                product = self.latest_native_product
                if product.child.poll() is None:
                    self.attach(product)
            return base.Supervisor.fail(self,error)
    return ManualDayOwner
