"""One-shot TEST day. Runs remotely even when laptop and SSH are absent."""
from pathlib import Path
import datetime,fcntl,json,os,signal,subprocess,sys,time,traceback
sys.dont_write_bytecode=True;os.umask(0o077)
from ops import *
from deployment import load
import sequence,scenarios,price_caps

deployment,work,cfg=load();client_isolation=deployment.client_isolation;setup=deployment.setup
public=dict(state='scheduled',source_git_sha=HEAD,scheduled_msk='2026-10-12 06:45',orders_started=False,raw_evidence_on_test=True,qualification_result='INCOMPLETE',scenarios={})
product=None;environment=None;lease=None;stopping=False
def touch():
 public['updated_utc_ns']=time.time_ns();atomic(work/'public.json',public)
def mark(name,value):public['scenarios'][name]=value;touch()
def interrupted(sig,frame):
 global stopping
 stopping=True;(work/'stop-requested').touch()
signal.signal(signal.SIGTERM,interrupted);signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGHUP,signal.SIG_IGN)
def guardian_ok():
 g=json.loads((work/'guardian-owned.json').read_text())
 if process_identity(g['pid'])!=g:raise Stop('guardian_not_alive')
def wait_until(stamp,observe=True):
 while time.time()<stamp:
  touch()
  if stopping or (work/'stop-requested').exists():raise Stop('stop_requested')
  if product and product.child.poll() is not None:raise Stop('product_exited')
  if product and observe:
   guardian_ok()
   state=product.status()
   if any(state.get(k) for k in ('journal_failed','cancel_only','log_error','cgate_key_check_failed','operator_action_required')):raise Stop('product_safety_alert')
   if not flat(state):raise Stop('unexpected_idle_exposure')
  time.sleep(min(5 if product else 30,max(.05,stamp-time.time())))
def stamp(hour,minute=0):return datetime.datetime(2026,10,12,hour-3,minute,tzinfo=UTC).timestamp()
def seq(stage):return sequence._Sequence(product.command,product.status,work,product.targets,stage)
def restore_entry():
 s=product.wait_ready(min(CLEANUP,time.time()+250),require_flat=True)
 if not flat(s):raise Stop('entry_restore_not_flat')
 product.command('kill off')
def stage(name,call):
 public['state']='running';public['phase']=name;touch()
 value=call();mark(name,value);return value
def interim_flatten():
 value=clean_product(product,until=min(CLEANUP,time.time()+180),final=False)
 if not value['flat']:raise Stop('interim_flatness_unproved')
 restore_entry()
def restart_working():
 s=seq('workingrestart');s.assert_flat(s.snapshot());old=[]
 for target in s.targets:
  state=s.snapshot();price=s.passive(state,target,1);cid=s.next_id(target)
  s.send(f"place {cid} {target['isin_id']} buy 1 {sequence._price(price)} day");row=s.accepted_order(target,cid);old.append(dict(target=target,order_id=row['order_id'],ext_id=row['ext_id'],remaining=row['remaining']))
 atomic(work/'working-restart-private.json',old)
 environment['inventory'](product.child.pid);product.crash()
 product.wait_ready(min(CLEANUP,time.time()+250),require_flat=False)
 state=product.status();recovered=[]
 for o in old:
  rows=[x for x in state['orders'] if x['instance_owned'] and x['isin_id']==o['target']['isin_id'] and x['order_id']==o['order_id'] and x['ext_id']==o['ext_id'] and x['remaining']==o['remaining'] and x['state'] in sequence.WORKING]
  if len(rows)!=1:raise Stop('working_restart_identity_or_remaining_mismatch')
  recovered.append(rows[0]['client_order_id'])
 for cid in recovered:s.cancel(cid)
 s.assert_flat(s.snapshot())
 target=s.targets[0];state=s.snapshot();price=s.passive(state,target,1);cid=s.next_id(target)
 s.send(f"place {cid} {target['isin_id']} buy 1 {sequence._price(price)} day");s.accepted_order(target,cid);s.cancel(cid)
 return dict(status='PASS',working_orders_recovered=2,post_recovery_add_cancel=True)
def fills_restart():
 result=sequence.fills(product.command,product.status,work,product.targets)
 product.close();environment['inventory']();product.start();product.wait_ready(min(CLEANUP,time.time()+250),require_flat=False)
 result.update(sequence.post_restart(product.command,product.status,work,product.targets))
 # Heartbeat rows are deliberately not journaled. Missing raw replay rows stay
 # missing evidence; compact server time is not re-labelled as a revision.
 return dict(status='PASS' if result['restart_replay_floor_proven'] else 'INCOMPLETE',**result)
def partial(target):
 value=scenarios.partial_move(seq('partial'),target,timeout_seconds=600)
 interim_flatten();return value
def phase_commands():
 result={}
 for target in product.targets:
  s=seq('phase');s.assert_flat(s.snapshot());state=s.snapshot();price=s.passive(state,target,1);cid=s.next_id(target)
  s.send(f"place {cid} {target['isin_id']} buy 1 {sequence._price(price)} day");old=s.accepted_order(target,cid)
  s.valid_price(s.snapshot(),target,price+next(x['session_terms']['min_step_scaled'] for x in state['instruments'] if x['isin_id']==target['isin_id']),1)
  newprice=price+next(x['session_terms']['min_step_scaled'] for x in state['instruments'] if x['isin_id']==target['isin_id'])
  s.send(f"move {cid} 1 {sequence._price(newprice)}")
  s.wait(lambda now: (x:=s.order(now,cid)) and x['order_id']!=old['order_id'] and x['state'] in sequence.WORKING and x['quantity']==1 and sequence._units(x['price'])==newprice,'phase_move_not_confirmed')
  s.cancel(cid);s.mass_cancel(target);result[target['symbol']]='PASS'
 return dict(status='PASS',commands_by_symbol=result)
def auction_order():
 p=seq('auction');p.assert_flat(p.snapshot());t=next(x for x in p.targets if x['symbol']=='CNY-12.26');state=p.snapshot()
 if not ready(state,product.targets):return dict(status='NOT_OBSERVED',reason='auction_entry_authority_unavailable')
 price=p.passive(state,t,1);cid=p.next_id(t);p.send(f"place {cid} {t['isin_id']} buy 1 {sequence._price(price)} day");p.accepted_order(t,cid);p.cancel(cid)
 return dict(status='PASS',day_add_cancel=True,ioc_and_move_not_sent=True)
def final_statistics():
 counts={};posts=[];timings={};first_replay=[]
 if not (work/'interaction.ndjson').exists():return {}
 for line in (work/'interaction.ndjson').open():
  try:r=json.loads(line)
  except json.JSONDecodeError:continue
  if r['event']=='command':
   name=r['data']['name'];counts[name]=counts.get(name,0)+1
  if r['event']=='command_timing':
   d=r['data'];n=d.get('post_started_utc_ns')
   if n:posts.append(n)
 posts.sort();max_rate=0;left=0
 for right,n in enumerate(posts):
  while n-posts[left]>=1000000000:left+=1
  max_rate=max(max_rate,right-left+1)
 return dict(command_counts=counts,orders_started=counts.get('AddOrder',0)>0,journal_bytes=(work/'interaction.ndjson').stat().st_size,max_native_posts_per_second=max_rate,rate_limit_observed=max_rate<=5 if posts else None)

try:
 lease=lock(work/'run.lock',nonblocking=True)
 atomic(work/'supervisor-owned.json',process_identity(os.getpid()))
 touch();wait_until(START,observe=False);claim(work/'claimed.json')
 if stopping:raise Stop('stop_requested')
 guardian_ok()
 public['state']='preflight';touch()
 manifest=json.loads((work/'manifest.json').read_text())
 if any(sha(work/name)!=value for name,value in manifest['files'].items()) or sha(work/'resolve')!=manifest['resolver_sha256']:raise Stop('qualification_bundle_changed')
 verify_public_ci(work)
 while True:
  try:environment=setup(work,cfg);break
  except Stop as e:
   if str(e)!='no_test_upstream' or time.time()>=stamp(7,5):raise
   touch();time.sleep(10)
 mark('router_stop_start',environment['router_faults']);mark('upstream_interruption',environment['router_faults'])
 for name in ('tcs_reload','reserve_switch','exchange_flood','controlled_lost_reply','scheme_evolution'):mark(name,dict(status='NOT_OBSERVED',reason='MOEX_coordination_required'))
 tracking=subprocess.check_output(['chronyc','tracking'],text=True);sources=subprocess.check_output(['chronyc','sources','-n'],text=True)
 (work/'clock-before-private.txt').write_text(tracking+'\n'+sources)
 public['clock_before']=clock_proof(tracking);touch()
 resolver=work/'resolve';expected=json.loads((work/'manifest.json').read_text())['resolver_sha256']
 if sha(resolver)!=expected:raise Stop('resolver_binary_changed')
 with (work/'resolve-private.json').open('wb') as out,(work/'resolve-stderr-private.log').open('wb') as err:
  discovery=subprocess.Popen([str(resolver),*environment['common'],'--read-only-market-data','--isin-id','1','--wait-ms','250000'],cwd=work,env=environment['env'],stdin=subprocess.DEVNULL,stdout=out,stderr=err,start_new_session=True)
  discovery_deadline=time.time()+270
  try:
   while discovery.poll() is None:
    touch()
    if stopping or (work/'stop-requested').exists() or time.time()>=discovery_deadline:raise Stop('read_only_resolution_interrupted')
    time.sleep(5)
  finally:
   if discovery.poll() is None:
    discovery.terminate()
    try:discovery.wait(timeout=15)
    except subprocess.TimeoutExpired:discovery.kill();discovery.wait(timeout=10)
 if discovery.returncode:raise Stop('fresh_refdata_resolution_failed')
 resolved=json.loads((work/'resolve-private.json').read_text());targets=resolved['targets']
 if not resolved.get('ok') or {x['symbol'] for x in targets}!={'ALRS-12.26','CNY-12.26'} or len(targets)!=2:raise Stop('fresh_refdata_identity_failed')
 if stopping or (work/'stop-requested').exists():raise Stop('stop_requested_before_product')
 # Resolve current-price caps without creating a trading publisher.
 environment['inventory']()
 quote_args=[str(environment['binary']),*environment['common'],'--read-only-market-data','--log',str(work/'quote-capture.ndjson'),'--state',str(environment['state']),'--command-socket',str(work/'cmd.sock')]
 for t in targets:quote_args+=['--isin-id',str(t['isin_id'])]
 quote_product=Product(work,environment['binary'],environment['env'],quote_args,targets,touch)
 quote_product.start()
 try:
  quote_deadline=min(LATEST_START,time.time()+250)
  while time.time()<quote_deadline:
   guardian_ok();quote_state=quote_product.wait_status(quote_deadline)
   if any(quote_state.get(k) for k in ('journal_failed','cancel_only','log_error','cgate_key_check_failed','operator_action_required')):raise Stop('quote_capture_safety_alert')
   client_isolation(work,quote_product.child.pid)
   if quote_state['configuration']['allow_orders'] or quote_state['transport']['publisher']['state']!=0:raise Stop('quote_capture_created_publisher')
   try:targets=price_caps.bind_caps(quote_state,targets);break
   except ValueError:time.sleep(.2)
  else:raise Stop('current_price_capture_deadline')
  atomic(work/'quote-cap-anchor-private.json',dict(targets=targets,status=quote_state))
 finally:quote_product.close()
 environment['inventory']();args=[str(environment['binary']),*environment['common'],'--log',str(work/'interaction.ndjson'),'--state',str(environment['state']),'--command-socket',str(work/'cmd.sock'),'--allow-orders','--sole-instance','--login-env','MOEX_LOGIN','--ext-id-range',cfg['ext_id_range'],'--max-commands-per-second','5','--max-quantity','2','--max-open-orders','5','--clock-offset-us',str(public['clock_before']['offset_us']),'--measure-timings']
 for t in targets:
  cap=t['quote_cap_scaled'];args+=['--isin-id',str(t['isin_id']),'--max-notional',str(t['isin_id'])+'='+sequence._price(cap),'--max-position',str(t['isin_id'])+'=2']
 atomic(work/'argv-private.json',args);atomic(work/'targets-private.json',targets)
 product=Product(work,environment['binary'],environment['env'],args,targets,touch);product.start();product.isolation=lambda:client_isolation(work,product.child.pid);product.risk_guard=guardian_ok
 initial=product.wait_ready(stamp(7,10),require_flat=True);environment['inventory'](product.child.pid)
 # Target resolver provenance is independently corroborated by the real host.
 for t in targets:
  x=next(x for x in initial['instruments'] if x['isin_id']==t['isin_id']);terms=x['session_terms']
  if x['symbol']!=t['symbol'] or terms['reference_price_scaled']!=t['reference_price_scaled'] or t['quote_cap_scaled']!=price_caps.approved_ceiling(t,terms):raise Stop('fresh_cap_changed_before_first_order')
 # All-eight-stream gates corroborate the read-only quote anchor before entry.
 for t in targets:
  try:price_caps.current_reference(initial,t)
  except ValueError as error:raise Stop(str(error)) from error
 reservations=[x['pending_fill_reservations'] for x in initial['instruments']]
 if any(x.get('buy_overflow',True) or x.get('sell_overflow',True) or not x.get('proof_valid',False) for x in reservations):raise Stop('initial_reservation_proof_invalid')
 if any(x['buy_quantity'] or x['sell_quantity'] for x in reservations):
  if not deployment.prove_fill_coverage(initial,work/'interaction.ndjson',work,product.status):raise Stop('fresh_fill_reservation_coverage_unproved')
 atomic(work/'effective-config-and-flat-private.json',dict(configuration=initial['configuration'],flat=True,printed_utc_ns=time.time_ns()));print(json.dumps(dict(effective_config=initial['configuration'],flat_account_before_first_order=True)),flush=True)
 public.update(state='running',orders_armed=True,effective_limits=dict(max_quantity=2,max_position=2,max_open_orders_global=5,commands_per_second=5,sole_instance=True),instrument_symbols=[x['symbol'] for x in targets]);touch()
 wait_until(max(stamp(7,5),time.time()+300))
 stage('morning_passive',lambda:dict(status='PASS',**sequence.passive_cycles(product.command,product.status,work,targets,count=20)))
 stage('morning_concurrent_cancel',lambda:scenarios.concurrent_cancel(seq('concurrent')))
 stage('restart_with_two_working',restart_working)
 stage('fill_and_restart',fills_restart);interim_flatten()
 cny=next(x for x in targets if x['symbol']=='CNY-12.26');alrs=next(x for x in targets if x['symbol']=='ALRS-12.26')
 stage('cny_partial_before_move',lambda:partial(cny));stage('alrs_partial_before_move',lambda:partial(alrs))
 wait_until(stamp(10,45));stage('opening_auction',auction_order)
 wait_until(stamp(11,5));product.wait_ready(time.time()+250,require_flat=True);stage('main_commands',phase_commands)
 if public['scenarios']['cny_partial_before_move']['status']!='PASS':stage('cny_partial_main_retry',lambda:partial(cny))
 wait_until(stamp(14,5));product.wait_ready(time.time()+250,require_flat=True);stage('evening_commands',phase_commands)
 wait_until(stamp(15,5));product.wait_ready(time.time()+250,require_flat=True);stage('post_settlement_commands',phase_commands)
 wait_until(CLEANUP);stage('cleanup',lambda:clean_product(product,until=stamp(15,50)))
 if not public['scenarios']['cleanup']['flat']:raise Stop('final_flatness_unproved')
 # Observe the announced16:00 clearing while protected and flat; no new risk.
 public['phase']='final_clearing_observation';touch()
 while time.time()<END:
  s=product.status()
  if not flat(s):raise Stop('exposure_after_cleanup')
  touch();time.sleep(5)
 public.update(state='complete',completed_utc_ns=time.time_ns(),**final_statistics())
except BaseException as error:
 public.update(state='blocked',error_code=str(error) if isinstance(error,Stop) else getattr(error,'code','supervisor_failure'),error_class=type(error).__name__)
 (work/'error-private.txt').write_text(traceback.format_exc())
finally:
 if product:
  try:
   result=clean_product(product,until=min(time.time()+180,stamp(16,15)));public['closeout']=result
  except BaseException as e:public['closeout']=dict(flat=False,error_class=type(e).__name__)
  try:product.close();public['product_exit']=product.child.returncode
  except BaseException as e:public['product_shutdown_error']=type(e).__name__
  if environment:
   try:public['after']=environment['inventory']()
   except BaseException as e:public['client_inventory_error']=type(e).__name__
  if not public.get('closeout',{}).get('flat') or not public['closeout'].get('cancel_confirmed') or public.get('product_exit')!=0 or public.get('product_shutdown_error') or public.get('client_inventory_error'):
   public.update(state='blocked',error_code='closeout_incomplete')
  try:
   deployment.reconstruct_orders(work)
  except BaseException:public['reconstruction_error']=True
 public.update(terminal=True,completed_utc_ns=time.time_ns(),**final_statistics());touch()
