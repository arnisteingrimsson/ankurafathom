"""Contract checks against the running local project host (no browser automation)."""
import json,time,urllib.request,urllib.error
from pathlib import Path
BASE='http://127.0.0.1:8087'
def request(path,body=None):
 data=None if body is None else json.dumps(body).encode()
 with urllib.request.urlopen(urllib.request.Request(BASE+path,data=data,headers={'Content-Type':'application/json'}),timeout=30) as r:return r.read()
def api(path,body=None):return json.loads(request(path,body))
checks=[]
for path,marker in [('/',b'Define the practice'),('/workshop/app.js',b'workshop/sessions'),('/workshop/style.css',b'.agent-grid'),('/monthly',b'live.js'),('/analysis',b'app.js')]:
 assert marker in request(path);checks.append(path)
catalog=api('/workshop/catalog');c=catalog['default'];assert sum(l['count'] for l in c['levels'])==150
receipt=api('/workshop/verification');assert receipt['available'] and receipt['passed'] and receipt['sources_current'];checks.append('Current source-matched verification receipt')
s=api('/workshop/sessions',c);key=s['id']
try:
 assert s['frame']['time']==0 and len(s['frame']['agents'])==150
 a=api(f'/workshop/sessions/{key}/step',dict(expected_revision=0,hours=12));assert a['frame']['time']==12 and a['frame']['checks_passed']
 b=api(f'/workshop/sessions/{key}/step',dict(expected_revision=0,hours=12));assert b==a
 record=api(f'/workshop/sessions/{key}/record');assert record['config']==c and len(record['frames'])==2
 evidence=api(f'/workshop/sessions/{key}/evidence',dict(source='Synthetic HTTP contract check',kind='synthetic',rows=[dict(time=12,metric='headcount',unit='person',actual=150,tolerance=0)]));assert evidence['passed']
 checks.extend(['Initial native state','12-hour advance and native checks','Idempotent retry','Exact captured definition','Evidence comparison'])
finally:api(f'/workshop/sessions/{key}/stop',{})
c['parameters']['days']=1;e=api('/workshop/experiments',dict(config=c,variable='license_share',values=[0,.3],replications=1));deadline=time.monotonic()+30
while time.monotonic()<deadline:
 e=api('/workshop/experiments/'+e['id'])
 if e['status'] not in ('queued','running'):break
 time.sleep(.1)
assert e['status']=='completed' and e['finished']==2;checks.append('Background experiment API')
out=Path(__file__).resolve().parents[3]/'artifacts/tr-operating-acceptance/http.json';out.write_text(json.dumps(dict(passed=True,checks=checks,session=key,experiment=e['id']),indent=2));print(json.dumps(dict(passed=True,checks=checks)))
