"""Independent VPS watcher; stops new risk if supervisor disappears or expires."""
from pathlib import Path
import json,os,signal,sys,time
sys.dont_write_bytecode=True;os.umask(0o077)
from ops import *
from deployment import load
signal.signal(signal.SIGHUP,signal.SIG_IGN)
deployment,work,cfg=load();client_isolation=deployment.client_isolation
atomic(work/'guardian-owned.json',process_identity(os.getpid()))
def safe_signal(owned):
 sig=signal.SIGTERM
 try:client_isolation(work,owned['pid'])
 except BaseException:sig=signal.SIGKILL
 return checked_signal(owned,sig)
while time.time()<END+900:
 try:state=json.loads((work/'public.json').read_text())
 except (FileNotFoundError,json.JSONDecodeError):state={}
 if state.get('terminal'):
  if state.get('product_shutdown_error') or state.get('product_exit') not in (None,0):
   try:
    owned=json.loads((work/'product-owned.json').read_text())
    if process_identity(owned['pid'])==owned:
     safe_signal(owned);grace=time.time()+15
     while time.time()<grace:
      try:
       if process_identity(owned['pid'])!=owned:break
      except (FileNotFoundError,ProcessLookupError):break
      time.sleep(1)
     else:checked_signal(owned,signal.SIGKILL)
     atomic(work/'guardian-public.json',dict(state='forced_shutdown',flat=False,reason='supervisor_shutdown_incomplete'))
   except (FileNotFoundError,ProcessLookupError):pass
  break
 stale=time.time_ns()-state.get('updated_utc_ns',0)>120000000000
 normal_closeout=cleanup_owner_active(state,time.time())
 deadline=(time.time()>=END+60 or time.time()>=CLEANUP+600) and not normal_closeout
 requested=(work/'stop-requested').exists() and not normal_closeout
 if time.time()>=START and (stale or deadline or requested):
  (work/'stop-requested').touch()
  if (work/'product-owned.json').exists() and (work/'targets-private.json').exists():
   try:
    owned=json.loads((work/'product-owned.json').read_text())
    if process_identity(owned['pid'])!=owned or sha(owned['executable'])!=cfg['binary_sha256']:raise Stop('guardian_product_identity_mismatch')
    targets=json.loads((work/'targets-private.json').read_text())
    p=Product(work,owned['executable'],dict(PATH='/usr/bin:/bin',HOME=str(work/'home'),TMPDIR=str(work/'tmp')),[],targets);p.isolation=lambda:client_isolation(work,owned['pid'])
    try:
     result=clean_product(p,until=min(time.time()+180,END+600));atomic(work/'guardian-public.json',dict(state='cleanup',**result))
    except BlockingIOError:
     if stale or deadline:
      safe_signal(owned);atomic(work/'guardian-public.json',dict(state='blocked',flat=False,reason='supervisor_cleanup_unresponsive'));break
     time.sleep(5);continue # Bounded healthy supervisor cleanup ownership.
    finally:
     # A held supervisor cleanup lock means it retains cleanup ownership;
     # do not interrupt its in-progress position reduction.
     pass
    safe_signal(owned)
   except (FileNotFoundError,ProcessLookupError):pass
   except BaseException as error:
    atomic(work/'guardian-public.json',dict(state='blocked',flat=False,error_class=type(error).__name__))
    try:
     if 'owned' in locals():safe_signal(owned)
    except BaseException:pass
  if stale or deadline:break
 time.sleep(15)
atomic(work/'guardian-done.json',dict(finished_utc_ns=time.time_ns(),no_new_entry=True))
