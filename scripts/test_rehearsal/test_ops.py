"""Hermetic scheduling/cleanup boundary checks; no CGate or network calls."""
from pathlib import Path
import importlib.util,json,subprocess,sys,tempfile,time,unittest
from unittest.mock import patch
spec=importlib.util.spec_from_file_location('ops',str(Path(__file__).with_name('ops.py')));ops=importlib.util.module_from_spec(spec);spec.loader.exec_module(ops)

class Boundaries(unittest.TestCase):
 def setUp(self):self.tmp=tempfile.TemporaryDirectory();self.w=Path(self.tmp.name)
 def tearDown(self):self.tmp.cleanup()
 def identity(self,ext=15001,user=14001):
  s='MOEXJ2 1 2 0 '+str(ext)+' '+str(user)+' 0';h=14695981039346656037
  for b in s.encode():h=((h^b)*1099511628211)&((1<<64)-1)
  p=self.w/'id';p.write_text(s+' '+str(h));a=self.w/'assignment';a.write_text(json.dumps(dict(instance='test',ext_id_begin=1000,ext_id_end=30999)));return p,a
 def test_claim_wrong_time_and_duplicate_refused(self):
  with self.assertRaises(ops.Stop):ops.claim(self.w/'claim',ops.START-1)
  self.assertFalse((self.w/'claim').exists());ops.claim(self.w/'claim',ops.START)
  with self.assertRaises(FileExistsError):ops.claim(self.w/'claim',ops.START+1)
 def test_flock_prevents_second_owner(self):
  with ops.lock(self.w/'lease'):
   with self.assertRaises(BlockingIOError):ops.lock(self.w/'lease',True)
 def test_checkpoint_bounded_even_when_checksum_valid(self):
  p,a=self.identity();self.assertEqual(ops.capacity(p,a,'test',1000,30999),15999)
  for ext,user in [(1,14001),(15001,0),(15001,4294967296),(29000,14001)]:
   p,a=self.identity(ext,user)
   with self.assertRaises(ops.Stop):ops.capacity(p,a,'test',1000,30999)
 def test_wrong_pid_identity_never_signalled(self):
  declared=dict(pid=99,start_ticks='1',executable='owned')
  with patch.object(ops,'process_identity',return_value=dict(declared,start_ticks='2')),patch.object(ops.os,'kill') as kill:
   with self.assertRaises(ops.Stop):ops.checked_signal(declared,15)
   kill.assert_not_called()
 def product(self):return ops.Product(self.w,sys.executable,{},[],[dict(isin_id=1,symbol='CNY-12.26')])
 def test_uncertain_add_is_sent_once(self):
  p=self.product();response=subprocess.CompletedProcess([],2,b'',b'uncertain')
  with patch.object(ops.time,'time',return_value=ops.START+100),patch.object(ops.subprocess,'run',return_value=response) as run:
   with self.assertRaises(ops.Stop):p.command('place q-test 1 buy 1 1 day')
   self.assertEqual(run.call_count,1)
 def test_new_risk_stopped_but_status_allowed(self):
  p=self.product();(self.w/'stop-requested').touch()
  with patch.object(ops.subprocess,'run',return_value=subprocess.CompletedProcess([],0,b'{"queued":0}',b'')) as run:
   with self.assertRaises(ops.Stop):p.command('place q-test 1 buy 1 1 day')
   self.assertEqual(p.command('status')['queued'],0);self.assertEqual(run.call_count,1)
 def test_deadline_stops_before_cli(self):
  with patch.object(ops.time,'time',return_value=ops.CLEANUP),patch.object(ops.subprocess,'run') as run:
   with self.assertRaises(ops.Stop):self.product().command('move q-test 2 1')
   run.assert_not_called()
 def test_detached_child_survives_parent_exit(self):
  marker=self.w/'detached-proof'
  child='import time;from pathlib import Path;time.sleep(.2);Path('+repr(str(marker))+').write_text("survived")'
  parent='import subprocess,sys;subprocess.Popen([sys.executable,"-c",'+repr(child)+'],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)'
  subprocess.run([sys.executable,'-c',parent],check=True)
  deadline=time.monotonic()+3
  while not marker.exists() and time.monotonic()<deadline:time.sleep(.05)
  self.assertEqual(marker.read_text(),'survived')

if __name__=='__main__':unittest.main()
