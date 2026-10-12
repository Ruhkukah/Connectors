"""Reviewable CLI orchestration only. No SDK, publisher or venue API imports."""
from pathlib import Path
import datetime,fcntl,hashlib,json,os,signal,subprocess,time,urllib.request

UTC=datetime.timezone.utc
START=datetime.datetime(2026,10,12,3,45,tzinfo=UTC).timestamp()
LATEST_START=datetime.datetime(2026,10,12,4,0,tzinfo=UTC).timestamp()
CLEANUP=datetime.datetime(2026,10,12,12,40,tzinfo=UTC).timestamp()
END=datetime.datetime(2026,10,12,13,5,tzinfo=UTC).timestamp()
HEAD='f849e05785e76de1d6a1c04fc879dea8a15829f2'
TERMINAL={'Filled','Cancelled','Rejected'}
REQUIRED={'FORTS_TRADE_REPL','FORTS_USERORDERBOOK_REPL','FORTS_POS_REPL','FORTS_PART_REPL','FORTS_REFDATA_REPL','FORTS_SESSIONSTATE_REPL','FORTS_INSTRUMENTSTATE_REPL','FORTS_AGGR20_REPL','p2mqreply'}

class Stop(RuntimeError):pass

def atomic(path,value):
 path=Path(path);tmp=path.with_name(path.name+'.'+str(os.getpid())+'.tmp')
 with tmp.open('w') as f:json.dump(value,f);f.write('\n');f.flush();os.fsync(f.fileno())
 tmp.replace(path)

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def lock(path,nonblocking=False):
 f=Path(path).open('a')
 try:fcntl.flock(f,fcntl.LOCK_EX|(fcntl.LOCK_NB if nonblocking else 0));return f
 except BaseException:f.close();raise

def claim(path,now=None):
 now=time.time() if now is None else now
 if not START<=now<=LATEST_START:raise Stop('outside_authorized_start_window')
 with Path(path).open('x') as f:json.dump(dict(claimed_utc_ns=time.time_ns(),head=HEAD),f);f.flush();os.fsync(f.fileno())

def process_identity(pid):
 p=Path('/proc')/str(pid)
 raw=(p/'stat').read_text().rsplit(')',1)[1].split()
 return dict(pid=pid,start_ticks=raw[19],executable=os.readlink(p/'exe'))

def checked_signal(identity,sig):
 try:actual=process_identity(identity['pid'])
 except (FileNotFoundError,ProcessLookupError):return False
 if actual!=identity:raise Stop('owned_process_identity_changed')
 os.kill(identity['pid'],sig);return True

def inspect_router(probe,binary,binary_sha256,owner_pid=None):
 """An expected product is an owned client, not a zero-client preflight."""
 return probe.inspect(dict(environment='test',binary=str(binary),binary_sha256=binary_sha256),owner_pid=owner_pid)

def fetch(url):
 req=urllib.request.Request(url,headers={'User-Agent':'MOEX-TEST-qualification','Accept':'application/vnd.github+json'})
 with urllib.request.urlopen(req,timeout=20) as r:
  data=r.read(5*1024*1024+1)
  if len(data)>5*1024*1024:raise Stop('oversize_preflight_response')
  return data

def verify_public_ci(work):
 pr=json.loads(fetch('https://api.github.com/repos/Ruhkukah/Connectors/pulls/68'))
 if pr['head']['sha']!=HEAD or not pr['draft'] or pr['state']!='open':raise Stop('published_frozen_head_changed')
 checks=json.loads(fetch('https://api.github.com/repos/Ruhkukah/Connectors/commits/'+HEAD+'/check-runs'))['check_runs']
 latest={}
 for c in checks:
  if c['name'] in ('connector-validation','component-sanitizers') and c['name'] not in latest:latest[c['name']]=c
 if len(latest)!=2 or any(c['status']!='completed' or c['conclusion']!='success' for c in latest.values()):raise Stop('frozen_ci_not_green')
 atomic(Path(work)/'ci-public.json',dict(head=HEAD,checks={k:v['conclusion'] for k,v in latest.items()},checked_utc_ns=time.time_ns()))
 schedule=fetch('https://www.moex.com/s438').decode('utf-8')
 if not all(x in schedule for x in ('07:00:00','10:40:00','11:00:00','16:00:00')):raise Stop('official_schedule_not_verified')
 (Path(work)/'schedule-private.html').write_text(schedule)

def capacity(state,assignment,instance,begin,end):
 a=json.loads(Path(assignment).read_text())
 if a.get('instance')!=instance or a.get('ext_id_begin')!=begin or a.get('ext_id_end')!=end:raise Stop('assignment_changed')
 s=Path(state).read_text().split()
 if len(s)!=8 or s[0]!='MOEXJ2':raise Stop('identity_checkpoint_format')
 checksum=14695981039346656037
 for b in ' '.join(s[:7]).encode():checksum=((checksum^b)*1099511628211)&((1<<64)-1)
 if int(s[7])!=checksum:raise Stop('identity_checkpoint_checksum')
 remaining=end-int(s[4])+1
 if not begin<=int(s[4])<=end+1 or not 0<int(s[5])<=4294967295 or any(not 0<=int(x)<=18446744073709551615 for x in s[1:7]):raise Stop('identity_checkpoint_numeric_bounds')
 if remaining<7000:raise Stop('insufficient_identifier_budget')
 return remaining

def ready(s,targets,entry=True):
 listeners=s['transport']['listeners'];replay=s['trade_replay'];anchor=replay['used_anchor'];pos=s['pos_anchor']
 good=(len(listeners)==len(REQUIRED) and {x['stream'] for x in listeners}==REQUIRED
       and all(x['online'] and x['snapshot_complete'] and x['native']['state']==3 and x['error']['code']==0 for x in listeners)
       and s['transport']['valid'] and s['transport']['connection']['state']==3 and s['transport']['publisher']['state']==3
       and not s['reconstructing'] and replay['anchor_ready'] and anchor and pos and pos['online'] and pos['snapshot_complete']
       and replay['accepted_lifenum']>0 and replay['accepted_lifenum']==anchor['trades_lifenum']==pos['trades_lifenum']
       and not any(s.get(k) for k in ('operator_action_required','journal_failed','cancel_only','log_error','cgate_key_check_failed')))
 if not good:return False
 if {x['isin_id']:x['symbol'] for x in s['instruments']}!={x['isin_id']:x['symbol'] for x in targets}:return False
 for x in s['instruments']:
  t=x['session_terms']
  if not t or t['sess_id']!=s['sess_id'] or t['refdata_lifenum']<=0 or not t['interval_valid'] or t['reference_price_scaled']<=0:return False
  if entry and (not x['order_entry_ready'] or not x['book']['valid']):return False
 return True

def flat(s):
 return s['queued']==0 and all(x['xpos']==0 for x in s['positions']) and all(x['state'] in TERMINAL and not x['operator_action_required'] for x in s['orders'])

class Product:
 def __init__(self,work,binary,env,args,targets,touch=lambda:None):
  self.work=Path(work);self.binary=str(binary);self.env=env;self.args=args;self.targets=targets;self.touch=touch;self.child=None;self.expected_sha=sha(binary);self.isolation=None;self.risk_guard=None
 def start(self):
  if sha(self.binary)!=self.expected_sha:raise Stop('product_binary_changed')
  out=(self.work/'product-stdout-private.log').open('ab');err=(self.work/'product-stderr-private.log').open('ab')
  try:self.child=subprocess.Popen(self.args,cwd=self.work,env=self.env,stdin=subprocess.DEVNULL,stdout=out,stderr=err,start_new_session=True)
  finally:out.close();err.close()
  for _ in range(20):
   try:i=process_identity(self.child.pid);break
   except FileNotFoundError:time.sleep(.01)
  else:raise Stop('product_start_failed')
  if Path(i['executable']).resolve()!=Path(self.binary).resolve():raise Stop('product_binary_identity_mismatch')
  atomic(self.work/'product-owned.json',i)
 def command(self,line,permit_risk=True):
  self.touch()
  if self.risk_guard and line.startswith(('place ','move ','kill off')):self.risk_guard()
  if self.isolation and line.startswith(('place ','move ','kill off','cancel-all ')):self.isolation()
  if line.startswith(('place ','move ','kill off')) and (not permit_risk or time.time()>=CLEANUP or (self.work/'stop-requested').exists()):raise Stop('new_risk_stopped')
  with lock(self.work/'command.lock'):
   if line.startswith(('place ','move ','kill off')) and (time.time()>=CLEANUP or (self.work/'stop-requested').exists()):raise Stop('new_risk_stopped')
   # Persist intent before a request. An uncertain Add is never resent.
   with (self.work/'operator-private.ndjson').open('a') as f:
    f.write(json.dumps(dict(utc_ns=time.time_ns(),intent=line))+'\n');f.flush();os.fsync(f.fileno())
   r=subprocess.run([self.binary,'plaza2','cmd','--command-socket',str(self.work/'cmd.sock'),line],cwd=self.work,env=self.env,capture_output=True,timeout=15)
   with (self.work/'operator-private.ndjson').open('a') as f:
    f.write(json.dumps(dict(utc_ns=time.time_ns(),line=line,exit=r.returncode,stdout=r.stdout.decode(errors='replace'),stderr=r.stderr.decode(errors='replace')))+'\n')
   if r.returncode:raise Stop('product_command_refused_or_uncertain')
   data=json.loads(r.stdout)
   if line!='status' and not data.get('ok'):raise Stop('product_command_refused_or_uncertain')
   return data
 def status(self):
  s=self.command('status')
  with (self.work/'status-private.ndjson').open('a') as f:f.write(json.dumps(dict(utc_ns=time.time_ns(),status=s))+'\n')
  return s
 def wait_ready(self,until,require_flat=False,entry=True):
  last=None
  while time.time()<until:
   self.touch()
   if self.child.poll() is not None:raise Stop('product_exited')
   if (self.work/'cmd.sock').exists():
    last=self.status()
    if any(last.get(k) for k in ('journal_failed','cancel_only','log_error','cgate_key_check_failed','operator_action_required')):raise Stop('product_safety_alert')
    if ready(last,self.targets,entry) and (not require_flat or flat(last)):return last
   time.sleep(1)
  raise Stop('readiness_or_flatness_deadline')
 def crash(self):
  i=json.loads((self.work/'product-owned.json').read_text());checked_signal(i,signal.SIGKILL);self.child.wait(timeout=10)
  self.start()
 def close(self):
  if self.child is not None and self.child.poll() is None:
   sig=signal.SIGTERM
   if self.isolation:
    try:self.isolation()
    except BaseException:sig=signal.SIGKILL # Sole-instance mass cancel is unsafe after competing-client loss.
   checked_signal(json.loads((self.work/'product-owned.json').read_text()),sig)
   self.child.wait(timeout=20)

def clean_product(product,until=None,final=True):
 """Serialize cleanup; no new entry, no uncertain-Add retry."""
 until=time.time()+180 if until is None else until
 with lock(product.work/'cleanup.lock',nonblocking=True):
  if final:(product.work/'stop-requested').touch()
  result=dict(cancel_confirmed=False,flat=False)
  product.command('kill on',permit_risk=False)
  isolated=True
  if product.isolation:
   try:product.isolation()
   except BaseException:isolated=False
  if isolated:
   for t in product.targets:product.command('cancel-all '+str(t['isin_id']),permit_risk=False)
  else:
   for o in product.status()['orders']:
    if o['instance_owned'] and o['state'] not in TERMINAL and o['order_id']>0:product.command('cancel '+o['client_order_id'],permit_risk=False)
  while time.time()<until:
   s=product.status()
   if s['queued']==0 and all(x['state'] in TERMINAL for x in s['orders']):
    result['cancel_confirmed']=True;break
   time.sleep(1)
  if not result['cancel_confirmed']:return result
  # At most four bounded position-reducing IOC attempts in total. Each
  # attempt gets a fresh confirmed POS; never re-send an uncertain request.
  attempts=0
  for t in product.targets:
   for n in range(4):
    if time.time()>=until or attempts>=4:break
    s=product.status();rows=[p for p in s['positions'] if p['isin_id']==t['isin_id']]
    if len(rows)!=1:raise Stop('cleanup_position_ambiguous')
    pos=rows[0]['xpos']
    if pos==0:break
    if product.isolation:product.isolation()
    if not ready(s,product.targets) or abs(pos)>2:raise Stop('cleanup_current_proof_unavailable')
    instrument=next(x for x in s['instruments'] if x['isin_id']==t['isin_id']);side='sell' if pos>0 else 'buy'
    level=instrument['book']['best_bid' if side=='sell' else 'best_ask'];terms=instrument['session_terms']
    if not level or not level['quantity']:raise Stop('cleanup_crossing_quote_unavailable')
    price=level['price_scaled'];cap=next(x['max_quote_notional_scaled'] for x in s['configuration']['risk']['instruments'] if x['isin_id']==t['isin_id'])
    qty=min(abs(pos),2,level['quantity'],cap//price if price>0 else 0)
    if qty<=0 or qty*price>cap or cap>2*terms['reference_price_scaled'] or price%terms['min_step_scaled'] or not terms['lower_price_scaled']<=price<=terms['upper_price_scaled']:raise Stop('cleanup_quote_outside_caps')
    # Cleanup bypasses only the scheduler's new-entry stop, never product risk.
    suffix=str(time.time_ns());cid='q-close-'+suffix;attempts+=1;before=rows[0];offset=(product.work/'interaction.ndjson').stat().st_size
    with lock(product.work/'command.lock'):
     def cli(line):
      if line.startswith(('kill off','place ')) and time.time()>=until:raise Stop('cleanup_command_expired')
      with (product.work/'cleanup-private.ndjson').open('a') as f:f.write(json.dumps(dict(utc_ns=time.time_ns(),intent=line))+'\n');f.flush();os.fsync(f.fileno())
      r=subprocess.run([product.binary,'plaza2','cmd','--command-socket',str(product.work/'cmd.sock'),line],cwd=product.work,env=product.env,capture_output=True,timeout=15)
      with (product.work/'cleanup-private.ndjson').open('a') as f:f.write(json.dumps(dict(utc_ns=time.time_ns(),line=line,exit=r.returncode,stdout=r.stdout.decode(errors='replace'),stderr=r.stderr.decode(errors='replace')))+'\n')
      data=json.loads(r.stdout)
      if r.returncode or (line!='status' and not data.get('ok')):raise Stop('cleanup_command_uncertain')
      return data
     try:
      cli('kill off')
      cli('place '+cid+' '+str(t['isin_id'])+' '+side+' '+str(qty)+' '+f'{price/100000:.5f}'+' ioc')
      post_deadline=min(until,time.time()+10);posted=False
      while time.time()<post_deadline:
       state=cli('status');o=[x for x in state['orders'] if x['client_order_id']==cid]
       if len(o)>1:raise Stop('cleanup_order_identity_ambiguous')
       if o and (o[0]['order_id']>0 or o[0]['state']=='Rejected'):posted=True;break
       time.sleep(.2)
      if not posted:raise Stop('cleanup_post_not_confirmed')
     finally:cli('kill on')
    settled=False;deadline=min(until,time.time()+60)
    while time.time()<deadline:
     after=product.status();orders=[x for x in after['orders'] if x['client_order_id']==cid]
     p=next((x for x in after['positions'] if x['isin_id']==t['isin_id']),None)
     if len(orders)>1:raise Stop('cleanup_order_identity_ambiguous')
     if orders:
      o=orders[0]
      if not o['instance_owned'] or o['operator_action_required'] or o['state']=='Unknown' or o['isin_id']!=t['isin_id'] or o['side']!=side or o['quantity']!=qty or not 0<=o['executed']<=qty:raise Stop('cleanup_order_identity_invalid')
      if o['state'] in TERMINAL and o['remaining']==0 and p and p['xpos']==pos+(o['executed'] if side=='buy' else -o['executed']):
       credited=[]
       with (product.work/'interaction.ndjson').open() as f:
        f.seek(offset)
        for line in f:
         try:r=json.loads(line)
         except json.JSONDecodeError:break
         if r['event']=='trade' and r['data'].get('client_order_id')==cid:credited.append(r['data'])
       keys=[(x['deal_id'],x['order_id']) for x in credited]
       counter='buys' if side=='buy' else 'sells'
       fill_proved=(len(keys)==len(set(keys)) and all(x['order_id']==o['order_id'] and x['quantity']>0 for x in credited) and sum(x['quantity'] for x in credited)==o['executed'] and (o['executed']==0 or (p[counter]>=before[counter]+o['executed'] and p['last_deal_id'] in {x['deal_id'] for x in credited})))
       if fill_proved:settled=True;break
     time.sleep(.5)
    if not settled:raise Stop('cleanup_fill_position_unconfirmed')
  final=product.status();result['flat']=flat(final) and ready(final,product.targets,entry=False)
  atomic(product.work/'end-flat-private.json',final)
  return result


def clock_proof(tracking):
 import re,math
 def number(label):
  m=re.search(r'^'+re.escape(label)+r'\s*:\s*([+-]?[0-9.]+)\s+seconds',tracking,re.M)
  if not m:raise Stop('clock_measurement_missing')
  v=float(m.group(1))
  if not math.isfinite(v):raise Stop('clock_measurement_invalid')
  return v
 offset=number('System time');delay=number('Root delay');dispersion=number('Root dispersion')
 bound=abs(offset)+abs(delay)/2+dispersion
 if not re.search(r'^Leap status\s*:\s*Normal\s*$',tracking,re.M) or dispersion<0 or bound>=1:raise Stop('clock_unsynchronized_or_outside_one_second')
 # Chrony reports the magnitude plus the direction of system-minus-NTP time.
 sign=-1 if re.search(r'^System time.*slow of NTP',tracking,re.M) else 1
 return dict(offset_us=round(abs(offset)*sign*1000000),root_delay_ms=delay*1000,root_dispersion_ms=dispersion*1000,bound_ms=bound*1000)
