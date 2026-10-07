"""Project-owned native hybrid sessions and paired scenario experiments."""
import copy
import json
import threading
import time
import uuid
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
from application.fathom_service.contracts import canonical, digest, require, fields, number, integer
from application.fathom_service.core import save
from live import LiveSession
from workshop.spec import validate, catalog, METRIC_UNITS

class Session(LiveSession):
    def __init__(self,folder,config,runner):
        import subprocess
        self.id=folder.name;self.folder=folder;self.lock=threading.RLock();self.frames=[];self.revision=0;self.status='initializing';self.last_access=time.monotonic();self.process=None
        self.config=validate(config);self.horizon=config['parameters']['days']*24
        root=runner.parents[4];build=json.loads(runner.with_suffix('.build.json').read_text());require(build['runner_sha256']==digest(runner.read_bytes()),'Runner changed since build receipt; rebuild the operating model');require(all((root/k).is_file() and digest((root/k).read_bytes())==v for k,v in build['sources'].items()),'Native model sources changed; rebuild the operating model');sources={**build['sources'],'projects/tr-ui/workshop/spec.py':digest((runner.parents[2]/'workshop/spec.py').read_bytes())}
        self.record=dict(id=self.id,created_at=time.time(),model='tr-operating-1',config=self.config,config_sha256=digest(canonical(config)),runner_sha256=digest(runner.read_bytes()),source_sha256=sources,build=build,methods=catalog()['methods'],limitations=catalog()['limitations'],frames=[],status=self.status,computed_through=0)
        save(folder/'definition.json',self.config);self.log=(folder/'native.log').open('wb')
        try:
            self.process=subprocess.Popen([str(runner)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.log,bufsize=0)
            self._send(config);self._accept(self._read(),0)
        except Exception:self.close('failed');raise
    def _accept(self,frame,expected):
        require(frame['time']==expected,'Unexpected native clock')
        frame['previous_sha256']=self.frames[-1]['sha256'] if self.frames else None;frame['sha256']=digest(canonical(frame));self.frames.append(frame)
        self.status='failed' if not frame['checks_passed'] else 'completed' if frame['complete'] else 'paused'
        self.record.update(status=self.status,computed_through=expected,frames=self.frames);self._save()
        if self.status in ('failed','completed'):self._finish_process()
    def view(self):return dict(id=self.id,status=self.status,revision=self.revision,frame=self.frames[-1],config_sha256=self.record['config_sha256'])
    def step(self,request):
        fields(request,('expected_revision','hours'),('expected_revision','hours'))
        with self.lock:
            self.last_access=time.monotonic();expected=request['expected_revision'];hours=request['hours']
            require(integer(expected,0,100000),'Invalid revision');require(number(hours) and .25<=hours<=24,'Step must be 0.25–24 hours')
            if expected==self.revision-1:return self.view()
            require(expected==self.revision,'Clock changed; refresh the session','REVISION',409);require(self.status=='paused','Session is not available to advance')
            target=min(self.horizon,self.frames[-1]['time']+hours);self.status='computing'
            try:self._send(dict(until=target));self.revision+=1;self._accept(self._read(),target)
            except Exception:self.close('failed');raise
            return self.view()
    def close(self,status='stopped'):
        with self.lock:
            if self.status not in ('completed','failed'):self.status=status
            self._finish_process();self.record.update(status=self.status);self._save()
            return dict(id=self.id,status=self.status,revision=self.revision)

class Workshop:
    def __init__(self,state,runner):
        self.state=state;state.mkdir(parents=True,exist_ok=True);self.runner=runner.resolve();self.sessions={};self.experiments={};self.lock=threading.RLock();self.pool=ThreadPoolExecutor(max_workers=1)
    def _new(self,config):
        validate(config);require(self.runner.is_file(),'Build the project hybrid runner first');folder=self.state/uuid.uuid4().hex;folder.mkdir();return Session(folder,config,self.runner)
    def verification(self):
        root=self.runner.parents[4];path=root/'artifacts/tr-operating-acceptance/verification.json'
        if not path.exists():return dict(available=False)
        receipt=json.loads(path.read_text());receipt['available']=True
        receipt['sources_current']=all((root/k).is_file() and digest((root/k).read_bytes())==v for k,v in receipt['sources'].items())
        return receipt
    def create(self,config):
        with self.lock:
            for s in self.sessions.values():
                if s.status=='paused' and time.monotonic()-s.last_access>900:s.close('expired')
            require(sum(s.status in ('paused','computing') for s in self.sessions.values())<4,'Reset an existing session before starting another')
            s=self._new(config);self.sessions[s.id]=s;return s.view()
    def get(self,key):
        with self.lock:require(key in self.sessions,'Session unavailable; start a new run');return self.sessions[key]
    def experiment(self,body):
        fields(body,('config','variable','values','replications'),('config','variable','values','replications'));config=validate(body['config']);var=body['variable']
        choices={'license_share','ai_saving','training_hours','prospects_day','review_extra','bd_fraction','secondary_skill','win_rate'}
        require(var in choices,'Unsupported experiment variable');values=body['values'];require(isinstance(values,list) and 2<=len(values)<=8 and all(number(v) for v in values) and len(set(values))==len(values),'Supply 2–8 distinct numeric values')
        require(integer(body['replications'],1,10),'Replications must be 1–10');require(config['parameters']['seed']+body['replications']-1<=1000000,'Seed range exceeded')
        for value in values:
            candidate=copy.deepcopy(config);candidate['parameters'][var]=value;validate(candidate)
        with self.lock:
            require(not any(e['status'] in ('queued','running','cancelling') for e in self.experiments.values()),'One experiment at a time')
            key=uuid.uuid4().hex;e=dict(id=key,status='queued',variable=var,values=values,replications=body['replications'],config=config,results=[],finished=0,total=len(values)*body['replications'],cancel=False);self.experiments[key]=e
            self.pool.submit(self._run,e)
            return copy.deepcopy(e)
    def _run(self,e):
        with self.lock:e['status']='running'
        try:
            for rep in range(e['replications']):
                for value in e['values']:
                    with self.lock:
                        if e['cancel']:e['status']='cancelled';return
                    c=copy.deepcopy(e['config']);c['parameters'][e['variable']]=value;c['parameters']['seed']+=rep;s=self._new(c)
                    try:
                        while s.status=='paused':
                            with self.lock:
                                if e['cancel']:e['status']='cancelled';return
                            s.step(dict(expected_revision=s.revision,hours=24))
                        require(s.status=='completed','A native check failed')
                        frame=s.frames[-1]
                        with self.lock:
                            e['results'].append(dict(value=value,seed=c['parameters']['seed'],session=s.id,metrics=frame['metrics'],queues=frame['queues'],checks_passed=frame['checks_passed'],record_sha256=s.record['record_sha256']));e['finished']+=1
                    finally:s.close()
            with self.lock:e['status']='completed'
        except Exception as error:
            with self.lock:e.update(status='failed',error=str(error))
        finally:
            with self.lock:save(self.state/(e['id']+'-experiment.json'),e)
    def experiment_state(self,key,cancel=False):
        with self.lock:
            require(key in self.experiments,'Unknown experiment');e=self.experiments[key]
            if cancel and e['status'] in ('queued','running'):e.update(cancel=True,status='cancelling')
            return copy.deepcopy(e)
    def evidence(self,key,body):
        fields(body,('source','kind','rows'),('source','kind','rows'));require(isinstance(body['source'],str) and 0<len(body['source'])<=1000,'Name the evidence source');require(body['kind'] in ('observed','synthetic'),'Evidence kind must be observed or synthetic');require(isinstance(body['rows'],list) and 0<len(body['rows'])<=1000,'Supply 1–1000 observations')
        s=self.get(key)
        with s.lock:
            frames={f['time']:f for f in s.frames};out=[];seen=set()
            for row in body['rows']:
                fields(row,('time','metric','actual','tolerance','unit'),('time','metric','actual','tolerance','unit'))
                require(number(row['time']) and row['time'] in frames,'Observation time must match a computed frame (hours from start)')
                f=frames[row['time']];metric=row['metric'];require(isinstance(metric,str) and row['unit']==METRIC_UNITS.get(metric),'Metric unit does not match the model');require(isinstance(metric,str) and metric in f['metrics'],'Unknown metric');pred=f['metrics'][metric]
                require((row['time'],metric) not in seen,'Duplicate observation');seen.add((row['time'],metric))
                require(number(row['actual']) and number(row['tolerance']) and row['tolerance']>=0 and number(pred),'Finite observation, prediction and nonnegative tolerance required')
                error=pred-row['actual'];out.append(dict(**row,predicted=pred,error=error,passed=abs(error)<=row['tolerance']))
            receipt=dict(session=key,config_sha256=s.record['config_sha256'],record_sha256=s.record['record_sha256'],source=body['source'],kind=body['kind'],rows=out,passed=all(r['passed'] for r in out),scope='Point comparisons to computed observations. Not automatic fitting, causal validation or proof of independent holdout.')
            receipt['sha256']=digest(canonical(receipt));save(s.folder/('evidence-'+uuid.uuid4().hex+'.json'),receipt);return receipt
    def close(self):
        with self.lock:
            for e in self.experiments.values():e['cancel']=True
        self.pool.shutdown(wait=True)
        for s in self.sessions.values():s.close('server_stopped')
