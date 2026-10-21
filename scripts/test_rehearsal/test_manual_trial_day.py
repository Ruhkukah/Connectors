import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch

MODULE_DIR=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('manual_owner',MODULE_DIR/'manual_trial_day.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class OwnerTests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name).resolve();self.root.chmod(0o700)
  self.original_binary=m.BINARY;m.BINARY=hashlib.sha256(b'fake-native-test-only').hexdigest()
  self.cfg=dict(authorized_date=m.DATE,source_git_sha=m.SOURCE,binary_sha256=m.BINARY,
       environment='test',target_venue='T0',timer_paused=True,owner_authorized=True,
       manual_commands_enabled=True,planned_starts=1,quantity_limit=2,position_limit=2,
       open_order_limit=5,rate_limit=5,frozen_quote_caps=dict(m.FROZEN_CAPS),no_watchdog_sigkill=True,
       fault_scenarios_authorized=False,router_fault_authorized=False,upstream_fault_authorized=False,
       restart_scenario_authorized=False,partial_fill_enabled=False,latency_gating=False,profiling_enabled=False,
       binary=str(self.root/'native'),state=str(self.root/'checkpoint'),assignment=str(self.root/'assignment'),
       instance='test-only-instance',ext_id_range='2000070000:2000119999',account_mapping_sha256='a'*64,
       start_baseline_receipt='baseline.json',**m.schedule())
  self.put(self.root/'native',b'fake-native-test-only')
  self.set_checkpoint()
  self.put(self.root/'assignment',dict(instance=self.cfg['instance'],ext_id_begin=2000070000,ext_id_end=2000119999))
  self.cfg['assignment_sha256']=m.sha(self.root/'assignment')
  previous=self.root/'old';previous.mkdir(mode=0o700);self.put(previous/'public.json',{'terminal':True})
  self.baseline=dict(authorized_date=m.DATE,baseline_blocks=3,source_git_sha=m.SOURCE,binary_sha256=m.BINARY,
       checkpoint_highwaters=dict(ext=2000073000,user=3001),checkpoint_sha256=m.sha(self.cfg['state']),
       state_path_sha256=hashlib.sha256(self.cfg['state'].encode()).hexdigest(),
       root_path_sha256=hashlib.sha256(str(self.root).encode()).hexdigest(),
       instance=self.cfg['instance'],ext_id_range=self.cfg['ext_id_range'],assignment_sha256=self.cfg['assignment_sha256'],
       account_mapping_sha256=self.cfg['account_mapping_sha256'],old_exact_owners_stopped=True,
       no_native_product_present=True,predecessor_root=str(previous),predecessor_files={'public.json':m.sha(previous/'public.json')})
  self.seal()
  self.args=[self.cfg['binary'],'plaza2','run','--environment','test','--router','127.0.0.1:4101',
       '--state',self.cfg['state'],'--instance-id',self.cfg['instance'],'--ext-id-range',self.cfg['ext_id_range'],
       '--allow-orders','--sole-instance','--max-commands-per-second','5','--max-quantity','2','--max-open-orders','5']
 def tearDown(self):
  m.BINARY=self.original_binary;self.tmp.cleanup()
 def put(self,path,value):
  raw=value if isinstance(value,bytes) else json.dumps(value,sort_keys=True).encode()
  Path(path).write_bytes(raw);Path(path).chmod(0o600)
 def set_checkpoint(self,ext=2000073000,user=3001):
  head=f'MOEXJ2 123 456 789 {ext} {user} 999'.encode();value=14695981039346656037
  for b in head:value=((value^b)*1099511628211)&((1<<64)-1)
  self.put(self.root/'checkpoint',head+b' '+str(value).encode()+b'\n')
 def seal(self):
  self.put(self.root/'baseline.json',self.baseline)
  self.cfg['start_baseline_receipt_sha256']=m.sha(self.root/'baseline.json')
  self.put(self.root/'configuration-private.json',self.cfg)
  self.put(self.root/'manifest.json',dict(source_git_sha=m.SOURCE,binary_sha256=m.BINARY,
      files={'baseline.json':m.sha(self.root/'baseline.json'),'configuration-private.json':m.sha(self.root/'configuration-private.json')}))
 def claim(self):
  with patch.object(m,'REVIEW_ONLY',False),patch.object(m.time,'time',return_value=m.epoch(6,40)):
   return m.claim_start_once(self.root,self.cfg,self.args)
 def test_held_import_cannot_claim(self):
  with patch.object(m.time,'time',return_value=m.epoch(6,40)):
   with self.assertRaisesRegex(m.Refused,'held_unarmed'):m.claim_start_once(self.root,self.cfg,self.args)
 def test_scope_main_only_soft_stop_and_day(self):
  m.validate(self.cfg,now=m.epoch(10,20),entry=True)
  for now in [m.epoch(10,19),m.epoch(13,50),m.epoch(14)]:
   with self.assertRaises(m.Refused):m.validate(self.cfg,now=now,entry=True)
  with self.assertRaisesRegex(m.Refused,'date_changed'):m.validate(self.cfg,now=m.epoch(10)+86400)
 def test_scope_rejects_raised_caps_faults_and_second_start(self):
  for key,value in [('planned_starts',2),('router_fault_authorized',True),('profiling_enabled',True),('quantity_limit',10)]:
   with self.assertRaises(m.Refused):m.validate(dict(self.cfg,**{key:value}))
  with self.assertRaises(m.Refused):m.validate(dict(self.cfg,frozen_quote_caps=dict(ALRS=10)))
 def test_real_cumulative_three_blocks_one_claim_then_refuses(self):
  receipt=self.claim();self.assertEqual(receipt['checkpoint_highwaters'],dict(ext=2000073000,user=3001))
  self.assertEqual(receipt['planned_starts'],1)
  raw=(self.root/'normal-product-attempt-private.json').read_bytes()
  with self.assertRaises(FileExistsError):self.claim()
  self.assertEqual((self.root/'normal-product-attempt-private.json').read_bytes(),raw)
 def test_popen_failure_cannot_release_start(self):
  self.claim()
  try:raise OSError('fake Popen failure after durable claim')
  except OSError:pass
  with self.assertRaises(FileExistsError):self.claim()
 def test_refuses_checkpoint_reset_before_claim(self):
  self.set_checkpoint(2000070000,1)
  with self.assertRaisesRegex(m.Refused,'baseline_changed'):self.claim()
  self.assertFalse((self.root/'normal-product-attempt-private.json').exists())
 def test_refuses_advanced_checkpoint_even_valid_checksum(self):
  self.set_checkpoint(2000074000,4001)
  with self.assertRaisesRegex(m.Refused,'baseline_changed'):self.claim()
 def test_refuses_old_baseline_one_block_and_bad_checksum(self):
  self.baseline['baseline_blocks']=1;self.seal()
  with self.assertRaises(m.Refused):self.claim()
  self.set_checkpoint();self.put(self.root/'checkpoint',b'MOEXJ2 1 2 3 2000073000 3001 4 0')
  with self.assertRaisesRegex(m.Refused,'checksum'):self.claim()
 def test_refuses_tampered_predecessor_and_configuration(self):
  self.put(Path(self.baseline['predecessor_root'])/'public.json',{'terminal':False})
  with self.assertRaisesRegex(m.Refused,'predecessor_changed'):self.claim()
  self.cfg['account_mapping_sha256']='b'*64
  with self.assertRaisesRegex(m.Refused,'configuration_not_sealed'):self.claim()
 def test_refuses_after_preopen_start_deadline(self):
  with patch.object(m,'REVIEW_ONLY',False),patch.object(m.time,'time',return_value=m.epoch(7)):
   with self.assertRaisesRegex(m.Refused,'start_window_missed'):m.claim_start_once(self.root,self.cfg,self.args)
 def test_profile_and_destination_refused(self):
  for args in [self.args+['--measure-timings'],self.args+['--profile'],[x.replace('127.0.0.1:4101','127.0.0.1:11200') for x in self.args]]:
   with self.assertRaises(m.Refused):m.check_arguments(args,self.cfg)
 def test_legacy_arguments_strip_profile_only_preserves_risk(self):
  original=list(self.args)+['--measure-timings']
  class Base:
   def arguments(_self,offset):return original
  short=types.SimpleNamespace(Supervisor=Base,base=types.SimpleNamespace(Supervisor=Base))
  owner=m.owner_class(short).__new__(m.owner_class(short));owner.cfg=self.cfg
  result=owner.arguments(12)
  self.assertEqual(result,self.args);self.assertIn('--sole-instance',result)
 def test_manual_risk_gate_blocked_after_stop_no_dispatch(self):
  class Base:pass
  short=types.SimpleNamespace(Supervisor=Base,base=types.SimpleNamespace(Supervisor=Base))
  cls=m.owner_class(short);owner=cls.__new__(cls);owner.cfg=self.cfg;owner.root=self.root;owner.work=self.root
  owner.closing=False;owner.risk_snapshot=lambda *a,**k:'ready'
  with patch.object(m,'REVIEW_ONLY',False),patch.object(m.time,'time',return_value=m.epoch(11)):
   self.assertEqual(owner.risk_gate(object()),'ready')
   (self.root/'stop-requested').touch()
   with self.assertRaisesRegex(m.Refused,'entry_stopped'):owner.risk_gate(object())
 def test_no_recovery_entry_point(self):
  class Base:pass
  short=types.SimpleNamespace(Supervisor=Base,base=types.SimpleNamespace(Supervisor=Base))
  cls=m.owner_class(short)
  with self.assertRaisesRegex(m.Refused,'second_product_forbidden'):cls.__new__(cls).restart()
 def test_oneproduct_claim_precedes_native_popen_even_failure(self):
  order=[]
  class Product:
   def __init__(self):self.args=self_args;self.child=None
   def start(self):order.append('Popen');raise OSError('fake startup failure')
  class Base:
   def product_class(self):return Product
  self_args=self.args
  short=types.SimpleNamespace(Supervisor=Base,base=types.SimpleNamespace(Supervisor=Base))
  cls=m.owner_class(short);owner=cls.__new__(cls);owner.root=self.root;owner.cfg=self.cfg;owner.ops=types.SimpleNamespace(Product=Product)
  product=owner.product_class()()
  def claim(*args):order.append('durable claim')
  with patch.object(m,'claim_start_once',side_effect=claim):
   with self.assertRaises(OSError):product.start()
  self.assertEqual(order,['durable claim','Popen'])
 def resolved_metadata(self):
  return dict(ok=True,read_only=True,committed_refdata=True,publisher_created=False,
      message_allocations=0,publisher_posts=0,captured_utc_ns=1_000_000_000,sess_id=123,refdata_lifenum=7,
      targets=[dict(symbol=symbol,isin_id=index+100,lower_price_scaled=100,upper_price_scaled=2000000000,
      min_step_scaled=100,reference_price_scaled=cap//2,definition_repl_rev=3,terms_repl_rev=4)
      for index,(symbol,cap) in enumerate(m.FROZEN_CAPS.items())])
 def test_metadata_preopen_no_book_preserves_fixed_caps(self):
  resolved=self.resolved_metadata();targets=m.metadata_targets(resolved,2_000_000_000)
  self.assertEqual({t['symbol']:t['quote_cap_scaled'] for t in targets},m.FROZEN_CAPS)
  self.assertFalse(any('book' in t for t in resolved['targets']))
  self.assertEqual([t['isin_id'] for t in targets],[100,101])
 def test_metadata_rejects_stale_identity_grid_and_publisher(self):
  for mutate in [lambda r:r.update(publisher_created=True),lambda r:r.update(captured_utc_ns=-200000000000),
          lambda r:r['targets'][0].update(symbol='wrong'),lambda r:r['targets'][0].update(min_step_scaled=0),
          lambda r:r['targets'][0].update(lower_price_scaled=101)]:
   resolved=self.resolved_metadata();mutate(resolved)
   with self.assertRaises(m.Refused):m.metadata_targets(resolved,2_000_000_000)
 def test_resolve_reuses_pinned_path_and_restores_callback(self):
  initial_callback=lambda *args:None
  base=types.SimpleNamespace(conservative_caps=initial_callback)
  class Base:
   def resolve(self):self.targets=base.conservative_caps(self.resolved,2_000_000_000)
  base.Supervisor=Base
  cls=m.owner_class(types.SimpleNamespace(Supervisor=Base,base=base));owner=cls.__new__(cls)
  owner.resolved=self.resolved_metadata();owner.resolve()
  self.assertIs(base.conservative_caps,initial_callback)
  self.assertEqual({t['symbol']:t['quote_cap_scaled'] for t in owner.targets},m.FROZEN_CAPS)
 def import_fresh(self,name):
  from importlib.machinery import SourceFileLoader
  path=MODULE_DIR/('fixtures/startup_guardian.py.txt' if name=='guardian' else name+'.py')
  spec=importlib.util.spec_from_loader(name,SourceFileLoader(name,str(path)))
  module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
 def fresh_book_state(self):
  ref=m.FROZEN_CAPS['CNY-12.26']//2
  target=dict(symbol='CNY-12.26',isin_id=101,quote_cap_scaled=m.FROZEN_CAPS['CNY-12.26'],reference_price_scaled=ref)
  terms=dict(sess_id=123,refdata_lifenum=7,interval_valid=True,reference_price_scaled=ref,
          min_step_scaled=100,lower_price_scaled=1000000,upper_price_scaled=2000000)
  state=dict(sess_id=123,queued=0,orders=[],positions=[dict(isin_id=101,xpos=1)],
       configuration=dict(risk=dict(instruments=[dict(isin_id=101,max_quote_notional_scaled=target['quote_cap_scaled'])])),
       instruments=[dict(isin_id=101,symbol='CNY-12.26',order_entry_ready=True,session_terms=terms,
       book=dict(valid=True,stream_last_callback_utc_ns=0,book_commit_monotonic_ns=5_000_000_000,
       exchange_moment_ns=99_000_000_000,best_bid=dict(price_scaled=ref,quantity=2),
       best_ask=dict(price_scaled=ref+100,quantity=2)))])
  return state,target
 def test_no_profile_price_freshness_uses_native_committed_clocks(self):
  prices=self.import_fresh('price_caps');state,target=self.fresh_book_state()
  with patch.object(prices.time,'monotonic_ns',return_value=6_000_000_000):
   self.assertEqual(prices.current_reference(state,target,100_000_000_000),1200200)
 def test_no_profile_price_rejects_stale_future_or_missing_book_clocks(self):
  prices=self.import_fresh('price_caps')
  for change in [('book_commit_monotonic_ns',1),('book_commit_monotonic_ns',99_000_000_000),
         ('exchange_moment_ns',1),('exchange_moment_ns',102_000_000_000)]:
   state,target=self.fresh_book_state();state['instruments'][0]['book'][change[0]]=change[1]
   with patch.object(prices.time,'monotonic_ns',return_value=20_000_000_000):
    with self.assertRaisesRegex(ValueError,'book_stale'):prices.current_reference(state,target,100_000_000_000)
 def test_guardian_reducing_ioc_uses_same_no_profile_freshness(self):
  import sys
  prices=self.import_fresh('price_caps');guardian=self.import_fresh('guardian');state,target=self.fresh_book_state()
  obj=guardian.Guardian.__new__(guardian.Guardian);obj.readonly_status=lambda *a:state
  owners=[];obj.owner=lambda meta:owners.append(True)
  with patch.dict(sys.modules,{'price_caps':prices}),patch.object(prices.time,'monotonic_ns',return_value=6_000_000_000),patch.object(prices.time,'time_ns',return_value=100_000_000_000):
   obj.validate_reduction(dict(targets=[target]),'place fake-cid 101 sell 1 12.002 ioc',None)
  self.assertEqual(owners,[True])
  state['instruments'][0]['book']['exchange_moment_ns']=1
  with patch.dict(sys.modules,{'price_caps':prices}),patch.object(prices.time,'monotonic_ns',return_value=6_000_000_000),patch.object(prices.time,'time_ns',return_value=100_000_000_000):
   with self.assertRaises(ValueError):obj.validate_reduction(dict(targets=[target]),'place fake-cid 101 sell 1 12.002 ioc',None)
 def test_native_kill_witness_requires_post_offset_request_and_enable(self):
  path=self.root/'native-journal'
  old=dict(event='kill_switch',data=dict(enabled=True))
  request=dict(event='operator_input',data=dict(line='kill on',channel='command_socket'))
  self.put(path,(json.dumps(old)+'\n').encode());begin=path.stat().st_size
  self.assertIsNone(m.kill_on_witness(path,begin))
  with path.open('ab') as out:out.write((json.dumps(request)+'\n'+json.dumps(old)+'\n').encode())
  proof=m.kill_on_witness(path,begin);self.assertTrue(proof['native_enabled']);self.assertEqual(proof['exchange_posts'],0)
  with path.open('ab') as out:out.write((json.dumps(dict(event='command',data={}))+'\n').encode())
  # Any exchange post before the witness is forbidden.
  self.put(path,(json.dumps(request)+'\n'+json.dumps(dict(event='command',data={}))+'\n'+json.dumps(old)+'\n').encode())
  with self.assertRaisesRegex(m.Refused,'exchange_post'):m.kill_on_witness(path,0)
 def test_closeout_cannot_signal_through_another_cleanup_owner(self):
  import fcntl
  calls=[]
  class Base:
   def closeout(self,product):calls.append('signal');return 'done'
  base=types.SimpleNamespace(Supervisor=Base)
  cls=m.owner_class(types.SimpleNamespace(Supervisor=Base,base=base));owner=cls.__new__(cls)
  owner.work=self.root;owner.publish=lambda value:None;owner.closing=False
  @contextlib.contextmanager
  def lock(path,nonblocking=False):
   with path.open('a') as out:
    fcntl.flock(out.fileno(),fcntl.LOCK_EX|(fcntl.LOCK_NB if nonblocking else 0));yield out
  owner.ops=types.SimpleNamespace(lock=lock)
  (self.root/'stop-requested').touch()
  with lock(self.root/'cleanup.lock'):
   with self.assertRaises(BlockingIOError):owner.closeout(object())
  self.assertEqual(calls,[])
  self.assertEqual(owner.closeout(object()),'done');self.assertEqual(calls,['signal'])

 def test_manual_failure_preserves_static_code_and_never_recovery(self):
  errors=[]
  class BaseRefused(Exception):pass
  class Base:
   def fail(self,error):errors.append(error)
  base=types.SimpleNamespace(Supervisor=Base,Refused=BaseRefused)
  cls=m.owner_class(types.SimpleNamespace(Supervisor=Base,base=base));owner=cls.__new__(cls)
  owner.product=None;owner.recovery_needed=True
  owner.fail(m.Refused('manual_product_start_window_missed'))
  self.assertIsInstance(errors[0],BaseRefused);self.assertEqual(str(errors[0]),'manual_product_start_window_missed')
  self.assertFalse(owner.recovery_needed)
 def lifecycle_fixture(self,cleanup_held=False):
  class Bare:pass
  base=types.SimpleNamespace(Supervisor=Bare,public_value=lambda x:x,
       strict_flat=lambda state,ops,targets:state['ready'] and state['flat'])
  cls=m.owner_class(types.SimpleNamespace(Supervisor=Bare,base=base))
  owner=cls.__new__(cls);owner.cfg=self.cfg;owner.root=self.root;owner.work=self.root
  owner.closing=False;owner.public={};owner.targets=[];owner.events=[];owner.cleanup_held=cleanup_held
  clock=types.SimpleNamespace(now=m.epoch(6,20))
  def sleep(seconds):clock.now+=max(.001,seconds)
  owner.rt=types.SimpleNamespace(atomic=lambda p,v:None)
  owner.ops=types.SimpleNamespace(process_identity=lambda pid:{'pid':pid},ready=lambda state,targets,entry=False:state['ready'])
  owner.await_startup_guardian=lambda:owner.events.append('guardian handshake')
  owner.await_installation_barrier=lambda:owner.events.append('seal barrier')
  owner.tick=lambda:None
  class Product:
   child=types.SimpleNamespace(poll=lambda:None)
   def status(self):
    return dict(ready=clock.now>=m.epoch(7),flat=not(m.epoch(13,45)<=clock.now<m.epoch(13,50)))
  product=Product()
  def start():owner.events.append('one normal start');clock.now=m.epoch(6,40);return product
  owner.start=start
  owner.coverage_gate=lambda state:owner.events.append('fresh coverage')
  owner.rt.configuration_gate=lambda *args:None
  def flatten(product,deadline,final=False):
   owner.events.append(('cleanup',clock.now,final))
   if owner.cleanup_held:raise BlockingIOError('another cleanup owner retains lock')
   return dict(cancel_confirmed=True,flat=True)
  owner.flatten=flatten
  def closeout(product):owner.events.append(('SIGTERM',clock.now));return dict(terminal=True,product_exit=0)
  owner.closeout=closeout
  return owner,clock,sleep
 def test_lifecycle_closed_preopen_waits_then_one_cleanup_and_terminal(self):
  owner,clock,sleep=self.lifecycle_fixture()
  with patch.object(m,'REVIEW_ONLY',False),patch.object(m.time,'time',side_effect=lambda:clock.now),patch.object(m.time,'sleep',side_effect=sleep):
   result=owner.run()
  self.assertTrue(result['terminal']);self.assertEqual(owner.events.count('one normal start'),1)
  self.assertEqual(owner.events.count('fresh coverage'),1)
  cleanups=[e for e in owner.events if isinstance(e,tuple) and e[0]=='cleanup']
  self.assertEqual(cleanups,[('cleanup',m.epoch(13,50),True)])
  self.assertEqual(owner.events[-1],('SIGTERM',m.epoch(16,5)))
 def test_lifecycle_cleanup_lock_is_not_bypassed_by_signal(self):
  owner,clock,sleep=self.lifecycle_fixture(cleanup_held=True)
  with patch.object(m,'REVIEW_ONLY',False),patch.object(m.time,'time',side_effect=lambda:clock.now),patch.object(m.time,'sleep',side_effect=sleep):
   with self.assertRaises(BlockingIOError):owner.run()
  self.assertFalse(any(isinstance(e,tuple) and e[0]=='SIGTERM' for e in owner.events))
  self.assertTrue(owner.closing)

if __name__=='__main__':unittest.main(verbosity=2)
