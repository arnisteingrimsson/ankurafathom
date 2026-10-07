"""Native JSON-line transport; the model waits for explicit advance commands."""
import json,os,selectors,time,subprocess
from application.fathom_service.contracts import canonical,digest,require
from application.fathom_service.core import save
class LiveSession:
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
                require(len(data)<=8_000_000,'Native observation too large','LIVE_PROTOCOL',500)
                if data.endswith(b'\n'):
                    frame=json.loads(data);require('error' not in frame,frame.get('error','Native error'),'LIVE_ENGINE',422)
                    return frame
        raise RuntimeError('Native step timed out')
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
