"""Demand-driven native sessions. A step request computes exactly one new month."""
import json
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import threading
import time
import uuid
from application.fathom_service.contracts import canonical,digest,fields,integer,require,validate_overrides
from application.fathom_service.core import save
from application.fathom_service.metrics import Results

class LiveSession:
    def __init__(self,folder,model,request,runner):
        self.id=folder.name;self.folder=folder;self.lock=threading.RLock();self.frames=[];self.revision=0;self.status='initializing';self.last_access=time.monotonic()
        self.description=model['description'];self.process=None
        shutil.copytree(model['directory'],folder/'inputs')
        parameters=validate_overrides(self.description,request['overrides'])
        self.record=dict(id=self.id,created_at=time.time(),mode='native-demand-driven',model_version=self.description['model_version'],
            input_sha256=model['hashes'],runner_sha256=digest(runner.read_bytes()),engine_identity=self.description['engine_identity'],
            parameters=parameters,seed=None,seed_note='Deterministic standalone SD; no random draws in this model.',
            command=[str(runner),str(folder/'inputs/model.json')],step_months=1,horizon_months=60,
            status=self.status,computed_through=0,frames=[],integrity='SHA-256 chained native frames; not a full batch-run manifest')
        self.log=(folder/'native.log').open('wb')
        try:
            self.process=subprocess.Popen(self.record['command'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.log,bufsize=0)
            self._send({'parameters':parameters});self._accept(self._read(),0)
        except Exception:
            self.close('failed');raise

    def _send(self,value):
        self.process.stdin.write(canonical(value)+b'\n');self.process.stdin.flush()
    def _read(self):
        deadline=time.monotonic()+120;data=bytearray()
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout,selectors.EVENT_READ)
            while time.monotonic()<deadline:
                ready=selector.select(max(0,deadline-time.monotonic()))
                require(bool(ready),'Native step timed out','LIVE_TIMEOUT',504)
                chunk=os.read(self.process.stdout.fileno(),65536)
                require(bool(chunk),'Native process ended before completing its step','LIVE_PROCESS',500)
                data.extend(chunk)
                require(len(data)<=2_000_000,'Native observation too large','LIVE_PROTOCOL',500)
                if data.endswith(b'\n'):
                    frame=json.loads(data);require('error' not in frame,frame.get('error','Native error'),'LIVE_ENGINE',422)
                    return frame
        raise RuntimeError('Native step timed out')
    def _accept(self,frame,expected):
        require(frame['month']==expected,'Unexpected native clock','LIVE_PROTOCOL',500)
        frame['previous_sha256']=self.frames[-1]['sha256'] if self.frames else None
        frame['sha256']=digest(canonical(frame));self.frames.append(frame);self.revision=expected
        self.status='failed' if not frame['checks_passed'] else 'completed' if frame['complete'] else 'paused'
        self.record.update(status=self.status,computed_through=self.revision,frames=self.frames)
        self._save()
        if self.status in ('failed','completed'):self._finish_process()
    def _finish_process(self):
        if self.process:
            if not self.process.stdin.closed:self.process.stdin.close()
            if self.process.poll() is None:
                try:self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:self.process.terminate();self.process.wait(timeout=2)
            if not self.process.stdout.closed:self.process.stdout.close()
        self.log.close()
    def _save(self):
        self.record['record_sha256']=digest(canonical({k:v for k,v in self.record.items() if k!='record_sha256'}))
        save(self.folder/'session.json',self.record)
    def view(self):
        frame=self.frames[-1];rows=[dict(scenario=0,replication=0,time=f['month'],output_id=k,value=v) for f in self.frames[-2:] for k,v in f['outputs'].items()]
        results=Results(self.description,rows);start=max(0,self.revision-1)
        values={m:results.value(m,0,0,start,self.revision)['value'] for m in ('revenue','ebitda','cash_flow','headcount','utilization','margin')}
        return dict(id=self.id,status=self.status,revision=self.revision,frame=frame,metrics=values,
            model_version=self.record['model_version'],runner_sha256=self.record['runner_sha256'])
    def step(self,expected):
        with self.lock:
            self.last_access=time.monotonic();require(integer(expected,0,60),'Expected revision must be an integer month')
            # Retry after a lost response returns the already committed observation.
            if expected==self.revision-1:return self.view()
            require(expected==self.revision,'Live clock changed; refresh state','LIVE_REVISION',409)
            require(self.status=='paused','Session is not available to advance','LIVE_STATE',409)
            self.status='computing'
            try:self._send({'command':'step'});self._accept(self._read(),self.revision+1)
            except Exception:
                self.close('failed');raise
            return self.view()
    def close(self,status='stopped'):
        with self.lock:
            if self.status not in ('completed','failed'):self.status=status
            self._finish_process();self.record.update(status=self.status,computed_through=self.revision)
            self._save()
            return dict(id=self.id,status=self.status,revision=self.revision)

class LiveSessions:
    def __init__(self,store,state,runner):
        self.store=store;self.state=state;self.runner=runner.resolve();state.mkdir(parents=True,exist_ok=True)
        self.sessions={};self.lock=threading.RLock()
    def create(self,request):
        fields(request,('model_version','overrides'),('model_version','overrides'))
        require(self.runner.is_file(),'Build the native live runner first: python projects/tr-ui/build_live.py','LIVE_UNAVAILABLE',503)
        model=self.store.model('ankura_tr');require(model['description']['model_version']==request['model_version'],'Model version changed','MODEL_VERSION_MISMATCH',409)
        self.store._verify_inputs(model);validate_overrides(model['description'],request['overrides'])
        with self.lock:
            for session in self.sessions.values():
                if session.status=='paused' and time.monotonic()-session.last_access>900:session.close('expired')
            require(sum(s.status in ('paused','computing') for s in self.sessions.values())<4,'Four live sessions already open; reset an existing session','LIVE_LIMIT',409)
            folder=self.state/uuid.uuid4().hex;folder.mkdir()
            session=LiveSession(folder,model,request,self.runner);self.sessions[session.id]=session
            return session.view()
    def get(self,key):
        with self.lock:require(key in self.sessions,'Live session unavailable; start a new simulation','LIVE_NOT_FOUND',404);return self.sessions[key]
    def close(self):
        for session in self.sessions.values():session.close('server_stopped')
