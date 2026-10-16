"""Run the actual guardian with a fake process and a held cleanup lock."""
from pathlib import Path
import ast,copy,json,runpy,sys,tempfile,types,unittest
from unittest.mock import patch
import ops

class EndIteration(BaseException):pass
class GuardianCloseout(unittest.TestCase):
 def run_guardian(self,state,now):
  with tempfile.TemporaryDirectory() as temp:
   w=Path(temp);owned=dict(pid=99,start_ticks='1',executable='/fake/moexctl')
   for name,value in [('public.json',state),('product-owned.json',owned),('targets-private.json',[dict(isin_id=1)])]:ops.atomic(w/name,value)
   (w/'stop-requested').touch()
   adapter=types.ModuleType('deployment');adapter.load=lambda:(types.SimpleNamespace(client_isolation=lambda *a:True),w,dict(binary_sha256='binary'))
   signals=[]
   with patch.dict(sys.modules,deployment=adapter),patch.object(ops.time,'time',return_value=now),patch.object(ops.time,'time_ns',return_value=int(now*1e9)),patch.object(ops.time,'sleep',side_effect=EndIteration),patch.object(ops,'process_identity',return_value=owned),patch.object(ops,'sha',return_value='binary'),patch.object(ops,'clean_product',side_effect=BlockingIOError),patch.object(ops,'checked_signal',side_effect=lambda i,s:signals.append(s)),patch.object(ops,'Product'):
    try:runpy.run_path(str(Path(__file__).with_name('guardian.py')))
    except EndIteration:pass
   return signals
 def test_healthy_final_cleanup_retains_ownership_after_old_60s_deadline(self):
  now=ops.END+61
  state=dict(state='closing',phase='final_cleanup',terminal=False,updated_utc_ns=int(now*1e9),cleanup_deadline_utc=ops.END+200)
  self.assertEqual(self.run_guardian(state,now),[])
 def test_stale_owner_is_still_interrupted(self):
  now=ops.END+61
  state=dict(state='closing',phase='final_cleanup',updated_utc_ns=int((now-121)*1e9),cleanup_deadline_utc=ops.END+200)
  self.assertTrue(self.run_guardian(state,now))
 def test_expired_cleanup_is_still_interrupted(self):
  now=ops.END+241
  state=dict(state='closing',phase='final_cleanup',updated_utc_ns=int(now*1e9),cleanup_deadline_utc=ops.END+200)
  self.assertTrue(self.run_guardian(state,now))
 def test_worker_publishes_complete_only_with_verified_terminal_closeout(self):
  source=ast.parse(Path(__file__).with_name('worker.py').read_text())
  final=next(n.finalbody for n in reversed(source.body) if isinstance(n,ast.Try) and n.finalbody)
  code=compile(ast.Module(body=final,type_ignores=[]),'worker-finally','exec')
  for exit_code in (0,8):
   public=dict(state='running',phase='final_clearing_observation');observed=[]
   product=types.SimpleNamespace(child=types.SimpleNamespace(returncode=exit_code),close=lambda:None)
   def cleanup(*a,**k):
    observed.append(copy.deepcopy(public));return dict(flat=True,cancel_confirmed=True)
   scope=dict(product=product,public=public,environment={'inventory':lambda:dict(client_count=0)},
              clean_product=cleanup,time=types.SimpleNamespace(time=lambda:ops.END,time_ns=lambda:0),
              stamp=lambda *a:ops.END+600,END=ops.END,touch=lambda:observed.append(copy.deepcopy(public)),
              final_statistics=lambda:{},deployment=types.SimpleNamespace(reconstruct_orders=lambda w:None),work=Path('.'))
   exec(code,scope)
   self.assertEqual(observed[0]['state'],'closing')
   self.assertFalse(any(s.get('state')=='complete' and not s.get('terminal') for s in observed))
   self.assertTrue(public['terminal'])
   self.assertEqual(public['state'],'complete' if exit_code==0 else 'blocked')

if __name__=='__main__':unittest.main()
